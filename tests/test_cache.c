/*
 * Unit tests for cache.c -- the on-disk thumbnail cache.
 *
 * The whole suite runs against a throwaway XDG_CACHE_HOME, so it can never
 * touch or be disturbed by the developer's real ~/.cache/stlpreview. The
 * environment variable is installed in main() before any cache function runs,
 * because glib resolves (and memoises) the cache directory on first use.
 */
#include <utime.h>

#include "../cache.c"

#include "test_fixtures.h"
#include "test_util.h"

/* Deterministic inputs for a cache key. */
#define KEY_PATH  "/models/widget.stl"
#define KEY_MTIME 1700000000
#define KEY_SIZE  4096

/* A CELL x CELL surface with a gradient, so round-trips can be compared
 * pixel-wise rather than just by dimensions. */
static cairo_surface_t *make_pattern(void)
{
    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CELL, CELL);
    cairo_surface_flush(s);
    unsigned char *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < CELL; y++)
        for (int x = 0; x < CELL; x++) {
            unsigned char *px = d + (size_t)y * stride + (size_t)x * 4;
            px[0] = (unsigned char)(x & 0xFF);       /* B */
            px[1] = (unsigned char)(y & 0xFF);       /* G */
            px[2] = (unsigned char)((x ^ y) & 0xFF); /* R */
            px[3] = 255;                             /* A */
        }
    cairo_surface_mark_dirty(s);
    return s;
}

static uint32_t pixel(cairo_surface_t *s, int x, int y)
{
    cairo_surface_flush(s);
    unsigned char *d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    uint32_t v;
    memcpy(&v, d + (size_t)y * stride + (size_t)x * 4, 4);
    cairo_surface_mark_dirty(s);
    return v;
}

static int file_exists(const char *path)
{
    return g_file_test(path, G_FILE_TEST_EXISTS);
}

/* A missed load leaves `loaded` NULL; fail loudly instead of dereferencing
 * it and turning an assertion failure into a segfault. */
static int require_surface(cairo_surface_t *s)
{
    T_CHECK(s != NULL);
    return s != NULL;
}

/* Cairo reads a fully opaque PNG back as RGB24, not ARGB32. The rasteriser
 * writes alpha 0xFF into every pixel, so *every* real cache entry comes back
 * as RGB24 -- a loader that insisted on ARGB32 would reject them all and the
 * cache would silently never hit. This test pins that behaviour. */
static void test_opaque_png_reads_back_as_rgb24(void)
{
    char *dir = fixture_tmpdir();
    char *png = g_build_filename(dir, "opaque.png", NULL);

    cairo_surface_t *opaque = make_pattern(); /* alpha == 255 everywhere */
    T_EQ_INT(cairo_surface_write_to_png(opaque, png), CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(opaque);

    cairo_surface_t *back = cairo_image_surface_create_from_png(png);
    T_EQ_INT(cairo_surface_status(back), CAIRO_STATUS_SUCCESS);
    T_EQ_INT(cairo_image_surface_get_format(back), CAIRO_FORMAT_RGB24);
    T_CHECK(CAIRO_FORMAT_RGB24 != CAIRO_FORMAT_ARGB32);
    cairo_surface_destroy(back);

    /* And a thumbnail with real transparency does stay ARGB32, so the loader
     * has to accept both rather than just switching its expectation. */
    cairo_surface_t *see_through =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CELL, CELL);
    cairo_surface_flush(see_through);
    unsigned char *d = cairo_image_surface_get_data(see_through);
    d[0] = 0x10;
    d[1] = 0x20;
    d[2] = 0x30;
    d[3] = 0x40; /* alpha < 255 in the first pixel */
    cairo_surface_mark_dirty(see_through);
    T_EQ_INT(cairo_surface_write_to_png(see_through, png),
             CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(see_through);

    back = cairo_image_surface_create_from_png(png);
    T_EQ_INT(cairo_image_surface_get_format(back), CAIRO_FORMAT_ARGB32);
    cairo_surface_destroy(back);

    g_free(png);
    fixture_rm_rf(dir);
    g_free(dir);
}

/* Write `text` over a cache sidecar. */
static void overwrite(const char *path, const char *text)
{
    GError *err = NULL;
    g_file_set_contents(path, text, -1, &err);
    g_assert_no_error(err);
}

/* Push a file's mtime into the past by `age_s` seconds. */
static void age_file(const char *path, time_t age_s)
{
    struct utimbuf t;
    time_t now = time(NULL) - age_s;
    t.actime = now;
    t.modtime = now;
    T_EQ_INT(utime(path, &t), 0);
}

/* ------------------------------------------------------------------ */
/* Key derivation.                                                     */
/* ------------------------------------------------------------------ */

static void test_cache_paths_are_stable_and_distinct(void)
{
    char *a = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    char *b = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    T_EQ_STR(a, b); /* same inputs, same key */
    T_CHECK(g_str_has_suffix(a, ".png"));
    g_free(b);

    /* Lives under $XDG_CACHE_HOME/stlpreview/thumbs. */
    char *want_dir =
        g_build_filename(g_get_user_cache_dir(), "stlpreview", "thumbs", NULL);
    T_CHECK(g_str_has_prefix(a, want_dir));
    g_free(want_dir);

    /* Editing the model, or changing the view, must invalidate the entry. */
    char *newer = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME + 1, KEY_SIZE);
    char *resized = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE + 1);
    char *other_view =
        cache_file_path(KEY_PATH, VIEW_FRONT, KEY_MTIME, KEY_SIZE);
    char *other_file =
        cache_file_path("/models/other.stl", VIEW_45, KEY_MTIME, KEY_SIZE);
    T_CHECK(strcmp(a, newer) != 0);
    T_CHECK(strcmp(a, resized) != 0);
    T_CHECK(strcmp(a, other_view) != 0);
    T_CHECK(strcmp(a, other_file) != 0);

    /* The PNG and its sidecar share a key and differ only by suffix. */
    char *meta = cache_meta_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    T_CHECK(g_str_has_suffix(meta, ".meta"));
    char *a_base = g_strndup(a, strlen(a) - 4);
    char *m_base = g_strndup(meta, strlen(meta) - 5);
    T_EQ_STR(a_base, m_base);
    g_free(a_base);
    g_free(m_base);

    g_free(newer);
    g_free(resized);
    g_free(other_view);
    g_free(other_file);
    g_free(meta);
    g_free(a);
}

