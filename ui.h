/*
 * stlpreview -- GTK front end: the shared item/window state and the handful
 * of entry points main.c needs. Everything else in ui.c is private to it.
 */
#ifndef STLVIEW_UI_H
#define STLVIEW_UI_H

#include <gtk/gtk.h>

#include "stlview.h"

typedef struct Item Item;
typedef struct UI UI;
typedef struct RenderJob RenderJob;
typedef struct RenderHooks RenderHooks;

typedef struct Item {
    char *name;
    char *path;             /* full path, for tree lookup on click */
    cairo_surface_t *thumb; /* NULL when streamed out to save memory */
    float bbox[6];          /* min x,y,z and max x,y,z */
    float center[3];        /* centroid */
    float ext;              /* max axis extent */
    int tri_count;          /* number of triangles */
    int vert_count;         /* number of vertices */
    int64_t file_size;      /* file size in bytes */
    time_t mtime;           /* last modification time */
    int file_type;          /* FTYPE_* detected by the loader */
    int selected; /* whether this item is currently selected in the grid */
    GtkWidget *grid_cell; /* this item's event box in the current grid */
    GtkWidget *image;     /* the GtkImage showing thumb, inside grid_cell */
} Item;

struct UI {
    GtkWidget *window;
    GtkWidget *tree;
    GtkTreeStore *store;
    GtkWidget *grid_scroll;
    GtkWidget *grid;
    GtkWidget *path_label; /* current root path, shown above the tree */
    GPtrArray *items;      /* items currently shown in the grid (one page) */
    Item *selected_item;   /* grid item currently showing the blue border */
    /* Pagination of the current selection: `all_paths` is the full sorted
     * listing, `shown_count` how many of its entries have been rendered. */
    GPtrArray *all_paths; /* char*, owned; NULL when nothing is selected */
    guint shown_count;    /* prefix of all_paths already rendered */
    char *cur_path;       /* path of the current tree selection, owned */
    int cur_is_dir;       /* whether cur_path is a folder */
    GtkWidget *more_btn;  /* "Load more" toolbar button, may be NULL */
    ViewMode view_mode;
    SortMode sort_mode;
    int grid_cols; /* columns in the currently built grid */
    /* Asynchronous rendering. Loading/rasterising happens on a worker
     * thread, so no re-entrancy guard is needed any more: a new request
     * cancels the job in flight and bumps job_gen, and results whose
     * generation no longer matches are dropped when they arrive. */
    RenderJob *job;            /* most recently started job, NULL when idle */
    guint job_gen;             /* generation counter, bumped per request */
    GtkWidget *progress_label; /* label in the loading panel, may be NULL */
    guint progress_src; /* timeout id polling job progress, 0 when none */
    /* Directory watching: the folder currently shown is monitored so models
     * added, removed or overwritten by something else (a slicer writing its
     * output, a file manager, a build) are picked up without the user having
     * to click the folder again. */
    GFileMonitor *monitor; /* NULL when nothing is watched */
    gulong monitor_sig;    /* "changed" handler id on `monitor` */
    guint reload_src;      /* debounce timeout id, 0 when none pending */
    char *watch_dir;       /* directory `monitor` is attached to, owned */
    GtkWidget *status_bar; /* bottom info bar for selected file */
    /* move-to-directory state */
    char *move_path;      /* full path of the file being moved */
    char *move_name;      /* basename of the file being moved */
    char *move_dest_path; /* currently selected destination dir in picker */
    int64_t move_mtime;   /* mtime of the file being moved, for cache cleanup */
    int64_t move_size;    /* size of the file being moved, for cache cleanup */
    GtkTreeStore *move_store;
    GtkWidget *move_tree;
    GtkWidget *move_cancel_btn;
    GtkWidget *move_confirm_btn;
};

/* Render every model in `paths` (full paths) into freshly allocated Items.
 * Never touches GTK, so it can run on a worker thread; the caller owns the
 * returned array and its Items. */
GPtrArray *render_paths_sync(GPtrArray *paths, int view_mode,
                             const RenderHooks *hooks);

/* List the *.stl and *.3mf files directly inside `dir`, sorted by `mode`.
 * Hidden entries are skipped. Returns full paths; the caller owns them. */
GPtrArray *list_model_files_sorted(const char *dir, SortMode mode);

/* The folder the user last browsed, restored when none is given on the
 * command line. */
void save_last_dir(const char *path);
char *load_last_dir(void);

/* Build the whole window and start the first render, then release everything
 * again once the main loop has returned. */
void ui_build(UI *ui, const char *root);
void ui_shutdown(UI *ui);

#endif /* STLVIEW_UI_H */
