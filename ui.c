/*
 * stlpreview -- GTK3 front end.
 *
 * Directory tree on the left, thumbnail grid on the right, plus the view and
 * sort toolbar, the status bar, and the move / delete / export dialogs.
 *
 * Rendering is asynchronous: a worker thread runs render_paths_sync() and
 * publishes its result back with g_idle_add, guarded by a generation counter
 * so a superseded job's result is dropped instead of overwriting newer
 * content. Folder listings are paginated and thumbnails are streamed in and
 * out of memory, so a directory with thousands of models stays responsive.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <glib/gstdio.h>

#include "ui.h"

/* Maximum number of files a single render job covers. A folder listing is
 * rendered one page at a time ("Load more" appends the next page) so a
 * directory with thousands of models cannot block the UI or exhaust memory.
 * Used for the initial render too, so startup and a folder click behave the
 * same way. */
#define RENDER_PAGE_SIZE 20

/* Thumbnail streaming: only the cells on screen (plus this many rows above
 * and below) keep their ARGB32 surface in memory. Everything else releases
 * the ~400 KB surface and reloads it from the on-disk cache when it scrolls
 * back into view. */
#define THUMB_STREAM_MARGIN_ROWS 1

/* How often the UI thread polls a running job for progress text. */
#define PROGRESS_POLL_MS 100

/* Copying or extracting a folder of models fires one event per file, so
 * filesystem events are coalesced and the folder is re-read once things go
 * quiet. */
#define MONITOR_DEBOUNCE_MS 500


struct RenderJob {
    GPtrArray *paths; /* char*, owned */
    int view_mode;
    int append;     /* non-zero: add to the grid instead of replacing it */
    guint page_end; /* ui->shown_count to apply when publishing */
    guint gen;      /* generation; stale results are dropped */
    UI *ui;         /* UI thread publishes there */
    gint cancelled; /* atomic: set by the UI thread, polled by the worker */
    GMutex lock;    /* protects the progress fields and `items` */
    guint files_done;
    guint files_total;
    size_t tris_done;
    size_t tris_total;
    char *current;    /* basename being processed, owned; guarded by lock */
    GPtrArray *items; /* result, set right before publishing */
};

/* --- forward declarations (generated) --- */
static void update_status_bar(UI *ui, Item *it);
static gboolean publish_render_job(gpointer data);
static void cancel_move(UI *ui);
static GtkWidget *build_grid(UI *ui);
static void show_items(UI *ui, GPtrArray *items);
static void append_items(UI *ui, GPtrArray *items);
static void render_current_selection(UI *ui);
static void monitor_set_dir(UI *ui, const char *dir);
static void on_selection_changed(GtkTreeSelection *sel, UI *ui);
static void on_parent_clicked(GtkButton *b, UI *ui);
/* Used through a cast rather than a call, so the generator's
 * "name followed by (" heuristic did not see these three. */
static int path_info_compare(gconstpointer a, gconstpointer b, gpointer data);
static int g_strcmp0_adapter(const void *a, const void *b, gpointer ud);
static void on_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cr,
                         GtkTreeModel *model, GtkTreeIter *iter, UI *ui);
/* --- end forward declarations --- */

/* CSS provider for the selected-item border */
static GtkCssProvider *border_provider = NULL;

static void ensure_border_provider(void)
{
    if (!border_provider) {
        border_provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(border_provider,
                                        ".preview-border { "
                                        "border: 2px solid #4a90d9; "
                                        "border-radius: 3px; "
                                        "}",
                                        -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(border_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

static const char *sort_mode_names[] = {"Name", "Size", "Date"};

/* Periodic progress hook so the caller can report status and stay
 * responsive. `done`/`total` are triangle counts; `user_data` is whatever
 * the caller passed to render_mesh(). Returning non-zero aborts the render
 * (the surface is left partially drawn and must be discarded).
 * The renderer itself never touches GTK or any UI toolkit: headless callers
 * simply pass NULL. */
typedef int (*RenderProgressFn)(size_t done, size_t total, void *user_data);

static void update_status_bar(UI *ui, Item *it);

enum { COL_NAME, COL_PATH, COL_ISDIR, COL_LOADED, N_COLS };

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static char *format_file_size(int64_t bytes)
{
    if (bytes < 1024)
        return g_strdup_printf("%" G_GINT64_FORMAT " B", bytes);
    if (bytes < 1024 * 1024)
        return g_strdup_printf("%.1f KB", bytes / 1024.0);
    return g_strdup_printf("%.1f MB", bytes / (1024.0 * 1024.0));
}

static char *format_file_date(time_t mtime)
{
    struct tm *tm = localtime(&mtime);
    if (!tm)
        return g_strdup("unknown");
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
    return g_strdup(buf);
}

static const char *file_type_label(int type)
{
    switch (type) {
        case FTYPE_STL_BINARY: return "binary STL";
        case FTYPE_STL_ASCII: return "ASCII STL";
        case FTYPE_3MF: return "3MF";
        default: return "";
    }
}

/* Update the status bar with info about the selected Item. */
static void update_status_bar(UI *ui, Item *it)
{
    if (!ui || !ui->status_bar || !it || !it->name || !it->path)
        return;
    char buf[512];
    if (it->tri_count > 0) {
        char *fsize = format_file_size(it->file_size);
        char *fdate = format_file_date(it->mtime);
        g_snprintf(buf, sizeof(buf),
                   "%s  ·  %d tris · %d verts · "
                   "%.0f×%.0f×%.0f mm · %s · %s  ·  %s",
                   it->name, it->tri_count, it->vert_count,
                   it->bbox[3] - it->bbox[0], it->bbox[4] - it->bbox[1],
                   it->bbox[5] - it->bbox[2], fsize, fdate,
                   file_type_label(it->file_type));
        g_free(fsize);
        g_free(fdate);
    } else {
        char *fsize = format_file_size(it->file_size);
        char *fdate = format_file_date(it->mtime);
        g_snprintf(buf, sizeof(buf), "%s  ·  %s  ·  %s", it->name, fsize,
                   fdate);
        g_free(fsize);
        g_free(fdate);
    }
    gtk_label_set_text(GTK_LABEL(ui->status_bar), buf);
}

/* Put `w` into the right panel, replacing whatever is there.
 * The scrolled window auto-wraps non-scrollable children in a viewport,
 * so we must destroy the viewport (the direct child), not just our widget. */
static void set_panel_content(UI *ui, GtkWidget *w)
{
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(ui->grid_scroll));
    if (old)
        gtk_widget_destroy(old);
    /* Whatever was there is gone: if it was the loading panel, its label
     * must not be touched by the progress poller any more. */
    ui->progress_label = NULL;
    gtk_container_add(GTK_CONTAINER(ui->grid_scroll), w);
    ui->grid = w;
    gtk_widget_show_all(w);
}

/* Show a spinner + status label in the right panel while a worker thread
 * loads and renders. The label is registered in ui->progress_label so the
 * progress poller can refresh it; set_panel_content() clears the
 * registration as soon as the panel is replaced. */
/* Drop every Item's back-pointers to grid widgets. Needed whenever the grid is
 * replaced by something else (loading placeholder): the GtkImage and frame
 * widgets are destroyed along with it, and update_thumb_stream() must not
 * dereference them afterwards. build_grid() re-assigns both pointers for every
 * Item it lays out, so this is only a safety net for the in-between window. */
static void detach_item_widgets(UI *ui)
{
    if (!ui || !ui->items)
        return;
    for (guint i = 0; i < ui->items->len; ++i) {
        Item *it = ui->items->pdata[i];
        it->image = NULL;
        it->grid_cell = NULL;
    }
}

static void show_loading(UI *ui, const char *text)
{
    /* The grid and all of its cell/image widgets are about to be destroyed.
     * (Only reached when replacing the grid; an append job keeps it and turns
     * the "Load more" button into the progress indicator instead.) */
    detach_item_widgets(ui);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *spin = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spin));
    GtkWidget *lbl = gtk_label_new(text);
    gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    set_panel_content(ui, box);
    ui->progress_label = lbl;
}

/* Release one Item. Used as the GDestroyNotify of every Item array, so
 * g_ptr_array_free(arr, TRUE) releases the elements *and* the segment.
 *
 * Array ownership convention used throughout this file: a GPtrArray always
 * owns its elements through the free function given at creation
 * (g_ptr_array_new_full()), and is always released with free_seg = TRUE.
 * Two GLib details make this easy to get wrong, so they are spelled out:
 *  - g_ptr_array_free(arr, FALSE) frees only the GPtrArray struct and leaks
 *    the pointer segment; TRUE frees the segment and, when a free function is
 *    set, every element.
 *  - g_ptr_array_set_size(arr, n) *also* applies the free function to the
 *    elements it drops, so it is not a way to detach them.
 * To hand elements to another owner, copy the pointers out, clear the free
 * function with g_ptr_array_set_free_func(src, NULL), then free with TRUE. */
static void item_free(Item *it)
{
    if (!it)
        return;
    if (it->thumb)
        cairo_surface_destroy(it->thumb);
    g_free(it->name);
    g_free(it->path);
    g_free(it);
}

/* GPtrArray constructors for the two element types used here. */
static GPtrArray *items_array_new(guint reserved)
{
    return g_ptr_array_new_full(reserved, (GDestroyNotify)item_free);
}

static GPtrArray *paths_array_new(guint reserved)
{
    return g_ptr_array_new_full(reserved, (GDestroyNotify)g_free);
}

/* ------------------------------------------------------------------ */
/* Thumbnail streaming                                                 */
/*                                                                     */
/* Every rendered thumbnail also lives in the on-disk PNG cache, so the   */
/* in-memory ARGB32 surface (~400 KB each) is only kept for cells that    */
/* are actually on screen. Off-screen items release their surface and      */
/* reload it from the cache when they scroll back into view, which keeps   */
/* memory bounded no matter how many models a folder holds.                */
/* ------------------------------------------------------------------ */
/* Reload `it`'s thumbnail from the on-disk PNG cache, or NULL when the
 * entry is gone. */
static cairo_surface_t *thumb_from_cache(const Item *it, int view_mode)
{
    char *path =
        cache_file_path(it->path, view_mode, (int64_t)it->mtime, it->file_size);
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    g_free(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    return s;
}

/* Fallback for a thumbnail whose cache entry disappeared (e.g. the cache was
 * cleared while the app ran). Renders one mesh, which is bounded work, and
 * repopulates the cache. */
static cairo_surface_t *thumb_rerender(const Item *it, int view_mode)
{
    Mesh mesh = {0};
    int type = FTYPE_UNKNOWN;
    if (!load_mesh(it->path, &mesh, &type)) {
        g_free(mesh.tris);
        return NULL;
    }
    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CELL, CELL);
    float bbox[6] = {0}, center[3] = {0}, ext = 0;
    if (render_mesh(&mesh, s, view_mode, bbox, center, &ext, NULL, NULL) !=
        RENDER_OK) {
        cairo_surface_destroy(s);
        s = NULL;
    } else {
        save_cached_thumb(it->path, view_mode, (int64_t)it->mtime,
                          it->file_size, s, it->file_type, it->tri_count,
                          it->vert_count, bbox, center, ext);
    }
    g_free(mesh.tris);
    return s;
}

/* Make sure `it`'s thumbnail is resident and shown in its cell. */
static void item_ensure_thumb(UI *ui, Item *it)
{
    if (!it->thumb) {
        it->thumb = thumb_from_cache(it, (int)ui->view_mode);
        if (!it->thumb)
            it->thumb = thumb_rerender(it, (int)ui->view_mode);
    }
    if (it->thumb && it->image && GTK_IS_IMAGE(it->image))
        gtk_image_set_from_surface(GTK_IMAGE(it->image), it->thumb);
}

/* Release `it`'s surface and blank its cell, keeping the cell geometry so
 * the grid does not reflow. */
static void item_release_thumb(Item *it)
{
    if (!it->thumb)
        return;
    cairo_surface_destroy(it->thumb);
    it->thumb = NULL;
    if (it->image && GTK_IS_IMAGE(it->image)) {
        gtk_image_clear(GTK_IMAGE(it->image));
        gtk_widget_set_size_request(it->image, CELL, CELL);
    }
}

/* Keep only the visible cells' thumbnails resident. A no-op until the
 * viewport has been allocated (page size unknown), so the initial grid is
 * never stripped and immediately reloaded. */
static void update_thumb_stream(UI *ui)
{
    if (!ui || !GTK_IS_GRID(ui->grid) || ui->grid_cols < 1 || !ui->items)
        return;
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ui->grid_scroll));
    if (!vadj)
        return;
    double page = gtk_adjustment_get_page_size(vadj);
    if (page <= 0.0)
        return; /* not laid out yet */
    double top = gtk_adjustment_get_value(vadj);

    double row_h = (double)(CELL + LABEL_H + GRID_ROW_SPACING);
    int first_row = (int)(top / row_h) - THUMB_STREAM_MARGIN_ROWS;
    int last_row = (int)((top + page) / row_h) + THUMB_STREAM_MARGIN_ROWS;
    if (first_row < 0)
        first_row = 0;

    for (guint i = 0; i < ui->items->len; ++i) {
        Item *it = (Item *)ui->items->pdata[i];
        int row = (int)(i / (guint)ui->grid_cols);
        /* The selected item stays resident: it is the one the user is
         * acting on (export, move, delete). */
        if (it->selected || (row >= first_row && row <= last_row))
            item_ensure_thumb(ui, it);
        else
            item_release_thumb(it);
    }
}

