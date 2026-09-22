# sliceview

A standalone GTK3 viewer for STL and 3MF 3D model files. Displays a directory tree
on the left and a scrollable grid of thumbnails on the right, with wireframe-style
rendering, file metadata, and multiple view angles.

## Features

- **Directory tree** — Left panel with lazy-loaded folders and `.stl` / `.3mf` files
- **Thumbnail grid** — SHA256-based PNG cache in `~/.cache/sliceview/thumbs/`
- **Software rendering** — Z-buffer rasterizer with Blinn-Phong shading, ambient occlusion, and CAD-style outline edges (no OpenGL)
- **View angles** — 45°, -45°, Front, Back, Left, Right (radio buttons in toolbar)
- **Sort options** — Name, Size, Date (combo box in toolbar)
- **Right-click menu** — Open Containing Folder, Copy Path, Export PNG, Open with Default App, Move to Directory, Delete
- **Move to Directory** — Drag-and-drop-style file relocation via a directory picker dialog
- **Info bar** — File name, triangles, vertices, bounding box, file size, date, type
- **File type detection** — Binary STL, ASCII STL, 3MF
- **3MF support** — Extracts `3D/3dmodel.model` from the ZIP via `unzip` and parses it with GLib's `GMarkup` XML parser (handles namespaced, multi-line, and CDATA-tagged markup)
- **PNG export** — Render the full grid to a PNG file (`-o out.png`)
- **Last directory** — Remembers and reopens the previously used directory
- **About dialog** — File → Help → About

## Requirements

### Build dependencies

| Dependency | Purpose |
| ------------ | --------- |
| GCC (C99) | C compiler |
| CMake ≥ 3.10 | Build system |
| pkg-config | Locates the GTK3 headers/libs |
| GTK3 development libraries | UI toolkit (links `gtk+-3.0` and `libm`) |

No other libraries are needed — the 3D renderer is a self-contained software
z-buffer (no OpenGL, no Qt, no mesh libraries).

### Runtime dependencies

| Dependency | Purpose |
| ------------ | --------- |
| GTK3 runtime libraries | Required |
| `unzip` | 3MF support (extracts `3D/3dmodel.model`); STL-only usage works without it |
| `xdg-open` | "Open Containing Folder" and "Open with Default App" menu items |

### Installing build dependencies

```bash
# openSUSE
sudo zypper install gcc make cmake pkg-config gtk3-devel unzip
```

## Building

### Quick build

```bash
./build.sh
```

### Manual CMake build

```bash
cmake -S . -B build && cmake --build build
```

### Build modes

| Command | Output dir | Flags |
| --------- | ----------- | ------- |
| `./build.sh` | `release/` | `-O2 -Wall` (release) |
| `./build.sh debug` | `debug/` | `-g -O0 -DDEBUG` |
| `./build.sh clean` | — | Removes all build dirs |

### Debug build (verbose)

```bash
cmake -S . -B debug -DSTL_GRID_DEBUG=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build debug
```

## Running

```bash
./release/sliceview
```

Then navigate a directory containing `.stl` or `.3mf` files.

### CLI options

```bash
# Open a specific directory
./release/sliceview /path/to/models

# Render the grid to a PNG (no GUI)
./release/sliceview /path/to/models -o output.png
```

## License

This project is licensed under the GNU General Public License v3.0 (GPLv3).
See the `LICENSE` file for the full text.
