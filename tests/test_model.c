/*
 * Unit tests for model.c -- the STL/3MF readers and the locale-independent
 * number helpers.
 *
 * model.c is #included rather than linked so the static helpers
 * (parse_vertex_line, parse_transform, read_3mf_xml, grow_array) are visible
 * too. Nothing here touches GTK.
 */
#include <locale.h>

#include "../model.c"

#include "test_fixtures.h"
#include "test_util.h"

/* ------------------------------------------------------------------ */
/* Locale-independent numbers: the regression that motivated item 1.   */
/* ------------------------------------------------------------------ */

static void test_ascii_strtod_parses_c_syntax(void)
{
    T_NEAR(ascii_strtod("1.5", 0), 1.5, 1e-12);
    T_NEAR(ascii_strtod("-2.25", 0), -2.25, 1e-12);
    T_NEAR(ascii_strtod("  3", 0), 3.0, 1e-12);
    T_NEAR(ascii_strtod("1e3", 0), 1000.0, 1e-9);
    T_NEAR(ascii_strtod("0", 0), 0.0, 1e-12);
}

static void test_ascii_strtod_uses_fallback(void)
{
    T_NEAR(ascii_strtod("", -7.5), -7.5, 1e-12);
    T_NEAR(ascii_strtod("abc", -7.5), -7.5, 1e-12);
    T_NEAR(ascii_strtod(NULL, -7.5), -7.5, 1e-12);
}

static void test_ascii_dtostr_roundtrips(void)
{
    char *s = ascii_dtostr(1.5);
    T_EQ_STR(s, "1.5");
    T_NEAR(ascii_strtod(s, 0), 1.5, 1e-12);
    g_free(s);

    s = ascii_dtostr(-0.125);
    T_EQ_STR(s, "-0.125");
    g_free(s);
}

static void test_parse_vertex_line(void)
{
    float out[3] = {0};

    T_EQ_INT(parse_vertex_line("vertex 1.5 2.5 -3.5", out), 1);
    T_NEAR(out[0], 1.5, 1e-6);
    T_NEAR(out[1], 2.5, 1e-6);
    T_NEAR(out[2], -3.5, 1e-6);

    /* Leading whitespace and mixed case are both legal in ASCII STL. */
    T_EQ_INT(parse_vertex_line("\t  VeRtEx 0 0 0", out), 1);
    T_NEAR(out[0], 0.0, 1e-6);

    /* Not a vertex line, or too few coordinates. */
    T_EQ_INT(parse_vertex_line("normal 1 2 3", out), 0);
    T_EQ_INT(parse_vertex_line("vertex 1 2", out), 0);
    T_EQ_INT(parse_vertex_line("vertex 1 two 3", out), 0);
}

/* Try to switch LC_NUMERIC to a comma-decimal locale. Returns 1 on success,
 * 0 when the machine does not have one generated (then tests skip it). */
static int use_comma_locale(void)
{
    static const char *candidates[] = {"fr_FR.UTF-8", "de_DE.UTF-8", "fr_FR",
                                       "de_DE"};
    for (size_t i = 0; i < G_N_ELEMENTS(candidates); i++)
        if (setlocale(LC_NUMERIC, candidates[i]))
            return 1;
    return 0;
}

