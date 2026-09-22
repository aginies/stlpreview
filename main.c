/*
 * sliceview — static grid viewer for all .stl files in a directory.
 *
 * Usage:
 *   sliceview <directory>            show a scrollable grid of thumbnails
 *   sliceview <directory> -o out.png render the grid to a PNG file instead
 *
 * The window shows a directory tree on the left (folders and .stl files,
 * loaded lazily) and a static grid of thumbnails on the right. Selecting
 * a folder shows the .stl files directly inside it; selecting a file
 * shows that single mesh.
 *
 * No zoom, no rotation: every mesh is drawn with a fixed orthographic
 * view fitted to its bounding box. Rendering is a small software
 * z-buffer rasterizer (no OpenGL), which is plenty for thumbnails.
 */

#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

/* realpath declaration (avoids needing _GNU_SOURCE/_POSIX_C_SOURCE) */
extern char *realpath(const char *path, char *resolved_path);

#define CELL 320
#define LABEL_H 24
#define MARGIN 8.0f

/* ------------------------------------------------------------------ */
/* STL loading                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    float v[9]; /* 3 triangle vertices, xyz each */
    uint32_t col; /* 0xRRGGBB face color; NO_COLOR = use default shade */
} Tri;

#define NO_COLOR 0xFFFFFFFFu

typedef struct {
    Tri *tris;
    size_t count, cap;
} Mesh;

typedef struct {
    float x, y, z;
} V3;

typedef enum {
    VIEW_45,
    VIEW_MINUS_45,
    VIEW_FRONT,
    VIEW_BACK,
    VIEW_LEFT,
    VIEW_RIGHT
} ViewMode;

typedef enum {
    SORT_NAME,
    SORT_SIZE,
    SORT_DATE
} SortMode;

static const char *sort_mode_names[] = { "Name", "Size", "Date" };

static void mesh_push_col(Mesh *m, float a[9], uint32_t col)
{
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 1024;
        Tri *tmp = realloc(m->tris, m->cap * sizeof(Tri));
        if (!tmp) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        m->tris = tmp;
    }
    memcpy(&m->tris[m->count].v, a, sizeof(float) * 9);
    m->tris[m->count].col = col;
    m->count++;
}

static void mesh_push(Mesh *m, float a[9])
{
    mesh_push_col(m, a, NO_COLOR);
}

static int read_stl_binary(FILE *f, Mesh *m)
{
    uint8_t header[80];
    uint32_t count;
    if (fread(header, 1, 80, f) != 80 || fread(&count, 4, 1, f) != 1)
        return 0;
    /* verify total size */
    if (fseek(f, 0, SEEK_END) != 0)
        return 0;
    long size = ftell(f);
    fseek(f, 84, SEEK_SET);
    if (size != 84L + (long)count * 50)
        return 0;

    uint8_t buf[50];
    for (uint32_t i = 0; i < count; ++i) {
        if (fread(buf, 1, 50, f) != 50)
            return 0;
        float v[9];
        /* bytes 12..48 hold the 9 vertex floats (skip normal + attr) */
        memcpy(v + 0, buf + 12, 12);
        memcpy(v + 3, buf + 24, 12);
        memcpy(v + 6, buf + 36, 12);
        mesh_push(m, v);
    }
    return m->count > 0;
}

static int read_stl_ascii(FILE *f, Mesh *m)
{
    char line[512];
    float v[9];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        float x, y, z;
        if (sscanf(line, " vertex %f %f %f", &x, &y, &z) == 3) {
            v[n * 3 + 0] = x;
            v[n * 3 + 1] = y;
            v[n * 3 + 2] = z;
            if (++n == 3) {
                mesh_push(m, v);
                n = 0;
            }
        }
    }
    return m->count > 0;
}

static int load_stl(const char *path, Mesh *m)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    int ok = 0;
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
        }
    }
    if (!ok) {
        fseek(f, 0, SEEK_SET);
        ok = read_stl_binary(f, m);
    }
    fclose(f);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Software z-buffer renderer (fixed orthographic view)                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 3MF XML parser (GLib GMarkup - handles multi-line attrs, namespaces) */
/* ------------------------------------------------------------------ */

typedef struct {
    int objtype;   /* objecttype id whose <map> entries we are collecting */
    int material;  /* material index */
    int base;      /* base color index the map resolves to */
} MfMap;

typedef struct {
    V3 *verts;
    size_t v_count;
    size_t v_cap;
    size_t mesh_base;   /* v_count at the start of the current <mesh> */
    Mesh *mesh;
    uint32_t *base_colors; size_t n_base, cap_base;
    MfMap *maps; size_t n_maps, cap_maps;
    int cur_objtype;    /* enclosing <object type="...">, -1 = direct index */
    int objtype;        /* enclosing <objecttype id="..."> */
} MfParseCtx;

/* Resolve a triangle's materialindex to a 0xRRGGBB color, or NO_COLOR. */
static uint32_t mf_resolve_color(MfParseCtx *p, int materialindex)
{
    if (materialindex < 0)
        return NO_COLOR;
    if (p->cur_objtype >= 0) {
        /* object references an <objecttype>: resolve via its <map> entries */
        for (size_t i = 0; i < p->n_maps; ++i)
            if (p->maps[i].objtype == p->cur_objtype &&
                p->maps[i].material == materialindex) {
                int b = p->maps[i].base;
                return (b >= 0 && b < (int)p->n_base) ? p->base_colors[b] : NO_COLOR;
            }
        return NO_COLOR; /* mapped object but no map entry for this material */
    }
    /* type="model" / no objecttype: materialindex indexes basematerials directly */
    return (materialindex < (int)p->n_base) ? p->base_colors[materialindex] : NO_COLOR;
}

static void on_start_element(GMarkupParseContext *ctx, const char *element_name,
                              const char **attr_names, const char **attr_values,
                              gpointer user_data, GError **error)
{
    (void)ctx; (void)error;
    MfParseCtx *p = (MfParseCtx *)user_data;

    if (strcmp(element_name, "vertex") == 0) {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        for (int i = 0; attr_names[i]; ++i)
            if (strcmp(attr_names[i], "x") == 0) x = (float)atof(attr_values[i]);
            else if (strcmp(attr_names[i], "y") == 0) y = (float)atof(attr_values[i]);
            else if (strcmp(attr_names[i], "z") == 0) z = (float)atof(attr_values[i]);
        if (p->v_count == p->v_cap) {
            p->v_cap = p->v_cap ? p->v_cap * 2 : 1024;
            V3 *tmp = realloc(p->verts, p->v_cap * sizeof(V3));
            if (!tmp) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
            p->verts = tmp;
        }
        p->verts[p->v_count++] = (V3){x, y, z};
    }
    else if (strcmp(element_name, "mesh") == 0 ||
             strcmp(element_name, "vertices") == 0) {
        /* each <mesh> has its own local vertex index space: a triangle's
         * v1/v2/v3 are relative to the first vertex of THIS mesh, not the
         * first vertex of the file. Remember where this mesh's vertices start. */
        p->mesh_base = p->v_count;
    }
    else if (strcmp(element_name, "triangle") == 0) {
        int v1 = 0, v2 = 0, v3 = 0, mat = -1;
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "v1") == 0) v1 = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "v2") == 0) v2 = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "v3") == 0) v3 = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "materialindex") == 0) mat = atoi(attr_values[i]);
        }
        v1 += (int)p->mesh_base;
        v2 += (int)p->mesh_base;
        v3 += (int)p->mesh_base;
        if (v1 >= 0 && v1 < (int)p->v_count &&
            v2 >= 0 && v2 < (int)p->v_count &&
            v3 >= 0 && v3 < (int)p->v_count) {
            float v[9];
            v[0] = p->verts[v1].x; v[1] = p->verts[v1].y; v[2] = p->verts[v1].z;
            v[3] = p->verts[v2].x; v[4] = p->verts[v2].y; v[5] = p->verts[v2].z;
            v[6] = p->verts[v3].x; v[7] = p->verts[v3].y; v[8] = p->verts[v3].z;
            mesh_push_col(p->mesh, v, mf_resolve_color(p, mat));
        }
    }
    else if (strcmp(element_name, "base") == 0) {
        /* <base name="..." color="RRGGBB"/> inside <basematerials> */
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "color") != 0) continue;
            if (p->n_base == p->cap_base) {
                p->cap_base = p->cap_base ? p->cap_base * 2 : 16;
                uint32_t *tmp = realloc(p->base_colors, p->cap_base * sizeof(uint32_t));
                if (!tmp) {
                    fprintf(stderr, "out of memory\n");
                    exit(1);
                }
                p->base_colors = tmp;
            }
            p->base_colors[p->n_base++] = (uint32_t)strtoul(attr_values[i], NULL, 16);
            break;
        }
    }
    else if (strcmp(element_name, "objecttype") == 0) {
        int id = -1;
        for (int i = 0; attr_names[i]; ++i)
            if (strcmp(attr_names[i], "id") == 0) id = atoi(attr_values[i]);
        p->objtype = id;
    }
    else if (strcmp(element_name, "map") == 0) {
        int material = -1, pin = -1;
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "material") == 0) material = atoi(attr_values[i]);
            else if (strcmp(attr_names[i], "pin") == 0) pin = atoi(attr_values[i]);
        }
        if (p->n_maps == p->cap_maps) {
            p->cap_maps = p->cap_maps ? p->cap_maps * 2 : 16;
            MfMap *tmp = realloc(p->maps, p->cap_maps * sizeof(MfMap));
            if (!tmp) {
                fprintf(stderr, "out of memory\n");
                exit(1);
            }
            p->maps = tmp;
        }
        p->maps[p->n_maps++] = (MfMap){p->objtype, material, pin};
    }
    else if (strcmp(element_name, "object") == 0) {
        int type = -1;
        for (int i = 0; attr_names[i]; ++i) {
            if (strcmp(attr_names[i], "type") != 0) continue;
            /* type="model" means "no objecttype" -> direct basematerials index */
            type = (g_ascii_strcasecmp(attr_values[i], "model") == 0) ? -1
                                                                     : atoi(attr_values[i]);
            break;
        }
        p->cur_objtype = type;
    }
}