/* ------------------------------------------------------------------ */
/* Batch load / render — pure computation, safe off the main thread     */
/* ------------------------------------------------------------------ */
/* Optional hooks letting the caller of render_paths_sync() report progress
 * and request cancellation. They run on whatever thread executes
 * render_paths_sync() (normally a worker), so they must not touch GTK
 * widgets; publish state and let the UI thread poll it instead.
 * Returning non-zero aborts the whole batch. Pass NULL when headless. */
typedef struct RenderHooks {
    /* Called before each file is processed. */
    int (*file)(guint index, guint total, const char *path, void *ud);
    /* Called from the rasteriser every RENDER_YIELD_INTERVAL triangles. */
    int (*tris)(size_t done, size_t total, void *ud);
    void *ud;
} RenderHooks;

/* Load every STL/3MF in `paths` (full paths) and render thumbnails into
 * freshly allocated Items. Uses the thumbnail cache to skip rendering for
 * previously-seen (path, mtime, size, view) tuples, and keeps mesh metadata
 * (bbox, centroid, extents, triangle/vertex counts) for the info bar.
 * Never calls into GTK, so it can run on a worker thread; the returned
 * array (and the cairo surfaces inside it) is then handed to the UI thread. */
GPtrArray *render_paths_sync(GPtrArray *paths, int view_mode,
                             const RenderHooks *hooks)
{
    GPtrArray *items = items_array_new(paths ? paths->len : 0);
    for (guint i = 0; i < paths->len; ++i) {
        const char *fpath = (const char *)paths->pdata[i];
        if (hooks && hooks->file &&
            hooks->file(i, paths->len, fpath, hooks->ud) != 0)
            break; /* cancelled */
        cairo_surface_t *thumb = NULL;
        int file_type = FTYPE_UNKNOWN;
        float bbox[6] = {0}, center[3] = {0}, ext = 0;
        int tri_count = 0, vert_count = 0;
        int aborted = 0; /* caller asked us to stop: not a bad file */
        struct stat st;
        int64_t file_size = 0, mtime = 0;

        /* Stat the file (always available) */
        if (stat(fpath, &st) == 0) {
            file_size = st.st_size;
            mtime = st.st_mtime;
        }

        /* Try cache first (key includes mtime + size, so edits invalidate) */
        if (!load_cached_thumb(fpath, view_mode, mtime, file_size, &thumb,
                               &file_type, &tri_count, &vert_count, bbox,
                               center, &ext)) {
            Mesh mesh = {0};
            /* The loader sniffs the real format (binary vs ASCII STL cannot
             * be told apart from the file name). */
            int ok = load_mesh(fpath, &mesh, &file_type);
            if (ok) {
                tri_count = (int)mesh.count;
                vert_count = (int)mesh_unique_vert_count(&mesh);

                thumb =
                    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CELL, CELL);
                int rc = render_mesh(&mesh, thumb, view_mode, bbox, center,
                                     &ext, hooks ? hooks->tris : NULL,
                                     hooks ? hooks->ud : NULL);
                if (rc != RENDER_OK) {
                    /* Never cache a partial thumbnail. Only RENDER_ABORT stops
                     * the batch: a file that merely fails to render is skipped
                     * and reported, so one bad model cannot truncate the rest
                     * of the directory. */
                    cairo_surface_destroy(thumb);
                    thumb = NULL;
                    aborted = (rc == RENDER_ABORT);
                } else {
                    save_cached_thumb(fpath, view_mode, mtime, file_size, thumb,
                                      file_type, tri_count, vert_count, bbox,
                                      center, ext);
                }
            }
            /* Always release the triangle array: a partially-read mesh (bad
             * size field, corrupt 3MF) has already allocated triangles by the
             * time the loader reports failure. */
            g_free(mesh.tris);
        }
        if (thumb) {
            Item *item = g_new0(Item, 1);
            item->name = g_path_get_basename(fpath);
            item->path = g_strdup(fpath);
            item->thumb = thumb;
            memcpy(item->bbox, bbox, sizeof(bbox));
            memcpy(item->center, center, sizeof(center));
            item->ext = ext;
            item->tri_count = tri_count;
            item->vert_count = vert_count;
            item->file_size = file_size;
            item->mtime = mtime;
            item->file_type = file_type;
            g_ptr_array_add(items, item);
        } else if (!aborted) {
            g_warning("skipping %s: could not be loaded or rendered", fpath);
        }
        if (aborted)
            break; /* stop the batch as soon as the caller asked us to */
    }
    return items;
}

static gboolean publish_render_job(gpointer data);

static void render_job_free(RenderJob *job)
{
    if (!job)
        return;
    if (job->items)
        g_ptr_array_free(job->items, TRUE);
    g_ptr_array_free(job->paths, TRUE);
    g_free(job->current);
    g_mutex_clear(&job->lock);
    g_free(job);
}

/* Worker hook: about to start `path`. Returns non-zero to abort the batch. */
static int job_file_hook(guint index, guint total, const char *path, void *ud)
{
    RenderJob *job = (RenderJob *)ud;
    char *base = g_path_get_basename(path);
    g_mutex_lock(&job->lock);
    job->files_done = index;
    job->files_total = total;
    job->tris_done = 0;
    job->tris_total = 0;
    g_free(job->current);
    job->current = base; /* transfer ownership */
    g_mutex_unlock(&job->lock);
    return g_atomic_int_get(&job->cancelled);
}

/* Worker hook: rasteriser progress, and the cancellation checkpoint that
 * keeps a superseded job from burning CPU. */
static int job_tris_hook(size_t done, size_t total, void *ud)
{
    RenderJob *job = (RenderJob *)ud;
    g_mutex_lock(&job->lock);
    job->tris_done = done;
    job->tris_total = total;
    g_mutex_unlock(&job->lock);
    return g_atomic_int_get(&job->cancelled);
}

/* Worker thread body. g_idle_add() must be the last use of `job`: after it
 * the UI thread owns the job and may free it. */
static gpointer render_worker(gpointer data)
{
    RenderJob *job = (RenderJob *)data;
    const RenderHooks hooks = {job_file_hook, job_tris_hook, job};
    GPtrArray *items = render_paths_sync(job->paths, job->view_mode, &hooks);
    g_mutex_lock(&job->lock);
    job->files_done = job->files_total;
    job->items = items;
    g_mutex_unlock(&job->lock);
    g_idle_add(publish_render_job, job);
    return NULL;
}

/* Refresh the window title from the current selection and how much of it has
 * been rendered so far. */
static void update_window_title(UI *ui)
{
    char *title;
    if (!ui->cur_path) {
        title = g_strdup("stlpreview");
    } else if (ui->cur_is_dir) {
        guint total = ui->all_paths ? ui->all_paths->len : 0;
        if (ui->shown_count < total)
            title = g_strdup_printf("stlpreview — %s (%u files, showing %u)",
                                    ui->cur_path, total, ui->shown_count);
        else
            title = g_strdup_printf("stlpreview — %s (%u files)", ui->cur_path,
                                    total);
    } else {
        title = g_strdup_printf("stlpreview — %s", ui->cur_path);
    }
    gtk_window_set_title(GTK_WINDOW(ui->window), title);
    g_free(title);
}