static void test_numbers_ignore_lc_numeric(void)
{
    if (!use_comma_locale()) {
        printf("    (skipped: no comma-decimal locale installed)\n");
        return;
    }

    /* Under fr_FR a naive atof("1.5") yields 1.0, which silently collapsed
     * every coordinate onto the unit grid. The helpers must not do that. */
    T_NEAR(ascii_strtod("1.5", 0), 1.5, 1e-12);

    float out[3] = {0};
    T_EQ_INT(parse_vertex_line("vertex 1.5 2.5 3.5", out), 1);
    T_NEAR(out[0], 1.5, 1e-6);
    T_NEAR(out[1], 2.5, 1e-6);
    T_NEAR(out[2], 3.5, 1e-6);

    /* The ASCII STL loader must also read '.' under the comma locale. Use
     * fractional coordinates: integer corners parse identically either way
     * and would prove nothing. */
    char *dir = fixture_tmpdir();
    char *path = g_build_filename(dir, "frac.stl", NULL);
    fixture_write_ascii(path, FIX_DEC, FIX_DEC_TRIS);

    Mesh m = {0};
    int type = FTYPE_UNKNOWN;
    T_EQ_INT(load_mesh(path, &m, &type), 1);
    T_EQ_INT(type, FTYPE_STL_ASCII);
    T_EQ_INT(m.count, FIX_DEC_TRIS);
    /* Under a naive atof() every one of these would come back as 0.0, which
     * is exactly how the original bug flattened every model. */
    T_NEAR(m.tris[0].v[0], 0.5, 1e-6);
    T_NEAR(m.tris[0].v[1], -1.25, 1e-6);
    T_NEAR(m.tris[0].v[2], 2.75, 1e-6);
    T_NEAR(m.tris[0].v[3], 10.125, 1e-6);
    T_NEAR(m.tris[0].v[4], 0.0625, 1e-6);
    T_NEAR(m.tris[0].v[5], -3.5, 1e-6);
    T_NEAR(m.tris[1].v[6], -0.0625, 1e-6);
    T_NEAR(m.tris[1].v[7], 7.5, 1e-6);
    T_NEAR(m.tris[1].v[8], 0.1875, 1e-6);
    g_free(m.tris);

    g_free(path);
    fixture_rm_rf(dir);
    g_free(dir);
    setlocale(LC_NUMERIC, "C");
}

/* ------------------------------------------------------------------ */
/* Extension predicates: the single source of truth for "is a model".  */
/* ------------------------------------------------------------------ */

static void test_is_model_file(void)
{
    T_EQ_INT(is_model_file("cube.stl"), 1);
    T_EQ_INT(is_model_file("CUBE.STL"), 1);
    T_EQ_INT(is_model_file("part.3mf"), 1);
    T_EQ_INT(is_model_file("part.3MF"), 1);
    T_EQ_INT(is_model_file("/a/b/c.stl"), 1);
    T_EQ_INT(is_model_file("notes.txt"), 0);
    T_EQ_INT(is_model_file("stl"), 0);
    T_EQ_INT(is_model_file("cube.stl.bak"), 0);
    T_EQ_INT(is_model_file(""), 0);

    /* The 3MF reader is chosen by a separate, narrower predicate. */
    T_EQ_INT(is_3mf_name("part.3mf"), 1);
    T_EQ_INT(is_3mf_name("PART.3MF"), 1);
    T_EQ_INT(is_3mf_name("cube.stl"), 0);
}

/* ------------------------------------------------------------------ */
/* grow_array: the shared growth helper every loader uses.             */
/* ------------------------------------------------------------------ */

static void test_grow_array_starts_at_init_then_doubles(void)
{
    void *base = NULL;
    size_t cap = 0;

    T_EQ_INT(grow_array(&base, &cap, 8, 4), 4);
    T_EQ_INT(cap, 4);
    T_EQ_INT(grow_array(&base, &cap, 8, 4), 8);
    T_EQ_INT(cap, 8);
    T_EQ_INT(grow_array(&base, &cap, 8, 4), 16);
    T_EQ_INT(cap, 16);
    g_free(base);
}

/* ------------------------------------------------------------------ */
/* Unique-vertex counting, reported in the info bar.                   */
/* ------------------------------------------------------------------ */

static void test_mesh_unique_vert_count_dedups(void)
{
    Mesh m = {0};
    fixture_mesh(&m, FIX_CUBE, FIX_CUBE_TRIS);
    /* 12 triangles x 3 = 36 slots, but a cube has 8 distinct corners. */
    T_EQ_INT(mesh_unique_vert_count(&m), 8);
    g_free(m.tris);

    /* Two identical triangles share all three corners. */
    FixtureTri dup[2];
    memcpy(dup[0], FIX_CUBE[0], sizeof(FixtureTri));
    memcpy(dup[1], FIX_CUBE[0], sizeof(FixtureTri));
    fixture_mesh(&m, dup, 2);
    T_EQ_INT(mesh_unique_vert_count(&m), 3);
    g_free(m.tris);

    Mesh empty = {0};
    T_EQ_INT(mesh_unique_vert_count(&empty), 0);
}

/* ------------------------------------------------------------------ */
/* STL loading, including the hostile-header cases.                    */
/* ------------------------------------------------------------------ */