static void on_end_element(GMarkupParseContext *ctx, const char *element_name,
                            gpointer user_data, GError **error)
{
    (void)ctx; (void)element_name; (void)user_data; (void)error;
}

static void on_text(GMarkupParseContext *ctx, const char *text, gsize text_len,
                     gpointer user_data, GError **error)
{
    (void)ctx; (void)text; (void)text_len; (void)user_data; (void)error;
}

static int read_3mf_xml(const char *xml, gsize xml_len, Mesh *m)
{
    MfParseCtx p = {0};
    p.mesh = m;
    p.cur_objtype = -1;
    p.objtype = -1;

    const GMarkupParser parser = {
        .start_element = on_start_element,
        .end_element   = on_end_element,
        .text          = on_text,
        .passthrough   = NULL,
        .error         = NULL
    };

    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, &p, NULL);
    if (!ctx) {
        fprintf(stderr, "3MF: failed to create parser\n");
        return 0;
    }

    GError *err = NULL;
    g_markup_parse_context_parse(ctx, xml, xml_len, &err);
    if (err) {
        g_warning("3MF parse error: %s", err->message);
        g_error_free(err);
    }

    g_markup_parse_context_free(ctx);
    free(p.verts);
    free(p.base_colors);
    free(p.maps);
    return m->count > 0;
}

static int load_3mf(const char *path, Mesh *m)
{
    char *quoted = g_shell_quote(path);
    char *cmd = g_strdup_printf("unzip -p %s 3D/3dmodel.model", quoted);
    g_free(quoted);

    GError *err = NULL;
    char *xml = NULL;
    int status = 0;
    g_spawn_command_line_sync(cmd, &xml, NULL, &status, &err);
    g_free(cmd);
    if (!xml || err) {
        if (err)
            g_warning("load_3mf: unzip failed: %s", err->message);
        else
            g_warning("load_3mf: unzip failed (unzip not installed?)");
        g_free(xml);
        if (err) g_error_free(err);
        return 0;
    }
    gsize xml_len = strlen(xml);

    int ok = read_3mf_xml(xml, xml_len, m);
    g_free(xml);
    return ok;
}


static int load_mesh(const char *path, Mesh *m)
{
    char *lower = g_ascii_strdown(path, -1);
    int ok = 0;
    if (g_str_has_suffix(lower, ".3mf")) {
        ok = load_3mf(path, m);
    } else if (g_str_has_suffix(lower, ".stl")) {
        ok = load_stl(path, m);
    }
    g_free(lower);
    return ok;
}

static V3 rotate_view(V3 p, int view_mode)
{
    const float pi = 3.14159265358979323846f;
    float angle = 0.0f;
    switch (view_mode) {
        case VIEW_45:      angle = pi / 4.0f;       break; /* 45° */
        case VIEW_MINUS_45: angle = -pi / 4.0f;      break; /* -45° */
        case VIEW_FRONT: angle = 0.0f;            break; /* 0° */
        case VIEW_BACK:  angle = pi;              break; /* 180° */
        case VIEW_LEFT:  angle = -pi / 2.0f;      break; /* -90° */
        case VIEW_RIGHT: angle = pi / 2.0f;       break; /* 90° */
        default:         angle = pi / 4.0f;       break;
    }
    float cy = cosf(angle), sy = sinf(angle);
    float x = p.x * cy + p.z * sy;
    float z = -p.x * sy + p.z * cy;
    return (V3){x, p.y, z};
}

static void render_mesh(const Mesh *mesh, cairo_surface_t *surface, int view_mode,
                        float *bbox_out, float *center_out, float *ext_out)
{
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    const int w = CELL, h = CELL;

    /* bounding box + center */
    float mnx = INFINITY, mny = INFINITY, mnz = INFINITY;
    float mxx = -INFINITY, mxy = -INFINITY, mxz = -INFINITY;
    for (size_t i = 0; i < mesh->count; ++i)
        for (int k = 0; k < 9; k += 3) {
            float x = mesh->tris[i].v[k], y = mesh->tris[i].v[k + 1],
                  z = mesh->tris[i].v[k + 2];
            if (x < mnx) mnx = x;
            if (y < mny) mny = y;
            if (z < mnz) mnz = z;
            if (x > mxx) mxx = x;
            if (y > mxy) mxy = y;
            if (z > mxz) mxz = z;
        }
    if (mesh->count == 0)
        return;
    V3 c = {(mnx + mxx) / 2, (mny + mxy) / 2, (mnz + mxz) / 2};
    float ext = fmaxf(mxx - mnx, fmaxf(mxy - mny, mxz - mnz));
    if (ext <= 0.0f)
        ext = 1.0f;
    float s = (w - 2 * MARGIN) / ext;

    /* Store bbox info if requested */
    if (bbox_out) {
        bbox_out[0] = mnx; bbox_out[1] = mny; bbox_out[2] = mnz;
        bbox_out[3] = mxx; bbox_out[4] = mxy; bbox_out[5] = mxz;
    }
    if (center_out) {
        center_out[0] = c.x; center_out[1] = c.y; center_out[2] = c.z;
    }
    if (ext_out)
        *ext_out = ext;

    /* background: Sleek Deep Obsidian Blue (cairo ARGB32 native order: B,G,R,A) */
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char *px = data + y * stride + x * 4;
            px[0] = 0x1A; /* B */
            px[1] = 0x14; /* G */
            px[2] = 0x0E; /* R */
            px[3] = 0xFF; /* A */
        }

    /* subtle slate-blue grid to help orient the viewer */
    for (int g = 40; g < w; g += 40) {
        for (int x = 0; x < w; ++x) {
            unsigned char *px = data + g * stride + x * 4;
            px[0] = (unsigned char)fmin(px[0] + 0x0B, 255); /* brighten B */
            px[1] = (unsigned char)fmin(px[1] + 0x08, 255); /* brighten G */
            px[2] = (unsigned char)fmin(px[2] + 0x05, 255); /* brighten R */
        }
        for (int y = 0; y < h; ++y) {
            unsigned char *px = data + y * stride + g * 4;
            px[0] = (unsigned char)fmin(px[0] + 0x0B, 255); /* brighten B */
            px[1] = (unsigned char)fmin(px[1] + 0x08, 255); /* brighten G */
            px[2] = (unsigned char)fmin(px[2] + 0x05, 255); /* brighten R */
        }
    }

    /* z-buffer: starts at +∞, front-to-back (smaller z wins)
     * Also track depth for shadow (ambient occlusion) */
    float *depth = calloc((size_t)w * h, sizeof(float));
    float *ao     = calloc((size_t)w * h, sizeof(float));  /* ambient occlusion */
    if (!depth || !ao)
        return;
    memset(depth, 0x7f, (size_t)w * h * sizeof(float));
    memset(ao, 0, (size_t)w * h * sizeof(float));

    /* light direction in view space (normalized) */
    V3 light = {-0.45f, 0.65f, 0.61f};
    float ll = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
    if (ll > 0) { light.x /= ll; light.y /= ll; light.z /= ll; }

    /* Halfway vector for Blinn-Phong specular highlights. View vector is (0,0,1) */
    V3 H = {light.x, light.y, light.z + 1.0f};
    float hl = sqrtf(H.x * H.x + H.y * H.y + H.z * H.z);
    if (hl > 0) { H.x /= hl; H.y /= hl; H.z /= hl; }

    /* cap triangles for thumbnail rendering */
    const size_t MAX_TRIS = 20000;
    size_t step = (mesh->count > MAX_TRIS)
                  ? (mesh->count + MAX_TRIS - 1) / MAX_TRIS : 1;

    for (size_t t = 0; t < mesh->count; t += step) {
        /* yield to main loop every 100 triangles so the UI stays responsive */
        if ((t & 0x63) == 0 && mesh->count > 512)
            gtk_main_iteration_do(FALSE);
        V3 p0 = rotate_view((V3){mesh->tris[t].v[0] - c.x,
                                 mesh->tris[t].v[1] - c.y,
                                 mesh->tris[t].v[2] - c.z}, view_mode);
        V3 p1 = rotate_view((V3){mesh->tris[t].v[3] - c.x,
                                 mesh->tris[t].v[4] - c.y,
                                 mesh->tris[t].v[5] - c.z}, view_mode);
        V3 p2 = rotate_view((V3){mesh->tris[t].v[6] - c.x,
                                 mesh->tris[t].v[7] - c.y,
                                 mesh->tris[t].v[8] - c.z}, view_mode);

        /* flat normal (view space) */
        V3 e1 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        V3 e2 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        V3 n = {e1.y * e2.z - e1.z * e2.y,
                e1.z * e2.x - e1.x * e2.z,
                e1.x * e2.y - e1.y * e2.x};
        float nl = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
        if (nl > 0) {
            n.x /= nl;
            n.y /= nl;
            n.z /= nl;
        }
        
        /* Blinn-Phong specular & diffuse lighting */
        float diff = fabsf(n.x * light.x + n.y * light.y + n.z * light.z);
        float spec_dot = fabsf(n.x * H.x + n.y * H.y + n.z * H.z);
        float spec = powf(spec_dot, 28.0f) * 0.45f;
        float inten = 0.35f + 0.65f * diff;
        
        /* Face color: per-triangle 3MF material color, else titanium blue-steel */
        uint32_t fc = mesh->tris[t].col;
        int dr, dg, db;
        if (fc != NO_COLOR) {
            dr = (fc >> 16) & 0xFF;
            dg = (fc >> 8) & 0xFF;
            db = fc & 0xFF;
        } else {
            dr = 0x9D; dg = 0xB5; db = 0xC8; /* titanium blue-steel */
        }
        int base_r = (int)(dr * inten + 0xFF * spec);
        int base_g = (int)(dg * inten + 0xFF * spec);
        int base_b = (int)(db * inten + 0xFF * spec);
        if (base_r > 255) base_r = 255;
        if (base_g > 255) base_g = 255;
        if (base_b > 255) base_b = 255;

        /* screen coords */
        float x0 = p0.x * s + w / 2, y0 = h / 2 - p0.y * s, z0 = p0.z;
        float x1 = p1.x * s + w / 2, y1 = h / 2 - p1.y * s, z1 = p1.z;
        float x2 = p2.x * s + w / 2, y2 = h / 2 - p2.y * s, z2 = p2.z;

        int minx = (int)floorf(fminf(x0, fminf(x1, x2)));
        int maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
        int miny = (int)floorf(fminf(y0, fminf(y1, y2)));
        int maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
        if (minx < 0) minx = 0;
        if (miny < 0) miny = 0;
        if (maxx >= w) maxx = w - 1;
        if (maxy >= h) maxy = h - 1;

        float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (fabsf(area) < 1e-8f)
            continue; /* degenerate */

        for (int py = miny; py <= maxy; ++py) {
            for (int px_ = minx; px_ <= maxx; ++px_) {
                float fx = px_ + 0.5f, fy = py + 0.5f;
                float u = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area;
                float v = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area;
                float wt = 1.0f - u - v;
                int inside = (u >= 0 && v >= 0 && wt >= 0) ||
                             (u <= 0 && v <= 0 && wt <= 0);
                if (!inside)
                    continue;
                float d = u * z0 + v * z1 + wt * z2;
                size_t idx = (size_t)py * w + px_;
                if (d < depth[idx]) {
                    depth[idx] = d;
                    /* accumulate AO: further from camera = darker */
                    float ao_val = 1.0f - 0.2f * (1.0f - fminf(d / ext, 1.0f));
                    ao[idx] = fmaxf(ao[idx], ao_val);
                    /* apply AO to color */
                    int fr = (int)(base_r * ao_val);
                    int fg = (int)(base_g * ao_val);
                    int fb = (int)(base_b * ao_val);
                    unsigned char *px = data + py * stride + px_ * 4;
                    px[0] = (unsigned char)(fb > 255 ? 255 : fb);
                    px[1] = (unsigned char)(fg > 255 ? 255 : fg);
                    px[2] = (unsigned char)(fr > 255 ? 255 : fr);
                    px[3] = 0xFF;
                }
            }
        }
        /* yield to main loop every 100 triangles so the UI stays responsive */
        if ((t & 0x63) == 0 && mesh->count > 512)
            gtk_main_iteration_do(FALSE);
    }

    /* CAD-style outline edge detection pass */
    float crease_thresh = 0.015f * ext;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t idx = (size_t)y * w + x;
            float d = depth[idx];
            if (d > 1e8f) /* background */
                continue;
                
            int is_edge = 0;
            size_t neighbors[4] = {
                (size_t)y * w + (x + 1),
                (size_t)y * w + (x - 1),
                (size_t)(y + 1) * w + x,
                (size_t)(y - 1) * w + x
            };
            for (int k = 0; k < 4; ++k) {
                float nd = depth[neighbors[k]];
                if (nd > 1e8f) {
                    /* Silhouette: neighbor is background */
                    is_edge = 1;
                    break;
                }
                if (fabsf(d - nd) > crease_thresh) {
                    /* Crease: sharp depth change */
                    is_edge = 1;
                    break;
                }
            }
            if (is_edge) {
                unsigned char *px = data + y * stride + x * 4;
                /* Sleek dark charcoal border */
                px[0] = 0x12; /* B */
                px[1] = 0x10; /* G */
                px[2] = 0x0A; /* R */
            }
        }
    }

    free(depth);
    free(ao);
}

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */

