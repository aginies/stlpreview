/*
 * stlpreview -- software rasteriser.
 *
 * A fixed orthographic view with a z-buffer, drawn straight into the pixel
 * data of a cairo image surface: no OpenGL, no cairo painting operations.
 * The view is fitted to the mesh bounding box, so thumbnails are comparable
 * whatever the model's unit or size.
 *
 * This file must not include GTK. Progress and cancellation go through the
 * RenderProgressFn callback, which is what lets the UI run renders on a
 * worker thread and the -o export run with no toolkit at all.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stlview.h"

/* Report progress every N triangles, but only for meshes big enough that the
 * bookkeeping is worth it. */
#define RENDER_YIELD_INTERVAL 100
#define RENDER_YIELD_MIN_TRIS 512

/* Thumbnails decimate meshes above this many triangles. At CELL x CELL
 * pixels the difference is invisible, and it bounds the rasteriser's runtime
 * for models with millions of facets. */
#define RENDER_MAX_TRIS 20000

/* Depth values at or above this count as background. The z-buffer starts at
 * FLT_MAX (every byte 0x7f) and real geometry is normalised into the
 * bounding box, so nothing legitimately lands near this magnitude. */
#define DEPTH_BACKGROUND 1e8f

/* Adjacent pixels whose depth differs by more than this fraction of the
 * model's extent are separated by a drawn edge (a crease or a silhouette). */
#define CREASE_FRACTION 0.015f

/* Columns of CELL-wide thumbnails that fit in `width_px`. Shared with the GTK
 * grid so the window and the PNG export cannot disagree. */
int grid_cols_for_width(int width_px)
{
    if (width_px <= 0)
        width_px = GRID_DEFAULT_WIDTH; /* nothing allocated yet */
    int cols = (width_px - 2 * GRID_MARGIN) / (CELL + GRID_COL_SPACING);
    return cols < 1 ? 1 : cols;
}

static V3 rotate_view(V3 p, int view_mode)
{
    const float pi = 3.14159265358979323846f;
    float angle = 0.0f;
    switch (view_mode) {
        case VIEW_45: angle = pi / 4.0f; break;        /* 45° */
        case VIEW_MINUS_45: angle = -pi / 4.0f; break; /* -45° */
        case VIEW_FRONT: angle = 0.0f; break;          /* 0° */
        case VIEW_BACK: angle = pi; break;             /* 180° */
        case VIEW_LEFT: angle = -pi / 2.0f; break;     /* -90° */
        case VIEW_RIGHT: angle = pi / 2.0f; break;     /* 90° */
        default: angle = pi / 4.0f; break;
    }
    float cy = cosf(angle), sy = sinf(angle);
    float x = p.x * cy + p.z * sy;
    float z = -p.x * sy + p.z * cy;
    return (V3){x, p.y, z};
}