static void test_load_binary_stl(void)
{
    char *dir = fixture_tmpdir();
    char *path = g_build_filename(dir, "cube.stl", NULL);
    fixture_write_binary(path, FIX_CUBE, FIX_CUBE_TRIS, 0);

    Mesh m = {0};
    int type = FTYPE_UNKNOWN;
    T_EQ_INT(load_mesh(path, &m, &type), 1);
    T_EQ_INT(type, FTYPE_STL_BINARY);
    T_EQ_INT(m.count, FIX_CUBE_TRIS);
    T_EQ_INT(mesh_unique_vert_count(&m), 8);

    /* Bounding box of the unit cube. */
    for (int i = 0; i < 3; i++) {
        T_NEAR(m.tris[0].v[i], 0.0, 1e-6);
    }
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t t = 0; t < m.count; t++)
        for (int v = 0; v < 3; v++)
            for (int i = 0; i < 3; i++) {
                float c = m.tris[t].v[v * 3 + i];
                if (c < lo[i])
                    lo[i] = c;
                if (c > hi[i])
                    hi[i] = c;
            }
    for (int i = 0; i < 3; i++) {
        T_NEAR(lo[i], 0.0, 1e-6);
        T_NEAR(hi[i], FIX_CUBE_EXT, 1e-6);
    }
    /* STL carries no material, so every face keeps the default shade. */
    T_CHECK(m.tris[0].col == NO_COLOR);

    g_free(m.tris);
    g_free(path);
    fixture_rm_rf(dir);
    g_free(dir);
}

static void test_load_ascii_stl(void)
{
    char *dir = fixture_tmpdir();
    char *path = g_build_filename(dir, "cube.stl", NULL);
    fixture_write_ascii(path, FIX_CUBE, FIX_CUBE_TRIS);

    Mesh m = {0};
    int type = FTYPE_UNKNOWN;
    T_EQ_INT(load_mesh(path, &m, &type), 1);
    T_EQ_INT(type, FTYPE_STL_ASCII);
    T_EQ_INT(m.count, FIX_CUBE_TRIS);
    T_EQ_INT(mesh_unique_vert_count(&m), 8);
    g_free(m.tris);

    g_free(path);
    fixture_rm_rf(dir);
    g_free(dir);
}

static void test_binary_stl_truncated_header_is_rejected(void)
{
    char *dir = fixture_tmpdir();
    char *path = g_build_filename(dir, "truncated.stl", NULL);
    /* Declares 12 triangles but the file only contains 6 of them. */
    fixture_write_binary(path, FIX_CUBE, FIX_CUBE_TRIS / 2, FIX_CUBE_TRIS);

    Mesh m = {0};
    int type = FTYPE_STL_ASCII;
    T_EQ_INT(load_mesh(path, &m, &type), 0);
    T_EQ_INT(type, FTYPE_UNKNOWN); /* a failed load must not claim a format */
    T_EQ_INT(m.count, 0);
    g_free(m.tris);

    g_free(path);
    fixture_rm_rf(dir);
    g_free(dir);
}

static void test_binary_stl_overflowing_count_is_rejected(void)
{
    char *dir = fixture_tmpdir();
    char *path = g_build_filename(dir, "overflow.stl", NULL);
    /* 0xFFFFFFFF triangles would need ~200 GiB; a 32-bit count*50 would wrap
     * to a small number and pass a naive size check. */
    fixture_write_binary(path, FIX_CUBE, FIX_CUBE_TRIS, 0xFFFFFFFFu);

    Mesh m = {0};
    int type = FTYPE_UNKNOWN;
    T_EQ_INT(load_mesh(path, &m, &type), 0);
    T_EQ_INT(type, FTYPE_UNKNOWN);
    g_free(m.tris);

    g_free(path);
    fixture_rm_rf(dir);
    g_free(dir);
}

static void test_load_mesh_rejects_non_models(void)
{
    char *dir = fixture_tmpdir();

    /* A perfectly good cube under an unsupported extension is not loaded. */
    char *txt = g_build_filename(dir, "cube.txt", NULL);
    fixture_write_binary(txt, FIX_CUBE, FIX_CUBE_TRIS, 0);
    Mesh m = {0};
    int type = FTYPE_STL_BINARY;
    T_EQ_INT(load_mesh(txt, &m, &type), 0);
    T_EQ_INT(type, FTYPE_UNKNOWN);
    g_free(m.tris);

    /* A missing file fails without crashing. */
    char *missing = g_build_filename(dir, "nope.stl", NULL);
    memset(&m, 0, sizeof m);
    type = FTYPE_STL_BINARY;
    T_EQ_INT(load_mesh(missing, &m, &type), 0);
    T_EQ_INT(type, FTYPE_UNKNOWN);
    g_free(m.tris);

    g_free(txt);
    g_free(missing);
    fixture_rm_rf(dir);
    g_free(dir);
}