typedef struct Item Item;
typedef struct UI UI;

static char *format_file_size(int bytes);
static char *format_file_date(time_t mtime);
static const char *file_type_label(int type);
static void start_move(UI *ui, Item *it);
static void cancel_move(UI *ui);
static void on_move_row_activated(GtkTreeView *tv, GtkTreePath *path,
                                   GtkTreeViewColumn *col, gpointer data);
static void on_move_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                                  GtkTreePath *path, gpointer data);
static void on_move_selection_changed(GtkTreeSelection *sel, gpointer data);
static void on_move_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cr,
                               GtkTreeModel *model, GtkTreeIter *iter, gpointer data);
static void on_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cr,
                          GtkTreeModel *model, GtkTreeIter *iter, UI *data);
static GtkWidget *build_grid(UI *ui);

/* ------------------------------------------------------------------ */
/* Item and UI structs (must be before helper functions)               */
/* ------------------------------------------------------------------ */

typedef struct Item {
    char *name;
    char *path;     /* full path, for tree lookup on click */
    cairo_surface_t *thumb;
    float bbox[6];  /* min x,y,z and max x,y,z */
    float center[3]; /* centroid */
    float ext;      /* max axis extent */
    int tri_count;  /* number of triangles */
    int vert_count; /* number of vertices */
    int file_size;  /* file size in bytes */
    time_t mtime;   /* last modification time */
    int file_type;  /* 0=unknown, 1=binary STL, 2=ascii STL, 3=3MF */
    int selected;   /* whether this item is currently selected in the grid */
} Item;

struct UI {
    GtkWidget *window;
    GtkWidget *tree;
    GtkTreeStore *store;
    GtkWidget *grid_scroll;
    GtkWidget *grid;
    GtkWidget *path_label; /* current root path, shown above the tree */
    GPtrArray *items; /* current items shown in the grid */
    ViewMode view_mode;
    SortMode sort_mode;
    int grid_cols;    /* columns in the currently built grid */
    int rendering;      /* re-entrancy guard: a render is in flight */
    int view_dirty;   /* view changed during a render; re-render after */
    char *pending_path; /* selection changed during a render; re-render after */
    GtkWidget *status_bar; /* bottom info bar for selected file */
    /* move-to-directory state */
    char *move_path;      /* full path of the file being moved */
    char *move_name;      /* basename of the file being moved */
    char *move_dest_path; /* currently selected destination dir in picker */
    GtkTreeStore *move_store;
    GtkWidget *move_tree;
    GtkWidget *move_cancel_btn;
    GtkWidget *move_confirm_btn;
};
typedef struct UI UI;


static void update_status_bar(UI *ui, Item *it);

enum { COL_NAME, COL_PATH, COL_ISDIR, COL_LOADED, N_COLS };
/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static char *format_file_size(int bytes)
{
    if (bytes < 1024)
        return g_strdup_printf("%d B", bytes);
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
        case 1: return "binary STL";
        case 2: return "ASCII STL";
        case 3: return "3MF";
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
                   it->bbox[3] - it->bbox[0],
                   it->bbox[4] - it->bbox[1],
                   it->bbox[5] - it->bbox[2],
                   fsize, fdate, file_type_label(it->file_type));
        g_free(fsize);
        g_free(fdate);
    } else {
        char *fsize = format_file_size(it->file_size);
        char *fdate = format_file_date(it->mtime);
        g_snprintf(buf, sizeof(buf),
                   "%s  ·  %s  ·  %s",
                   it->name, fsize, fdate);
        g_free(fsize);
        g_free(fdate);
    }
    gtk_label_set_text(GTK_LABEL(ui->status_bar), buf);
}

/* ------------------------------------------------------------------ */
/* App                                                                 */
/* ------------------------------------------------------------------ */

static void print_usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [directory] [-o out.png]\n"
            "  Shows a directory tree (left) and a static grid of all\n"
            "  .stl and .3mf files in the selected folder (right). No zoom,\n"
            "  no rotation. With -o, renders the grid to a PNG.\n"
            "  Without a directory, reopens the last used one (or $HOME).\n",
            argv0);
}

/* Put `w` into the right panel, replacing whatever is there.
 * The scrolled window auto-wraps non-scrollable children in a viewport,
 * so we must destroy the viewport (the direct child), not just our widget. */
static void set_panel_content(UI *ui, GtkWidget *w)
{
    GtkWidget *old = gtk_bin_get_child(GTK_BIN(ui->grid_scroll));
    if (old)
        gtk_widget_destroy(old);
    gtk_container_add(GTK_CONTAINER(ui->grid_scroll), w);
    ui->grid = w;
    gtk_widget_show_all(w);
}

/* Show a spinner + status label in the right panel while loading. */
static GtkWidget *show_loading(UI *ui, const char *text)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *spin = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spin));
    GtkWidget *lbl = gtk_label_new(text);
    gtk_box_pack_start(GTK_BOX(box), spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    set_panel_content(ui, box);
    return lbl;
}

static void free_items(GPtrArray *items)
{
    for (guint i = 0; i < items->len; ++i) {
        Item *it = items->pdata[i];
        cairo_surface_destroy(it->thumb);
        g_free(it->name);
        g_free(it->path);
        g_free(it);
    }
    g_ptr_array_set_size(items, 0);
}

/* ------------------------------------------------------------------ */
/* Thumbnail cache                                                     */
/* ------------------------------------------------------------------ */

static char *cache_dir_path(void)
{
    return g_build_filename(g_get_user_cache_dir(), "sliceview", "thumbs", NULL);
}

/* Bump when the renderer/parser changes so stale cached thumbnails are
 * discarded (the key does not include file contents or app version). */
#define CACHE_VERSION 2