/* ------------------------------------------------------------------ */
/* Round trip.                                                         */
/* ------------------------------------------------------------------ */

static void test_save_then_load_roundtrip(void)
{
    const float bbox[6] = {-1, -2, -3, 4, 5, 6};
    const float center[3] = {1.5, 1.5, 1.5};
    const float ext = 7.25f;

    cairo_surface_t *pattern = make_pattern();
    save_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, pattern,
                      FTYPE_STL_BINARY, 12, 8, bbox, center, ext);

    char *png = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    char *meta = cache_meta_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    T_CHECK(file_exists(png));
    T_CHECK(file_exists(meta));
    /* The atomic write must not leave its temporary behind. */
    char *tmp = g_strconcat(png, ".tmp", NULL);
    T_CHECK(!file_exists(tmp));
    g_free(tmp);

    cairo_surface_t *loaded = NULL;
    int type = FTYPE_UNKNOWN, tris = 0, verts = 0;
    float got_bbox[6] = {0}, got_center[3] = {0}, got_ext = 0;
    T_EQ_INT(load_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, &loaded,
                               &type, &tris, &verts, got_bbox, got_center,
                               &got_ext),
             1);

    T_EQ_INT(type, FTYPE_STL_BINARY);
    T_EQ_INT(tris, 12);
    T_EQ_INT(verts, 8);
    for (int i = 0; i < 6; i++)
        T_NEAR(got_bbox[i], bbox[i], 1e-5);
    for (int i = 0; i < 3; i++)
        T_NEAR(got_center[i], center[i], 1e-5);
    T_NEAR(got_ext, ext, 1e-5);

    /* Pixels survived the PNG round trip. The format is RGB24 here because
     * the pattern is fully opaque -- see
     * test_opaque_png_reads_back_as_rgb24. */
    if (require_surface(loaded)) {
        cairo_format_t fmt = cairo_image_surface_get_format(loaded);
        T_CHECK(fmt == CAIRO_FORMAT_RGB24 || fmt == CAIRO_FORMAT_ARGB32);
        T_EQ_INT(cairo_image_surface_get_width(loaded), CELL);
        T_EQ_INT(cairo_image_surface_get_height(loaded), CELL);
        T_CHECK(pixel(loaded, 0, 0) == pixel(pattern, 0, 0));
        T_CHECK(pixel(loaded, CELL / 2, CELL / 3) ==
                pixel(pattern, CELL / 2, CELL / 3));
        T_CHECK(pixel(loaded, CELL - 1, CELL - 1) ==
                pixel(pattern, CELL - 1, CELL - 1));
        cairo_surface_destroy(loaded);
    }

    /* Sidecar floats must be plain ASCII, whatever LC_NUMERIC is in force. */
    char *contents = NULL;
    T_CHECK(g_file_get_contents(meta, &contents, NULL, NULL));
    T_CHECK(contents != NULL && strchr(contents, ',') == NULL);
    g_free(contents);

    cairo_surface_destroy(pattern);
    cache_remove_for_file(KEY_PATH, KEY_MTIME, KEY_SIZE);
    g_free(png);
    g_free(meta);
}

