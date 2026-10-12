/*
 * stlpreview -- static grid viewer for the 3D models in a directory.
 *
 * Usage:
 *   stlpreview <directory>            show a scrollable grid of thumbnails
 *   stlpreview <directory> -o out.png render the grid to a PNG file instead
 *   stlpreview                        reopen the last directory used
 *
 * The window shows a directory tree on the left (folders and model files,
 * loaded lazily) and a grid of thumbnails on the right. Selecting a folder
 * shows the models directly inside it; selecting a file shows that one mesh.
 *
 * There is no zoom and no rotation: every mesh is drawn with a fixed
 * orthographic view fitted to its bounding box, by a small software z-buffer
 * rasteriser (see render.c).
 */

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* PNG export styling. The window paints thumbnails on a dark background and
 * labels them in light grey; the export mirrors that so the written file
 * looks like what the user was seeing. */
#define EXPORT_BG_RGB      0.07, 0.09, 0.11
#define EXPORT_LABEL_RGB   0.86, 0.88, 0.90
#define EXPORT_FONT_FAMILY "monospace"
#define EXPORT_FONT_SIZE   13.0

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
    /* No directory given: reuse the last used one, else start in $HOME.
     * `dir_owned` holds the allocation for the whole of main(): `dir_arg`
     * may alias it and is printed again by the -o error paths below, so it
     * must not be freed early (that was a use-after-free). */
    char *dir_owned = NULL;
    if (!dir_arg) {
        dir_owned = load_last_dir();
        if (!dir_owned)
            dir_owned = g_strdup(g_get_home_dir());
        dir_arg = dir_owned;
    }

    char *root = g_canonicalize_filename(dir_arg, NULL);
    if (!g_file_test(root, G_FILE_TEST_IS_DIR)) {
        fprintf(stderr, "Cannot open directory: %s\n", dir_arg);
        g_free(root);
        g_free(dir_owned);
        return 1;
    }
    save_last_dir(root);

    /* PNG export never opens a window: the rasterizer is pure cairo and
     * reports progress through a callback instead of pumping the GTK main
     * loop, so GTK is not initialised at all in headless mode. */
    if (!png_out)
        gtk_init(&argc, &argv);

    /* gtk_init() above installed the user's locale. Force the numeric
     * locale back to "C" so any remaining printf/scanf-style conversion
     * (and every file we parse or write) uses '.' as decimal separator.
     * All float I/O additionally goes through the g_ascii_* helpers. */
    setlocale(LC_NUMERIC, "C");

    /* Bound the on-disk thumbnail cache (sweep interrupted writes, drop
     * orphans, evict least-recently-used pairs) once per session. */
    cache_prune_once();

    if (png_out) {
        /* Only the export path needs the root listing up front; the GUI reads
         * the folder through render_current_selection(). */
        GPtrArray *paths = list_model_files_sorted(root, SORT_NAME);
        if (paths->len == 0) {
            fprintf(stderr, "No .stl or .3mf files found in: %s\n", dir_arg);
            g_ptr_array_free(paths, TRUE);
            g_free(root);
            g_free(dir_owned);
            return 1;
        }
        /* Headless export: synchronous, no hooks, no GTK involved. */
        GPtrArray *items = render_paths_sync(paths, VIEW_45, NULL);
        if (items->len == 0) {
            fprintf(stderr, "No 3D files could be loaded in: %s\n", dir_arg);
            g_ptr_array_free(items, TRUE);
            g_ptr_array_free(paths, TRUE);
            g_free(root);
            g_free(dir_owned);
            return 1;
        }

        /* Lay the export out exactly like the window: the same column count
         * for the same nominal width, and the same cell pitch. */
        int cols = grid_cols_for_width(GRID_DEFAULT_WIDTH);
        int rows = ((int)items->len + cols - 1) / cols;
        if (rows < 1)
            rows = 1;
        int pitch_x = CELL + GRID_COL_SPACING;
        int pitch_y = CELL + LABEL_H + GRID_ROW_SPACING;
        int W = 2 * GRID_MARGIN + cols * CELL + (cols - 1) * GRID_COL_SPACING;
        int H = 2 * GRID_MARGIN + rows * (CELL + LABEL_H) +
                (rows - 1) * GRID_ROW_SPACING;

        int status = 1;
        cairo_surface_t *grid =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
        if (cairo_surface_status(grid) != CAIRO_STATUS_SUCCESS) {
            fprintf(stderr, "Cannot allocate a %dx%d image: %s\n", W, H,
                    cairo_status_to_string(cairo_surface_status(grid)));
        } else {
            cairo_t *cr = cairo_create(grid);
            cairo_set_source_rgb(cr, EXPORT_BG_RGB);
            cairo_paint(cr);
            cairo_select_font_face(cr, EXPORT_FONT_FAMILY,
                                   CAIRO_FONT_SLANT_NORMAL,
                                   CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, EXPORT_FONT_SIZE);
            for (guint i = 0; i < items->len; ++i) {
                Item *it = items->pdata[i];
                int x = GRID_MARGIN + (int)(i % (guint)cols) * pitch_x;
                int y = GRID_MARGIN + (int)(i / (guint)cols) * pitch_y;
                cairo_set_source_surface(cr, it->thumb, x, y);
                cairo_paint(cr);
                /* Centre the name in the label strip below the thumbnail. */
                cairo_text_extents_t ext;
                cairo_text_extents(cr, it->name, &ext);
                cairo_set_source_rgb(cr, EXPORT_LABEL_RGB);
                cairo_move_to(cr, x + (CELL - ext.width) / 2 - ext.x_bearing,
                              y + CELL + (LABEL_H - ext.height) / 2 -
                                  ext.y_bearing);
                cairo_show_text(cr, it->name);
            }
            cairo_destroy(cr);

            cairo_status_t st = cairo_surface_write_to_png(grid, png_out);
            if (st != CAIRO_STATUS_SUCCESS) {
                fprintf(stderr, "Cannot write %s: %s\n", png_out,
                        cairo_status_to_string(st));
            } else {
                printf("Wrote %s (%d files, %dx%d px)\n", png_out,
                       (int)items->len, W, H);
                status = 0;
            }
        }
        cairo_surface_destroy(grid);
        g_ptr_array_free(items, TRUE);
        g_ptr_array_free(paths, TRUE);
        g_free(root);
        g_free(dir_owned);
        return status;
    } else {
        UI ui = {0};
        ui_build(&ui, root);
        gtk_main();
        ui_shutdown(&ui);
    }

    g_free(root);
    g_free(dir_owned);
    return 0;
}