/* Build a cache filename from (path, view_mode, CELL, CACHE_VERSION). The
 * key is a hex-encoded SHA256 of the concatenated string so it is path-safe. */
static void cache_key(const char *file_path, int view_mode, char *out, size_t out_sz)
{
    /* Use g_compute_checksum for a portable SHA256 */
    char buf[256];
    int n = g_snprintf(buf, sizeof(buf), "%s:%d:%d:v%d", file_path, CELL, view_mode, CACHE_VERSION);
    gchar *cs = g_compute_checksum_for_string(G_CHECKSUM_SHA256, buf, n);
    g_strlcpy(out, cs, out_sz);
    g_free(cs);
}

static char *cache_file_path(const char *file_path, int view_mode)
{
    char key[65];
    cache_key(file_path, view_mode, key, sizeof(key));
    return g_build_filename(cache_dir_path(), key, "png", NULL);
}

/* Try to load a cached thumbnail. Returns a new cairo_surface_t or NULL. */
static cairo_surface_t *load_cached_thumb(const char *file_path, int view_mode)
{
    char *path = cache_file_path(file_path, view_mode);
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    g_free(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    return s;
}

/* Save a thumbnail to the cache. */
static void save_cached_thumb(const char *file_path, int view_mode,
                              cairo_surface_t *thumb)
{
    char *dir = cache_dir_path();
    char *file = cache_file_path(file_path, view_mode);
    if (g_mkdir_with_parents(dir, 493 /* 0755 */) == 0) {
        cairo_surface_write_to_png(thumb, file);
    }
    g_free(file);
    g_free(dir);
}

/* Load every STL/3MF in `paths` (full paths) and render thumbnails.
 * If ui != NULL a spinner + status label is shown in the right panel.
 * Uses the thumbnail cache to skip rendering for previously-seen files.
 * Stores mesh metadata (bbox, center, extents) for the info bar. */
static GPtrArray *render_paths(GPtrArray *paths, UI *ui)
{
    GPtrArray *items = g_ptr_array_new();
    GtkWidget *lbl = ui ? show_loading(ui, "Loading…") : NULL;
    int view_mode = ui ? ui->view_mode : 0;
    for (guint i = 0; i < paths->len; ++i) {
        if (ui) {
            char text[300];
            g_snprintf(text, sizeof(text), "Loading %s (%u/%u)…",
                       g_path_get_basename((char *)paths->pdata[i]), i + 1,
                       paths->len);
            gtk_label_set_text(GTK_LABEL(lbl), text);
            gtk_main_iteration_do(FALSE);
        }
        const char *fpath = (const char *)paths->pdata[i];
        cairo_surface_t *thumb = NULL;
        int file_type = 0;
        float bbox[6] = {0}, center[3] = {0}, ext = 0;
        int tri_count = 0, vert_count = 0;
        struct stat st;
        int file_size = 0;
        time_t mtime = 0;

        /* Stat the file (always available) */
        if (stat(fpath, &st) == 0) {
            file_size = (int)st.st_size;
            mtime = st.st_mtime;
        }

        /* Try cache first */
        thumb = load_cached_thumb(fpath, view_mode);
        if (!thumb) {
            Mesh mesh = {0};
            int ok = load_mesh(fpath, &mesh);
            if (ok) {
                /* Determine file type from the extension */
                char *lower = g_ascii_strdown(fpath, -1);
                if (g_str_has_suffix(lower, ".3mf")) {
                    file_type = 3;
                } else if (g_str_has_suffix(lower, ".stla")) {
                    file_type = 2; /* ASCII STL */
                } else if (g_str_has_suffix(lower, ".stl")) {
                    file_type = 1; /* binary STL (default) */
                }
                g_free(lower);

                tri_count = (int)mesh.count;
                vert_count = (int)(mesh.count * 3);

                thumb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, CELL, CELL);
                render_mesh(&mesh, thumb, view_mode, bbox, center, &ext);
                cairo_surface_mark_dirty(thumb);
                save_cached_thumb(fpath, view_mode, thumb);
                free(mesh.tris);
            }
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
        } else {
            g_warning("skipping %s: not a valid STL or 3MF file",
                      fpath);
        }
    }
    return items;
}

typedef struct {
    const char *target_path;
    GtkTreePath *found_path;
    GtkTreeIter found_iter;
    gboolean found;
} FindPathData;

static gboolean find_path_callback(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
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
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
        gtk_tree_selection_select_path(sel, fd.found_path);
        gtk_tree_path_free(fd.found_path);
    }
}

/* Forward declarations for parent button click and delete handling */
static void on_parent_clicked(GtkButton *b, UI *ui);
static void render_current_selection(UI *ui);
static void after_render(UI *ui);

static void confirm_and_delete_file(UI *ui, Item *it)
{
    GtkWidget *dialog = gtk_message_dialog_new(
        GTK_WINDOW(ui->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        "Are you sure you want to delete the file:\n%s?",
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
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_OK,
                "Failed to delete file:\n%s",
                it->path);
            gtk_window_set_title(GTK_WINDOW(err_dialog), "Error");
            gtk_dialog_run(GTK_DIALOG(err_dialog));
            gtk_widget_destroy(err_dialog);
            return;
        }

        gboolean showing_single_deleted = FALSE;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
        GtkTreeIter iter;
        GtkTreeModel *model;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            char *cur_path;
            int is_dir;
            gtk_tree_model_get(model, &iter, COL_PATH, &cur_path, COL_ISDIR, &is_dir, -1);
            if (!is_dir && g_strcmp0(cur_path, it->path) == 0) {
                showing_single_deleted = TRUE;
            }
            g_free(cur_path);
        }

        /* Find in tree store */
        FindPathData fd = {it->path, NULL, {0}, FALSE};
        gtk_tree_model_foreach(GTK_TREE_MODEL(ui->store), find_path_callback, &fd);
        
        if (fd.found) {
            if (showing_single_deleted) {
                /* Since the single deleted file was showing, select its parent in the tree */
                GtkTreeIter parent_iter;
                if (gtk_tree_model_iter_parent(model, &parent_iter, &fd.found_iter)) {
                    GtkTreePath *parent_path = gtk_tree_model_get_path(model, &parent_iter);
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
            /* If we are showing the folder, re-render the folder */
            ui->rendering = 1;
            render_current_selection(ui);
            after_render(ui);
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

/* ------------------------------------------------------------------ */
/* Move-to-directory helpers                                            */
/* ------------------------------------------------------------------ */

/* Forward declarations for g_strcmp0_adapter to avoid ordering issues. */
static int g_strcmp0_adapter(const void *a, const void *b, gpointer ud);

/* Populate `parent` in the move-dialog tree with subdirectories of `path`.
 * Only directories are shown — files are not listed. */
static void populate_move_tree(GtkTreeIter *parent, const char *path,
                                GtkTreeStore *store)
{
    GDir *gd = g_dir_open(path, 0, NULL);
    if (!gd)
        return;

    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    const char *e;
    while ((e = g_dir_read_name(gd))) {
        if (e[0] == '.')
            continue; /* skip hidden directories */
        char *full = g_build_filename(path, e, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR))
            g_ptr_array_add(dirs, g_strdup(e));
        g_free(full);
    }
    g_dir_close(gd);
    g_sort_array(dirs->pdata, dirs->len, sizeof(gpointer),
                 (GCompareDataFunc)g_strcmp0_adapter, NULL);

    /* Collect existing placeholder children so we can remove them AFTER
     * adding new children. Removing all children first causes GTK to
     * immediately collapse the row. */
    GtkTreeIter child;
    GList *to_remove = NULL;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &child, parent)) {
        do {
            to_remove = g_list_prepend(to_remove, gtk_tree_iter_copy(&child));
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &child));
    }

    for (guint i = 0; i < dirs->len; ++i) {
        char *name = dirs->pdata[i];
        GtkTreeIter it;
        gtk_tree_store_append(store, &it, parent);
        char *p = g_build_filename(path, name, NULL);
        gtk_tree_store_set(store, &it, COL_NAME, name, COL_PATH, p,
                           COL_ISDIR, 1, COL_LOADED, 0, -1);
        /* placeholder child so the expander arrow appears */
        GtkTreeIter ph;
        gtk_tree_store_append(store, &ph, &it);
        gtk_tree_store_set(store, &ph, COL_NAME, "…", COL_PATH, "",
                           COL_ISDIR, 0, COL_LOADED, 0, -1);
        g_free(p);
    }

    for (GList *l = to_remove; l; l = l->next) {
        GtkTreeIter *it_rem = (GtkTreeIter *)l->data;
        gtk_tree_store_remove(store, it_rem);
        gtk_tree_iter_free(it_rem);
    }
    g_list_free(to_remove);

    gtk_tree_store_set(store, parent, COL_LOADED, 1, -1);
    g_ptr_array_free(dirs, TRUE);
}

