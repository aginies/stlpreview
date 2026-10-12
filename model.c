/*
 * stlpreview -- model readers.
 *
 * Binary and ASCII STL, and 3MF (a ZIP holding an XML model part, inflated
 * with zlib). Everything here is pure parsing: no GTK, no cairo, no global
 * state, so it runs unchanged on the render worker thread.
 *
 * Numbers are parsed and formatted with the g_ascii_* helpers only. STL and
 * 3MF always use '.' as the decimal separator, and so does the cache sidecar,
 * so a user running under LC_NUMERIC=fr_FR must not change what we read.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

#include "stlview.h"

/* Binary STL layout: an opaque 80-byte header, a little-endian 4-byte
 * triangle count, then 50 bytes per triangle (12-byte normal, 3 x 12-byte
 * vertices, 2-byte attribute). A file that is not exactly this size is not a
 * binary STL -- which is also how an ASCII file that happens to start with
 * "solid" is told apart from one. */
#define STL_BINARY_HEADER 84u
#define STL_TRI_SIZE      50u

/* The locale-independent number helpers (ascii_strtod / ascii_dtostr) live in
 * stlview.h so cache.c can share them without depending on this module. */

/* Locale-independent "vertex x y z" scan. Returns 1 and fills out[0..2]
 * when the line holds three numbers after a leading "vertex" keyword. */
static int parse_vertex_line(const char *line, float out[3])
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (g_ascii_strncasecmp(p, "vertex", 6) != 0)
        return 0;
    p += 6;
    for (int k = 0; k < 3; ++k) {
        char *end = NULL;
        double d = g_ascii_strtod(p, &end);
        if (end == p)
            return 0; /* missing or malformed coordinate */
        out[k] = (float)d;
        p = end;
    }
    return 1;
}

/* The supported model extensions are listed exactly once, here. Matching is
 * case-insensitive; `is_model_file` answers "can we load this at all" and
 * `is_3mf_name` picks the reader. */
int is_model_file(const char *name)
{
    char *lower = g_ascii_strdown(name, -1);
    int ok = g_str_has_suffix(lower, ".stl") || g_str_has_suffix(lower, ".3mf");
    g_free(lower);
    return ok;
}

static int is_3mf_name(const char *name)
{
    char *lower = g_ascii_strdown(name, -1);
    int ok = g_str_has_suffix(lower, ".3mf");
    g_free(lower);
    return ok;
}

/* Double (or initialise) the capacity of a growable array.
 * `*cap` is only updated when the allocation succeeds, so a failure leaves the
 * array valid and the caller can still free it -- the previous pattern bumped
 * the capacity first, which would have desynchronised it from the block that
 * is actually allocated. Returns 0 on failure, the new capacity otherwise. */
static size_t grow_array(void **ptr, size_t *cap, size_t elem, size_t init)
{
    size_t want = *cap ? *cap * 2 : init;
    void *tmp = g_try_realloc(*ptr, want * elem);
    if (!tmp)
        return 0;
    *ptr = tmp;
    *cap = want;
    return want;
}

/* Append one triangle. Returns 0 on success, -1 when out of memory; callers
 * treat that as a failed load rather than terminating the process. */
static int mesh_push_col(Mesh *m, float a[9], uint32_t col)
{
    if (m->count == m->cap &&
        !grow_array((void **)&m->tris, &m->cap, sizeof(Tri), 1024))
        return -1;
    memcpy(&m->tris[m->count].v, a, sizeof(float) * 9);
    m->tris[m->count].col = col;
    m->count++;
    return 0;
}

static int mesh_push(Mesh *m, float a[9])
{
    return mesh_push_col(m, a, NO_COLOR);
}