int render_mesh(const Mesh *mesh, cairo_surface_t *surface, int view_mode,
                float *bbox_out, float *center_out, float *ext_out,
                RenderProgressFn progress, void *progress_data)
{
    const int w = CELL, h = CELL;

    /* This function writes raw pixels, so it insists on exactly the surface
     * shape it assumes. cairo_image_surface_get_data() returns NULL for a
     * finished or failed surface, and writing through it would crash. */
    if (!surface || !mesh ||
        cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
        cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32 ||
        cairo_image_surface_get_width(surface) != w ||
        cairo_image_surface_get_height(surface) != h ||
        cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        return RENDER_FAIL;
    if (mesh->count == 0)
        return RENDER_FAIL;

    cairo_surface_flush(surface); /* required before touching the pixels */
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    if (!data || stride < w * 4)
        return RENDER_FAIL;

    /* bounding box + center */
    float mnx = INFINITY, mny = INFINITY, mnz = INFINITY;
    float mxx = -INFINITY, mxy = -INFINITY, mxz = -INFINITY;
    for (size_t i = 0; i < mesh->count; ++i)
        for (int k = 0; k < 9; k += 3) {
            float x = mesh->tris[i].v[k], y = mesh->tris[i].v[k + 1],
                  z = mesh->tris[i].v[k + 2];
            if (x < mnx)
                mnx = x;
            if (y < mny)
                mny = y;
            if (z < mnz)
                mnz = z;
            if (x > mxx)
                mxx = x;
            if (y > mxy)
                mxy = y;
            if (z > mxz)
                mxz = z;
        }
    V3 c = {(mnx + mxx) / 2, (mny + mxy) / 2, (mnz + mxz) / 2};
    float ext = fmaxf(mxx - mnx, fmaxf(mxy - mny, mxz - mnz));
    if (ext <= 0.0f)
        ext = 1.0f;
    float s = (w - 2 * MARGIN) / ext;

    /* Store bbox info if requested */
    if (bbox_out) {
        bbox_out[0] = mnx;
        bbox_out[1] = mny;
        bbox_out[2] = mnz;
        bbox_out[3] = mxx;
        bbox_out[4] = mxy;
        bbox_out[5] = mxz;
    }
    if (center_out) {
        center_out[0] = c.x;
        center_out[1] = c.y;
        center_out[2] = c.z;
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

    /* z-buffer: starts at FLT_MAX (0x7f7f7f7f), front-to-back (smaller z wins) */
    float *depth = g_try_new0(float, (gsize)w *(gsize)h);
    if (!depth)
        return RENDER_FAIL;
    memset(depth, 0x7f, (size_t)w * h * sizeof(float));

    /* light direction in view space (normalized) */
    V3 light = {-0.45f, 0.65f, 0.61f};
    float ll = sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
    if (ll > 0) {
        light.x /= ll;
        light.y /= ll;
        light.z /= ll;
    }

    /* Halfway vector for Blinn-Phong specular highlights. View vector is (0,0,1) */
    V3 H = {light.x, light.y, light.z + 1.0f};
    float hl = sqrtf(H.x * H.x + H.y * H.y + H.z * H.z);
    if (hl > 0) {
        H.x /= hl;
        H.y /= hl;
        H.z /= hl;
    }

    /* Decimate very dense meshes: every RENDER_MAX_TRIS-th triangle is
     * drawn, which is indistinguishable at thumbnail size. */
    size_t step = (mesh->count > RENDER_MAX_TRIS)
                      ? (mesh->count + RENDER_MAX_TRIS - 1) / RENDER_MAX_TRIS
                      : 1;

    int aborted = 0;
    for (size_t t = 0; t < mesh->count; t += step) {
        /* Let the caller report progress / abort every
         * RENDER_YIELD_INTERVAL triangles. */
        if (progress && mesh->count > RENDER_YIELD_MIN_TRIS &&
            (t % RENDER_YIELD_INTERVAL) == 0 &&
            progress(t, mesh->count, progress_data) != 0) {
            aborted = 1;
            break;
        }
        V3 p0 =
            rotate_view((V3){mesh->tris[t].v[0] - c.x, mesh->tris[t].v[1] - c.y,
                             mesh->tris[t].v[2] - c.z},
                        view_mode);
        V3 p1 =
            rotate_view((V3){mesh->tris[t].v[3] - c.x, mesh->tris[t].v[4] - c.y,
                             mesh->tris[t].v[5] - c.z},
                        view_mode);
        V3 p2 =
            rotate_view((V3){mesh->tris[t].v[6] - c.x, mesh->tris[t].v[7] - c.y,
                             mesh->tris[t].v[8] - c.z},
                        view_mode);

        /* flat normal (view space) */
        V3 e1 = {p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        V3 e2 = {p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        V3 n = {e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
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
            dr = 0x9D;
            dg = 0xB5;
            db = 0xC8; /* titanium blue-steel */
        }
        int base_r = (int)(dr * inten + 0xFF * spec);
        int base_g = (int)(dg * inten + 0xFF * spec);
        int base_b = (int)(db * inten + 0xFF * spec);
        if (base_r > 255)
            base_r = 255;
        if (base_g > 255)
            base_g = 255;
        if (base_b > 255)
            base_b = 255;

        /* screen coords: the projection is centred, so shift by half a cell.
         * Written as float multiplies, not w / 2, because the division would
         * be evaluated in integers before being widened. */
        float cx = w * 0.5f, cy = h * 0.5f;
        float x0 = p0.x * s + cx, y0 = cy - p0.y * s, z0 = p0.z;
        float x1 = p1.x * s + cx, y1 = cy - p1.y * s, z1 = p1.z;
        float x2 = p2.x * s + cx, y2 = cy - p2.y * s, z2 = p2.z;

        int minx = (int)floorf(fminf(x0, fminf(x1, x2)));
        int maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
        int miny = (int)floorf(fminf(y0, fminf(y1, y2)));
        int maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
        if (minx < 0)
            minx = 0;
        if (miny < 0)
            miny = 0;
        if (maxx >= w)
            maxx = w - 1;
        if (maxy >= h)
            maxy = h - 1;

        float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (fabsf(area) < 1e-8f)
            continue; /* degenerate */

        for (int py = miny; py <= maxy; ++py) {
            for (int px_ = minx; px_ <= maxx; ++px_) {
                float fx = px_ + 0.5f, fy = py + 0.5f;
                float u =
                    ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area;
                float v =
                    ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area;
                float wt = 1.0f - u - v;
                int inside = (u >= 0 && v >= 0 && wt >= 0) ||
                             (u <= 0 && v <= 0 && wt <= 0);
                if (!inside)
                    continue;
                float d = u * z0 + v * z1 + wt * z2;
                size_t idx = (size_t)py * w + px_;
                if (d < depth[idx]) {
                    depth[idx] = d;
                    /* depth cue: darker toward the back of the mesh
                     * (d < 0 = in front of the mesh center → no darkening) */
                    float dn = fminf(fmaxf(d / ext, 0.0f), 1.0f);
                    float ao_val = 1.0f - 0.2f * dn;
                    /* apply to color */
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
    }

    /* CAD-style outline edge detection pass (skipped when aborted: the
     * caller discards a partially rendered surface anyway) */
    if (aborted) {
        g_free(depth);
        return RENDER_ABORT;
    }
    float crease_thresh = CREASE_FRACTION * ext;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            size_t idx = (size_t)y * w + x;
            float d = depth[idx];
            if (d > DEPTH_BACKGROUND)
                continue;

            int is_edge = 0;
            size_t neighbors[4] = {
                (size_t)y * w + (x + 1), (size_t)y * w + (x - 1),
                (size_t)(y + 1) * w + x, (size_t)(y - 1) * w + x};
            for (int k = 0; k < 4; ++k) {
                /* ndepth: the neighbour's depth, versus d for this pixel. */
                float ndepth = depth[neighbors[k]];
                if (ndepth > DEPTH_BACKGROUND) {
                    /* Silhouette: the neighbour is background */
                    is_edge = 1;
                    break;
                }
                if (fabsf(d - ndepth) > crease_thresh) {
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

    g_free(depth);
    /* Tell cairo the pixels it handed out have changed; without this the
     * surface may still be treated as its (empty) cached snapshot. */
    cairo_surface_mark_dirty(surface);
    return RENDER_OK;
}
