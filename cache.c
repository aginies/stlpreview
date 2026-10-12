/*
 * stlpreview -- on-disk thumbnail cache.
 *
 * Each rendered thumbnail is stored as a PNG plus a small text sidecar under
 * $XDG_CACHE_HOME/stlpreview/thumbs, keyed by a hash of the file's path,
 * modification time and size, so editing a model invalidates its entry.
 * The sidecar carries the metadata the info bar needs (format, triangle and
 * vertex counts, bounding box), which saves re-parsing the model just to
 * describe it.
 *
 * The cache is bounded: cache_prune_once() sweeps interrupted writes and
 * orphans, then evicts least-recently-used pairs until it fits in both
 * CACHE_MAX_BYTES and CACHE_MAX_ENTRIES. Writes go to a temporary file and
 * are renamed into place, so a crash cannot leave a truncated PNG that later
 * looks valid.
 */

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <glib/gstdio.h>

#include "stlview.h"

/* Bump when the renderer or a parser changes, so stale cached thumbnails are
 * discarded. The cache key also holds the file's mtime and size, so editing a
 * model in place invalidates its entry on its own. */
#define CACHE_VERSION 5

/* Bounds for the on-disk cache. When either is exceeded the least recently
 * used entries are dropped (both halves of a pair together), so the cache
 * cannot grow without limit across sessions. */
#define CACHE_MAX_BYTES   (256u * 1024u * 1024u)
#define CACHE_MAX_ENTRIES 4096u

/* Leftover *.tmp files from an interrupted write older than this are swept. */
#define CACHE_TMP_MAX_AGE_S 3600

static char *cache_dir_path(void)
{
    return g_build_filename(g_get_user_cache_dir(), "stlpreview", "thumbs",
                            NULL);
}

/* Hex SHA256 of (path, mtime, size, CELL, view, version) — path-safe and
 * computed over the full path (no truncation). Caller frees. */
static char *cache_key(const char *file_path, int view_mode, int64_t mtime,
                       int64_t fsize)
{
    char *input = g_strdup_printf(
        "%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT "|%d|%d|v%d", file_path,
        mtime, fsize, CELL, view_mode, CACHE_VERSION);
    char *cs = g_compute_checksum_for_string(G_CHECKSUM_SHA256, input, -1);
    g_free(input);
    return cs;
}