/* FNV-1a over the raw 12 bytes of a vertex (3 floats). */
static guint vert_key_hash(gconstpointer p)
{
    const unsigned char *b = (const unsigned char *)p;
    guint h = 2166136261u;
    for (int i = 0; i < 12; ++i) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static gboolean vert_key_equal(gconstpointer a, gconstpointer b)
{
    return memcmp(a, b, 12) == 0;
}

/* Number of distinct vertex positions in the mesh. STL repeats every
 * corner once per adjacent triangle, so "3 * triangle count" is not a
 * vertex count; the info bar shows this deduplicated figure instead. */
size_t mesh_unique_vert_count(const Mesh *m)
{
    GHashTable *seen = g_hash_table_new(vert_key_hash, vert_key_equal);
    for (size_t i = 0; i < m->count; ++i)
        for (int k = 0; k < 9; k += 3)
            g_hash_table_add(seen, &m->tris[i].v[k]); /* keys alias m->tris */
    size_t n = g_hash_table_size(seen);
    g_hash_table_destroy(seen);
    return n;
}

static int read_stl_binary(FILE *f, Mesh *m)
{
    uint8_t header[80];
    uint32_t count;
    if (fread(header, 1, 80, f) != 80 || fread(&count, 4, 1, f) != 1)
        return 0;

    /* Validate the declared count against the bytes actually present. Both
     * ftell() and the seeks can fail (non-seekable stdin, I/O error) and must
     * not be trusted unchecked. The arithmetic is 64-bit on purpose: count is
     * attacker-controlled and count * 50 overflows a 32-bit long, which would
     * make a truncated file pass validation. */
    if (fseek(f, 0, SEEK_END) != 0)
        return 0;
    long size = ftell(f);
    if (size < 0)
        return 0;
    int64_t need = (int64_t)STL_BINARY_HEADER + (int64_t)count * STL_TRI_SIZE;
    if (need != (int64_t)size)
        return 0;
    if (fseek(f, (long)STL_BINARY_HEADER, SEEK_SET) != 0)
        return 0;

    uint8_t buf[STL_TRI_SIZE];
    for (uint32_t i = 0; i < count; ++i) {
        if (fread(buf, 1, STL_TRI_SIZE, f) != STL_TRI_SIZE)
            return 0;
        float v[9];
        /* bytes 12..48 hold the 9 vertex floats (skip normal + attr) */
        memcpy(v + 0, buf + 12, 12);
        memcpy(v + 3, buf + 24, 12);
        memcpy(v + 6, buf + 36, 12);
        if (mesh_push(m, v) != 0)
            return 0;
    }
    return m->count > 0;
}

static int read_stl_ascii(FILE *f, Mesh *m)
{
    char line[512];
    float v[9];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        float xyz[3];
        /* Locale-independent: "vertex 1.5 2.5 3.5" must parse the same
         * under LC_NUMERIC=fr_FR (which would make sscanf("%f") read 1). */
        if (parse_vertex_line(line, xyz)) {
            v[n * 3 + 0] = xyz[0];
            v[n * 3 + 1] = xyz[1];
            v[n * 3 + 2] = xyz[2];
            if (++n == 3) {
                if (mesh_push(m, v) != 0)
                    return 0;
                n = 0;
            }
        }
    }
    return m->count > 0;
}

/* Load an STL file, sniffing ASCII vs binary from the content (a binary
 * STL may also start with "solid"). On success *type_out receives
 * FTYPE_STL_ASCII or FTYPE_STL_BINARY. */
static int load_stl(const char *path, Mesh *m, int *type_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    int ok = 0;
    int type = FTYPE_UNKNOWN;
    char head[8];
    size_t got = fread(head, 1, 5, f);
    head[got] = '\0';
    if (got == 5 && strncasecmp(head, "solid", 5) == 0) {
        /* could be ascii; skip the rest of the "solid <name>" line,
         * then scan forward past blank lines for a facet/endsolid line */
        char line[512];
        int is_ascii = 0;
        if (fgets(line, sizeof(line), f)) { /* consume rest of "solid ..." */
            while (fgets(line, sizeof(line), f)) {
                const char *p = line;
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p == '\0' || *p == '\n' || *p == '\r')
                    continue;
                is_ascii = (strncasecmp(p, "facet", 5) == 0 ||
                            strncasecmp(p, "endsolid", 8) == 0);
                break;
            }
        }
        if (is_ascii) {
            fseek(f, 0, SEEK_SET);
            ok = read_stl_ascii(f, m);
            if (ok)
                type = FTYPE_STL_ASCII;
        }
    }
    if (!ok) {
        fseek(f, 0, SEEK_SET);
        ok = read_stl_binary(f, m);
        if (ok)
            type = FTYPE_STL_BINARY;
    }
    fclose(f);
    if (type_out)
        *type_out = ok ? type : FTYPE_UNKNOWN;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Software z-buffer renderer (fixed orthographic view)                */