/* Show "Load more" only while part of the listing is still unrendered. */
static void update_more_btn(UI *ui)
{
    if (!ui->more_btn)
        return;
    guint total = ui->all_paths ? ui->all_paths->len : 0;
    guint remaining = (ui->shown_count < total) ? total - ui->shown_count : 0;
    if (remaining == 0) {
        gtk_widget_set_visible(ui->more_btn, FALSE);
        return;
    }
    guint page = MIN(remaining, (guint)RENDER_PAGE_SIZE);
    char *label = g_strdup_printf("Load %u more", page);
    gtk_button_set_label(GTK_BUTTON(ui->more_btn), label);
    g_free(label);
    gtk_widget_set_sensitive(ui->more_btn, TRUE);
    gtk_widget_set_visible(ui->more_btn, TRUE);
}

/* UI thread: adopt a finished job's result, or drop it when the job was
 * superseded or cancelled while it was running. */
static gboolean publish_render_job(gpointer data)
{
    RenderJob *job = (RenderJob *)data;
    UI *ui = job->ui;

    g_mutex_lock(&job->lock);
    GPtrArray *items = job->items;
    job->items = NULL;
    g_mutex_unlock(&job->lock);

    gboolean stale =
        (job->gen != ui->job_gen) || g_atomic_int_get(&job->cancelled) != 0;

    /* Only the still-current job may touch shared UI state. */
    gboolean was_current = (ui->job == job);
    if (was_current) {
        ui->job = NULL;
        if (ui->progress_src) {
            g_source_remove(ui->progress_src);
            ui->progress_src = 0;
        }
    }

    if (stale || !items) {
        if (items)
            g_ptr_array_free(items, TRUE);
        /* A cancelled "Load more" must not leave the button stuck. */
        if (was_current)
            update_more_btn(ui);
        render_job_free(job);
        return G_SOURCE_REMOVE;
    }

    if (job->append)
        append_items(ui, items);
    else
        show_items(ui, items);
    ui->shown_count = job->page_end;
    update_window_title(ui);
    update_more_btn(ui);

    /* Keep the info bar consistent with what is on screen: prefer the item
     * matching the current tree selection, else the first thumbnail. */
    Item *shown = NULL;
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        char *p = NULL;
        int is_dir = 0;
        gtk_tree_model_get(model, &iter, COL_PATH, &p, COL_ISDIR, &is_dir, -1);
        if (!is_dir && p && *p) {
            for (guint i = 0; i < ui->items->len; ++i) {
                Item *it = (Item *)ui->items->pdata[i];
                if (g_strcmp0(it->path, p) == 0) {
                    shown = it;
                    break;
                }
            }
        }
        g_free(p);
    }
    if (!shown && ui->items->len > 0)
        shown = (Item *)ui->items->pdata[0];
    if (shown)
        update_status_bar(ui, shown);

    render_job_free(job);
    return G_SOURCE_REMOVE;
}

/* UI thread poller: copies the worker's progress counters into the loading
 * label. A timer drives this rather than the worker, so the worker thread
 * never touches a widget. */
static gboolean progress_tick(gpointer data)
{
    UI *ui = (UI *)data;
    RenderJob *job = ui->job;
    if (!job || job->gen != ui->job_gen || !ui->progress_label) {
        ui->progress_src = 0; /* this source is about to be destroyed */
        return G_SOURCE_REMOVE;
    }

    g_mutex_lock(&job->lock);
    guint files_done = job->files_done, files_total = job->files_total;
    size_t tris_done = job->tris_done, tris_total = job->tris_total;
    char *cur = job->current ? g_strdup(job->current) : NULL;
    g_mutex_unlock(&job->lock);

    char *text;
    if (tris_total > 0)
        text = g_strdup_printf("Rendering %s (%zu/%zu triangles)…",
                               cur ? cur : "", tris_done, tris_total);
    else
        text = g_strdup_printf("Loading %s (%u/%u)…", cur ? cur : "",
                               files_total ? files_done + 1 : 0, files_total);
    gtk_label_set_text(GTK_LABEL(ui->progress_label), text);
    g_free(text);
    g_free(cur);
    return G_SOURCE_CONTINUE;
}

/* Start an asynchronous load+render of `paths` (ownership is taken).
 * Any job still running is cancelled; its result is discarded on arrival.
 * `append` non-zero adds the result to the grid ("Load more") instead of
 * replacing it; `page_end` is the ui->shown_count to apply on success.
 * Returns immediately. */
static void start_render_job(UI *ui, GPtrArray *paths, int view_mode,
                             int append, guint page_end)
{
    if (ui->job)
        g_atomic_int_set(&ui->job->cancelled, 1);
    ui->job_gen++;

    RenderJob *job = g_new0(RenderJob, 1);
    job->paths = paths ? paths : paths_array_new(0);
    job->view_mode = view_mode;
    job->append = append;
    job->page_end = page_end;
    job->gen = ui->job_gen;
    job->ui = ui;
    job->files_total = job->paths->len;
    g_mutex_init(&job->lock);
    ui->job = job;

    if (append) {
        /* Keep the page already on screen; the "Load more" button doubles as
         * the progress indicator for an append job. */
        if (ui->more_btn) {
            gtk_button_set_label(GTK_BUTTON(ui->more_btn), "Loading…");
            gtk_widget_set_sensitive(ui->more_btn, FALSE);
        }
    } else {
        show_loading(ui, "Loading…");
    }
    if (ui->progress_src) {
        g_source_remove(ui->progress_src);
        ui->progress_src = 0;
    }
    ui->progress_src = g_timeout_add(PROGRESS_POLL_MS, progress_tick, ui);

    /* Detached: publish_render_job() frees the job on the UI thread. */
    g_thread_unref(g_thread_new("stlpreview-render", render_worker, job));
}

typedef struct {
    const char *target_path;
    GtkTreePath *found_path;
    GtkTreeIter found_iter;
    gboolean found;
} FindPathData;

static gboolean find_path_callback(GtkTreeModel *model, GtkTreePath *path,
                                   GtkTreeIter *iter, gpointer data)
{
    FindPathData *fd = (FindPathData *)data;
    char *iter_path;
    gtk_tree_model_get(model, iter, COL_PATH, &iter_path, -1);
    if (g_strcmp0(iter_path, fd->target_path) == 0) {
        fd->found_path = gtk_tree_path_copy(path);
        fd->found_iter = *iter;
        fd->found = TRUE;
        g_free(iter_path);
        return TRUE; /* stop traversal */
    }
    g_free(iter_path);
    return FALSE; /* continue traversal */
}

/* Find a tree row matching `path` and select it. */
static void select_path_in_tree(UI *ui, const char *path)
{
    FindPathData fd = {path, NULL, {0}, FALSE};
    gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), find_path_callback, &fd);
    if (fd.found) {
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
        gtk_tree_selection_select_path(sel, fd.found_path);
        gtk_tree_path_free(fd.found_path);
    }
}

static void confirm_and_delete_file(UI *ui, Item *it)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(ui->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO, "Are you sure you want to delete the file:\n%s?",
        it->name);

    gtk_window_set_title(GTK_WINDOW(dialog), "Confirm Delete");
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (response == GTK_RESPONSE_YES) {
        /* Delete file from disk */
        if (remove(it->path) != 0) {
            GtkWidget *err_dialog = gtk_message_dialog_new(
                GTK_WINDOW(ui->window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to delete file:\n%s",
                it->path);
            gtk_window_set_title(GTK_WINDOW(err_dialog), "Error");
            gtk_dialog_run(GTK_DIALOG(err_dialog));
            gtk_widget_destroy(err_dialog);
            return;
        }

        /* The file is gone: its cached thumbnails (one pair per view angle)
         * would otherwise linger until the cache is pruned. */
        cache_remove_for_file(it->path, (int64_t)it->mtime, it->file_size);

        gboolean showing_single_deleted = FALSE;
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
        GtkTreeIter iter;
        GtkTreeModel *model;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            char *cur_path;
            int is_dir;
            gtk_tree_model_get(model, &iter, COL_PATH, &cur_path, COL_ISDIR,
                               &is_dir, -1);
            if (!is_dir && g_strcmp0(cur_path, it->path) == 0) {
                showing_single_deleted = TRUE;
            }
            g_free(cur_path);
        }

        /* Find in tree store */
        FindPathData fd = {it->path, NULL, {0}, FALSE};
        gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), find_path_callback,
                               &fd);

        if (fd.found) {
            if (showing_single_deleted) {
                /* Since the single deleted file was showing, select its parent in the tree */
                GtkTreeIter parent_iter;
                if (gtk_tree_model_iter_parent(model, &parent_iter,
                                               &fd.found_iter)) {
                    GtkTreePath *parent_path =
                        gtk_tree_model_get_path(model, &parent_iter);
                    gtk_tree_selection_select_path(sel, parent_path);
                    gtk_tree_path_free(parent_path);
                } else {
                    /* If no parent, go up */
                    on_parent_clicked(NULL, ui);
                }
            }
            /* Now remove from tree store */
            gtk_tree_store_remove(ui->store, &fd.found_iter);
            gtk_tree_path_free(fd.found_path);
        }

        if (!showing_single_deleted) {
            /* If we are showing the folder, re-render it. The render runs on
             * a worker thread and publishes when done, so there is nothing to
             * wait for or clean up here. */
            render_current_selection(ui);
        }
    }
}

static void on_delete_menu_activated(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    UI *ui = g_object_get_data(G_OBJECT(menuitem), "ui");
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (ui && it) {
        confirm_and_delete_file(ui, it);
    }
}

/* Rows to insert below a lazily expanded tree node. */
typedef struct {
    const char *path; /* directory being expanded (borrowed) */
    GPtrArray *dirs;  /* sub-directory names, sorted (owned) */
    GPtrArray *files; /* model-file names, sorted (owned, may be NULL) */
} TreeRows;

/* One directory scan shared by the sidebar tree and the move-dialog tree.
 * Produces sorted arrays that own their strings; release each with
 * g_ptr_array_free(arr, TRUE). A NULL output pointer means "not wanted".
 * Hidden directories are skipped; model files are matched by extension. */
static void scan_dir_names(const char *path, GPtrArray **out_dirs,
                           GPtrArray **out_files)
{
    GPtrArray *dirs = paths_array_new(0);
    GPtrArray *files = paths_array_new(0);
    GDir *gd = g_dir_open(path, 0, NULL);
    if (gd) {
        const char *e;
        while ((e = g_dir_read_name(gd))) {
            char *full = g_build_filename(path, e, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
                if (e[0] != '.')
                    g_ptr_array_add(dirs, g_strdup(e));
            } else if (is_model_file(e)) {
                g_ptr_array_add(files, g_strdup(e));
            }
            g_free(full);
        }
        g_dir_close(gd);
    }
    g_ptr_array_sort_with_data(dirs, (GCompareDataFunc)g_strcmp0_adapter, NULL);
    g_ptr_array_sort_with_data(files, (GCompareDataFunc)g_strcmp0_adapter,
                               NULL);

    if (out_dirs)
        *out_dirs = dirs;
    else
        g_ptr_array_free(dirs, TRUE);
    if (out_files)
        *out_files = files;
    else
        g_ptr_array_free(files, TRUE);
}