char *cache_file_path(const char *file_path, int view_mode, int64_t mtime,
                      int64_t fsize)
{
    char *key = cache_key(file_path, view_mode, mtime, fsize);
    char *dir = cache_dir_path();
    char *name = g_strconcat(key, ".png", NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_free(name);
    g_free(dir);
    g_free(key);
    return path;
}

static char *cache_meta_path(const char *file_path, int view_mode,
                             int64_t mtime, int64_t fsize)
{
    char *key = cache_key(file_path, view_mode, mtime, fsize);
    char *dir = cache_dir_path();
    char *name = g_strconcat(key, ".meta", NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_free(name);
    g_free(dir);
    g_free(key);
    return path;
}

/* Drop every cached thumbnail of one source file, across all view angles.
 * Called when the file is deleted or moved away, so its entries do not
 * linger as orphans (the cache key is a hash and cannot be searched). */
void cache_remove_for_file(const char *file_path, int64_t mtime, int64_t fsize)
{
    for (int view = 0; view <= VIEW_RIGHT; ++view) {
        char *png = cache_file_path(file_path, view, mtime, fsize);
        char *meta = cache_meta_path(file_path, view, mtime, fsize);
        g_unlink(png);
        g_unlink(meta);
        g_free(png);
        g_free(meta);
    }
}

/* One cache key's two halves, plus the data needed to evict it. */
typedef struct {
    char *key;
    char *png;  /* path, or NULL when that half is missing */
    char *meta; /* path, or NULL when that half is missing */
    guint64 bytes;
    gint64 stamp; /* newest mtime of the halves: our "last used" proxy */
} CacheEntry;

static void cache_entry_free(CacheEntry *e)
{
    if (!e)
        return;
    g_free(e->key);
    g_free(e->png);
    g_free(e->meta);
    g_free(e);
}

/* Add `path`'s size into *size_out and return its mtime (0 when absent). */
static gint64 file_stamp_and_size(const char *path, guint64 *size_out)
{
    struct stat st;
    if (g_stat(path, &st) != 0)
        return 0;
    *size_out += (guint64)st.st_size;
    return (gint64)st.st_mtime;
}

/* Oldest first; key as a stable tie-breaker. g_ptr_array_sort() passes
 * pointers to the elements, i.e. CacheEntry **. */
static int cache_entry_compare_by_stamp(gconstpointer a, gconstpointer b)
{
    CacheEntry *const *ea = (CacheEntry *const *)a;
    CacheEntry *const *eb = (CacheEntry *const *)b;
    if ((*ea)->stamp != (*eb)->stamp)
        return (*ea)->stamp < (*eb)->stamp ? -1 : 1;
    return g_strcmp0((*ea)->key, (*eb)->key);
}

/* Bound the thumbnail cache: sweep interrupted-write leftovers, drop entries
 * missing one half, then evict least-recently-used pairs until both the byte
 * and the entry budget are met. Runs once per session (see cache_prune_once). */
static void cache_prune(void)
{
    char *dir = cache_dir_path();
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) {
        g_free(dir);
        return;
    }

    GHashTable *entries = g_hash_table_new_full(
        g_str_hash, g_str_equal, NULL, (GDestroyNotify)cache_entry_free);
    gint64 now = (gint64)time(NULL);
    const char *name;
    while ((name = g_dir_read_name(gd))) {
        char *full = g_build_filename(dir, name, NULL);

        if (g_str_has_suffix(name, ".tmp")) {
            guint64 sz = 0;
            gint64 stamp = file_stamp_and_size(full, &sz);
            if (stamp == 0 || now - stamp > CACHE_TMP_MAX_AGE_S)
                g_unlink(full);
            g_free(full);
            continue;
        }

        char *key = NULL;
        int is_png = 0;
        if (g_str_has_suffix(name, ".png")) {
            key = g_strndup(name, strlen(name) - 4);
            is_png = 1;
        } else if (g_str_has_suffix(name, ".meta")) {
            key = g_strndup(name, strlen(name) - 5);
        }
        if (!key) {
            g_free(full);
            continue;
        }

        CacheEntry *e = (CacheEntry *)g_hash_table_lookup(entries, key);
        if (!e) {
            e = g_new0(CacheEntry, 1);
            e->key = key; /* the hash table borrows the key from the entry */
            g_hash_table_insert(entries, e->key, e);
        } else {
            g_free(key);
        }
        guint64 sz = 0;
        gint64 stamp = file_stamp_and_size(full, &sz);
        e->bytes += sz;
        if (stamp > e->stamp)
            e->stamp = stamp;
        if (is_png)
            e->png = full;
        else
            e->meta = full;
    }
    g_dir_close(gd);

    GPtrArray *list = g_ptr_array_new();
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, entries);
    while (g_hash_table_iter_next(&it, NULL, &value))
        g_ptr_array_add(list, value);

    /* Pass 1: sweep keys that lost one half (interrupted write, or a half
     * deleted behind our back). They can never become a cache hit again. */
    GPtrArray *pairs = g_ptr_array_new();
    guint64 total = 0;
    guint orphans = 0;
    for (guint i = 0; i < list->len; ++i) {
        CacheEntry *e = (CacheEntry *)list->pdata[i];
        if (e->png && e->meta) {
            total += e->bytes;
            g_ptr_array_add(pairs, e);
        } else {
            if (e->png)
                g_unlink(e->png);
            if (e->meta)
                g_unlink(e->meta);
            orphans++;
        }
    }

    /* Pass 2: evict least-recently-used pairs until both budgets are met. */
    g_ptr_array_sort(pairs, cache_entry_compare_by_stamp);
    guint remaining = pairs->len;
    guint evicted = 0;
    for (guint i = 0; i < pairs->len; ++i) {
        if (total <= CACHE_MAX_BYTES && remaining <= CACHE_MAX_ENTRIES)
            break; /* sorted oldest-first: the rest fits in the budget */
        CacheEntry *e = (CacheEntry *)pairs->pdata[i];
        g_unlink(e->png);
        g_unlink(e->meta);
        total -= e->bytes;
        remaining--;
        evicted++;
    }
    if (orphans > 0 || evicted > 0) {
        char *sz = g_format_size(total);
        g_debug("thumbnail cache: swept %u orphan%s, evicted %u LRU entr%s, "
                "%u pairs (%s) remaining",
                orphans, orphans == 1 ? "" : "s", evicted,
                evicted == 1 ? "y" : "ies", remaining, sz);
        g_free(sz);
    }

    /* Both arrays borrow the entries (the hash table owns them) and were
     * created without a free function, so TRUE releases just the segments. */
    g_ptr_array_free(pairs, TRUE);
    g_ptr_array_free(list, TRUE);
    g_hash_table_destroy(entries);
    g_free(dir);
}

/* cache_prune() is only worth running once per session. */
static gint cache_pruned = 0;

void cache_prune_once(void)
{
    if (g_atomic_int_compare_and_exchange(&cache_pruned, 0, 1))
        cache_prune();
}

/* Try to load a cached thumbnail + its metadata sidecar.
 * Returns 1 on hit (thumb_out + metadata filled), 0 on miss.
 * An entry only counts as a hit when BOTH halves exist and agree: a .png
 * without a sidecar (interrupted write, or sidecar deleted) used to be
 * accepted and then reported zeroed metadata, and a thumbnail whose geometry
 * no longer matches CELL must be re-rendered rather than stretched. */