/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* 3MF XML parser (GLib GMarkup - handles multi-line attrs, namespaces) */
/* ------------------------------------------------------------------ */
typedef struct {
    int objtype;  /* objecttype id whose <map> entries we are collecting */
    int material; /* material index (1-based, as in the file) */
    int base;     /* 0-based base color index the map resolves to (-1 = none) */
} MfMap;

/* One <component objectid="K" transform="..."/>: object K placed by a
 * transform inside an assembly. A component without a usable transform keeps
 * the identity, which is a no-op. */
typedef struct {
    int objectid;
    float m[12]; /* 3x3 matrix row-major, then the translation (3MF order) */
} MfComponent;

/* Parse a 3MF transform attribute: twelve numbers (3x3 matrix row-major, then
 * the translation) separated by whitespace and/or commas. Locale-independent
 * like every other number this file reads. Returns 1 only when exactly twelve
 * well-formed numbers are present: trailing junk means a corrupt attribute,
 * and silently using the first twelve would hide that. */
static int parse_transform(const char *s, float out[12])
{
    int n = 0;
    while (n < 12 && s && *s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == ',')
            ++s;
        if (!*s)
            break;
        char *end = NULL;
        double v = g_ascii_strtod(s, &end);
        if (end == s)
            return 0;
        out[n++] = (float)v;
        s = end;
    }
    if (n != 12)
        return 0;
    /* Only separators may follow the twelfth number. */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == ',')
        ++s;
    return *s == '\0';
}

static int mf_is_identity(const float m[12])
{
    return m[0] == 1.0f && m[4] == 1.0f && m[8] == 1.0f && m[1] == 0.0f &&
           m[2] == 0.0f && m[3] == 0.0f && m[5] == 0.0f && m[6] == 0.0f &&
           m[7] == 0.0f && m[9] == 0.0f && m[10] == 0.0f && m[11] == 0.0f;
}

typedef struct {
    V3 *verts;
    size_t v_count;
    size_t v_cap;
    size_t mesh_base; /* v_count at the start of the current <mesh> */
    Mesh *mesh;
    uint32_t *base_colors;
    size_t n_base, cap_base;
    MfMap *maps;
    size_t n_maps, cap_maps;
    int cur_objtype; /* enclosing <object type="...">, -1 = direct index */
    int objtype;     /* enclosing <objecttype id="..."> */
    /* Assembly bookkeeping: which object each emitted triangle belongs to,
     * every <component> placement seen, and what <build> instantiates. */
    int cur_object_id;  /* enclosing <object id>, -1 = none */
    int in_build;       /* inside <build> */
    GArray *tri_obj;    /* int per triangle in mesh, kept parallel to it */
    GArray *components; /* MfComponent */
    GArray *build_ids;  /* int */
} MfParseCtx;

/* Resolve a triangle's materialindex to a 0xRRGGBB color, or NO_COLOR.
 * 3MF material indices are 1-based; 0/absent means "no material". */