/* ------------------------------------------------------------------ */
/* 3MF: transform parsing and the XML reader.                          */
/* ------------------------------------------------------------------ */

static void test_parse_transform(void)
{
    float m[12] = {0};

    T_EQ_INT(parse_transform("1 0 0 0 1 0 0 0 1 5 -2.5 7", m), 1);
    T_NEAR(m[0], 1.0, 1e-6);
    T_NEAR(m[4], 1.0, 1e-6);
    T_NEAR(m[8], 1.0, 1e-6);
    T_NEAR(m[9], 5.0, 1e-6);
    T_NEAR(m[10], -2.5, 1e-6);
    T_NEAR(m[11], 7.0, 1e-6);

    /* Commas are an equally valid separator in the 3MF grammar. */
    T_EQ_INT(parse_transform("2,0,0,0,2,0,0,0,2,0,0,0", m), 1);
    T_NEAR(m[0], 2.0, 1e-6);

    /* Too few numbers, trailing garbage, and non-numeric input all fail
     * rather than silently producing a half-filled matrix. */
    T_EQ_INT(parse_transform("1 0 0 0 1 0", m), 0);
    T_EQ_INT(parse_transform("1 0 0 0 1 0 0 0 1 0 0 0 9", m), 0);
    T_EQ_INT(parse_transform("a b c d e f g h i j k l", m), 0);
    T_EQ_INT(parse_transform("", m), 0);
    T_EQ_INT(parse_transform(NULL, m), 0);
}

/* A tetrahedron: the smallest closed solid, easy to reason about. */
#define MF_TETRA                                                           \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"                           \
    "<model unit=\"millimeter\" "                                          \
    "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">" \
    "<resources>"                                                          \
    "<object id=\"1\" type=\"model\"><mesh>"                               \
    "<vertices>"                                                           \
    "<vertex x=\"0\" y=\"0\" z=\"0\"/>"                                    \
    "<vertex x=\"10\" y=\"0\" z=\"0\"/>"                                   \
    "<vertex x=\"0\" y=\"10\" z=\"0\"/>"                                   \
    "<vertex x=\"0\" y=\"0\" z=\"10\"/>"                                   \
    "</vertices>"                                                          \
    "<triangles>"                                                          \
    "<triangle v1=\"0\" v2=\"1\" v3=\"2\"/>"                               \
    "<triangle v1=\"0\" v2=\"1\" v3=\"3\"/>"                               \
    "<triangle v1=\"0\" v2=\"2\" v3=\"3\"/>"                               \
    "<triangle v1=\"1\" v2=\"2\" v3=\"3\"/>"                               \
    "</triangles></mesh></object>"                                         \
    "</resources>"                                                         \
    "<build><item objectid=\"1\"/></build>"                                \
    "</model>"

static void test_read_3mf_xml_basic(void)
{
    Mesh m = {0};
    T_EQ_INT(read_3mf_xml(MF_TETRA, strlen(MF_TETRA), &m), 1);
    T_EQ_INT(m.count, 4);
    T_EQ_INT(mesh_unique_vert_count(&m), 4);
    /* The last triangle references vertex 1 at (10,0,0). */
    T_NEAR(m.tris[3].v[0], 10.0, 1e-6);
    g_free(m.tris);
}

static void test_read_3mf_xml_rejects_truncated_document(void)
{
    /* Cutting the document in half must fail the whole load: returning the
     * partial geometry would cache a broken thumbnail that looks valid. */
    size_t full = strlen(MF_TETRA);
    Mesh m = {0};
    T_EQ_INT(read_3mf_xml(MF_TETRA, full / 2, &m), 0);
    g_free(m.tris);

    /* Unterminated tag is a parse error, not an incomplete document. */
    const char *bad = "<model><resources><object id=\"1\"><mesh>";
    memset(&m, 0, sizeof m);
    T_EQ_INT(read_3mf_xml(bad, strlen(bad), &m), 0);
    g_free(m.tris);
}