/* Append one directory row below `parent`, with an unloaded placeholder
 * child. The placeholder is what makes GTK draw the expander arrow; the real
 * entries replace it when that folder is opened (see tree_node_load()). */
static void tree_add_dir_row(GtkTreeStore *store, GtkTreeIter *parent,
                             const char *name, const char *path)
{
    GtkTreeIter it;
    gtk_tree_store_append(store, &it, parent);
    gtk_tree_store_set(store, &it, COL_NAME, name, COL_PATH, path, COL_ISDIR, 1,
                       COL_LOADED, 0, -1);
    GtkTreeIter ph;
    gtk_tree_store_append(store, &ph, &it);
    gtk_tree_store_set(store, &ph, COL_NAME, "…", COL_PATH, "", COL_ISDIR, 0,
                       COL_LOADED, 0, -1);
}

/* Append `rows` below `parent`: directories first, then files when wanted. */
static void tree_rows_add(GtkTreeStore *store, GtkTreeIter *parent,
                          const TreeRows *rows)
{
    for (int pass = 0; pass < (rows->files ? 2 : 1); ++pass) {
        GPtrArray *arr = pass == 0 ? rows->dirs : rows->files;
        int is_dir = pass == 0;
        for (guint i = 0; i < arr->len; ++i) {
            const char *name = arr->pdata[i];
            char *full = g_build_filename(rows->path, name, NULL);
            if (is_dir) {
                tree_add_dir_row(store, parent, name, full);
            } else {
                GtkTreeIter it;
                gtk_tree_store_append(store, &it, parent);
                gtk_tree_store_set(store, &it, COL_NAME, name, COL_PATH, full,
                                   COL_ISDIR, 0, COL_LOADED, 0, -1);
            }
            g_free(full);
        }
    }
}

/* Expand a lazily loaded node: scan `path`, insert its rows, then drop the
 * placeholder. The new rows are appended *before* the placeholders are
 * removed -- emptying the row first would make GTK collapse it immediately.
 * `with_files` non-zero lists model files too (the sidebar wants them, the
 * move dialog only offers directories). */
static void tree_node_load(GtkTreeStore *store, GtkTreeIter *parent,
                           const char *path, int with_files)
{
    GPtrArray *dirs = NULL, *files = NULL;
    scan_dir_names(path, &dirs, with_files ? &files : NULL);

    GtkTreeIter child;
    GList *to_remove = NULL;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &child, parent)) {
        do {
            to_remove = g_list_prepend(to_remove, gtk_tree_iter_copy(&child));
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &child));
    }

    TreeRows rows = {path, dirs, files};
    tree_rows_add(store, parent, &rows);

    for (GList *l = to_remove; l; l = l->next) {
        GtkTreeIter *it_rem = (GtkTreeIter *)l->data;
        gtk_tree_store_remove(store, it_rem);
        gtk_tree_iter_free(it_rem);
    }
    g_list_free(to_remove);

    gtk_tree_store_set(store, parent, COL_LOADED, 1, -1);
    g_ptr_array_free(dirs, TRUE);
    if (files)
        g_ptr_array_free(files, TRUE);
}

/* Populate `parent` in the move-dialog tree with subdirectories of `path`.
 * Only directories are shown -- files are not listed. */
static void populate_move_tree(GtkTreeIter *parent, const char *path,
                               GtkTreeStore *store)
{
    tree_node_load(store, parent, path, 0);
}

/* Expand a row in the move-dialog tree (lazy-load). */
static void on_move_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                                 GtkTreePath *path, gpointer data)
{
    (void)tv;
    (void)path;
    UI *ui = (UI *)data;
    int is_dir = 0, loaded = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->move_store), iter, COL_ISDIR, &is_dir,
                       COL_LOADED, &loaded, -1);
    if (!is_dir || loaded)
        return;

    char *p = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->move_store), iter, COL_PATH, &p, -1);
    if (p && *p) {
        populate_move_tree(iter, p, ui->move_store);
    }
    g_free(p);
}

/* Cell data func for the move dialog tree — grey out non-directory rows. */
static void on_move_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cr,
                              GtkTreeModel *model, GtkTreeIter *iter,
                              gpointer data)
{
    (void)data;
    (void)col;
    char *name;
    int is_dir;
    gtk_tree_model_get(model, iter, COL_NAME, &name, COL_ISDIR, &is_dir, -1);
    g_object_set(cr, "text", name, NULL);
    g_free(name);
    if (!is_dir) {
        g_object_set(cr, "sensitive", FALSE, NULL);
    } else {
        g_object_set(cr, "sensitive", TRUE, NULL);
    }
}

/* Row selected in the move dialog tree view: update destination path. */
static void on_move_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    UI *ui = (UI *)data;
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        int is_dir = 0;
        gtk_tree_model_get(model, &iter, COL_ISDIR, &is_dir, -1);
        if (is_dir) {
            char *dest = NULL;
            gtk_tree_model_get(model, &iter, COL_PATH, &dest, -1);
            if (dest && *dest) {
                g_free(ui->move_dest_path);
                ui->move_dest_path = dest;
                gtk_widget_set_sensitive(ui->move_confirm_btn, TRUE);
                return;
            }
            g_free(dest);
        }
    }
    gtk_widget_set_sensitive(ui->move_confirm_btn, FALSE);
}

/* Row double-clicked in the move dialog: accept if it's a directory. */
static void on_move_row_activated(GtkTreeView *tv, GtkTreePath *path,
                                  GtkTreeViewColumn *col, gpointer data)
{
    (void)tv;
    (void)col;
    UI *ui = (UI *)data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(ui->move_store), &iter, path))
        return;
    int is_dir = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->move_store), &iter, COL_ISDIR,
                       &is_dir, -1);
    if (is_dir) {
        char *dest = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(ui->move_store), &iter, COL_PATH,
                           &dest, -1);
        if (dest && *dest) {
            g_free(ui->move_dest_path);
            ui->move_dest_path = dest;
            gtk_widget_set_sensitive(ui->move_confirm_btn, TRUE);
        } else {
            g_free(dest);
        }
    }
}

/* Called when the user clicks "Move here". */
static void on_move_confirm_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    UI *ui = (UI *)data;
    if (!ui->move_path || !ui->move_dest_path)
        return;

    char *new_path = g_build_filename(ui->move_dest_path, ui->move_name, NULL);

    /* Ask about a name collision, but do NOT unlink the target ourselves:
     * g_file_move() is given G_FILE_COPY_OVERWRITE so the replacement is
     * atomic. Pre-deleting left a window where the destination was already
     * gone and the source had not arrived yet (and raced other processes). */
    gboolean overwrite = FALSE;
    if (g_file_test(new_path, G_FILE_TEST_EXISTS)) {
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(ui->window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
            "A file named '%s' already exists in this folder.\nOverwrite?",
            ui->move_name);
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (resp != GTK_RESPONSE_YES) {
            g_free(new_path);
            return;
        }
        overwrite = TRUE;
    }

    /* Move the file */
    GError *err = NULL;
    GFile *src = g_file_new_for_path(ui->move_path);
    GFile *dst = g_file_new_for_path(new_path);
    gboolean ok = g_file_move(
        src, dst, overwrite ? G_FILE_COPY_OVERWRITE : G_FILE_COPY_NONE, NULL,
        NULL, NULL, &err);
    g_object_unref(src);
    g_object_unref(dst);
    if (!ok) {
        GtkWidget *err_dlg = gtk_message_dialog_new(
            GTK_WINDOW(ui->window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to move file:\n%s\n%s",
            ui->move_path, err->message);
        gtk_dialog_run(GTK_DIALOG(err_dlg));
        gtk_widget_destroy(err_dlg);
        g_error_free(err);
        g_free(new_path);
        return;
    }
    g_free(new_path);

    /* The old path no longer holds the file, so its cached thumbnails are
     * orphans; the destination gets its own entries on the next render. */
    cache_remove_for_file(ui->move_path, ui->move_mtime, ui->move_size);

    /* Remove old row from left tree store */
    FindPathData fd = {ui->move_path, NULL, {0}, FALSE};
    gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), find_path_callback, &fd);
    if (fd.found) {
        gtk_tree_store_remove(ui->store, &fd.found_iter);
        gtk_tree_path_free(fd.found_path);
    }

    /* Insert new row under the destination folder if it is loaded */
    FindPathData fd2 = {ui->move_dest_path, NULL, {0}, FALSE};
    gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), find_path_callback, &fd2);
    if (fd2.found) {
        int loaded = 0;
        gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &fd2.found_iter,
                           COL_LOADED, &loaded, -1);
        if (loaded) {
            char *moved_file_path =
                g_build_filename(ui->move_dest_path, ui->move_name, NULL);
            GtkTreeIter child;
            gtk_tree_store_append(ui->store, &child, &fd2.found_iter);
            gtk_tree_store_set(ui->store, &child, COL_NAME, ui->move_name,
                               COL_PATH, moved_file_path, COL_ISDIR, 0,
                               COL_LOADED, 0, -1);
            g_free(moved_file_path);
        }
        gtk_tree_path_free(fd2.found_path);
    }

    /* If the moved file is currently shown in the grid, re-render */
    gboolean showing_single = FALSE;
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        char *cur_path;
        int is_dir;
        gtk_tree_model_get(model, &iter, COL_PATH, &cur_path, COL_ISDIR,
                           &is_dir, -1);
        if (!is_dir && g_strcmp0(cur_path, ui->move_path) == 0)
            showing_single = TRUE;
        g_free(cur_path);
    }

    /* Restore the grid first: render_current_selection() swaps the panel for
     * a loading spinner and publishes the fresh thumbnails from the worker
     * thread later, so ui->items must not be touched after starting it. */
    cancel_move(ui);

    if (showing_single) {
        /* The moved file was the one on display: re-render the destination. */
        render_current_selection(ui);
    } else if (ui->items->len > 0) {
        update_status_bar(ui, ui->items->pdata[0]);
    }
}

/* Called when user clicks "Cancel" in the move dialog. */
static void on_move_cancel_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    UI *ui = (UI *)data;
    cancel_move(ui);
}