/* Expand a row in the move-dialog tree (lazy-load). */
static void on_move_row_expanded(GtkTreeView *tv, GtkTreeIter *iter,
                                  GtkTreePath *path, gpointer data)
{
    (void)tv; (void)path;
    UI *ui = (UI *)data;
    int is_dir = 0, loaded = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->move_store), iter,
                       COL_ISDIR, &is_dir, COL_LOADED, &loaded, -1);
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
                               GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)data; (void)col;
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
    (void)tv; (void)col;
    UI *ui = (UI *)data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(ui->move_store), &iter, path))
        return;
    int is_dir = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(ui->move_store), &iter, COL_ISDIR, &is_dir, -1);
    if (is_dir) {
        char *dest = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(ui->move_store), &iter, COL_PATH, &dest, -1);
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

    /* Check for name collision */
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
        /* Remove existing file */
        if (remove(new_path) != 0) {
            GtkWidget *err = gtk_message_dialog_new(
                GTK_WINDOW(ui->window),
                GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Cannot overwrite '%s': %s",
                new_path, g_strerror(errno));
            gtk_dialog_run(GTK_DIALOG(err));
            gtk_widget_destroy(err);
            g_free(new_path);
            return;
        }
    }

    /* Move the file */
    GError *err = NULL;
    gboolean ok = g_file_move(g_file_new_for_path(ui->move_path),
                               g_file_new_for_path(new_path),
                               0, NULL, NULL, NULL, &err);
    if (!ok) {
        GtkWidget *err_dlg = gtk_message_dialog_new(
            GTK_WINDOW(ui->window),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "Failed to move file:\n%s\n%s",
            ui->move_path, err->message);
        gtk_dialog_run(GTK_DIALOG(err_dlg));
        gtk_widget_destroy(err_dlg);
        g_error_free(err);
        g_free(new_path);
        return;
    }
    g_free(new_path);

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
            char *moved_file_path = g_build_filename(ui->move_dest_path, ui->move_name, NULL);
            GtkTreeIter child;
            gtk_tree_store_append(ui->store, &child, &fd2.found_iter);
            gtk_tree_store_set(ui->store, &child,
                               COL_NAME, ui->move_name,
                               COL_PATH, moved_file_path,
                               COL_ISDIR, 0, COL_LOADED, 0, -1);
            g_free(moved_file_path);
        }
        gtk_tree_path_free(fd2.found_path);
    }

    /* If the moved file is currently shown in the grid, re-render */
    gboolean showing_single = FALSE;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        char *cur_path;
        int is_dir;
        gtk_tree_model_get(model, &iter, COL_PATH, &cur_path, COL_ISDIR, &is_dir, -1);
        if (!is_dir && g_strcmp0(cur_path, ui->move_path) == 0)
            showing_single = TRUE;
        g_free(cur_path);
    }

    if (showing_single) {
        /* Re-render the destination folder */
        ui->rendering = 1;
        render_current_selection(ui);
        after_render(ui);
    }

    /* Update status bar */
    if (ui->items->len > 0)
        update_status_bar(ui, ui->items->pdata[0]);

    /* Clean up and restore the grid */
    cancel_move(ui);
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

    ui->move_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui->move_store));
    g_object_unref(ui->move_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ui->move_tree), FALSE);

    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, icon,
                                             (GtkTreeCellDataFunc)on_cell_data,
                                             ui, NULL);
    gtk_tree_view_column_set_cell_data_func(col, text,
                                             (GtkTreeCellDataFunc)on_move_cell_data,
                                             ui, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ui->move_tree), col);

    /* Populate root */
    char *root_path = NULL;
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ui->store), &iter)) {
        gtk_tree_model_get(GTK_TREE_MODEL(ui->store), &iter, COL_PATH, &root_path, -1);
    }
    if (root_path) {
        char *base = g_path_get_basename(root_path);
        const char *display_name = (*base == '\0') ? "/" : base;
        GtkTreeIter root_it;
        gtk_tree_store_append(ui->move_store, &root_it, NULL);
        gtk_tree_store_set(ui->move_store, &root_it,
                           COL_NAME, display_name,
                           COL_PATH, root_path,
                           COL_ISDIR, 1, COL_LOADED, 1, -1);
        /* Load its children */
        populate_move_tree(&root_it, root_path, ui->move_store);
        GtkTreePath *p = gtk_tree_path_new_from_indices(0, -1);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(ui->move_tree), p, FALSE);
        gtk_tree_path_free(p);
        g_free(base);
        g_free(root_path);
    }

    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->move_tree));
    g_signal_connect(sel, "changed",
                     G_CALLBACK(on_move_selection_changed), ui);
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
    if (!ui->move_path) return; /* not in move mode */

    /* Restore the grid */
    set_panel_content(ui, build_grid(ui));

    /* Free state */
    g_free(ui->move_path);
    ui->move_path = NULL;
    g_free(ui->move_name);
    ui->move_name = NULL;
    g_free(ui->move_dest_path);
    ui->move_dest_path = NULL;
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
    if (!it || !it->path) return;
    char *dir = g_path_get_dirname(it->path);
    char *cmd = g_strdup_printf("xdg-open \"%s\" &", dir);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    g_free(dir);
}

static void on_copy_path(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (!it || !it->path) return;
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, it->path, -1);
}

static void on_export_png(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (!it || !it->thumb) return;
    char *base = g_path_get_basename(it->path);
    char *name = g_strconcat(base, ".png", NULL);
    g_free(base);
    char *out = g_build_filename(g_get_home_dir(), name, NULL);
    g_free(name);
    cairo_surface_write_to_png(it->thumb, out);
    GtkWidget *dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
        "Exported to:\n%s", out);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(out);
}

static void on_open_default(GtkMenuItem *menuitem, gpointer data)
{
    (void)data;
    Item *it = g_object_get_data(G_OBJECT(menuitem), "item");
    if (!it || !it->path) return;
    char *cmd = g_strdup_printf("xdg-open \"%s\" &", it->path);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
}

/* Click on a preview thumbnail: select the corresponding file in the tree,
 * or right-click to show a context menu. */
static void on_selection_changed(GtkTreeSelection *sel, UI *ui);

static gboolean on_preview_clicked(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    Item *it = g_object_get_data(G_OBJECT(w), "item");
    if (!it || !it->path)
        return FALSE;
    UI *ui = (UI *)data;

    if (ev->button == 1) { /* Left click */
        /* Select the file in the tree (shows the highlight) without
         * re-rendering the grid. This avoids destroying the current
         * grid so other thumbnails remain visible. */
        it->selected = 1;
        char *path = g_strdup(it->path);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(
                GTK_TREE_VIEW(ui->tree));
        g_signal_handlers_block_by_func(sel,
                                        G_CALLBACK(on_selection_changed), ui);
        select_path_in_tree(ui, path);
        g_signal_handlers_unblock_by_func(sel,
                                          G_CALLBACK(on_selection_changed), ui);
        /* Update the status bar with info about the selected Item */
        update_status_bar(ui, it);
        g_free(path);
        return TRUE;
    } else if (ev->button == 3) { /* Right click */
        /* Select the file in the tree (shows the highlight) without re-rendering the grid.
         * This avoids destroying this event box, the GdkEventButton 'ev', and freeing 'it'. */
        char *path = g_strdup(it->path);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
        g_signal_handlers_block_by_func(sel, G_CALLBACK(on_selection_changed), ui);
        select_path_in_tree(ui, path);
        g_signal_handlers_unblock_by_func(sel, G_CALLBACK(on_selection_changed), ui);

        /* Update the status bar with info about the selected Item */
        update_status_bar(ui, it);

        GtkWidget *menu = gtk_menu_new();
        g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);

        /* Open containing folder */
        GtkWidget *open_folder = gtk_menu_item_new_with_label("Open Containing Folder");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_folder);
        g_object_set_data(G_OBJECT(open_folder), "ui", ui);
        g_object_set_data(G_OBJECT(open_folder), "item", it);
        g_signal_connect(open_folder, "activate",
                         G_CALLBACK(on_open_containing_folder), NULL);

        /* Copy path */
        GtkWidget *copy_path = gtk_menu_item_new_with_label("Copy Path");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), copy_path);
        g_object_set_data(G_OBJECT(copy_path), "ui", ui);
        g_object_set_data(G_OBJECT(copy_path), "item", it);
        g_signal_connect(copy_path, "activate",
                         G_CALLBACK(on_copy_path), NULL);

        /* Export PNG */
        GtkWidget *export_png = gtk_menu_item_new_with_label("Export PNG");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), export_png);
        g_object_set_data(G_OBJECT(export_png), "ui", ui);
        g_object_set_data(G_OBJECT(export_png), "item", it);
        g_signal_connect(export_png, "activate",
                         G_CALLBACK(on_export_png), NULL);

        /* Open with default app */
        GtkWidget *open_app = gtk_menu_item_new_with_label("Open with Default App");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_app);
        g_object_set_data(G_OBJECT(open_app), "ui", ui);
        g_object_set_data(G_OBJECT(open_app), "item", it);
        g_signal_connect(open_app, "activate",
                         G_CALLBACK(on_open_default), NULL);

        /* Separator */
        GtkWidget *sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

        /* Move to Directory */
        GtkWidget *move_item = gtk_menu_item_new_with_label("Move to Directory…");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), move_item);
        g_object_set_data(G_OBJECT(move_item), "ui", ui);
        g_object_set_data(G_OBJECT(move_item), "item", it);
        g_signal_connect(move_item, "activate",
                         G_CALLBACK(on_move_menu_activated), NULL);

        /* Delete */
        GtkWidget *delete_item = gtk_menu_item_new_with_label("Delete File");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), delete_item);
        g_object_set_data(G_OBJECT(delete_item), "ui", ui);
        g_object_set_data(G_OBJECT(delete_item), "item", it);
        g_signal_connect(delete_item, "activate",
                         G_CALLBACK(on_delete_menu_activated), NULL);

        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
        g_free(path);
        return TRUE;
    }
    return FALSE;
}

/* Forward declarations */
static void show_items(UI *ui, GPtrArray *items, const char *title);
static void render_current_selection(UI *ui);
static void after_render(UI *ui);
static void rerender_current_items(UI *ui);
static void save_last_dir(const char *path);
static void on_open_containing_folder(GtkMenuItem *menuitem, gpointer data);
static void on_copy_path(GtkMenuItem *menuitem, gpointer data);
static void on_export_png(GtkMenuItem *menuitem, gpointer data);
static void on_open_default(GtkMenuItem *menuitem, gpointer data);

/* Called after a render finishes: re-render the latest selection if the
 * user changed it while we were busy (it was deferred by the
 * re-entrancy guard). */