static void test_read_3mf_xml_loads_geometry_without_build(void)
{
    /* A document with parts but no <build> still yields geometry. That is
     * deliberate: this is a thumbnail viewer, and refusing to preview a file
     * that merely omits its build list would hide models the user can see
     * elsewhere. The test pins the lenient contract so it cannot silently
     * become strict. */
    const char *no_build =
        "<?xml version=\"1.0\"?>"
        "<model "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">"
        "<resources><object id=\"1\" type=\"model\"><mesh>"
        "<vertices><vertex x=\"0\" y=\"0\" z=\"0\"/>"
        "<vertex x=\"1\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"1\" z=\"0\"/>"
        "</vertices>"
        "<triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\"/></triangles>"
        "</mesh></object></resources></model>";
    Mesh m = {0};
    T_EQ_INT(read_3mf_xml(no_build, strlen(no_build), &m), 1);
    T_EQ_INT(m.count, 1);
    g_free(m.tris);

    /* A document with no geometry at all is still a miss. */
    const char *empty =
        "<?xml version=\"1.0\"?>"
        "<model "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">"
        "<resources></resources><build></build></model>";
    memset(&m, 0, sizeof m);
    T_EQ_INT(read_3mf_xml(empty, strlen(empty), &m), 0);
    g_free(m.tris);
}

/* An assembly: object 1 is a tetrahedron at the origin, object 2 places it
 * 100mm along X. Only object 2 is built. */
#define MF_ASSEMBLY                                                        \
    "<?xml version=\"1.0\"?>"                                              \
    "<model "                                                              \
    "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">" \
    "<resources>"                                                          \
    "<object id=\"1\" type=\"model\"><mesh>"                               \
    "<vertices>"                                                           \
    "<vertex x=\"0\" y=\"0\" z=\"0\"/>"                                    \
    "<vertex x=\"10\" y=\"0\" z=\"0\"/>"                                   \
    "<vertex x=\"0\" y=\"10\" z=\"0\"/>"                                   \
    "<vertex x=\"0\" y=\"0\" z=\"10\"/>"                                   \
    "</vertices>"                                                          \
    "<triangles>"                                                          \
    "<triangle v1=\"0\" v2=\"1\" v3=\"2\"/>"                               \
    "<triangle v1=\"0\" v2=\"1\" v3=\"3\"/>"                               \
    "<triangle v1=\"0\" v2=\"2\" v3=\"3\"/>"                               \
    "<triangle v1=\"1\" v2=\"2\" v3=\"3\"/>"                               \
    "</triangles></mesh></object>"                                         \
    "<object id=\"2\" type=\"model\"><components>"                         \
    "<component objectid=\"1\" transform=\"1 0 0 0 1 0 0 0 1 100 0 0\"/>"  \
    "</components></object>"                                               \
    "</resources>"                                                         \
    "<build><item objectid=\"2\"/></build>"                                \
    "</model>"

static void test_read_3mf_xml_applies_component_transform(void)
{
    Mesh m = {0};
    T_EQ_INT(read_3mf_xml(MF_ASSEMBLY, strlen(MF_ASSEMBLY), &m), 1);
    T_EQ_INT(m.count, 4);

    /* Every vertex moved +100 along X; Y and Z are untouched. */
    float minx = 1e9f, maxx = -1e9f, maxy = -1e9f, maxz = -1e9f;
    for (size_t t = 0; t < m.count; t++)
        for (int v = 0; v < 3; v++) {
            float x = m.tris[t].v[v * 3 + 0];
            float y = m.tris[t].v[v * 3 + 1];
            float z = m.tris[t].v[v * 3 + 2];
            if (x < minx)
                minx = x;
            if (x > maxx)
                maxx = x;
            if (y > maxy)
                maxy = y;
            if (z > maxz)
                maxz = z;
        }
    T_NEAR(minx, 100.0, 1e-4);
    T_NEAR(maxx, 110.0, 1e-4);
    T_NEAR(maxy, 10.0, 1e-4);
    T_NEAR(maxz, 10.0, 1e-4);
    g_free(m.tris);
}