static uint32_t mf_resolve_color(MfParseCtx *p, int materialindex)
{
    if (materialindex <= 0)
        return NO_COLOR;
    if (p->cur_objtype >= 0) {
        /* object references an <objecttype>: resolve via its <map> entries */
        for (size_t i = 0; i < p->n_maps; ++i)
            if (p->maps[i].objtype == p->cur_objtype &&
                p->maps[i].material == materialindex) {
                int b = p->maps[i].base; /* stored 0-based */
                return (b >= 0 && b < (int)p->n_base) ? p->base_colors[b]
                                                      : NO_COLOR;
            }
        return NO_COLOR; /* mapped object but no map entry for this material */
    }
    /* type="model" / no objecttype: materialindex indexes basematerials directly */
    return (materialindex - 1 < (int)p->n_base)
               ? p->base_colors[materialindex - 1]
               : NO_COLOR;
}

/* Abort a GMarkup parse on allocation failure: setting *error stops the
 * parser, so read_3mf_xml() reports a failed load instead of the process
 * exiting. */
static void mf_oom(GError **error)
{
    g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE,
                        "3MF: out of memory");
}

static void on_start_element(GMarkupParseContext *ctx, const char *element_name,
                             const char **attr_names, const char **attr_values,
                             gpointer user_data, GError **error)
{
    (void)ctx;
    MfParseCtx *p = (MfParseCtx *)user_data;

    if (strcmp(element_name, "vertex") == 0) {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        /* 3MF is XML: coordinates always use '.', never the locale's
         * decimal separator, so atof()/strtod() must not be used here. */
        for (int i = 0; attr_names[i]; ++i)
            if (strcmp(attr_names[i], "x") == 0)
                x = (float)ascii_strtod(attr_values[i], 0.0);
            else if (strcmp(attr_names[i], "y") == 0)
                y = (float)ascii_strtod(attr_values[i], 0.0);
            else if (strcmp(attr_names[i], "z") == 0)
                z = (float)ascii_strtod(attr_values[i], 0.0);
        if (p->v_count == p->v_cap &&
            !grow_array((void **)&p->verts, &p->v_cap, sizeof(V3), 1024)) {
            mf_oom(error);
            return;
        }
        p->verts[p->v_count++] = (V3){x, y, z};
    } else if (strcmp(element_name, "mesh") == 0 ||
               strcmp(element_name, "vertices") == 0) {
        /* each <mesh> has its own local vertex index space: a triangle's
         * v1/v2/v3 are relative to the first vertex of THIS mesh, not the
         * first vertex of the file. Remember where this mesh's vertices start. */
        p->mesh_base = p->v_count;
    } else if (strcmp(element_name, "triangle") == 0) {
        int v1 = 0, v2 = 0, v3 = 0, mat = -1;
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "v1") == 0)
                v1 = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "v2") == 0)
                v2 = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "v3") == 0)
                v3 = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "materialindex") == 0)
                mat = atoi(attr_values[i]);
        }
        v1 += (int)p->mesh_base;
        v2 += (int)p->mesh_base;
        v3 += (int)p->mesh_base;
        if (v1 >= 0 && v1 < (int)p->v_count && v2 >= 0 &&
            v2 < (int)p->v_count && v3 >= 0 && v3 < (int)p->v_count) {
            float v[9];
            v[0] = p->verts[v1].x;
            v[1] = p->verts[v1].y;
            v[2] = p->verts[v1].z;
            v[3] = p->verts[v2].x;
            v[4] = p->verts[v2].y;
            v[5] = p->verts[v2].z;
            v[6] = p->verts[v3].x;
            v[7] = p->verts[v3].y;
            v[8] = p->verts[v3].z;
            if (mesh_push_col(p->mesh, v, mf_resolve_color(p, mat)) != 0) {
                mf_oom(error);
                return;
            }
            /* Remember the owning object so <component> transforms can be
             * applied to exactly the right triangles once parsing is done. */
            g_array_append_val(p->tri_obj, p->cur_object_id);
        }
    } else if (strcmp(element_name, "base") == 0) {
        /* <base name="..." color="RRGGBB"/> inside <basematerials> */
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "color") != 0)
                continue;
            if (p->n_base == p->cap_base &&
                !grow_array((void **)&p->base_colors, &p->cap_base,
                            sizeof(uint32_t), 16)) {
                mf_oom(error);
                return;
            }
            p->base_colors[p->n_base++] =
                (uint32_t)g_ascii_strtoull(attr_values[i], NULL, 16);
            break;
        }
    } else if (strcmp(element_name, "objecttype") == 0) {
        int id = -1;
        for (int i = 0; attr_names[i]; ++i)
            if (strcmp(attr_names[i], "id") == 0)
                id = atoi(attr_values[i]);
        p->objtype = id;
    } else if (strcmp(element_name, "map") == 0) {
        int material = -1, pin = -1;
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "material") == 0)
                material = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "pin") == 0)
                pin = atoi(attr_values[i]);
        }
        if (p->n_maps == p->cap_maps &&
            !grow_array((void **)&p->maps, &p->cap_maps, sizeof(MfMap), 16)) {
            mf_oom(error);
            return;
        }
        /* pin is 1-based in the file; store 0-based (-1 or less = absent) */
        p->maps[p->n_maps++] = (MfMap){p->objtype, material, pin - 1};
    } else if (strcmp(element_name, "object") == 0) {
        int type = -1;
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "type") == 0) {
                /* type="model" means "no objecttype" -> direct basematerials
                 * index; any other value names an <objecttype>. */
                type = (g_ascii_strcasecmp(attr_values[i], "model") == 0)
                           ? -1
                           : atoi(attr_values[i]);
            } else if (strcmp(attr_names[i], "id") == 0) {
                p->cur_object_id = atoi(attr_values[i]);
            }
        }
        p->cur_objtype = type;
    } else if (strcmp(element_name, "component") == 0) {
        MfComponent c = {-1, {0}};
        /* Identity unless the file supplies a usable transform. */
        c.m[0] = c.m[4] = c.m[8] = 1.0f;
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "objectid") == 0)
                c.objectid = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "transform") == 0 &&
                     !parse_transform(attr_values[i], c.m)) {
                g_warning("3MF: malformed transform \"%s\" ignored",
                          attr_values[i]);
                c.m[0] = c.m[4] = c.m[8] = 1.0f;
                c.m[1] = c.m[2] = c.m[3] = c.m[5] = c.m[6] = c.m[7] = 0.0f;
                c.m[9] = c.m[10] = c.m[11] = 0.0f;
            }
        }
        if (c.objectid >= 0)
            g_array_append_val(p->components, c);
    } else if (strcmp(element_name, "build") == 0) {
        p->in_build = 1;
    } else if (strcmp(element_name, "item") == 0 && p->in_build) {
        for (int i = 0; attr_names[i]; ++i)
            if (strcmp(attr_names[i], "objectid") == 0) {
                int id = atoi(attr_values[i]);
                g_array_append_val(p->build_ids, id);
                break;
            }
    }
}