/* Set up the move-to-directory dialog in the right panel. */
static void start_move(UI *ui, Item *it)
{
    /* Guard against re-entry */
    if (ui->move_path)
        cancel_move(ui);

    ui->move_path = g_strdup(it->path);
    ui->move_name = g_strdup(it->name);
    ui->move_dest_path = NULL;
    /* Needed to drop the cached thumbnails of the old path afterwards: the
     * cache key is a hash of (path, mtime, size, view), so it cannot be
     * looked up from the file name alone. */
    ui->move_mtime = (int64_t)it->mtime;
    ui->move_size = it->file_size;

    /* Build the picker panel */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    /* Title label */
    GtkWidget *title = gtk_label_new(NULL);
    char *title_text = g_strdup_printf("Move <b>%s</b> to:", it->name);
    gtk_label_set_markup(GTK_LABEL(title), title_text);
    g_free(title_text);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    /* Tree view for destination selection */
    ui->move_store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_INT, G_TYPE_INT);
    GtkWidget *tree_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(tree_scroll, -1, 300);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tree_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    ui->move_tree =
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->move_store));
    g_object_unref(ui->move_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ui->move_tree), FALSE);

    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_set_cell_data_func(
        col, icon, (GtkTreeCellDataFunc)on_cell_data, ui, NULL);
    gtk_tree_view_column_set_cell_data_func(
        col, text, (GtkTreeCellDataFunc)on_move_cell_data, ui, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ui->move_tree), col);

    /* Populate root */
    char *root_path = NULL;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->store), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, COL_PATH,
                           &root_path, -1);
    }
    if (root_path) {
        char *base = g_path_get_basename(root_path);
        const char *display_name = (*base == '\0') ? "/" : base;
        GtkTreeIter root_it;
        gtk_tree_store_append(ui->move_store, &root_it, NULL);
        gtk_tree_store_set(ui->move_store, &root_it, COL_NAME, display_name,
                           COL_PATH, root_path, COL_ISDIR, 1, COL_LOADED, 1,
                           -1);
        /* Load its children */
        populate_move_tree(&root_it, root_path, ui->move_store);
        GtkTreePath *p = gtk_tree_path_new_from_indices(0, -1);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(ui->move_tree), p, FALSE);
        gtk_tree_path_free(p);
        g_free(base);
        g_free(root_path);
    }

    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->move_tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_move_selection_changed), ui);
    g_signal_connect(ui->move_tree, "row-activated",
                     G_CALLBACK(on_move_row_activated), ui);
    g_signal_connect(ui->move_tree, "row-expanded",
                     G_CALLBACK(on_move_row_expanded), ui);

    gtk_container_add(GTK_CONTAINER(tree_scroll), ui->move_tree);
    gtk_box_pack_start(GTK_BOX(box), tree_scroll, TRUE, TRUE, 0);

    /* Buttons */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);

    ui->move_cancel_btn = gtk_button_new_with_label("Cancel");
    g_signal_connect(ui->move_cancel_btn, "clicked",
                     G_CALLBACK(on_move_cancel_clicked), ui);
    gtk_box_pack_start(GTK_BOX(btn_box), ui->move_cancel_btn, FALSE, FALSE, 0);

    ui->move_confirm_btn = gtk_button_new_with_label("Move here");
    gtk_widget_set_sensitive(ui->move_confirm_btn, FALSE);
    g_signal_connect(ui->move_confirm_btn, "clicked",
                     G_CALLBACK(on_move_confirm_clicked), ui);
    gtk_box_pack_start(GTK_BOX(btn_box), ui->move_confirm_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(box), btn_box, FALSE, FALSE, 0);

    set_panel_content(ui, box);
}

/* Tear down the move dialog and restore the grid. */
static void cancel_move(UI *ui)
{
    if (!ui->move_path)
        return; /* not in move mode */

    /* Restore the grid */
    set_panel_content(ui, build_grid(ui));

    /* Free state */
    g_free(ui->move_path);
    ui->move_path = NULL;
    g_free(ui->move_name);
    ui->move_name = NULL;
    g_free(ui->move_dest_path);
    ui->move_dest_path = NULL;
    ui->move_mtime = 0;
    ui->move_size = 0;
    ui->move_store = NULL;
    ui->move_tree = NULL;
    ui->move_cancel_btn = NULL;
    ui->move_confirm_btn = NULL;
}

/* Context-menu handler for "Move to Directory…" */
static void on_move_menu_activated(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    UI *ui = g_object_get_data(G_OBJECT(menuitem), "ui");
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (ui && it) {
        start_move(ui, it);
    }
}

/* Context menu handlers */
static void on_open_containing_folder(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (!it || !it->path)
        return;
    char *dir = g_path_get_dirname(it->path);
    GFile *f = g_file_new_for_path(dir);
    char *uri = g_file_get_uri(f);
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err))
        g_warning("cannot open folder: %s",
                  err ? err->message : "unknown error");
    if (err)
        g_error_free(err);
    g_free(uri);
    g_object_unref(f);
    g_free(dir);
}

static void on_copy_path(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (!it || !it->path)
        return;
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, it->path, -1);
}

static void on_export_png(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (!it || !it->path)
        return;
    /* The surface may have been streamed out to save memory. */
    UI *ui = g_object_get_data(G_OBJECT(menuitem), "ui");
    if (ui)
        item_ensure_thumb(ui, it);
    if (!it->thumb)
        return;
    char *base = g_path_get_basename(it->path);
    char *name = g_strconcat(base, ".png", NULL);
    g_free(base);
    char *out = g_build_filename(g_get_home_dir(), name, NULL);
    g_free(name);
    cairo_surface_write_to_png(it->thumb, out);
    GtkWidget *dlg =
        gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_OK, "Exported to:\n%s", out);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(out);
}

static void on_open_default(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (!it || !it->path)
        return;
    GFile *f = g_file_new_for_path(it->path);
    char *uri = g_file_get_uri(f);
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err))
        g_warning("cannot open file: %s", err ? err->message : "unknown error");
    if (err)
        g_error_free(err);
    g_free(uri);
    g_object_unref(f);
}

/* Move the blue selection border to `it`'s thumbnail without rebuilding
 * the grid (rebuilding would destroy the other thumbnails). */
static void set_grid_selection(UI *ui, Item *it)
{
    ensure_border_provider();
    if (ui->selected_item && ui->selected_item != it &&
        ui->selected_item->grid_cell) {
        gtk_style_context_remove_class(
            gtk_widget_get_style_context(ui->selected_item->grid_cell),
            "preview-border");
        ui->selected_item->selected = 0;
    }
    it->selected = 1;
    ui->selected_item = it;
    if (it->grid_cell)
        gtk_style_context_add_class(gtk_widget_get_style_context(it->grid_cell),
                                    "preview-border");
}

/* Click on a preview thumbnail: select the corresponding file in the tree,
 * or right-click to show a context menu. */
static void on_selection_changed(GtkTreeSelection *sel, UI *ui);

/* Append one entry to a thumbnail's context menu. The handlers all need the
 * same two pointers, so they travel as object data on the menu item itself --
 * that keeps one shared signature and avoids allocating a callback struct per
 * entry. */
static void context_menu_add(GtkWidget *menu, const char *label,
                             GCallback handler, UI *ui, Item *it)
{
    GtkWidget *entry = gtk_menu_item_new_with_label(label);
    g_object_set_data(G_OBJECT(entry), "ui", ui);
    g_object_set_data(G_OBJECT(entry), "item", it);
    g_signal_connect(entry, "activate", handler, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), entry);
}

/* Add a top-level entry to the menu bar and return its (empty) submenu. */
static GtkWidget *menubar_add_submenu(GtkWidget *bar, const char *label)
{
    GtkWidget *entry = gtk_menu_item_new_with_mnemonic(label);
    GtkWidget *submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(entry), submenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), entry);
    return submenu;
}

static gboolean on_preview_clicked(GtkWidget *w, GdkEventButton *ev,
                                   gpointer data)
{
    Item *it = g_object_get_data(G_OBJECT(w), "item");
    if (!it || !it->path)
        return FALSE;
    UI *ui = (UI *)data;

    if (ev->button == 1) { /* Left click */
        set_grid_selection(ui, it);
        /* Select the file in the tree (shows the highlight) without
         * re-rendering the grid. This avoids destroying the current
         * grid so other thumbnails remain visible. */
        char *path = g_strdup(it->path);
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
        g_signal_handlers_block_by_func(sel, G_CALLBACK(on_selection_changed),
                                        ui);
        select_path_in_tree(ui, path);
        g_signal_handlers_unblock_by_func(sel, G_CALLBACK(on_selection_changed),
                                          ui);
        /* Update the status bar with info about the selected Item */
        update_status_bar(ui, it);
        g_free(path);
        return TRUE;
    } else if (ev->button == 3) { /* Right click */
        set_grid_selection(ui, it);
        /* Select the file in the tree (shows the highlight) without re-rendering the grid.
         * This avoids destroying this event box, the GdkEventButton 'ev', and freeing 'it'. */
        char *path = g_strdup(it->path);
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
        g_signal_handlers_block_by_func(sel, G_CALLBACK(on_selection_changed),
                                        ui);
        select_path_in_tree(ui, path);
        g_signal_handlers_unblock_by_func(sel, G_CALLBACK(on_selection_changed),
                                          ui);

        /* Update the status bar with info about the selected Item */
        update_status_bar(ui, it);

        GtkWidget *menu = gtk_menu_new();
        g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy),
                         NULL);

        context_menu_add(menu, "Open Containing Folder",
                         G_CALLBACK(on_open_containing_folder), ui, it);
        context_menu_add(menu, "Copy Path", G_CALLBACK(on_copy_path), ui, it);
        context_menu_add(menu, "Export PNG", G_CALLBACK(on_export_png), ui, it);
        context_menu_add(menu, "Open with Default App",
                         G_CALLBACK(on_open_default), ui, it);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        context_menu_add(menu, "Move to Directory…",
                         G_CALLBACK(on_move_menu_activated), ui, it);
        context_menu_add(menu, "Delete File",
                         G_CALLBACK(on_delete_menu_activated), ui, it);

        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
        g_free(path);
        return TRUE;
    }
    return FALSE;
}

/* A model path together with the stat data needed to sort it. Collecting the
 * metadata once (instead of calling stat() from inside the comparator) keeps
 * a directory listing at O(n) syscalls rather than O(n log n). */
typedef struct {
    char *path;    /* full path, owned by the containing array */
    int64_t size;  /* st_size, 0 when stat failed */
    int64_t mtime; /* st_mtime, 0 when stat failed */
    int have_stat; /* false: fall back to sorting by name */
} PathInfo;

