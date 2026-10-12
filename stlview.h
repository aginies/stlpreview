/*
 * stlpreview -- shared types, constants and cross-module entry points.
 *
 * Module layout:
 *   model.c   STL (binary + ASCII) and 3MF (ZIP + XML) readers, producing a
 *             Mesh. Also the locale-independent number helpers, because the
 *             file formats and the cache sidecar are both strict ASCII.
 *   render.c  software z-buffer rasteriser turning one Mesh into a thumbnail.
 *   cache.c   on-disk PNG + sidecar thumbnail cache: bounded, LRU-evicted.
 *   ui.c      GTK3 front end -- tree, grid, toolbar, dialogs, render jobs.
 *   main.c    command line, and the two entry paths (GUI, -o PNG export).
 *
 * model.c, render.c and cache.c never include GTK, so the headless export and
 * any test can use them without initialising a toolkit.
 */
#ifndef STLVIEW_H
#define STLVIEW_H

#include <cairo/cairo.h>
#include <glib.h>
#include <stddef.h>
#include <stdint.h>

/* Thumbnail geometry. Every mesh is drawn into a fixed CELL x CELL
 * orthographic view with MARGIN pixels of padding, so thumbnails stay
 * comparable whatever the model's own unit or extent. LABEL_H is the strip
 * reserved under each cell for the file name. */
#define CELL    320
#define LABEL_H 24
#define MARGIN  8.0f

/* Grid geometry, shared by the GTK grid and the headless PNG export so both
 * place thumbnails identically and neither can drift away from the other. */
#define GRID_ROW_SPACING 6
#define GRID_COL_SPACING 6
#define GRID_MARGIN      6

/* Width assumed when the grid has no real allocation yet, and by the PNG
 * export, which has no window to measure. */
#define GRID_DEFAULT_WIDTH 1280

/* How many columns of CELL-wide thumbnails fit in `width_px` pixels. A
 * non-positive width means "not measured yet" and falls back to
 * GRID_DEFAULT_WIDTH. Never returns less than 1. */
int grid_cols_for_width(int width_px);

/* Mode bits for the directories this app creates (cache, PNG export). */
#define DIR_MODE 0755

/* Detected model file format. Reported by the loader -- never guessed from
 * the file name -- and written verbatim into the thumbnail-cache sidecar. */
enum {
    FTYPE_UNKNOWN = 0,
    FTYPE_STL_BINARY = 1,
    FTYPE_STL_ASCII = 2,
    FTYPE_3MF = 3
};

typedef struct {
    float x, y, z;
} V3;

typedef struct {
    float v[9];   /* 3 triangle vertices, xyz each */
    uint32_t col; /* 0xRRGGBB face colour; NO_COLOR = use the default shade */
} Tri;

#define NO_COLOR 0xFFFFFFFFu

typedef struct {
    Tri *tris;
    size_t count, cap;
} Mesh;

typedef enum {
    VIEW_45,
    VIEW_MINUS_45,
    VIEW_FRONT,
    VIEW_BACK,
    VIEW_LEFT,
    VIEW_RIGHT
} ViewMode;

typedef enum { SORT_NAME, SORT_SIZE, SORT_DATE } SortMode;

/* ------------------------------------------------------------------ */
/* model.c                                                             */
/* ------------------------------------------------------------------ */

/* Locale-independent number I/O.
 *
 * gtk_init() installs the user's locale, which on comma-decimal locales
 * (fr_FR, de_DE, ...) makes sscanf("%f"), atof() and "%.9g" read and write
 * '12,5'. Model coordinates and the thumbnail-cache sidecar are both file
 * formats, so they must always use '.' as the decimal separator and every
 * float conversion goes through these helpers.
 *
 * They live here rather than in model.c because cache.c needs them too, and a
 * shared header keeps that from becoming a cache -> model dependency.
 * `static inline` means each translation unit gets its own copy and no
 * linker-visible symbol, so the unit tests can include a single .c file. */

/* Parse a decimal number the way the C locale does. Returns `fallback` when
 * `s` is NULL or does not start with a number. */
static inline double ascii_strtod(const char *s, double fallback)
{
    if (!s)
        return fallback;
    char *end = NULL;
    double v = g_ascii_strtod(s, &end);
    if (end == s) /* nothing consumed */
        return fallback;
    return v;
}

/* Format a double with '.' as the decimal separator. Caller frees. */
static inline char *ascii_dtostr(double v)
{
    char *buf = g_malloc(G_ASCII_DTOSTR_BUF_SIZE);
    g_ascii_dtostr(buf, G_ASCII_DTOSTR_BUF_SIZE, v);
    return buf;
}

int is_model_file(const char *name);
size_t mesh_unique_vert_count(const Mesh *m);
int load_mesh(const char *path, Mesh *m, int *type_out);

/* ------------------------------------------------------------------ */
/* render.c                                                            */
/* ------------------------------------------------------------------ */

/* Progress / cancellation hook for render_mesh(). Called from whatever
 * thread runs the render, so it must not touch GTK. Returning non-zero asks
 * the rasteriser to stop. Pass NULL when headless. */
typedef int (*RenderProgressFn)(size_t done, size_t total, void *user_data);

/* render_mesh() result codes. FAIL and ABORT must stay distinct: a failed
 * file is skipped while the batch continues, an aborted batch stops. */
enum {
    RENDER_OK = 0,
    RENDER_FAIL = 1, /* unusable surface, empty mesh, or allocation failure */
    RENDER_ABORT = 2 /* the progress callback asked us to stop */
};

int render_mesh(const Mesh *mesh, cairo_surface_t *surface, int view_mode,
                float *bbox_out, float *center_out, float *ext_out,
                RenderProgressFn progress, void *progress_data);

/* ------------------------------------------------------------------ */
/* cache.c                                                             */
/* ------------------------------------------------------------------ */

char *cache_file_path(const char *file_path, int view_mode, int64_t mtime,
                      int64_t fsize);
void cache_remove_for_file(const char *file_path, int64_t mtime, int64_t fsize);
void cache_prune_once(void);
int load_cached_thumb(const char *file_path, int view_mode, int64_t mtime,
                      int64_t fsize, cairo_surface_t **thumb_out,
                      int *file_type, int *tri_count, int *vert_count,
                      float *bbox, float *center, float *ext);
void save_cached_thumb(const char *file_path, int view_mode, int64_t mtime,
                       int64_t fsize, cairo_surface_t *thumb, int file_type,
                       int tri_count, int vert_count, const float *bbox,
                       const float *center, float ext);

#endif /* STLVIEW_H */