/* ------------------------------------------------------------------ */
/* A pair is only a hit when both halves exist and agree.              */
/* ------------------------------------------------------------------ */

static void test_load_misses_without_sidecar(void)
{
    cairo_surface_t *pattern = make_pattern();
    save_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, pattern,
                      FTYPE_3MF, 4, 4, (const float[6]){0}, (const float[3]){0},
                      1.0f);
    cairo_surface_destroy(pattern);

    char *png = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    char *meta = cache_meta_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    T_EQ_INT(g_unlink(meta), 0);

    cairo_surface_t *loaded = NULL;
    int type, tris, verts;
    float bbox[6], center[3], ext;
    T_EQ_INT(load_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, &loaded,
                               &type, &tris, &verts, bbox, center, &ext),
             0);
    /* The now-orphaned PNG is swept rather than left to accumulate. */
    T_CHECK(!file_exists(png));

    g_free(png);
    g_free(meta);
}

static void test_load_misses_without_png(void)
{
    cairo_surface_t *pattern = make_pattern();
    save_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, pattern,
                      FTYPE_STL_ASCII, 12, 8, (const float[6]){0},
                      (const float[3]){0}, 1.0f);
    cairo_surface_destroy(pattern);

    char *png = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    char *meta = cache_meta_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    T_EQ_INT(g_unlink(png), 0);

    cairo_surface_t *loaded = NULL;
    int type, tris, verts;
    float bbox[6], center[3], ext;
    T_EQ_INT(load_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, &loaded,
                               &type, &tris, &verts, bbox, center, &ext),
             0);
    /* A sidecar with no image is useless too: drop it so the next render
     * repopulates a consistent pair. */
    T_CHECK(!file_exists(meta));

    g_free(png);
    g_free(meta);
}

static void test_load_rejects_wrong_png_geometry(void)
{
    cairo_surface_t *pattern = make_pattern();
    save_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, pattern,
                      FTYPE_STL_BINARY, 12, 8, (const float[6]){0},
                      (const float[3]){0}, 1.0f);
    cairo_surface_destroy(pattern);

    char *png = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    char *meta = cache_meta_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);

    /* Replace the thumbnail with a valid PNG of the wrong size: it must be
     * re-rendered, not stretched into a CELL x CELL cell. */
    cairo_surface_t *wrong =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CELL / 2, CELL / 2);
    T_EQ_INT(cairo_surface_write_to_png(wrong, png), CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(wrong);

    cairo_surface_t *loaded = NULL;
    int type, tris, verts;
    float bbox[6], center[3], ext;
    T_EQ_INT(load_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, &loaded,
                               &type, &tris, &verts, bbox, center, &ext),
             0);
    T_CHECK(!file_exists(png));
    T_CHECK(!file_exists(meta));

    g_free(png);
    g_free(meta);
}

static void test_load_rejects_malformed_sidecar(void)
{
    cairo_surface_t *pattern = make_pattern();
    save_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, pattern,
                      FTYPE_STL_BINARY, 12, 8, (const float[6]){0},
                      (const float[3]){0}, 1.0f);
    cairo_surface_destroy(pattern);

    char *png = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    char *meta = cache_meta_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);

    /* The sidecar is 13 whitespace-separated tokens; anything else is a miss
     * rather than a crash or a silently zeroed metadata read-out. */
    const char *bad[] = {"", "1 2 3", "not a number at all here",
                         "1 2 3 4 5 6 7 8 9 10 11 12 13 14"};
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        /* Re-save, because a miss deletes both halves. */
        cairo_surface_t *p2 = make_pattern();
        save_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, p2,
                          FTYPE_STL_BINARY, 12, 8, (const float[6]){0},
                          (const float[3]){0}, 1.0f);
        cairo_surface_destroy(p2);

        overwrite(meta, bad[i]);
        cairo_surface_t *loaded = NULL;
        int type, tris, verts;
        float bbox[6], center[3], ext;
        T_EQ_INT(load_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE,
                                   &loaded, &type, &tris, &verts, bbox, center,
                                   &ext),
                 0);
        T_CHECK(!file_exists(png));
        T_CHECK(!file_exists(meta));
    }

    g_free(png);
    g_free(meta);
}

/* ------------------------------------------------------------------ */
/* Invalidation and pruning.                                           */
/* ------------------------------------------------------------------ */