int load_cached_thumb(const char *file_path, int view_mode, int64_t mtime,
                      int64_t fsize, cairo_surface_t **thumb_out,
                      int *file_type, int *tri_count, int *vert_count,
                      float *bbox, float *center, float *ext)
{
    char *png_path = cache_file_path(file_path, view_mode, mtime, fsize);
    char *meta_path = cache_meta_path(file_path, view_mode, mtime, fsize);

    int type = FTYPE_UNKNOWN, tris = 0, verts = 0;
    float bb[6] = {0}, cc[3] = {0}, ex = 0;

    char *contents = NULL;
    if (!g_file_get_contents(meta_path, &contents, NULL, NULL)) {
        /* No sidecar: the .png (if any) is an orphan, remove it. */
        g_unlink(png_path);
        g_free(png_path);
        g_free(meta_path);
        return 0;
    }

    /* Sidecar floats are locale-independent (see ascii_dtostr/ascii_strtod):
     * a cache written under LC_NUMERIC=fr_FR must read back identically.
     * Layout: type tris verts bbox[6] center[3] ext  (13 tokens). */
    int parsed = 0;
    char **tok = g_strsplit_set(contents, " \t\r\n", 0);
    guint n_tok = 0;
    for (char **t = tok; *t; ++t)
        n_tok++;
    if (n_tok == 13) {
        type = (int)g_ascii_strtoll(tok[0], NULL, 10);
        tris = (int)g_ascii_strtoll(tok[1], NULL, 10);
        verts = (int)g_ascii_strtoll(tok[2], NULL, 10);
        for (int i = 0; i < 6; ++i)
            bb[i] = (float)ascii_strtod(tok[3 + i], 0.0);
        for (int i = 0; i < 3; ++i)
            cc[i] = (float)ascii_strtod(tok[9 + i], 0.0);
        ex = (float)ascii_strtod(tok[12], 0.0);
        parsed = 1;
    }
    g_strfreev(tok);
    g_free(contents);

    cairo_surface_t *s = NULL;
    if (parsed) {
        s = cairo_image_surface_create_from_png(png_path);
        /* Accept RGB24 as well as ARGB32. The rasteriser writes alpha 0xFF
         * into every pixel, and cairo reads a fully opaque PNG back as
         * RGB24, so demanding ARGB32 here would reject every entry this
         * program ever wrote and silently turn the cache into a no-op.
         * Both formats serve equally well as a paint source, which is all a
         * cached thumbnail is used for. */
        cairo_format_t fmt = cairo_image_surface_get_format(s);
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS ||
            (fmt != CAIRO_FORMAT_ARGB32 && fmt != CAIRO_FORMAT_RGB24) ||
            cairo_image_surface_get_width(s) != CELL ||
            cairo_image_surface_get_height(s) != CELL) {
            cairo_surface_destroy(s);
            s = NULL;
        }
    }
    if (!s) {
        /* Corrupt or stale pair: drop both halves so the next render
         * repopulates a consistent entry. */
        g_unlink(png_path);
        g_unlink(meta_path);
        g_free(png_path);
        g_free(meta_path);
        return 0;
    }

    *thumb_out = s;
    *file_type = type;
    *tri_count = tris;
    *vert_count = verts;
    memcpy(bbox, bb, sizeof(bb));
    memcpy(center, cc, sizeof(cc));
    *ext = ex;
    g_free(png_path);
    g_free(meta_path);
    return 1;
}

/* Save a thumbnail + metadata sidecar to the cache. Both halves are written
 * atomically (temporary name + rename, and g_file_set_contents) so a reader
 * never sees a truncated PNG: load_cached_thumb() treats a half-written pair
 * as a miss, and cache_prune() sweeps the leftovers. */
void save_cached_thumb(const char *file_path, int view_mode, int64_t mtime,
                       int64_t fsize, cairo_surface_t *thumb, int file_type,
                       int tri_count, int vert_count, const float *bbox,
                       const float *center, float ext)
{
    char *dir = cache_dir_path();
    char *file = cache_file_path(file_path, view_mode, mtime, fsize);
    if (g_mkdir_with_parents(dir, DIR_MODE) != 0) {
        g_free(file);
        g_free(dir);
        return;
    }

    char *tmp = g_strconcat(file, ".tmp", NULL);
    if (cairo_surface_write_to_png(thumb, tmp) == CAIRO_STATUS_SUCCESS) {
        if (g_rename(tmp, file) != 0)
            g_unlink(tmp); /* could not publish: do not leave a stray temp */

        /* Write floats with '.' regardless of LC_NUMERIC. */
        GString *meta_s = g_string_new(NULL);
        g_string_append_printf(meta_s, "%d %d %d", file_type, tri_count,
                               vert_count);
        double vals[10];
        for (int i = 0; i < 6; ++i)
            vals[i] = bbox[i];
        for (int i = 0; i < 3; ++i)
            vals[6 + i] = center[i];
        vals[9] = ext;
        for (int i = 0; i < 10; ++i) {
            char *num = ascii_dtostr(vals[i]);
            g_string_append_printf(meta_s, " %s", num);
            g_free(num);
        }
        char *meta_path = cache_meta_path(file_path, view_mode, mtime, fsize);
        g_file_set_contents(meta_path, meta_s->str, -1, NULL);
        g_string_free(meta_s, TRUE);
        g_free(meta_path);
    } else {
        g_unlink(tmp);
    }
    g_free(tmp);
    g_free(file);
    g_free(dir);
}
