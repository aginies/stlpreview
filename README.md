# stlpreview

A standalone GTK3 viewer for STL and 3MF 3D model files. It shows a directory
tree on the left and a scrollable grid of rendered thumbnails on the right,
with per-file metadata and six fixed view angles.

There is no 3D interaction — no zoom, no orbit, no OpenGL. It is a *previewer*:
pick a folder, see every model in it at a glance.

## Features

- **Directory tree** — left panel with lazily loaded folders and `.stl` /
  `.3mf` files; nested directories are only scanned when first expanded
- **Thumbnail grid** — software-rendered previews, paginated 20 at a time with
  a "Load more" button, and streamed in as each render finishes so a large
  folder stays responsive
- **Software rendering** — a z-buffer rasteriser with Blinn-Phong diffuse and
  specular shading plus CAD-style crease and silhouette edges. No GPU, no
  OpenGL, no mesh library
- **View angles** — 45°, −45°, Front, Back, Left, Right
- **Sorting** — by name, size or modification date
- **Live updates** — the selected folder is watched with a `GFileMonitor`, so
  files added, removed or renamed elsewhere appear without a manual refresh.
  Bursts are coalesced behind a short debounce
- **Right-click menu** — open containing folder, copy path, export PNG, open
  with the default application, move to directory, delete
- **Move to directory** — relocate files through a directory-picker dialog
  with a lazily populated tree
- **Info bar** — file name, triangle count, unique vertex count, bounding box,
  file size, modification date and detected format
- **Format detection** — binary STL, ASCII STL and 3MF are distinguished by
  sniffing the file's contents, never by trusting its extension
- **3MF support** — the ZIP container is read by a built-in inflate-based
  reader (zlib) and the XML is parsed with GLib's `GMarkup`, so it handles
  namespaced, multi-line and CDATA-tagged markup. Per-face material colours
  and `<component>` assembly transforms are honoured
- **PNG export** — render the whole grid to a single PNG (`-o out.png`)
  without ever opening a window