static void after_render(UI *ui)
{
    ui->rendering = 0;
    if (ui->view_dirty) {
        ui->view_dirty = 0;
        ui->rendering = 1;
        rerender_current_items(ui);
        ui->rendering = 0;
    }
    if (!ui->pending_path)
        return;
    char *p = ui->pending_path;
    ui->pending_path = NULL;
    select_path_in_tree(ui, p);
    /* select_path does not emit "changed" if the row is already selected;
     * render it directly in that case. */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(
            GTK_TREE_VIEW(ui->tree));
    GtkTreeIter iter;
    GtkTreeModel *model;
    int same = 0;
    if (gtk_tree_selection_get_selected(
            sel, &model, &iter)) {
        char *cur;
        gtk_tree_model_get(model, &iter, COL_PATH, &cur, -1);
        same = g_strcmp0(cur, p) == 0;
        g_free(cur);
    }
    g_free(p);
    if (same) {
        ui->rendering = 1;
        render_current_selection(ui);
        ui->rendering = 0;
    }
    /* Update status bar with selected file info */
    if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
        char *path;
        int is_dir;
        gtk_tree_model_get(model, &iter, COL_PATH, &path, COL_ISDIR, &is_dir, -1);
        if (!is_dir && path && *path) {
            /* Find the Item matching this path */
            for (guint i = 0; i < ui->items->len; ++i) {
                Item *it = ui->items->pdata[i];
                if (g_strcmp0(it->path, path) == 0) {
                    update_status_bar(ui, it);
                    break;
                }
            }
        }
        g_free(path);
    }
}

/* Re-render the currently shown thumbnails with ui->view_mode. */
static void rerender_current_items(UI *ui)
{
    GPtrArray *paths = g_ptr_array_new();
    for (guint i = 0; i < ui->items->len; ++i) {
        Item *it = ui->items->pdata[i];
        if (it->path)
            g_ptr_array_add(paths, g_strdup(it->path));
    }
    GPtrArray *items = render_paths(paths, ui);
    show_items(ui, items, gtk_window_get_title(GTK_WINDOW(ui->window)));
    for (guint i = 0; i < paths->len; ++i)
        g_free(paths->pdata[i]);
    g_ptr_array_free(paths, FALSE);
}

/* One of the view-angle toggle buttons was activated. */
static void on_view_toggled(GtkToggleButton *b, UI *ui)
{
    if (!gtk_toggle_button_get_active(b))
        return; /* group member being deselected */
    if (ui->rendering) {
        ui->view_dirty = 1; /* re-render with the new view once done */
        return;
    }
    ui->rendering = 1;
    ui->view_mode = (ViewMode)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "view_mode"));
    rerender_current_items(ui);
    after_render(ui);
}

/* Sort mode changed: re-render with new sort order. */
static void on_sort_changed(GtkComboBox *combo, UI *ui)
{
    int idx = gtk_combo_box_get_active(combo);
    if (idx < 0 || idx >= SORT_DATE)
        return;
    if (ui->rendering) {
        ui->view_dirty = 1;
        return;
    }
    ui->rendering = 1;
    ui->sort_mode = (SortMode)idx;
    rerender_current_items(ui);
    after_render(ui);
}


/* Forward declarations for g_strcmp0_adapter to avoid ordering issues. */
static int g_strcmp0_adapter(const void *a, const void *b, gpointer ud);

/* Sort comparison for full file paths, respecting a SortMode passed via data. */
static int sort_paths_compare(const void *a, const void *b, gpointer data)
{
    const char *pa = *(const char **)a;
    const char *pb = *(const char **)b;
    SortMode mode = GPOINTER_TO_INT(data);
    switch (mode) {
        case SORT_NAME:
            return g_strcmp0(pa, pb);
        case SORT_SIZE:
        case SORT_DATE: {
            struct stat sa, sb;
            if (stat(pa, &sa) != 0) return g_strcmp0(pa, pb);
            if (stat(pb, &sb) != 0) return g_strcmp0(pa, pb);
            if (mode == SORT_SIZE) {
                if (sa.st_size < sb.st_size) return -1;
                if (sa.st_size > sb.st_size) return 1;
                return g_strcmp0(pa, pb); /* tie-break by name */
            }
            /* SORT_DATE: newer first */
            if (sa.st_mtime < sb.st_mtime) return 1;
            if (sa.st_mtime > sb.st_mtime) return -1;
            return g_strcmp0(pa, pb); /* tie-break by name */
        }
        default:
            return g_strcmp0(pa, pb);
    }
}

/* Wrapper to cast g_strcmp0 to GCompareDataFunc. */
static int g_strcmp0_adapter(const void *a, const void *b, gpointer ud)
{
    (void)ud;
    return g_strcmp0(a, b);
}

/* Number of thumbnail columns that fit in `w` pixels of grid area
 * (CELL per column, 6px spacing, 6px margins on each side). */
static int grid_cols_for_width(int w)
{
    if (w <= 0)
        w = 1280; /* not allocated yet (initial render before show) */
    int cols = (w - 6) / (CELL + 6);
    return cols < 1 ? 1 : cols;
}

/* Build the thumbnail grid widget from ui->items, sized to the current
 * right-panel width. Thumbnails are reused, never re-rendered. */
static GtkWidget *build_grid(UI *ui)
{
    int w = gtk_widget_get_allocated_width(ui->grid_scroll);
    ui->grid_cols = grid_cols_for_width(w);

    GtkWidget *grid_w = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_w), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid_w), 6);
    gtk_widget_set_margin_top(grid_w, 6);
    gtk_widget_set_margin_bottom(grid_w, 6);
    gtk_widget_set_margin_start(grid_w, 6);
    gtk_widget_set_margin_end(grid_w, 6);

    for (guint i = 0; i < ui->items->len; ++i) {
        Item *it = ui->items->pdata[i];

        GtkWidget *eb = gtk_event_box_new();
        gtk_event_box_set_above_child(GTK_EVENT_BOX(eb), TRUE);
        gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK);
        g_object_set_data(G_OBJECT(eb), "item", it);

        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *img = gtk_image_new_from_surface(it->thumb);
        GtkWidget *label = gtk_label_new(it->name);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_size_request(label, CELL - 8, -1);
        gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(eb), box);

        /* Wrap selected items in a frame border */
        GtkWidget *child = eb;
        if (it->selected) {
            GtkWidget *frame = gtk_frame_new(NULL);
            gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
            gtk_widget_set_margin_start(frame, 2);
            gtk_widget_set_margin_end(frame, 2);
            gtk_widget_set_margin_top(frame, 2);
            gtk_widget_set_margin_bottom(frame, 2);
            gtk_container_add(GTK_CONTAINER(frame), eb);
            child = frame;
        }

        gtk_grid_attach(GTK_GRID(grid_w), child, (int)(i % ui->grid_cols),
                        (int)(i / ui->grid_cols), 1, 1);
        g_signal_connect(eb, "button-press-event",
                         G_CALLBACK(on_preview_clicked), ui);
    }
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
    int cols = grid_cols_for_width(gtk_widget_get_allocated_width(ui->grid_scroll));
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
    if (grid_cols_for_width(alloc->width) == ui->grid_cols)
        return;
    g_idle_add(relayout_grid_idle, ui);
}

/* Replace the grid contents with `items` and update the title. */
static void show_items(UI *ui, GPtrArray *items, const char *title)
{
    free_items(ui->items);
    for (guint i = 0; i < items->len; ++i)
        g_ptr_array_add(ui->items, items->pdata[i]);
    g_ptr_array_free(items, FALSE);

    set_panel_content(ui, build_grid(ui));
    gtk_window_set_title(GTK_WINDOW(ui->window), title);
}

/* ---------------- directory tree (lazy) ---------------- */

/* Populate `parent` with the subdirectories and .stl files of `path`. */
static void load_dir_into(GtkTreeIter *parent, const char *path, UI *ui)
{
    GDir *gd = g_dir_open(path, 0, NULL);
    if (!gd)
        return;
    GPtrArray *dirs = g_ptr_array_new();
    GPtrArray *files = g_ptr_array_new();
    const char *e;
    while ((e = g_dir_read_name(gd))) {
        char *full = g_build_filename(path, e, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            if (e[0] == '.') {
                g_free(full);
                continue; /* skip hidden directories */
            }
            g_ptr_array_add(dirs, g_strdup(e));
        }
        else {
            char *lower = g_ascii_strdown(e, -1);
            if (g_str_has_suffix(lower, ".stl") || g_str_has_suffix(lower, ".3mf"))
                g_ptr_array_add(files, g_strdup(e));
            g_free(lower);
        }
        g_free(full);
    }
    g_dir_close(gd);
    g_sort_array(dirs->pdata, dirs->len, sizeof(gpointer),
                 (GCompareDataFunc)g_strcmp0_adapter, NULL);
    g_sort_array(files->pdata, files->len, sizeof(gpointer),
                 (GCompareDataFunc)g_strcmp0_adapter, NULL);

    /* Collect existing placeholder children so we can remove them AFTER
     * adding new children. Removing all children first causes GTK to
     * immediately collapse the row. */
    GtkTreeIter child;
    GList *to_remove = NULL;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(ui->store), &child, parent)) {
        do {
            to_remove = g_list_prepend(to_remove, gtk_tree_iter_copy(&child));
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(ui->store), &child));
    }

    for (guint i = 0; i < dirs->len + files->len; ++i) {
        int is_dir = i < dirs->len;
        char *name = is_dir ? (char *)dirs->pdata[i]
                            : (char *)files->pdata[i - dirs->len];
        GtkTreeIter it;
        gtk_tree_store_append(ui->store, &it, parent);
        char *p = g_build_filename(path, name, NULL);
        gtk_tree_store_set(ui->store, &it, COL_NAME, name, COL_PATH, p,
                           COL_ISDIR, is_dir, COL_LOADED, 0, -1);
        if (is_dir) {
            /* placeholder child: makes the expander arrow appear; it is
             * replaced by the real entries when the folder is opened */
            GtkTreeIter ph;
            gtk_tree_store_append(ui->store, &ph, &it);
            gtk_tree_store_set(ui->store, &ph, COL_NAME, "…", COL_PATH, "",
                               COL_ISDIR, 0, COL_LOADED, 0, -1);
        }
        g_free(p);
        g_free(name);
    }

    for (GList *l = to_remove; l; l = l->next) {
        GtkTreeIter *it_rem = (GtkTreeIter *)l->data;
        gtk_tree_store_remove(ui->store, it_rem);
        gtk_tree_iter_free(it_rem);
    }
    g_list_free(to_remove);

    gtk_tree_store_set(ui->store, parent, COL_LOADED, 1, -1);
    g_ptr_array_free(dirs, TRUE);
    g_ptr_array_free(files, TRUE);
}