/* Re-render the currently shown thumbnails with ui->view_mode, applying
 * ui->sort_mode. The paths are re-sorted here (not just when a folder is
 * clicked) so changing the Sort combo reorders the visible grid.
 * The work happens on a worker thread, so a view or sort change that lands
 * while another render is in flight simply supersedes it. */
static void rerender_current_items(UI *ui)
{
    /* The Items already carry size and mtime from when they were loaded, so
     * the page can be re-sorted without touching the filesystem at all. */
    GArray *infos = g_array_new(FALSE, FALSE, sizeof(PathInfo));
    for (guint i = 0; i < ui->items->len; ++i) {
        Item *it = ui->items->pdata[i];
        if (!it->path)
            continue;
        PathInfo pi;
        pi.path = g_strdup(it->path);
        pi.size = it->file_size;
        pi.mtime = (int64_t)it->mtime;
        pi.have_stat = 1;
        g_array_append_val(infos, pi);
    }
    g_sort_array(infos->data, infos->len, sizeof(PathInfo), path_info_compare,
                 GINT_TO_POINTER(ui->sort_mode));

    GPtrArray *paths = paths_array_new(infos->len);
    for (guint i = 0; i < infos->len; ++i)
        g_ptr_array_add(paths, g_array_index(infos, PathInfo, i).path);
    g_array_free(infos, TRUE); /* frees the structs; paths moved to `paths` */

    /* Same page, new view angle: pagination is left untouched. */
    start_render_job(ui, paths, (int)ui->view_mode, 0, ui->shown_count);
}

/* One of the view-angle toggle buttons was activated. */
static void on_view_toggled(GtkToggleButton *b, UI *ui)
{
    if (!gtk_toggle_button_get_active(b))
        return; /* group member being deselected */
    ui->view_mode =
        (ViewMode)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "view_mode"));
    rerender_current_items(ui);
}

/* Sort mode changed: re-render with new sort order. */
static void on_sort_changed(GtkComboBox *combo, UI *ui)
{
    int idx = gtk_combo_box_get_active(combo);
    /* SORT_DATE is the last valid index, so reject only idx > SORT_DATE
     * (the previous `>=` silently discarded the "Date" choice). */
    if (idx < 0 || idx > SORT_DATE)
        return;
    ui->sort_mode = (SortMode)idx;
    /* Sorting applies to the whole listing, so re-read it from page one
     * instead of merely re-rendering the page currently on screen. */
    render_current_selection(ui);
}

/* Order two PathInfo entries according to the SortMode in `data`. Used with
 * g_sort_array() over an array of PathInfo structs, so the arguments point
 * directly at the elements (no double indirection). Name is always the
 * tie-breaker, which keeps the order stable and deterministic. */
static int path_info_compare(gconstpointer a, gconstpointer b, gpointer data)
{
    const PathInfo *ia = (const PathInfo *)a;
    const PathInfo *ib = (const PathInfo *)b;
    SortMode mode = (SortMode)GPOINTER_TO_INT(data);

    if (ia->have_stat && ib->have_stat) {
        if (mode == SORT_SIZE && ia->size != ib->size)
            return ia->size < ib->size ? -1 : 1;
        if (mode == SORT_DATE && ia->mtime != ib->mtime)
            return ia->mtime > ib->mtime ? -1 : 1; /* newest first */
    }
    return g_strcmp0(ia->path, ib->path);
}

/* List the .stl/.3mf files directly inside `dir`, stat each one exactly once
 * and return the full paths (newly allocated strings) sorted by `mode`.
 * Hidden entries are skipped. */
GPtrArray *list_model_files_sorted(const char *dir, SortMode mode)
{
    GPtrArray *out = paths_array_new(0);
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd)
        return out;

    GArray *infos = g_array_new(FALSE, FALSE, sizeof(PathInfo));
    const char *e;
    while ((e = g_dir_read_name(gd))) {
        if (!is_model_file(e))
            continue;
        PathInfo pi;
        pi.path = g_build_filename(dir, e, NULL);
        struct stat st;
        pi.have_stat = (stat(pi.path, &st) == 0);
        pi.size = pi.have_stat ? (int64_t)st.st_size : 0;
        pi.mtime = pi.have_stat ? (int64_t)st.st_mtime : 0;
        g_array_append_val(infos, pi);
    }
    g_dir_close(gd);

    g_sort_array(infos->data, infos->len, sizeof(PathInfo), path_info_compare,
                 GINT_TO_POINTER(mode));
    for (guint i = 0; i < infos->len; ++i)
        g_ptr_array_add(out, g_array_index(infos, PathInfo, i).path);
    g_array_free(infos, TRUE); /* frees the structs; paths moved to `out` */
    return out;
}

/* Comparator for g_sort_array()/g_qsort_with_data() over an array of
 * `char *`. Those helpers pass pointers to the elements, so `a` and `b`
 * are `char **`: dereference before comparing. Passing the pointers
 * straight to g_strcmp0() would compare the raw pointer bytes as a string
 * (out-of-bounds read, arbitrary order). */
static int g_strcmp0_adapter(const void *a, const void *b, gpointer ud)
{
    (void)ud;
    return g_strcmp0(*(char *const *)a, *(char *const *)b);
}

/* Number of thumbnail columns that fit in `w` pixels of grid area. Uses the
 * same spacing/margin constants as build_grid() so the two can never
 * disagree. */
/* Build the thumbnail grid widget from ui->items, sized to the current
 * right-panel width. Thumbnails are reused, never re-rendered. */
static GtkWidget *build_grid(UI *ui)
{
    int w = gtk_widget_get_allocated_width(ui->grid_scroll);
    ui->grid_cols = grid_cols_for_width(w);

    GtkWidget *grid_w = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_w), GRID_ROW_SPACING);
    gtk_grid_set_column_spacing(GTK_GRID(grid_w), GRID_COL_SPACING);
    gtk_widget_set_margin_top(grid_w, GRID_MARGIN);
    gtk_widget_set_margin_bottom(grid_w, GRID_MARGIN);
    gtk_widget_set_margin_start(grid_w, GRID_MARGIN);
    gtk_widget_set_margin_end(grid_w, GRID_MARGIN);

    for (guint i = 0; i < ui->items->len; ++i) {
        Item *it = ui->items->pdata[i];

        GtkWidget *eb = gtk_event_box_new();
        gtk_event_box_set_above_child(GTK_EVENT_BOX(eb), TRUE);
        gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK);
        g_object_set_data(G_OBJECT(eb), "item", it);
        it->grid_cell = eb;

        /* Add border class for selected items */
        if (it->selected) {
            ensure_border_provider();
            GtkStyleContext *ctx = gtk_widget_get_style_context(eb);
            gtk_style_context_add_class(ctx, "preview-border");
            ui->selected_item = it;
        }

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        /* A streamed-out item has no surface yet: reserve the cell size so
         * the layout is stable, update_thumb_stream() fills it in. */
        GtkWidget *img;
        if (it->thumb) {
            img = gtk_image_new_from_surface(it->thumb);
        } else {
            img = gtk_image_new();
            gtk_widget_set_size_request(img, CELL, CELL);
        }
        it->image = img;
        GtkWidget *label = gtk_label_new(it->name);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_size_request(label, CELL - 8, -1);
        gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(eb), box);

        gtk_grid_attach(GTK_GRID(grid_w), eb, (int)(i % ui->grid_cols),
                        (int)(i / ui->grid_cols), 1, 1);
        g_signal_connect(eb, "button-press-event",
                         G_CALLBACK(on_preview_clicked), ui);
    }
    /* Release the thumbnails that are not on screen. */
    update_thumb_stream(ui);
    return grid_w;
}

/* Deferred re-layout: rebuild the grid with the column count that fits
 * the panel's new width. Runs from the idle loop so we never destroy a
 * widget in the middle of an allocation pass. */
static gboolean relayout_grid_idle(gpointer data)
{
    UI *ui = (UI *)data;
    if (!GTK_IS_GRID(ui->grid))
        return G_SOURCE_REMOVE;
    int cols =
        grid_cols_for_width(gtk_widget_get_allocated_width(ui->grid_scroll));
    if (cols == ui->grid_cols)
        return G_SOURCE_REMOVE;
    set_panel_content(ui, build_grid(ui));
    return G_SOURCE_REMOVE;
}

static void on_grid_scroll_size_allocate(GtkWidget *w, GdkRectangle *alloc,
                                         UI *ui)
{
    (void)w;
    if (!GTK_IS_GRID(ui->grid))
        return;
    /* The viewport changed: re-evaluate which thumbnails stay resident. */
    update_thumb_stream(ui);
    if (grid_cols_for_width(alloc->width) == ui->grid_cols)
        return;
    g_idle_add(relayout_grid_idle, ui);
}

/* Scrolling the grid streams thumbnails in and out of memory. */
static void on_grid_scrolled(GtkAdjustment *adj, UI *ui)
{
    (void)adj;
    update_thumb_stream(ui);
}

/* Replace the grid contents with `items` (ownership is taken). */
static void show_items(UI *ui, GPtrArray *items)
{
    ui->selected_item = NULL; /* old items (and their grid cells) are freed */
    /* ui->items owns its Items through item_free: truncating frees them. */
    g_ptr_array_set_size(ui->items, 0);
    for (guint i = 0; i < items->len; ++i)
        g_ptr_array_add(ui->items, items->pdata[i]);
    /* Detach before releasing the shell: item_free must not run on Items that
     * ui->items has just adopted. */
    g_ptr_array_set_free_func(items, NULL);
    g_ptr_array_free(items, TRUE);

    set_panel_content(ui, build_grid(ui));
}

/* Append `items` (ownership is taken) to the grid: the "Load more" page. */
static void append_items(UI *ui, GPtrArray *items)
{
    for (guint i = 0; i < items->len; ++i)
        g_ptr_array_add(ui->items, items->pdata[i]);
    /* Detach before releasing the shell (see show_items). */
    g_ptr_array_set_free_func(items, NULL);
    g_ptr_array_free(items, TRUE);

    set_panel_content(ui, build_grid(ui));
}

/* ---------------- directory tree (lazy) ---------------- */
/* Populate `parent` with the subdirectories and .stl files of `path`. */
/* Lazy-load a sidebar folder node: sub-directories *and* model files. */
static void load_dir_into(GtkTreeIter *parent, const char *path, UI *ui)
{
    tree_node_load(ui->store, parent, path, 1);
}