- **Persistent cache** — thumbnails are cached on disk, bounded and
  LRU-evicted (see [Thumbnail cache](#thumbnail-cache))
- **Last directory** — reopens the folder you were last looking at

## Requirements

### Build dependencies

| Dependency | Purpose |
| --- | --- |
| GCC or Clang (C99) | C compiler |
| CMake ≥ 3.10 | Build system |
| pkg-config | Locates the GTK3 and cairo headers/libs |
| GTK3 development libraries | UI toolkit (`gtk+-3.0`) |
| zlib development libraries | Inflating the ZIP container inside a 3MF |

Nothing else is required. The renderer is a self-contained software z-buffer,
and the 3MF reader is built in — no `unzip` binary, no Qt, no OpenGL, no
external mesh or XML library.

### Runtime dependencies

| Dependency | Purpose |
| --- | --- |
| GTK3 runtime libraries | Required |
| zlib | Required (3MF support) |
| `xdg-open` | "Open containing folder" and "Open with default app" menu items |

### Installing build dependencies

```bash
# openSUSE
sudo zypper install gcc make cmake pkg-config gtk3-devel zlib-devel

# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libgtk-3-dev zlib1g-dev
```

## Building

```bash
./build.sh              # release build into release/
./build.sh debug        # debug build into debug/
./build.sh test         # debug build + run the unit tests
./build.sh clean        # remove every build directory
```

Or with CMake directly:

```bash
cmake -S . -B build && cmake --build build
```

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `CMAKE_BUILD_TYPE` | `Release` | `Release` optimises, `Debug` adds `-g`. Optimisation level comes from CMake, not from hardcoded flags, so a packager can override it normally |
| `STLPREVIEW_WERROR` | `OFF` | Turns warnings into errors. Used by `./build.sh test` and CI |

The tree builds clean with `-Wall -Wextra -Wshadow`, which the build enables
unconditionally. `-Wconversion` is deliberately *not* enabled: the rasteriser
does a great deal of intentional float/int narrowing.

## Running

```bash
./release/stlpreview                    # reopens the last folder
./release/stlpreview /path/to/models    # open a specific folder
./release/stlpreview /path/to/models -o grid.png   # headless PNG export
```

```text
Usage: stlpreview [directory] [-o out.png]
  Shows a directory tree (left) and a static grid of all
  .stl and .3mf files in the selected folder (right). No zoom,
  no rotation. With -o, renders the grid to a PNG.
  Without a directory, reopens the last used one (or $HOME).
```

The PNG export never initialises GTK, so it works over SSH with no display
and no `xvfb`.

## Tests

```bash
./build.sh test
# or
cmake -S . -B build && cmake --build build && (cd build && ctest --output-on-failure)
```

Three suites cover `model.c`, `render.c` and `cache.c` — 39 test functions in
total. They run headless, need no display, and never touch your real cache
(each sets its own `XDG_CACHE_HOME` to a scratch directory).

Each test `#include`s the `.c` file under test rather than linking it, so the
static helpers are reachable too. That works precisely because those three
modules have no GTK dependency.

`ui.c` is not unit tested — it is exercised by the CI smoke tests, which start
the real viewer on a virtual display and check that it renders without GTK
complaints.

## Thumbnail cache

Rendered thumbnails and their metadata live in
`$XDG_CACHE_HOME/stlpreview/thumbs/` (normally `~/.cache/stlpreview/thumbs/`).

Each entry is a **pair**: a PNG plus a small text sidecar holding the format,
triangle and vertex counts, bounding box, centre and extent. The sidecar means
the info bar can be populated without re-parsing the model.

- **Keying** — the file name is a SHA-256 of the model's path, modification
  time, size, thumbnail size, view angle and `CACHE_VERSION`. Editing a model
  therefore invalidates its own entries automatically.
- **Atomic writes** — both halves are written to a temporary name and renamed
  into place, so a crash cannot leave a truncated PNG that later looks valid.
  A pair only counts as a hit when *both* halves exist and agree.
- **Bounded** — once per session the cache is swept: interrupted-write
  leftovers and half-pairs are removed, then least-recently-used entries are
  evicted until it fits in both **256 MB** and **4096 entries**.
- **Invalidation** — deleting or moving a model removes its cached
  thumbnails across all six view angles.
- **Versioning** — `CACHE_VERSION` is bumped whenever the rasteriser or a
  parser changes, which orphans every old entry so the sweep collects them.

To clear it by hand:

```bash
rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}/stlpreview"
```

## Architecture

Six modules, split so that everything except the UI can be used — and tested —
without a toolkit. `stlview.h` documents the layout and holds the shared
types, constants and cross-module entry points.

| Module | Approx. lines | Responsibility |
| --- | --- | --- |
| `stlview.h` | 170 | Shared types (`Mesh`, `Tri`, `V3`), enums, geometry constants, the locale-independent number helpers, cross-module prototypes |
| `ui.h` | 100 | `Item` / `UI` structs and the UI entry points |
| `model.c` | 850 | STL (binary + ASCII) and 3MF (ZIP + XML) readers producing a `Mesh`; extension predicates |
| `render.c` | 360 | The z-buffer rasteriser: `render_mesh()` and `grid_cols_for_width()` |
| `cache.c` | 400 | On-disk thumbnail cache: keys, atomic save/load, orphan sweep, LRU eviction |
| `ui.c` | 2400 | The GTK3 front end: tree, grid, toolbar, render jobs, dialogs, file monitor |
| `main.c` | 190 | Command line and the two entry paths (GUI, `-o` PNG export) |

The counts are deliberately approximate: they are here to show where the weight
is, not to be a metric anyone has to keep in sync. `ui.c` is by far the largest
module and the only one without unit-test coverage, which is the obvious next
target if you want to reduce risk further.

### Invariants worth knowing before you change something

- **`model.c`, `render.c` and `cache.c` never include GTK.** This is what
  makes the headless export and the unit tests possible. Keep it that way.
- **All number I/O is locale-independent.** Model files and the cache sidecar
  are strict-ASCII file formats, so every float conversion goes through
  `ascii_strtod()` / `ascii_dtostr()` in `stlview.h` — never `atof()`,
  `sscanf("%f")` or `"%.9g"`. Under a comma-decimal locale such as `fr_FR`,
  those read `1.5` as `1.0`. This was a real bug that silently flattened
  every model.
- **`render_mesh()` owns `cairo_surface_flush()` and
  `cairo_surface_mark_dirty()`.** It writes raw pixels, so callers must not
  duplicate either call. It returns `RENDER_OK`, `RENDER_FAIL` or
  `RENDER_ABORT`, and the distinction matters: a *failed* file is skipped and
  the batch continues, an *abort* stops the whole batch. One unrenderable
  file used to silently discard the rest of the directory.
- **Rendering never touches GTK directly.** Progress and cancellation go
  through a `RenderProgressFn` callback, which is what lets the UI run
  renders on a worker thread and publish results with `g_idle_add()`.
- **Every `GPtrArray` owns its elements.** Arrays are created with
  `g_ptr_array_new_full()` and a destroy function, and always freed with
  `free_seg = TRUE` (`FALSE` leaks the data segment). To steal the elements
  out, call `g_ptr_array_set_free_func(arr, NULL)` first.
- **Allocation failure is an error, not a fatal exit.** The loaders grow
  through `grow_array()`, which uses `g_try_realloc` and reports failure;
  there is no `exit(1)` anywhere in the tree.
- **Grid geometry has one source.** `grid_cols_for_width()` and the
  `GRID_*` constants in `stlview.h` are shared by the GTK grid and the PNG
  export, so the two cannot drift apart.

### Known limitations

- 3MF assembly semantics are applied only where unambiguous. Full semantics
  can require *duplicating* a mesh when one object is instantiated by several
  components or is both a component target and a direct build item. Because
  the mesh is emitted in a single pass, transforms are applied only when the
  referenced object is instantiated exactly once and is not also built
  directly; everything else keeps the part in its own coordinates, so all
  parts stay visible rather than being silently dropped.
- Meshes above 20,000 triangles are decimated for thumbnails — every Nth
  triangle is drawn. At 320×320 the difference is invisible, and it bounds
  render time for models with millions of facets.

## Continuous integration

GitHub Actions (`.github/workflows/ci.yml`) builds with warnings as errors,
runs the unit tests, checks formatting, and then runs three smoke tests: a
headless PNG export over a generated binary STL, ASCII STL and 3MF; the real
GUI under Xvfb; and a check that the thumbnail cache is actually *reused* on a
second run.

Formatting is enforced with a version-pinned `clang-format`, because its output
is not stable across major versions. `.clang-format` describes the existing
style — 4-space indent, 80 columns, braces on their own line for functions and
attached elsewhere — so running it over the tree is a no-op:

```bash
clang-format -i *.c *.h tests/*.c tests/*.h
```

## License

GNU General Public License v3.0 (GPLv3). See `LICENSE` for the full text.