static void on_end_element(GMarkupParseContext *ctx, const char *element_name,
                           gpointer user_data, GError **error)
{
    (void)ctx;
    (void)error;
    MfParseCtx *p = (MfParseCtx *)user_data;

    /* Reset the per-object state so it cannot leak into the next sibling:
     * without this an <object> that omits type= inherits the previous
     * object's type and resolves its materials through the wrong
     * <objecttype>. */
    if (strcmp(element_name, "object") == 0) {
        p->cur_object_id = -1;
        p->cur_objtype = -1;
    } else if (strcmp(element_name, "objecttype") == 0) {
        p->objtype = -1;
    } else if (strcmp(element_name, "build") == 0) {
        p->in_build = 0;
    }
}

static void on_text(GMarkupParseContext *ctx, const char *text, gsize text_len,
                    gpointer user_data, GError **error)
{
    (void)ctx;
    (void)text;
    (void)text_len;
    (void)user_data;
    (void)error;
}

/* Apply assembly placements to the triangles already emitted.
 *
 * Full 3MF assembly semantics can require *duplicating* an object's mesh --
 * when one object is instantiated by several components, or is both a
 * component target and a direct <build> item. The mesh is emitted in a single
 * pass, so only unambiguous placements are applied: the referenced object is
 * instantiated exactly once and is not also built directly. Everything else
 * keeps the part in its own coordinates, so all parts stay visible rather
 * than silently vanishing; the situation is reported. */