/* Icon per row. */
static void on_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cr,
                         GtkTreeModel *model, GtkTreeIter *iter, UI *ui)
{
    (void)col;
    (void)ui;
    int is_dir;
    gtk_tree_model_get(model, iter, COL_ISDIR, &is_dir, -1);
    g_object_set(cr, "icon-name", is_dir ? "folder" : "image-x-generic", NULL);
}

/* Expanding a folder (e.g. via the triangle) loads it and shows its files */
static void on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                            GtkTreePath *path, UI *ui)
{
    (void)path;
    int is_dir, loaded;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->store), iter, COL_ISDIR, &is_dir,
                       COL_LOADED, &loaded, -1);
    if (!is_dir)
        return;
    if (!loaded) {
        char *p;
        gtk_tree_model_get(GTK_TREE_MODEL(ui->store), iter, COL_PATH, &p, -1);
        load_dir_into(iter, p, ui);
        g_free(p);
    }
    GtkTreePath *tp = gtk_tree_model_get_path(GTK_TREE_MODEL(ui->store), iter);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(tv), tp);
    gtk_tree_path_free(tp);
}

static void render_current_selection(UI *ui)
{
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    char *path;
    int is_dir;
    gtk_tree_model_get(model, &iter, COL_PATH, &path, COL_ISDIR, &is_dir, -1);
    if (*path == '\0') {
        g_free(path);
        return; /* placeholder row */
    }
    if (is_dir)
        save_last_dir(path);

    GPtrArray *paths = NULL;
    if (is_dir) {
        /* clicking a folder opens it: load + expand its children once */
        int loaded = 0;
        gtk_tree_model_get(model, &iter, COL_LOADED, &loaded, -1);
        if (!loaded) {
            load_dir_into(&iter, path, ui);
            GtkTreePath *tp = gtk_tree_model_get_path(model, &iter);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(ui->tree), tp, FALSE);
            gtk_tree_path_free(tp);
        }
        paths = list_model_files_sorted(path, ui->sort_mode);
    } else {
        paths = paths_array_new(1);
        g_ptr_array_add(paths, g_strdup(path));
    }

    /* Remember the complete listing, then render only the first page: a
     * folder holding thousands of models must not block the UI or hold every
     * thumbnail in memory. "Load more" appends the following pages. */
    g_free(ui->cur_path);
    ui->cur_path = g_strdup(path);
    ui->cur_is_dir = is_dir;
    if (ui->all_paths)
        g_ptr_array_free(ui->all_paths, TRUE);
    ui->all_paths = paths; /* takes ownership of the strings */
    ui->shown_count = 0;

    guint end = MIN(paths->len, (guint)RENDER_PAGE_SIZE);
    GPtrArray *page = paths_array_new(end);
    for (guint i = 0; i < end; ++i)
        g_ptr_array_add(page, g_strdup((char *)paths->pdata[i]));

    g_free(path);
    /* Track the folder on disk while it is the one being shown. A single
     * file selection is not watched: its own row would be a child of the
     * watched folder and refreshing that folder would drop the selection. */
    monitor_set_dir(ui, ui->cur_is_dir ? ui->cur_path : NULL);

    start_render_job(ui, page, (int)ui->view_mode, 0, end);
}

/* Render the next page of the current listing and append it to the grid. */
static void start_next_page(UI *ui)
{
    if (!ui->all_paths || ui->shown_count >= ui->all_paths->len)
        return;
    guint start = ui->shown_count;
    guint end = MIN(ui->all_paths->len, start + (guint)RENDER_PAGE_SIZE);
    GPtrArray *page = paths_array_new(end - start);
    for (guint i = start; i < end; ++i)
        g_ptr_array_add(page, g_strdup((char *)ui->all_paths->pdata[i]));
    start_render_job(ui, page, (int)ui->view_mode, 1, end);
}

static void on_load_more_clicked(GtkButton *btn, UI *ui)
{
    (void)btn;
    start_next_page(ui);
}

/* Sidebar row lookup, so a change on disk can be reflected in the tree too. */
typedef struct {
    const char *dir;
    GtkTreeIter iter;
    gboolean found;
} FindDirData;

static gboolean find_dir_callback(GtkTreeModel *model, GtkTreePath *path,
                                  GtkTreeIter *iter, gpointer data)
{
    (void)model;
    (void)path;
    FindDirData *fd = (FindDirData *)data;
    char *p = NULL;
    int is_dir = 0;
    gtk_tree_model_get(model, iter, COL_PATH, &p, COL_ISDIR, &is_dir, -1);
    gboolean hit = is_dir && p && g_strcmp0(p, fd->dir) == 0;
    g_free(p);
    if (!hit)
        return FALSE; /* keep looking */
    fd->iter = *iter;
    fd->found = TRUE;
    return TRUE;
}

/* Re-read the watched folder: refresh its sidebar children, then re-render
 * the grid. Only the selected folder is watched, so its own row -- the one
 * holding the selection -- is never removed by the child refresh. */
static gboolean monitor_reload(gpointer data)
{
    UI *ui = (UI *)data;
    ui->reload_src = 0;
    if (!ui->watch_dir || !ui->cur_is_dir)
        return G_SOURCE_REMOVE;

    FindDirData fd = {ui->watch_dir, {0}, FALSE};
    gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), find_dir_callback, &fd);
    if (fd.found)
        load_dir_into(&fd.iter, ui->watch_dir, ui);

    render_current_selection(ui);
    return G_SOURCE_REMOVE;
}

/* A change arrived: restart the debounce window rather than reloading now. */
static void on_dir_changed(GFileMonitor *mon, GFile *file, GFile *other,
                           GFileMonitorEvent ev, gpointer data)
{
    (void)mon;
    (void)file;
    (void)other;
    UI *ui = (UI *)data;
    switch (ev) {
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_DELETED:
        case G_FILE_MONITOR_EVENT_CHANGED:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
        case G_FILE_MONITOR_EVENT_MOVED_OUT: break;
        default:
            /* Attribute-only and pre-delete hints do not change the listing. */
            return;
    }
    if (ui->reload_src)
        g_source_remove(ui->reload_src);
    ui->reload_src = g_timeout_add(MONITOR_DEBOUNCE_MS, monitor_reload, ui);
}

/* Watch `dir`, or stop watching when it is NULL. Re-watching the directory
 * that is already watched does nothing, so refreshing the grid cannot tear
 * down the monitor that just reported the change. */
static void monitor_set_dir(UI *ui, const char *dir)
{
    if (g_strcmp0(ui->watch_dir, dir) == 0)
        return;

    if (ui->reload_src) {
        g_source_remove(ui->reload_src);
        ui->reload_src = 0;
    }
    if (ui->monitor) {
        g_signal_handler_disconnect(ui->monitor, ui->monitor_sig);
        g_object_unref(ui->monitor);
        ui->monitor = NULL;
        ui->monitor_sig = 0;
    }
    g_free(ui->watch_dir);
    ui->watch_dir = dir ? g_strdup(dir) : NULL;
    if (!dir)
        return;

    GFile *gf = g_file_new_for_path(dir);
    GError *err = NULL;
    /* SEND_MOVED reports a rename inside the folder as one moved-out/moved-in
     * pair instead of a delete plus a create -- that is what the toolbar's
     * own "move" action produces, so it must not look like two changes. */
    ui->monitor =
        g_file_monitor_directory(gf, G_FILE_MONITOR_SEND_MOVED, NULL, &err);
    g_object_unref(gf);
    if (!ui->monitor) {
        /* Watching is an optimisation: without it the folder simply stops
         * tracking external changes, which is not worth failing over. */
        g_warning("cannot watch %s: %s", dir,
                  err ? err->message : "unknown error");
        g_clear_error(&err);
        return;
    }
    g_clear_error(&err);
    ui->monitor_sig = g_signal_connect(ui->monitor, "changed",
                                       G_CALLBACK(on_dir_changed), ui);
}

/* Selection changed: render the folder's .stl files (or the single file).
 * Starting a job is non-blocking and supersedes whatever is in flight, so
 * rapid selection changes need no re-entrancy guard. */
static void on_selection_changed(GtkTreeSelection *sel, UI *ui)
{
    (void)sel;
    render_current_selection(ui);
}

/* Make `path` the new tree root (replaces the whole tree). */
static void set_root(UI *ui, const char *path)
{
    /* g_canonicalize_filename() makes the path absolute and collapses "." /
     * ".." / duplicate separators. Unlike realpath() it returns GLib memory,
     * so it is released with g_free() like every other string here, and it
     * leaves symlinks alone (the tree keeps showing what the user picked). */
    char *real = g_canonicalize_filename(path, NULL);
    if (!g_file_test(real, G_FILE_TEST_IS_DIR)) {
        g_free(real);
        return;
    }
    save_last_dir(real);
    char *base = g_path_get_basename(real);
    const char *display = (*base == '\0') ? "/" : base;
    gtk_tree_store_clear(ui->store);
    /* gtk_tree_store_set() copies the strings, so both temporaries are freed
     * here rather than leaked. */
    tree_add_dir_row(ui->store, NULL, display, real);
    g_free(base);
    GtkTreePath *p = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(ui->tree), p, FALSE);
    gtk_tree_selection_select_path(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree)), p);
    gtk_tree_path_free(p);
    if (ui->path_label) {
        gtk_label_set_text(GTK_LABEL(ui->path_label), real);
        gtk_widget_set_tooltip_text(ui->path_label, real);
    }
    g_free(real);
}

/* ---------------- last-directory persistence ---------------- */
static char *last_dir_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "stlpreview", "state",
                            NULL);
}

void save_last_dir(const char *path)
{
    char *file = last_dir_file();
    char *dir = g_path_get_dirname(file);
    if (g_mkdir_with_parents(dir, DIR_MODE) == 0) {
        GError *err = NULL;
        g_file_set_contents(file, path, -1, &err);
        if (err) {
            g_warning("cannot save last directory: %s", err->message);
            g_error_free(err);
        }
    }
    g_free(dir);
    g_free(file);
}

/* Return the last used directory if it still exists, else NULL. */
char *load_last_dir(void)
{
    char *file = last_dir_file();
    char *contents = NULL;
    if (g_file_get_contents(file, &contents, NULL, NULL)) {
        g_strchomp(contents);
        if (*contents && g_file_test(contents, G_FILE_TEST_IS_DIR))
            return contents;
        g_free(contents);
    }
    g_free(file);
    return NULL;
}