static void test_read_3mf_xml_ignores_malformed_transform(void)
{
    /* A transform with only six numbers is unusable: the part must stay in
     * its own coordinates rather than being collapsed by a zero matrix. */
    const char *xml =
        "<?xml version=\"1.0\"?>"
        "<model "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">"
        "<resources>"
        "<object id=\"1\" type=\"model\"><mesh>"
        "<vertices><vertex x=\"0\" y=\"0\" z=\"0\"/>"
        "<vertex x=\"10\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"10\" z=\"0\"/>"
        "<vertex x=\"0\" y=\"0\" z=\"10\"/></vertices>"
        "<triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\"/>"
        "<triangle v1=\"0\" v2=\"1\" v3=\"3\"/>"
        "<triangle v1=\"0\" v2=\"2\" v3=\"3\"/>"
        "<triangle v1=\"1\" v2=\"2\" v3=\"3\"/></triangles>"
        "</mesh></object>"
        "<object id=\"2\" type=\"model\"><components>"
        "<component objectid=\"1\" transform=\"1 0 0 0 1 0\"/>"
        "</components></object>"
        "</resources>"
        "<build><item objectid=\"2\"/></build></model>";
    Mesh m = {0};
    T_EQ_INT(read_3mf_xml(xml, strlen(xml), &m), 1);
    T_EQ_INT(m.count, 4);
    float maxx = -1e9f;
    for (size_t t = 0; t < m.count; t++)
        for (int v = 0; v < 3; v++)
            if (m.tris[t].v[v * 3] > maxx)
                maxx = m.tris[t].v[v * 3];
    T_NEAR(maxx, 10.0, 1e-4); /* identity, not a translation */
    g_free(m.tris);
}

static void test_read_3mf_xml_object_type_does_not_leak(void)
{
    /* The second <object> omits type=. Before the per-element reset it
     * inherited the first object's type and resolved materials wrongly.
     * Here both objects are plain geometry, so the triangle count and the
     * absence of a crash are the observable contract. */
    const char *xml =
        "<?xml version=\"1.0\"?>"
        "<model "
        "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">"
        "<resources>"
        "<object id=\"1\" type=\"model\"><mesh>"
        "<vertices><vertex x=\"0\" y=\"0\" z=\"0\"/>"
        "<vertex x=\"1\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"1\" z=\"0\"/>"
        "</vertices>"
        "<triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\"/></triangles>"
        "</mesh></object>"
        "<object id=\"2\"><mesh>"
        "<vertices><vertex x=\"0\" y=\"0\" z=\"0\"/>"
        "<vertex x=\"2\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"2\" z=\"0\"/>"
        "</vertices>"
        "<triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\"/></triangles>"
        "</mesh></object>"
        "</resources>"
        "<build><item objectid=\"1\"/><item objectid=\"2\"/></build></model>";
    Mesh m = {0};
    T_EQ_INT(read_3mf_xml(xml, strlen(xml), &m), 1);
    T_EQ_INT(m.count, 2);
    /* Each <mesh> has its own vertex index space: object 2's v2="1" and
     * v3="2" resolve to its own (2,0,0) and (0,2,0). Without that base
     * offset they would land on object 1's (1,0,0) and (0,1,0). */
    T_NEAR(m.tris[1].v[0], 0.0, 1e-6);
    T_NEAR(m.tris[1].v[3], 2.0, 1e-6);
    T_NEAR(m.tris[1].v[7], 2.0, 1e-6);
    g_free(m.tris);
}

int main(void)
{
    printf("test_model\n");
    T_RUN(test_ascii_strtod_parses_c_syntax);
    T_RUN(test_ascii_strtod_uses_fallback);
    T_RUN(test_ascii_dtostr_roundtrips);
    T_RUN(test_parse_vertex_line);
    T_RUN(test_numbers_ignore_lc_numeric);
    T_RUN(test_is_model_file);
    T_RUN(test_grow_array_starts_at_init_then_doubles);
    T_RUN(test_mesh_unique_vert_count_dedups);
    T_RUN(test_load_binary_stl);
    T_RUN(test_load_ascii_stl);
    T_RUN(test_binary_stl_truncated_header_is_rejected);
    T_RUN(test_binary_stl_overflowing_count_is_rejected);
    T_RUN(test_load_mesh_rejects_non_models);
    T_RUN(test_parse_transform);
    T_RUN(test_read_3mf_xml_basic);
    T_RUN(test_read_3mf_xml_rejects_truncated_document);
    T_RUN(test_read_3mf_xml_loads_geometry_without_build);
    T_RUN(test_read_3mf_xml_applies_component_transform);
    T_RUN(test_read_3mf_xml_ignores_malformed_transform);
    T_RUN(test_read_3mf_xml_object_type_does_not_leak);
    return T_REPORT();
}