static void mf_apply_components(const MfParseCtx *p, Mesh *m)
{
    for (guint ci = 0; ci < p->components->len; ++ci) {
        const MfComponent *c = &g_array_index(p->components, MfComponent, ci);
        if (mf_is_identity(c->m))
            continue; /* nothing to do, and nothing to complain about */

        int refs = 0;
        for (guint k = 0; k < p->components->len; ++k)
            if (g_array_index(p->components, MfComponent, k).objectid ==
                c->objectid)
                ++refs;
        int built = 0;
        for (guint k = 0; k < p->build_ids->len; ++k)
            if (g_array_index(p->build_ids, int, k) == c->objectid)
                built = 1;

        if (refs != 1 || built) {
            g_warning("3MF: transform for object %d not applied (%d component "
                      "reference(s), direct build item: %s); the part is shown "
                      "in its own coordinates",
                      c->objectid, refs, built ? "yes" : "no");
            continue;
        }

        const float *t = c->m;
        for (guint i = 0; i < p->tri_obj->len && (size_t)i < m->count; ++i) {
            if (g_array_index(p->tri_obj, int, i) != c->objectid)
                continue;
            for (int k = 0; k < 9; k += 3) {
                float x = m->tris[i].v[k], y = m->tris[i].v[k + 1],
                      z = m->tris[i].v[k + 2];
                /* 3MF uses the row-vector convention: v' = v * M + offset */
                m->tris[i].v[k] = x * t[0] + y * t[3] + z * t[6] + t[9];
                m->tris[i].v[k + 1] = x * t[1] + y * t[4] + z * t[7] + t[10];
                m->tris[i].v[k + 2] = x * t[2] + y * t[5] + z * t[8] + t[11];
            }
        }
    }
}

static int read_3mf_xml(const char *xml, gsize xml_len, Mesh *m)
{
    MfParseCtx p = {0};
    p.mesh = m;
    p.cur_objtype = -1;
    p.objtype = -1;
    p.cur_object_id = -1;
    p.tri_obj = g_array_new(FALSE, FALSE, sizeof(int));
    p.components = g_array_new(FALSE, FALSE, sizeof(MfComponent));
    p.build_ids = g_array_new(FALSE, FALSE, sizeof(int));

    const GMarkupParser parser = {.start_element = on_start_element,
                                  .end_element = on_end_element,
                                  .text = on_text,
                                  .passthrough = NULL,
                                  .error = NULL};

    /* A malformed or truncated document must fail the whole load: returning
     * the partial geometry that was parsed before the error would cache a
     * broken thumbnail that looks valid forever after. */
    int ok = 0;
    GError *err = NULL;
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, &p, NULL);
    if (!ctx) {
        g_warning("3MF: failed to create parser");
    } else if (!g_markup_parse_context_parse(ctx, xml, xml_len, &err)) {
        g_warning("3MF parse error: %s", err ? err->message : "unknown");
    } else if (!g_markup_parse_context_end_parse(ctx, &err)) {
        g_warning("3MF document incomplete: %s",
                  err ? err->message : "unknown");
    } else {
        mf_apply_components(&p, m);
        ok = m->count > 0;
    }
    g_clear_error(&err);
    if (ctx)
        g_markup_parse_context_free(ctx);

    g_free(p.verts);
    g_free(p.base_colors);
    g_free(p.maps);
    g_array_unref(p.tri_obj);
    g_array_unref(p.components);
    g_array_unref(p.build_ids);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Minimal ZIP reader (zlib inflate) — extracts the 3MF .model part    */
