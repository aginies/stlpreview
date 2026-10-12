/*
 * Unit tests for render.c -- the software rasteriser and the shared grid
 * geometry. render.c has no GTK dependency, so these run headless.
 */
#include "../render.c"

#include "test_fixtures.h"
#include "test_util.h"

/* An asymmetric solid, so the six view angles must produce visibly different
 * images. A cube would render identically from front and back. */
static const FixtureTri FIX_TETRA[4] = {
    {0, 0, 0, 4, 0, 0, 0, 3, 0},
    {0, 0, 0, 4, 0, 0, 0, 0, 9},
    {0, 0, 0, 0, 3, 0, 0, 0, 9},
    {4, 0, 0, 0, 3, 0, 0, 0, 9},
};

static cairo_surface_t *make_surface(int w, int h)
{
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    g_assert_cmpint(cairo_surface_status(s), ==, CAIRO_STATUS_SUCCESS);
    return s;
}

/* Read one pixel of an ARGB32 surface as 0xAARRGGBB. */
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

/* Count distinct pixel values: a blank surface has 1, a shaded solid many. */
static guint distinct_colors(cairo_surface_t *s)
{
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (int y = 0; y < cairo_image_surface_get_height(s); y++)
        for (int x = 0; x < cairo_image_surface_get_width(s); x++)
            g_hash_table_add(seen, GUINT_TO_POINTER(pixel(s, x, y)));
    guint n = g_hash_table_size(seen);
    g_hash_table_destroy(seen);
    return n;
}

/* ------------------------------------------------------------------ */
/* Grid geometry: shared by the GTK grid and the PNG export.             */
/* ------------------------------------------------------------------ */

static void test_grid_cols_for_width(void)
{
    int pitch = CELL + GRID_COL_SPACING;

    /* The nominal window width the PNG export also assumes. */
    T_EQ_INT(grid_cols_for_width(GRID_DEFAULT_WIDTH), 3);

    /* Non-positive means "not measured yet" and falls back, never divides by
     * a nonsense width. */
    T_EQ_INT(grid_cols_for_width(0), grid_cols_for_width(GRID_DEFAULT_WIDTH));
    T_EQ_INT(grid_cols_for_width(-100),
             grid_cols_for_width(GRID_DEFAULT_WIDTH));

    /* Never fewer than one column, however narrow the allocation. */
    T_EQ_INT(grid_cols_for_width(1), 1);
    T_EQ_INT(grid_cols_for_width(2 * GRID_MARGIN), 1);

    /* Exact multiples of the pitch add exactly one column. */
    T_EQ_INT(grid_cols_for_width(2 * GRID_MARGIN + pitch), 1);
    T_EQ_INT(grid_cols_for_width(2 * GRID_MARGIN + 2 * pitch), 2);
    T_EQ_INT(grid_cols_for_width(2 * GRID_MARGIN + 7 * pitch), 7);

    /* A partial column does not count. */
    T_EQ_INT(grid_cols_for_width(2 * GRID_MARGIN + 2 * pitch + pitch / 2), 2);
}

/* ------------------------------------------------------------------ */
/* Surface validation: render_mesh must refuse anything it cannot write. */
/* ------------------------------------------------------------------ */