/* Icon per row. */
static void on_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *cr,
                         GtkTreeModel *model, GtkTreeIter *iter, UI *ui)
{
    (void)col; (void)ui;
    int is_dir;
    gtk_tree_model_get(model, iter, COL_ISDIR, &is_dir, -1);
    g_object_set(cr, "icon-name", is_dir ? "folder" : "image-x-generic", NULL);
}

/* Expanding a folder (e.g. via the triangle) loads it and shows its files */
static void on_row_expanded(GtkTreeView *tv, GtkTreeIter *iter, GtkTreePath *path,
                            UI *ui)
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

/* Render whatever is currently selected. Caller must have set
 * ui->rendering (re-entrancy guard). */
static void render_current_selection(UI *ui)
{
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
    GtkTreeIter iter;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    char *path, *name;
    int is_dir;
    gtk_tree_model_get(model, &iter, COL_PATH, &path, COL_NAME, &name,
                       COL_ISDIR, &is_dir, -1);
    if (*path == '\0') {
        g_free(path);
        g_free(name);
        return; /* placeholder row */
    }
    if (is_dir)
        save_last_dir(path);

    GPtrArray *paths = g_ptr_array_new();
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
        GDir *gd = g_dir_open(path, 0, NULL);
        if (gd) {
            const char *e;
            while ((e = g_dir_read_name(gd))) {
                char *lower = g_ascii_strdown(e, -1);
                if (g_str_has_suffix(lower, ".stl") || g_str_has_suffix(lower, ".3mf"))
                    g_ptr_array_add(paths, g_build_filename(path, e, NULL));
                g_free(lower);
            }
            g_dir_close(gd);
        }
        g_ptr_array_sort_with_data(paths, (GCompareDataFunc)sort_paths_compare,
                                   GINT_TO_POINTER(ui->sort_mode));
    } else {
        g_ptr_array_add(paths, g_strdup(path));
    }

    char *title;
    if (is_dir)
        title = g_strdup_printf("sliceview — %s (%d files)", path,
                                (int)paths->len);
    else
        title = g_strdup_printf("sliceview — %s", path);

    GPtrArray *items = render_paths(paths, ui);
    show_items(ui, items, title);

    for (guint i = 0; i < paths->len; ++i)
        g_free(paths->pdata[i]);
    g_ptr_array_free(paths, FALSE);
    g_free(title);
    g_free(path);
    g_free(name);
}

/* Selection changed: render the folder's .stl files (or the single file). */
static void on_selection_changed(GtkTreeSelection *sel, UI *ui)
{
    if (ui->rendering) {
        /* A render is in flight: it pumps the main loop (to keep the UI
         * responsive), so this handler can re-enter. Re-entering would
         * destroy the in-flight render's loading label (use-after-free /
         * heap corruption). Remember the newest selection and re-render it
         * once the in-flight render finishes. */
        GtkTreeModel *model;
        GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            char *p;
            gtk_tree_model_get(model, &iter, COL_PATH, &p, -1);
            g_free(ui->pending_path);
            ui->pending_path = p;
        }
        return;
    }
    ui->rendering = 1;
    render_current_selection(ui);
    after_render(ui);
}

/* Make `path` the new tree root (replaces the whole tree). */
static void set_root(UI *ui, const char *path)
{
    char *real = realpath(path, NULL);
    if (!real || !g_file_test(real, G_FILE_TEST_IS_DIR)) {
        g_free(real);
        return;
    }
    save_last_dir(real);
    const char *base = g_path_get_basename(real);
    if (*base == '\0')
        base = "/";
    gtk_tree_store_clear(ui->store);
    GtkTreeIter it;
    gtk_tree_store_append(ui->store, &it, NULL);
    gtk_tree_store_set(ui->store, &it, COL_NAME, base, COL_PATH, real,
                       COL_ISDIR, 1, -1);
    /* placeholder child so the root shows an expander arrow */
    GtkTreeIter ph;
    gtk_tree_store_append(ui->store, &ph, &it);
    gtk_tree_store_set(ui->store, &ph, COL_NAME, "…", COL_PATH, "",
                       COL_ISDIR, 0, -1);
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
    return g_build_filename(g_get_user_config_dir(), "sliceview", "state", NULL);
}