/* ------------------------------------------------------------------ */
/* Find the End Of Central Directory record; returns its offset or -1. */
static long zip_find_eocd(const uint8_t *z, size_t len)
{
    if (len < 22)
        return -1;
    size_t floor = len > 22 + 65535 ? len - 22 - 65535 : 0;
    for (size_t i = len - 22;; i--) {
        if (z[i] == 0x50 && z[i + 1] == 0x4b && z[i + 2] == 0x05 &&
            z[i + 3] == 0x06)
            return (long)i;
        if (i == floor)
            break;
    }
    return -1;
}

/* Cap on how large an inflated .model part may become. Both the compressed
 * and uncompressed sizes are read out of the archive itself, so neither can
 * be trusted: without a cap, a crafted 3MF could make us allocate until the
 * OOM killer steps in. Half a gigabyte of XML is far beyond any real model. */
#define ZIP_MAX_MODEL_BYTES (512u * 1024u * 1024u)

/* Extract the first archive entry whose name ends with ".model" (the 3MF
 * model part, conventionally 3D/3dmodel.model). Returns a g_malloc'd
 * buffer, or NULL. */
static uint8_t *zip_extract_model(const uint8_t *z, size_t len, size_t *out_len)
{
    long eocd = zip_find_eocd(z, len);
    if (eocd < 0)
        return NULL;
    uint16_t n_entries;
    uint32_t cd_off;
    memcpy(&n_entries, z + eocd + 10, 2);
    memcpy(&cd_off, z + eocd + 16, 4);

    size_t pos = cd_off;
    for (int i = 0; i < n_entries; ++i) {
        if (pos + 46 > len || memcmp(z + pos, "\x50\x4b\x01\x02", 4) != 0)
            return NULL;
        uint16_t method, name_len, extra_len, comment_len;
        uint32_t comp_size, uncomp_size, local_off;
        memcpy(&method, z + pos + 10, 2);
        memcpy(&comp_size, z + pos + 20, 4);
        memcpy(&uncomp_size, z + pos + 24, 4);
        memcpy(&name_len, z + pos + 28, 2);
        memcpy(&extra_len, z + pos + 30, 2);
        memcpy(&comment_len, z + pos + 32, 2);
        memcpy(&local_off, z + pos + 42, 4);
        size_t name_off = pos + 46;
        if (name_off + name_len > len)
            return NULL;
        char name[1024];
        if (name_len < sizeof(name)) {
            memcpy(name, z + name_off, name_len);
            name[name_len] = '\0';
        } else {
            name[0] = '\0'; /* too long to be a model part */
        }
        pos = name_off + name_len + extra_len + comment_len;
        if (!g_str_has_suffix(name, ".model"))
            continue;

        /* local file header → data offset */
        if (local_off + 30 > len ||
            memcmp(z + local_off, "\x50\x4b\x03\x04", 4) != 0)
            continue;
        uint16_t lname_len, lextra_len;
        memcpy(&lname_len, z + local_off + 26, 2);
        memcpy(&lextra_len, z + local_off + 28, 2);
        size_t data_off = local_off + 30 + lname_len + lextra_len;
        if (data_off + comp_size > len)
            continue;
        const uint8_t *src = z + data_off;

        if (method == 0) { /* stored: the data follows verbatim */
            /* The declared size is attacker-controlled and only comp_size
             * bytes are known to be present, so the two must agree --
             * otherwise the memcpy below reads past the end of the archive.
             * Say which rule fired: a bare "not found" from the caller would
             * make an unsafe archive look like a missing one. */
            if (uncomp_size != comp_size) {
                g_warning("3MF: stored entry \"%s\" declares %u bytes but only "
                          "%u are present; refusing rather than reading past "
                          "the archive",
                          name, uncomp_size, comp_size);
                continue;
            }
            if (uncomp_size > ZIP_MAX_MODEL_BYTES) {
                g_warning("3MF: entry \"%s\" is %u bytes, over the %u byte "
                          "limit",
                          name, uncomp_size, (unsigned)ZIP_MAX_MODEL_BYTES);
                continue;
            }
            uint8_t *out = g_try_malloc(uncomp_size ? uncomp_size : 1);
            if (!out)
                continue;
            memcpy(out, src, uncomp_size);
            *out_len = uncomp_size;
            return out;
        }
        if (method != 8) /* deflate only */
            continue;

        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        /* ZIP stores raw deflate (no zlib header): windowBits = -15 */
        if (inflateInit2(&zs, -15) != Z_OK)
            continue;
        /* Start from the declared size, clamped: it is only a hint, and the
         * loop below grows the buffer as far as the cap allows. */
        size_t cap = uncomp_size > 0 ? uncomp_size : 65536;
        if (cap > ZIP_MAX_MODEL_BYTES)
            cap = ZIP_MAX_MODEL_BYTES;
        uint8_t *out = g_try_malloc(cap);
        if (!out) {
            inflateEnd(&zs);
            continue;
        }
        zs.next_in = (Bytef *)src;
        zs.avail_in = comp_size;
        zs.next_out = out;
        zs.avail_out = cap;
        int ok = 0;
        int limit_hit = 0;
        for (;;) {
            if (zs.avail_out == 0) {
                if (cap >= ZIP_MAX_MODEL_BYTES) {
                    g_warning("3MF: entry \"%s\" inflates past the %u byte "
                              "limit; not allocating more",
                              name, (unsigned)ZIP_MAX_MODEL_BYTES);
                    limit_hit = 1;
                    break; /* a zip bomb is not worth the memory */
                }
                size_t want = cap * 2;
                if (want > ZIP_MAX_MODEL_BYTES)
                    want = ZIP_MAX_MODEL_BYTES;
                uint8_t *grown = g_try_realloc(out, want);
                if (!grown)
                    break;
                out = grown;
                cap = want;
                zs.next_out = out + zs.total_out;
                zs.avail_out = (uInt)(cap - zs.total_out);
            }
            uInt avail_before = zs.avail_out;
            int ret = inflate(&zs, Z_NO_FLUSH);
            if (ret == Z_STREAM_END) {
                ok = 1;
                break;
            }
            if (ret != Z_OK ||
                (zs.avail_in == 0 && zs.avail_out == avail_before))
                break; /* error, or stuck: truncated/corrupt stream */
        }
        inflateEnd(&zs);
        if (!ok) {
            /* The over-limit case already warned above, and "corrupt" would
             * be the wrong diagnosis for an archive that is merely too big. */
            if (!limit_hit)
                g_warning("3MF: entry \"%s\" is truncated or corrupt", name);
            g_free(out);
            continue;
        }
        *out_len = zs.total_out;
        return out;
    }
    return NULL;
}