static void test_render_mesh_rejects_bad_surfaces(void)
{
    Mesh m = {0};
    fixture_mesh(&m, FIX_CUBE, FIX_CUBE_TRIS);

    /* NULL surface and NULL mesh. */
    cairo_surface_t *good = make_surface(CELL, CELL);
    T_EQ_INT(render_mesh(&m, NULL, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_FAIL);
    T_EQ_INT(render_mesh(NULL, good, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_FAIL);

    /* Wrong pixel format: the rasteriser writes ARGB32 by hand. */
    cairo_surface_t *rgb24 =
        cairo_image_surface_create(CAIRO_FORMAT_RGB24, CELL, CELL);
    T_EQ_INT(render_mesh(&m, rgb24, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_FAIL);
    cairo_surface_destroy(rgb24);

    /* Wrong dimensions, in both directions. */
    cairo_surface_t *small = make_surface(CELL / 2, CELL / 2);
    cairo_surface_t *wide = make_surface(CELL + 1, CELL);
    T_EQ_INT(render_mesh(&m, small, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_FAIL);
    T_EQ_INT(render_mesh(&m, wide, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_FAIL);
    cairo_surface_destroy(small);
    cairo_surface_destroy(wide);

    /* An empty mesh has nothing to draw and no meaningful bounding box. */
    Mesh empty = {0};
    T_EQ_INT(render_mesh(&empty, good, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_FAIL);

    cairo_surface_destroy(good);
    g_free(m.tris);
}

/* ------------------------------------------------------------------ */
/* Geometry outputs and actual pixels.                                 */
/* ------------------------------------------------------------------ */

static void test_render_mesh_reports_geometry(void)
{
    Mesh m = {0};
    fixture_mesh(&m, FIX_CUBE, FIX_CUBE_TRIS);
    cairo_surface_t *s = make_surface(CELL, CELL);

    float bbox[6] = {0}, center[3] = {0}, ext = -1;
    T_EQ_INT(render_mesh(&m, s, VIEW_45, bbox, center, &ext, NULL, NULL),
             RENDER_OK);

    T_NEAR(bbox[0], 0.0, 1e-6); /* min x */
    T_NEAR(bbox[1], 0.0, 1e-6); /* min y */
    T_NEAR(bbox[2], 0.0, 1e-6); /* min z */
    T_NEAR(bbox[3], FIX_CUBE_EXT, 1e-6);
    T_NEAR(bbox[4], FIX_CUBE_EXT, 1e-6);
    T_NEAR(bbox[5], FIX_CUBE_EXT, 1e-6);

    T_NEAR(center[0], 0.5, 1e-6);
    T_NEAR(center[1], 0.5, 1e-6);
    T_NEAR(center[2], 0.5, 1e-6);

    T_NEAR(ext, FIX_CUBE_EXT, 1e-6);

    /* The outputs are optional: passing NULL must not crash. */
    T_EQ_INT(render_mesh(&m, s, VIEW_FRONT, NULL, NULL, NULL, NULL, NULL),
             RENDER_OK);

    cairo_surface_destroy(s);
    g_free(m.tris);
}

static void test_render_mesh_paints_the_model(void)
{
    Mesh m = {0};
    fixture_mesh(&m, FIX_CUBE, FIX_CUBE_TRIS);
    cairo_surface_t *s = make_surface(CELL, CELL);

    T_EQ_INT(render_mesh(&m, s, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_OK);

    /* The corners are outside the model, the middle is inside it. */
    uint32_t corner = pixel(s, 0, 0);
    uint32_t middle = pixel(s, CELL / 2, CELL / 2);
    T_CHECK(corner != middle);

    /* Shading is flat -- one colour per triangle face -- so a solid yields
     * only a handful of distinct values, not a gradient. The background
     * alone (base colour plus its grid lines) accounts for two, so requiring
     * four means at least one lit face and the crease/silhouette edge pass
     * really drew something. */
    guint colors = distinct_colors(s);
    if (colors < 4)
        T_FAILF("distinct colours: got %u, want at least 4", colors);

    cairo_surface_destroy(s);
    g_free(m.tris);
}

/* render_mesh owns cairo_surface_mark_dirty(): without it, cairo keeps a
 * stale snapshot and anything drawn from the surface afterwards -- including
 * the PNG the export writes -- comes out blank. */
static void test_render_mesh_marks_surface_dirty(void)
{
    Mesh m = {0};
    fixture_mesh(&m, FIX_TETRA, 4);
    cairo_surface_t *s = make_surface(CELL, CELL);

    T_EQ_INT(render_mesh(&m, s, VIEW_45, NULL, NULL, NULL, NULL, NULL),
             RENDER_OK);

    char *dir = fixture_tmpdir();
    char *png = g_build_filename(dir, "thumb.png", NULL);
    T_EQ_INT(cairo_surface_write_to_png(s, png), CAIRO_STATUS_SUCCESS);

    cairo_surface_t *back = cairo_image_surface_create_from_png(png);
    T_EQ_INT(cairo_surface_status(back), CAIRO_STATUS_SUCCESS);
    T_CHECK(pixel(back, 0, 0) != pixel(back, CELL / 2, CELL / 2));
    cairo_surface_destroy(back);

    g_free(png);
    fixture_rm_rf(dir);
    g_free(dir);
    cairo_surface_destroy(s);
    g_free(m.tris);
}

static void test_render_mesh_view_angles_differ(void)
{
    Mesh m = {0};
    fixture_mesh(&m, FIX_TETRA, 4);

    const int views[] = {VIEW_45,   VIEW_MINUS_45, VIEW_FRONT,
                         VIEW_BACK, VIEW_LEFT,     VIEW_RIGHT};
    guint32 signature[G_N_ELEMENTS(views)];

    for (guint i = 0; i < G_N_ELEMENTS(views); i++) {
        cairo_surface_t *s = make_surface(CELL, CELL);
        T_EQ_INT(render_mesh(&m, s, views[i], NULL, NULL, NULL, NULL, NULL),
                 RENDER_OK);
        /* A cheap content fingerprint: a few sample pixels. */
        signature[i] = pixel(s, CELL / 2, CELL / 2) ^
                       (pixel(s, CELL / 3, CELL / 2) >> 3) ^
                       (pixel(s, CELL / 2, CELL / 3) >> 6);
        cairo_surface_destroy(s);
    }

    /* Front and back of an asymmetric solid cannot look the same, otherwise
     * the view selector is being ignored. */
    T_CHECK(signature[2] != signature[3]);
    T_CHECK(signature[4] != signature[5]);
    T_CHECK(signature[0] != signature[1]);

    g_free(m.tris);
}

/* ------------------------------------------------------------------ */
/* Progress and cancellation.                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int calls;
    size_t first_done, first_total;
    int ret; /* what the callback should return */
} ProgressProbe;

static int progress_probe(size_t done, size_t total, void *user_data)
{
    ProgressProbe *p = (ProgressProbe *)user_data;
    if (p->calls == 0) {
        p->first_done = done;
        p->first_total = total;
    }
    p->calls++;
    return p->ret;
}

/* Build a mesh above RENDER_YIELD_MIN_TRIS so the callback is exercised. */
static void fixture_big_mesh(Mesh *m, size_t tris)
{
    m->tris = g_try_new0(Tri, tris);
    g_assert_nonnull(m->tris);
    for (size_t i = 0; i < tris; i++) {
        const FixtureTri *src = &FIX_CUBE[i % FIX_CUBE_TRIS];
        memcpy(m->tris[i].v, *src, sizeof(float) * 9);
        m->tris[i].col = NO_COLOR;
    }
    m->count = tris;
    m->cap = tris;
}

static void test_render_mesh_reports_progress(void)
{
    Mesh m = {0};
    const size_t n = RENDER_YIELD_MIN_TRIS + 88; /* 600, above the threshold */
    fixture_big_mesh(&m, n);
    cairo_surface_t *s = make_surface(CELL, CELL);

    ProgressProbe probe = {0, 0, 0, 0};
    T_EQ_INT(
        render_mesh(&m, s, VIEW_45, NULL, NULL, NULL, progress_probe, &probe),
        RENDER_OK);

    T_CHECK(probe.calls > 1);
    T_EQ_INT(probe.first_done, 0);
    T_EQ_INT(probe.first_total, (long)n);

    cairo_surface_destroy(s);
    g_free(m.tris);
}

static void test_render_mesh_skips_progress_for_small_meshes(void)
{
    /* Below RENDER_YIELD_MIN_TRIS the bookkeeping is not worth it, and the
     * callback must not fire at all. */
    Mesh m = {0};
    fixture_mesh(&m, FIX_CUBE, FIX_CUBE_TRIS);
    cairo_surface_t *s = make_surface(CELL, CELL);

    ProgressProbe probe = {0, 0, 0, 0};
    T_EQ_INT(
        render_mesh(&m, s, VIEW_45, NULL, NULL, NULL, progress_probe, &probe),
        RENDER_OK);
    T_EQ_INT(probe.calls, 0);

    cairo_surface_destroy(s);
    g_free(m.tris);
}

static void test_render_mesh_honours_cancellation(void)
{
    Mesh m = {0};
    fixture_big_mesh(&m, RENDER_YIELD_MIN_TRIS + 88);
    cairo_surface_t *s = make_surface(CELL, CELL);

    ProgressProbe probe = {0, 0, 0, 1}; /* always ask to stop */
    T_EQ_INT(
        render_mesh(&m, s, VIEW_45, NULL, NULL, NULL, progress_probe, &probe),
        RENDER_ABORT);
    T_EQ_INT(probe.calls, 1); /* stopped at the first opportunity */

    /* ABORT must stay distinct from FAIL: a cancelled batch is not an error,
     * while a failed file is skipped and the batch continues. */
    T_CHECK(RENDER_ABORT != RENDER_FAIL);
    T_CHECK(RENDER_OK != RENDER_ABORT);

    cairo_surface_destroy(s);
    g_free(m.tris);
}

int main(void)
{
    printf("test_render\n");
    T_RUN(test_grid_cols_for_width);
    T_RUN(test_render_mesh_rejects_bad_surfaces);
    T_RUN(test_render_mesh_reports_geometry);
    T_RUN(test_render_mesh_paints_the_model);
    T_RUN(test_render_mesh_marks_surface_dirty);
    T_RUN(test_render_mesh_view_angles_differ);
    T_RUN(test_render_mesh_reports_progress);
    T_RUN(test_render_mesh_skips_progress_for_small_meshes);
    T_RUN(test_render_mesh_honours_cancellation);
    return T_REPORT();
}