/* "Up": navigate to the parent of the selected folder (or the root). */
static void on_parent_clicked(GtkButton *b, UI *ui)
{
    (void)b;
    char *path = NULL;
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        int is_dir;
        gtk_tree_model_get(model, &iter, COL_PATH, &path, COL_ISDIR, &is_dir,
                           -1);
        if (!is_dir) {
            g_free(path);
            path = NULL;
        }
    }
    if (!path &&
        gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->store), &iter))
        gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, COL_PATH, &path,
                           -1);
    if (path) {
        char *parent = g_path_get_dirname(path);
        if (strcmp(parent, path) != 0)
            set_root(ui, parent);
        g_free(parent);
        g_free(path);
    }
}

static void on_menu_about_clicked(GtkMenuItem *menuitem, gpointer data)
{
    (void)menuitem;
    UI *ui = (UI *)data;
    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "stlpreview");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), "1.1");
    gtk_about_dialog_set_comments(
        GTK_ABOUT_DIALOG(dialog),
        "A static grid viewer for .stl and .3mf 3D model files in a "
        "directory.\n"
        "Features high-fidelity Blinn-Phong shading and CAD-style outlines.");
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog),
                                   "Copyright © 2026 aginies");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                                 "https://github.com/aginies/stlpreview");

    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(ui->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ---------------- main ---------------- */
/* Build the whole window: menu bar, directory tree, view/sort toolbar,
 * thumbnail grid, status bar, drag-and-drop and key bindings, then start the
 * first render. `root` is the folder shown initially. The caller owns `ui`
 * (it may live on the stack) and must call ui_shutdown() once the main loop
 * returns. */
void ui_build(UI *ui, const char *root)
{
    ui->items = items_array_new(0);

    ui->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 1000, 640);
    g_signal_connect(ui->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* Main vertical box containing menu bar (top) and paned views (main area) */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(ui->window), main_box);

    GtkWidget *menu_bar = gtk_menu_bar_new();

    GtkWidget *file_menu = menubar_add_submenu(menu_bar, "_File");
    GtkWidget *quit_item = gtk_menu_item_new_with_mnemonic("_Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(gtk_main_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);

    GtkWidget *help_menu = menubar_add_submenu(menu_bar, "_Help");
    GtkWidget *about_item = gtk_menu_item_new_with_mnemonic("_About");
    g_signal_connect(about_item, "activate", G_CALLBACK(on_menu_about_clicked),
                     ui);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);

    gtk_box_pack_start(GTK_BOX(main_box), menu_bar, FALSE, FALSE, 0);

    /* Main split horizontal view */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(main_box), paned, TRUE, TRUE, 0);

    /* left panel: navigation bar + directory tree */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* 1. navigation bar: [Up] + current root path */
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(nav, 4);
    gtk_widget_set_margin_bottom(nav, 4);
    gtk_widget_set_margin_start(nav, 4);
    gtk_widget_set_margin_end(nav, 4);
    GtkWidget *parent_btn = gtk_button_new_with_mnemonic("_Up");
    gtk_box_pack_start(GTK_BOX(nav), parent_btn, FALSE, FALSE, 0);
    ui->path_label = gtk_label_new(root);
    gtk_label_set_ellipsize(GTK_LABEL(ui->path_label), PANGO_ELLIPSIZE_START);
    g_object_set(ui->path_label, "xalign", 0.0f, NULL);
    gtk_widget_set_tooltip_text(ui->path_label, root);
    gtk_box_pack_start(GTK_BOX(nav), ui->path_label, TRUE, TRUE, 0);
    g_signal_connect(parent_btn, "clicked", G_CALLBACK(on_parent_clicked), ui);
    gtk_box_pack_start(GTK_BOX(left), nav, FALSE, FALSE, 0);

    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 2);
    gtk_widget_set_margin_bottom(sep, 2);
    gtk_box_pack_start(GTK_BOX(left), sep, FALSE, FALSE, 0);

    GtkWidget *tree_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(tree_scroll, 260, -1);
    ui->store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_INT, G_TYPE_INT);
    ui->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->store));
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", COL_NAME);
    gtk_tree_view_column_set_cell_data_func(
        col, icon, (GtkTreeCellDataFunc)on_cell_data, ui, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ui->tree), col);
    gtk_container_add(GTK_CONTAINER(tree_scroll), ui->tree);
    gtk_box_pack_start(GTK_BOX(left), tree_scroll, TRUE, TRUE, 0);
    gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);
    g_signal_connect(ui->tree, "row-expanded", G_CALLBACK(on_row_expanded), ui);
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), ui);

    /* Suppress the selection handler while the tree is set up below:
     * the initial (limited) render is started explicitly, and the setup
     * selection must not kick off a second, unlimited job for the whole
     * root folder. */
    g_signal_handlers_block_by_func(sel, G_CALLBACK(on_selection_changed), ui);

    /* right: view-angle toolbar + thumbnail grid */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *view_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(view_bar, 4);
    gtk_widget_set_margin_bottom(view_bar, 4);
    gtk_widget_set_margin_start(view_bar, 4);
    gtk_widget_set_margin_end(view_bar, 4);

    /* The view angles are a radio group drawn as flat toggle buttons, so the
     * active view is always visible. Label and mode live in one table: adding
     * an angle is then a single line, and the button cannot drift out of step
     * with the ViewMode it selects. */
    static const struct {
        const char *label;
        ViewMode mode;
    } views[] = {
        {"45°", VIEW_45},    {"-45°", VIEW_MINUS_45}, {"Front", VIEW_FRONT},
        {"Back", VIEW_BACK}, {"Left", VIEW_LEFT},     {"Right", VIEW_RIGHT},
    };
    GtkRadioButton *view_group = NULL;
    for (gsize k = 0; k < G_N_ELEMENTS(views); ++k) {
        GtkWidget *btn =
            view_group ? gtk_radio_button_new_with_label_from_widget(
                             view_group, views[k].label)
                       : gtk_radio_button_new_with_label(NULL, views[k].label);
        view_group = GTK_RADIO_BUTTON(btn);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
        g_object_set_data(G_OBJECT(btn), "view_mode",
                          GINT_TO_POINTER(views[k].mode));
        g_signal_connect(btn, "toggled", G_CALLBACK(on_view_toggled), ui);
        gtk_box_pack_start(GTK_BOX(view_bar), btn, FALSE, FALSE, 0);
    }

    /* Separator */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_top(sep2, 2);
    gtk_widget_set_margin_bottom(sep2, 2);
    gtk_box_pack_start(GTK_BOX(view_bar), sep2, FALSE, FALSE, 0);

    /* Sort mode combo box */
    GtkWidget *sort_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *sort_label = gtk_label_new("Sort:");
    gtk_widget_set_margin_start(sort_label, 6);
    gtk_box_pack_start(GTK_BOX(sort_box), sort_label, FALSE, FALSE, 0);
    GtkWidget *sort_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sort_combo), "name",
                              sort_mode_names[SORT_NAME]);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sort_combo), "size",
                              sort_mode_names[SORT_SIZE]);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sort_combo), "date",
                              sort_mode_names[SORT_DATE]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(sort_combo), (int)ui->sort_mode);
    g_signal_connect(sort_combo, "changed", G_CALLBACK(on_sort_changed), ui);
    gtk_box_pack_start(GTK_BOX(sort_box), sort_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view_bar), sort_box, FALSE, FALSE, 0);

    /* Pagination: a folder is rendered RENDER_PAGE_SIZE files at a time,
     * this button appends the next page. Hidden while nothing is left. */
    ui->more_btn = gtk_button_new_with_label("Load more");
    gtk_widget_set_margin_start(ui->more_btn, 6);
    gtk_widget_set_no_show_all(ui->more_btn, TRUE);
    gtk_widget_set_visible(ui->more_btn, FALSE);
    g_signal_connect(ui->more_btn, "clicked", G_CALLBACK(on_load_more_clicked),
                     ui);
    gtk_box_pack_start(GTK_BOX(view_bar), ui->more_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(right), view_bar, FALSE, FALSE, 0);

    ui->grid_scroll = gtk_scrolled_window_new(NULL, NULL);
    g_signal_connect(ui->grid_scroll, "size-allocate",
                     G_CALLBACK(on_grid_scroll_size_allocate), ui);
    /* Scrolling streams thumbnails in and out of memory. */
    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(ui->grid_scroll)),
                     "value-changed", G_CALLBACK(on_grid_scrolled), ui);
    gtk_box_pack_start(GTK_BOX(right), ui->grid_scroll, TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);

    /* Status bar at the bottom */
    ui->status_bar = gtk_label_new("");
    gtk_widget_set_margin_start(ui->status_bar, 6);
    gtk_widget_set_margin_end(ui->status_bar, 6);
    gtk_widget_set_margin_top(ui->status_bar, 2);
    gtk_widget_set_margin_bottom(ui->status_bar, 2);
    gtk_widget_set_hexpand(ui->status_bar, TRUE);
    gtk_box_pack_start(GTK_BOX(main_box), ui->status_bar, FALSE, FALSE, 0);

    /* root node = the directory we were started with */
    gtk_widget_show_all(ui->window);
    gtk_paned_set_position(GTK_PANED(paned), 260);

    /* Seed the tree with the start folder. set_root() expands and selects it,
     * which is why the selection handler is still blocked here: the initial
     * render is started explicitly below, so startup and a folder click take
     * exactly the same path. */
    set_root(ui, root);

    g_signal_handlers_unblock_by_func(sel, G_CALLBACK(on_selection_changed),
                                      ui);

    /* The initial render goes through the normal selection path, so startup
     * and a folder click behave identically: the root listing is read once
     * and the first page is rendered on a worker thread while the window is
     * already interactive. */
    render_current_selection(ui);
}

/* Release everything ui_build() set up. Call after gtk_main() returns: the
 * worker thread is detached, but with the main loop gone its result idle is
 * never dispatched, so it cannot touch the UI any more. */
void ui_shutdown(UI *ui)
{
    /* Shut down: cancel any job still running and stop the progress poller.
     * The worker is detached, but with the main loop gone its result idle is
     * never dispatched, so it cannot touch the (stack allocated) UI. */
    if (ui->job) {
        g_atomic_int_set(&ui->job->cancelled, 1);
        ui->job_gen++; /* make a late publish drop the result */
        ui->job = NULL;
    }

    if (ui->progress_src) {
        g_source_remove(ui->progress_src);
        ui->progress_src = 0;
    }

    /* Stop watching before the tree and the UI go away: the debounce timeout
 * and the monitor callback both dereference `ui`. */
    monitor_set_dir(ui, NULL);

    g_ptr_array_free(ui->items, TRUE);

    if (ui->all_paths) {
        g_ptr_array_free(ui->all_paths, TRUE);
        ui->all_paths = NULL;
    }

    g_free(ui->cur_path);

    ui->cur_path = NULL;
}