static int load_3mf(const char *path, Mesh *m)
{
    GError *err = NULL;
    char *blob = NULL;
    gsize blob_len = 0;
    if (!g_file_get_contents(path, &blob, &blob_len, &err)) {
        g_warning("load_3mf: cannot read %s: %s", path,
                  err ? err->message : "unknown error");
        if (err)
            g_error_free(err);
        return 0;
    }
    size_t xml_len = 0;
    uint8_t *xml = zip_extract_model((const uint8_t *)blob, blob_len, &xml_len);
    g_free(blob);
    if (!xml) {
        /* zip_extract_model has already said why when it rejected a part it
         * did find, so only claim "absent" here. */
        g_warning("load_3mf: no usable .model part in %s", path);
        return 0;
    }
    int ok = read_3mf_xml((const char *)xml, xml_len, m);
    g_free(xml);
    return ok;
}

/* Load a model by extension. On success *type_out receives the detected
 * format (FTYPE_3MF / FTYPE_STL_ASCII / FTYPE_STL_BINARY). */
int load_mesh(const char *path, Mesh *m, int *type_out)
{
    int ok = 0;
    int type = FTYPE_UNKNOWN;
    if (is_3mf_name(path)) {
        ok = load_3mf(path, m);
        if (ok)
            type = FTYPE_3MF;
    } else if (is_model_file(path)) { /* .stl: ASCII or binary */
        ok = load_stl(path, m, &type);
    }
    if (type_out)
        *type_out = type;
    return ok;
}
