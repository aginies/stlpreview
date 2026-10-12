/*
 * Shared test fixtures: a unit cube written as binary and ASCII STL, plus
 * scratch-directory helpers.
 *
 * The cube has 12 triangles over 8 distinct corners (24 vertex slots), so a
 * loader can be checked both for its triangle count and for the unique-vertex
 * de-duplication the info bar reports.
 */
#ifndef STLVIEW_TEST_FIXTURES_H
#define STLVIEW_TEST_FIXTURES_H

#include <glib.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Self-contained: Mesh/Tri/NO_COLOR come from the shared header, and its
 * include guard makes this safe after the .c under test has already pulled
 * it in. The path is relative to this file so the header also parses on its
 * own, without the include directories CMake passes to the test targets. */
#include "../stlview.h"

/* One triangle as 9 floats: v0.x v0.y v0.z v1.x ... v2.z. */
typedef float FixtureTri[9];

#define FIX_CUBE_TRIS 12u

/* Unit cube with a corner at the origin, wound outwards. */
static const FixtureTri FIX_CUBE[FIX_CUBE_TRIS] = {
    /* z = 0 */ {0, 0, 0, 0, 1, 0, 1, 1, 0},
    {0, 0, 0, 1, 1, 0, 1, 0, 0},
    /* z = 1 */ {0, 0, 1, 1, 0, 1, 1, 1, 1},
    {0, 0, 1, 1, 1, 1, 0, 1, 1},
    /* y = 0 */ {0, 0, 0, 1, 0, 0, 1, 0, 1},
    {0, 0, 0, 1, 0, 1, 0, 0, 1},
    /* y = 1 */ {0, 1, 0, 0, 1, 1, 1, 1, 1},
    {0, 1, 0, 1, 1, 1, 1, 1, 0},
    /* x = 0 */ {0, 0, 0, 0, 0, 1, 0, 1, 1},
    {0, 0, 0, 0, 1, 1, 0, 1, 0},
    /* x = 1 */ {1, 0, 0, 1, 1, 0, 1, 1, 1},
    {1, 0, 0, 1, 1, 1, 1, 0, 1},
};

/* Bounding box of FIX_CUBE, for assertions. */
#define FIX_CUBE_EXT 1.0

/* A solid whose coordinates are all fractional. FIX_CUBE only has integer
 * corners, which a comma-decimal locale parses identically -- so the
 * LC_NUMERIC regression test needs values that actually contain a '.',
 * otherwise a naive atof("0.5") == 0.0 would go unnoticed. */
#define FIX_DEC_TRIS 2u
static const FixtureTri FIX_DEC[FIX_DEC_TRIS] = {
    {0.5f, -1.25f, 2.75f, 10.125f, 0.0625f, -3.5f, 1.5f, 2.5f, 3.5f},
    {0.5f, -1.25f, 2.75f, 1.5f, 2.5f, 3.5f, -0.0625f, 7.5f, 0.1875f},
};

/* Write a binary STL. `count_override` fakes the declared triangle count
 * (0 means "write the real one"), which is how truncated and overflowing
 * headers are produced. */
static inline void fixture_write_binary(const char *path,
                                        const FixtureTri *tris, size_t n,
                                        uint32_t count_override)
{
    FILE *f = fopen(path, "wb");
    g_assert_nonnull(f);

    char header[80];
    memset(header, 0, sizeof header);
    memcpy(header, "stlpreview test fixture", 23);
    fwrite(header, 1, sizeof header, f);

    uint32_t count = count_override ? count_override : (uint32_t)n;
    fwrite(&count, sizeof count, 1, f);

    for (size_t i = 0; i < n; i++) {
        float record[12]; /* normal, then the three vertices */
        memset(record, 0, sizeof record);
        memcpy(record + 3, tris[i], 9 * sizeof(float));
        fwrite(record, sizeof record, 1, f);
        uint16_t attr = 0;
        fwrite(&attr, sizeof attr, 1, f);
    }
    fclose(f);
}

/* Write an ASCII STL. Numbers go through g_ascii_dtostr so the fixture stays
 * valid even when the test suite runs under a comma-decimal locale -- which is
 * the whole point of one of the tests. */
static inline void fixture_write_ascii(const char *path, const FixtureTri *tris,
                                       size_t n)
{
    FILE *f = fopen(path, "w");
    g_assert_nonnull(f);
    fputs("solid fixture\n", f);
    for (size_t i = 0; i < n; i++) {
        fputs("  facet normal 0 0 0\n    outer loop\n", f);
        for (int v = 0; v < 3; v++) {
            char sx[32], sy[32], sz[32];
            g_ascii_dtostr(sx, sizeof sx, tris[i][v * 3 + 0]);
            g_ascii_dtostr(sy, sizeof sy, tris[i][v * 3 + 1]);
            g_ascii_dtostr(sz, sizeof sz, tris[i][v * 3 + 2]);
            fprintf(f, "      vertex %s %s %s\n", sx, sy, sz);
        }
        fputs("    endloop\n  endfacet\n", f);
    }
    fputs("endsolid fixture\n", f);
    fclose(f);
}

/* Build a Mesh in memory from a fixture triangle list. Caller frees .tris. */
static inline void fixture_mesh(Mesh *m, const FixtureTri *tris, size_t n)
{
    m->tris = g_try_new0(Tri, n);
    g_assert_nonnull(m->tris);
    for (size_t i = 0; i < n; i++) {
        memcpy(m->tris[i].v, tris[i], 9 * sizeof(float));
        /* Tri.col must be set explicitly: NO_COLOR means "use the default
         * shade", and leaving it uninitialised would feed the rasteriser
         * arbitrary face colours. */
        m->tris[i].col = NO_COLOR;
    }
    m->count = n;
    m->cap = n;
}

/* A disposable directory, removed again by fixture_rm_rf(). */
static inline char *fixture_tmpdir(void)
{
    GError *err = NULL;
    char *dir = g_dir_make_tmp("stlpreview-test-XXXXXX", &err);
    g_assert_no_error(err);
    g_assert_nonnull(dir);
    return dir;
}

static inline void fixture_rm_rf(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (!d) {
        g_unlink(path);
        return;
    }
    const char *name;
    while ((name = g_dir_read_name(d))) {
        char *child = g_build_filename(path, name, NULL);
        fixture_rm_rf(child);
        g_free(child);
    }
    g_dir_close(d);
    g_rmdir(path);
}


#endif /* STLVIEW_TEST_FIXTURES_H */