static void test_remove_for_file_covers_every_view(void)
{
    cairo_surface_t *pattern = make_pattern();
    for (int v = VIEW_45; v <= VIEW_RIGHT; v++)
        save_cached_thumb(KEY_PATH, v, KEY_MTIME, KEY_SIZE, pattern,
                          FTYPE_STL_BINARY, 12, 8, (const float[6]){0},
                          (const float[3]){0}, 1.0f);
    cairo_surface_destroy(pattern);

    /* Confirm they are all really there before removing them. */
    for (int v = VIEW_45; v <= VIEW_RIGHT; v++) {
        char *png = cache_file_path(KEY_PATH, v, KEY_MTIME, KEY_SIZE);
        T_CHECK(file_exists(png));
        g_free(png);
    }

    cache_remove_for_file(KEY_PATH, KEY_MTIME, KEY_SIZE);

    for (int v = VIEW_45; v <= VIEW_RIGHT; v++) {
        char *png = cache_file_path(KEY_PATH, v, KEY_MTIME, KEY_SIZE);
        char *meta = cache_meta_path(KEY_PATH, v, KEY_MTIME, KEY_SIZE);
        T_CHECK(!file_exists(png));
        T_CHECK(!file_exists(meta));
        g_free(png);
        g_free(meta);
    }
}

static void test_prune_sweeps_orphans_and_stale_temps(void)
{
    char *dir = cache_dir_path();
    T_EQ_INT(g_mkdir_with_parents(dir, DIR_MODE), 0);

    /* One healthy pair. */
    cairo_surface_t *pattern = make_pattern();
    save_cached_thumb(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE, pattern,
                      FTYPE_STL_BINARY, 12, 8, (const float[6]){0},
                      (const float[3]){0}, 1.0f);
    cairo_surface_destroy(pattern);
    char *good_png = cache_file_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);
    char *good_meta = cache_meta_path(KEY_PATH, VIEW_45, KEY_MTIME, KEY_SIZE);

    /* Junk that must be swept. */
    char *meta_only = g_build_filename(dir, "deadbeef.meta", NULL);
    char *png_only = g_build_filename(dir, "cafebabe.png", NULL);
    char *old_tmp = g_build_filename(dir, "interrupted.png.tmp", NULL);
    char *fresh_tmp = g_build_filename(dir, "inprogress.png.tmp", NULL);
    overwrite(meta_only, "1 2 3");
    overwrite(png_only, "x");
    overwrite(old_tmp, "x");
    overwrite(fresh_tmp, "x");
    age_file(old_tmp, CACHE_TMP_MAX_AGE_S + 600);

    /* cache_prune_once() runs at most once per process; reset the latch so
     * this test can drive it deterministically. */
    g_atomic_int_set(&cache_pruned, 0);
    cache_prune_once();

    T_CHECK(file_exists(good_png)); /* a complete pair survives */
    T_CHECK(file_exists(good_meta));
    T_CHECK(!file_exists(meta_only)); /* half a pair can never hit again */
    T_CHECK(!file_exists(png_only));
    T_CHECK(!file_exists(old_tmp));  /* leftovers from an interrupted write */
    T_CHECK(file_exists(fresh_tmp)); /* ... but a recent one may be in flight */

    g_free(good_png);
    g_free(good_meta);
    g_free(meta_only);
    g_free(png_only);
    g_free(old_tmp);
    g_free(fresh_tmp);
    g_free(dir);
}

static void test_prune_runs_once_per_session(void)
{
    char *dir = cache_dir_path();
    char *orphan = g_build_filename(dir, "second.meta", NULL);
    overwrite(orphan, "1 2 3");

    /* Already latched by the previous test: nothing happens. */
    cache_prune_once();
    T_CHECK(file_exists(orphan));

    /* A fresh session (or a fresh process) does sweep it. */
    g_atomic_int_set(&cache_pruned, 0);
    cache_prune_once();
    T_CHECK(!file_exists(orphan));

    /* And the latch holds again. */
    overwrite(orphan, "1 2 3");
    cache_prune_once();
    T_CHECK(file_exists(orphan));
    g_unlink(orphan);

    g_free(orphan);
    g_free(dir);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Point the cache at a scratch directory before glib resolves (and
     * memoises) XDG_CACHE_HOME. */
    char *xdg = fixture_tmpdir();
    setenv("XDG_CACHE_HOME", xdg, 1);

    printf("test_cache (XDG_CACHE_HOME=%s)\n", xdg);
    T_RUN(test_opaque_png_reads_back_as_rgb24);
    T_RUN(test_cache_paths_are_stable_and_distinct);
    T_RUN(test_save_then_load_roundtrip);
    T_RUN(test_load_misses_without_sidecar);
    T_RUN(test_load_misses_without_png);
    T_RUN(test_load_rejects_wrong_png_geometry);
    T_RUN(test_load_rejects_malformed_sidecar);
    T_RUN(test_remove_for_file_covers_every_view);
    T_RUN(test_prune_sweeps_orphans_and_stale_temps);
    T_RUN(test_prune_runs_once_per_session);
    int failed = T_REPORT();

    fixture_rm_rf(xdg);
    g_free(xdg);
    return failed;
}