static void save_last_dir(const char *path)
{
    char *file = last_dir_file();
    char *dir = g_path_get_dirname(file);
    if (g_mkdir_with_parents(dir, 493 /* 0755 */) == 0) {
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
static char *load_last_dir(void)
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
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ui->tree));
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
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "sliceview");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), "1.1");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
        "A static grid viewer for .stl and .3mf 3D model files in a directory.\n"
        "Features high-fidelity Blinn-Phong shading and CAD-style outlines.");
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), "Copyright © 2026 aginies");
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "https://github.com/aginies/sliceview");
    
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(ui->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
    const char *dir_arg = NULL;
    const char *png_out = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            png_out = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (!dir_arg)
            dir_arg = argv[i];
        else {
            print_usage(argv[0]);
            return 1;
        }
    }
    /* No directory given: reuse the last used one, else start in $HOME. */
    char *last = NULL;
    if (!dir_arg) {
        last = load_last_dir();
        dir_arg = last ? last : g_get_home_dir();
    }

    char *root = realpath(dir_arg, NULL);
    if (!root || !g_file_test(root, G_FILE_TEST_IS_DIR)) {
        fprintf(stderr, "Cannot open directory: %s\n", dir_arg);
        g_free(root);
        g_free(last);
        return 1;
    }
    g_free(last);
    save_last_dir(root);

    if (png_out)
        gtk_init_check(&argc, &argv); /* not needed for PNG, but harmless */
    else
        gtk_init(&argc, &argv);

    /* collect *.stl and *.3mf files directly in the root directory */
    GDir *gd = g_dir_open(root, 0, NULL);
    GPtrArray *paths = g_ptr_array_new();
    if (gd) {
        const char *e;
        while ((e = g_dir_read_name(gd))) {
            char *lower = g_ascii_strdown(e, -1);
            if (g_str_has_suffix(lower, ".stl") || g_str_has_suffix(lower, ".3mf"))
                g_ptr_array_add(paths, g_build_filename(root, e, NULL));
            g_free(lower);
        }
        g_dir_close(gd);
    }
    g_ptr_array_sort_with_data(paths, (GCompareDataFunc)sort_paths_compare,
                               GINT_TO_POINTER(SORT_NAME));

    if (png_out) {
        if (paths->len == 0) {
            fprintf(stderr, "No .stl or .3mf files found in: %s\n", dir_arg);
            return 1;
        }
        GPtrArray *items = render_paths(paths, NULL);
        if (items->len == 0) {
            fprintf(stderr, "No 3D files could be loaded in: %s\n", dir_arg);
            return 1;
        }

        int cols = (1280 - 40) / CELL;
        if (cols < 1)
            cols = 1;
        int rows = (int)((items->len + cols - 1) / cols);

        /* render the whole grid to one PNG */
        int W = cols * CELL;
        int H = rows * (CELL + LABEL_H);
        cairo_surface_t *grid =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        cairo_t *cr = cairo_create(grid);
        cairo_set_source_rgb(cr, 0.07, 0.09, 0.11);
        cairo_paint(cr);
        for (guint i = 0; i < items->len; ++i) {
            Item *it = items->pdata[i];
            int x = (int)(i % cols) * CELL;
            int y = (int)(i / cols) * (CELL + LABEL_H);
            cairo_set_source_surface(cr, it->thumb, x, y);
            cairo_paint(cr);
            cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 13);
            cairo_set_source_rgb(cr, 0.86, 0.88, 0.90);
            cairo_text_extents_t ext;
            cairo_text_extents(cr, it->name, &ext);
            cairo_move_to(cr, x + (CELL - ext.width) / 2 - ext.x_bearing,
                          y + CELL + (LABEL_H - ext.height) / 2 - ext.y_bearing);
            cairo_show_text(cr, it->name);
        }
        cairo_destroy(cr);
        cairo_status_t st = cairo_surface_write_to_png(grid, png_out);
        if (st != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "Cannot write %s: %s\n", png_out,
                    cairo_status_to_string(st));
            cairo_surface_destroy(grid);
            return 1;
        }
        cairo_surface_destroy(grid);
        printf("Wrote %s (%d files, %dx%d px)\n", png_out, (int)items->len, W,
               H);
        free_items(items);
        g_ptr_array_free(items, FALSE);
    } else {
        UI ui = {0};
        ui.items = g_ptr_array_new();

        ui.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_default_size(GTK_WINDOW(ui.window), 1000, 640);
        g_signal_connect(ui.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

        /* Main vertical box containing menu bar (top) and paned views (main area) */
        GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_container_add(GTK_CONTAINER(ui.window), main_box);

        /* Create Menu Bar */
        GtkWidget *menu_bar = gtk_menu_bar_new();
        
        /* File Menu */
        GtkWidget *file_item = gtk_menu_item_new_with_mnemonic("_File");
        GtkWidget *file_menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
        
        GtkWidget *quit_item = gtk_menu_item_new_with_mnemonic("_Quit");
        gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);
        g_signal_connect(quit_item, "activate", G_CALLBACK(gtk_main_quit), NULL);
        
        gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), file_item);
        
        /* Help Menu */
        GtkWidget *help_item = gtk_menu_item_new_with_mnemonic("_Help");
        GtkWidget *help_menu = gtk_menu_new();
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
        
        GtkWidget *about_item = gtk_menu_item_new_with_mnemonic("_About");
        gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);
        g_signal_connect(about_item, "activate", G_CALLBACK(on_menu_about_clicked), &ui);
        
        gtk_menu_shell_append(GTK_MENU_SHELL(menu_bar), help_item);
        
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
        ui.path_label = gtk_label_new(root);
        gtk_label_set_ellipsize(GTK_LABEL(ui.path_label), PANGO_ELLIPSIZE_START);
        g_object_set(ui.path_label, "xalign", 0.0f, NULL);
        gtk_widget_set_tooltip_text(ui.path_label, root);
        gtk_box_pack_start(GTK_BOX(nav), ui.path_label, TRUE, TRUE, 0);
        g_signal_connect(parent_btn, "clicked",
                         G_CALLBACK(on_parent_clicked), &ui);
        gtk_box_pack_start(GTK_BOX(left), nav, FALSE, FALSE, 0);

        GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_margin_top(sep, 2);
        gtk_widget_set_margin_bottom(sep, 2);
        gtk_box_pack_start(GTK_BOX(left), sep, FALSE, FALSE, 0);

        GtkWidget *tree_scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_widget_set_size_request(tree_scroll, 260, -1);
        ui.store = gtk_tree_store_new(N_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                      G_TYPE_INT, G_TYPE_INT);
        ui.tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ui.store));
        GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
        GtkCellRenderer *text = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new();
        gtk_tree_view_column_pack_start(col, icon, FALSE);
        gtk_tree_view_column_pack_start(col, text, TRUE);
        gtk_tree_view_column_add_attribute(col, text, "text", COL_NAME);
        gtk_tree_view_column_set_cell_data_func(col, icon,
                                                (GtkTreeCellDataFunc)on_cell_data,
                                                &ui, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(ui.tree), col);
        gtk_container_add(GTK_CONTAINER(tree_scroll), ui.tree);
        gtk_box_pack_start(GTK_BOX(left), tree_scroll, TRUE, TRUE, 0);
        gtk_paned_pack1(GTK_PANED(paned), left, FALSE, FALSE);
        g_signal_connect(ui.tree, "row-expanded", G_CALLBACK(on_row_expanded),
                         &ui);
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree));
        g_signal_connect(sel, "changed", G_CALLBACK(on_selection_changed), &ui);

        /* suppress the selection handler while we set up the tree and do
         * the initial (limited) render below */
        ui.rendering = 1;

        /* right: view-angle toolbar + thumbnail grid */
        GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        GtkWidget *view_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_top(view_bar, 4);
        gtk_widget_set_margin_bottom(view_bar, 4);
        gtk_widget_set_margin_start(view_bar, 4);
        gtk_widget_set_margin_end(view_bar, 4);

        /* radio group rendered as flat toggle buttons, so the active
         * view is always visible */
        GtkWidget *vb_45 = gtk_radio_button_new_with_label(NULL, "45°");
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(vb_45), FALSE);
        GtkWidget *vb_minus45 = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(vb_45), "-45°");
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(vb_minus45), FALSE);
        GtkWidget *vb_front = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(vb_45), "Front");
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(vb_front), FALSE);
        GtkWidget *vb_back = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(vb_45), "Back");
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(vb_back), FALSE);
        GtkWidget *vb_left = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(vb_45), "Left");
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(vb_left), FALSE);
        GtkWidget *vb_right = gtk_radio_button_new_with_label_from_widget(
            GTK_RADIO_BUTTON(vb_45), "Right");
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(vb_right), FALSE);

        g_object_set_data(G_OBJECT(vb_45),     "view_mode", GINT_TO_POINTER(VIEW_45));
        g_object_set_data(G_OBJECT(vb_minus45), "view_mode", GINT_TO_POINTER(VIEW_MINUS_45));
        g_object_set_data(G_OBJECT(vb_front), "view_mode", GINT_TO_POINTER(VIEW_FRONT));
        g_object_set_data(G_OBJECT(vb_back),  "view_mode", GINT_TO_POINTER(VIEW_BACK));
        g_object_set_data(G_OBJECT(vb_left),  "view_mode", GINT_TO_POINTER(VIEW_LEFT));
        g_object_set_data(G_OBJECT(vb_right), "view_mode", GINT_TO_POINTER(VIEW_RIGHT));

        g_signal_connect(vb_45,      "toggled", G_CALLBACK(on_view_toggled), &ui);
        g_signal_connect(vb_minus45, "toggled", G_CALLBACK(on_view_toggled), &ui);
        g_signal_connect(vb_front, "toggled", G_CALLBACK(on_view_toggled), &ui);
        g_signal_connect(vb_back,  "toggled", G_CALLBACK(on_view_toggled), &ui);
        g_signal_connect(vb_left,  "toggled", G_CALLBACK(on_view_toggled), &ui);
        g_signal_connect(vb_right, "toggled", G_CALLBACK(on_view_toggled), &ui);

        gtk_box_pack_start(GTK_BOX(view_bar), vb_45,      FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(view_bar), vb_minus45, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(view_bar), vb_front, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(view_bar), vb_back, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(view_bar), vb_left, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(view_bar), vb_right, FALSE, FALSE, 0);

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
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sort_combo),
                                  "name", sort_mode_names[SORT_NAME]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sort_combo),
                                  "size", sort_mode_names[SORT_SIZE]);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(sort_combo),
                                  "date", sort_mode_names[SORT_DATE]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(sort_combo), (int)ui.sort_mode);
        g_signal_connect(sort_combo, "changed",
                         G_CALLBACK(on_sort_changed), &ui);
        gtk_box_pack_start(GTK_BOX(sort_box), sort_combo, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(view_bar), sort_box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(right), view_bar, FALSE, FALSE, 0);

        ui.grid_scroll = gtk_scrolled_window_new(NULL, NULL);
        g_signal_connect(ui.grid_scroll, "size-allocate",
                         G_CALLBACK(on_grid_scroll_size_allocate), &ui);
        gtk_box_pack_start(GTK_BOX(right), ui.grid_scroll, TRUE, TRUE, 0);
        gtk_paned_pack2(GTK_PANED(paned), right, TRUE, FALSE);

        /* Status bar at the bottom */
        ui.status_bar = gtk_label_new("");
        gtk_widget_set_margin_start(ui.status_bar, 6);
        gtk_widget_set_margin_end(ui.status_bar, 6);
        gtk_widget_set_margin_top(ui.status_bar, 2);
        gtk_widget_set_margin_bottom(ui.status_bar, 2);
        gtk_widget_set_hexpand(ui.status_bar, TRUE);
        gtk_box_pack_start(GTK_BOX(main_box), ui.status_bar, FALSE, FALSE, 0);

    /* root node = the directory we were started with */
    gtk_widget_show_all(ui.window);
    gtk_paned_set_position(GTK_PANED(paned), 260);

    /* Build the tree without triggering render (render is lazy). */
    gtk_tree_store_clear(ui.store);
    GtkTreeIter it;
    gtk_tree_store_append(ui.store, &it, NULL);
    gtk_tree_store_set(ui.store, &it, COL_NAME, g_path_get_basename(root),
                       COL_PATH, g_strdup(root), COL_ISDIR, 1, -1);
    GtkTreeIter ph;
    gtk_tree_store_append(ui.store, &ph, &it);
    gtk_tree_store_set(ui.store, &ph, COL_NAME, "…", COL_PATH, "",
                       COL_ISDIR, 0, -1);
    GtkTreePath *p = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(ui.tree), p, FALSE);
    gtk_tree_selection_select_path(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ui.tree)), p);
    gtk_tree_path_free(p);

    /* Render initial thumbnails AFTER the window is shown and the main loop
     * can process draw events. Render a limited number of files at startup
     * (20) and let the user click to see more. */
    const guint INITIAL_RENDER_LIMIT = 20;
    guint n_init = paths->len < INITIAL_RENDER_LIMIT ? paths->len : INITIAL_RENDER_LIMIT;
    GPtrArray *init_paths = g_ptr_array_sized_new((guint)n_init);
    for (guint i = 0; i < n_init; ++i)
        g_ptr_array_add(init_paths, paths->pdata[i]);
    show_loading(&ui, "Loading…");
    GPtrArray *items = render_paths(init_paths, &ui);
    char *title = g_strdup_printf("sliceview — %s (%d files, showing %d)", root, paths->len, n_init);
    show_items(&ui, items, title);
    g_ptr_array_free(init_paths, FALSE);

    /* the root (first 20 files) is what is shown now; drop any selection
     * that was deferred during setup */
    g_free(ui.pending_path);
    ui.pending_path = NULL;
    ui.rendering = 0;

    /* Update status bar for the first item (if any) */
    if (ui.items->len > 0)
        update_status_bar(&ui, ui.items->pdata[0]);



    gtk_main();

    free_items(ui.items);
    g_ptr_array_free(ui.items, FALSE);
    }

    for (guint i = 0; i < paths->len; ++i)
        g_free(paths->pdata[i]);
    g_ptr_array_free(paths, FALSE);
    g_free(root);
    return 0;
}
