# YarnBall on Linux

Linux build of [YarnBall](README.md) — GPU yarn simulator from
"Stable Cosserat Rods" (SIGGRAPH 2025). Tested on WSL2 Ubuntu 24.04
with RTX 5090 (sm_120) and CUDA 12.8. The original Windows
`YarnBall.sln` is untouched and still builds in Visual Studio.

## Quick start

```bash
# 1. One-time setup: apt deps, CUDA 12.8 toolkit, git-lfs pull, GLAD generation
./scripts/install-linux-deps.sh

# 2. Configure + build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# 3. Run from the build directory (paths are cwd-relative)
cd build
./Gui --headless configs/cable_work_pattern.json --twist -s -e -n 30 --exit -o /tmp/out/frame_
```

## Requirements

- Linux (or WSL2). Tested on Ubuntu 24.04.
- NVIDIA GPU, compute capability ≥ 7.5 (Turing or newer)
- CUDA Toolkit 12.8+ (the install script handles this)
- Network access at first configure (CMake fetches ImGui + CLI11 and
  generates GLAD)

If `nvcc` isn't on PATH after CUDA install:
```bash
export PATH=/usr/local/cuda-12.8/bin:$PATH
export LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:$LD_LIBRARY_PATH
```

To target a specific GPU explicitly:
```bash
cmake ... -DCMAKE_CUDA_ARCHITECTURES=86   # RTX 3090
cmake ... -DCMAKE_CUDA_ARCHITECTURES=89   # RTX 4090
cmake ... -DCMAKE_CUDA_ARCHITECTURES=120  # RTX 5090
```
Default is auto-detected from the local GPU.

## Running

Always `cd build` first — engine asset paths are cwd-relative.

### Headless export

```bash
# Twist scenario, 30 frames, OBJ per frame
./Gui --headless configs/cable_work_pattern.json --twist -s -e -n 30 --exit -o /tmp/out/frame_

# Pull, BCC format, last frame only
./Gui --headless configs/cable_work_pattern.json --pull -s --exportlast --bcc -o /tmp/final.bcc

# Letter examples
./Gui --headless configs/letterS.json -e -n 60 --exit -o /tmp/letterS_
```

### GUI (interactive)

```bash
DISPLAY=:0 ./Gui configs/cable_work_pattern.json --twist -s
```

(WSL2 needs `DISPLAY=:0` if SSH'd in. Native Linux usually has it set.)

Controls: left-click orbit, right-click pan, scroll zoom, space pause.

### CLI reference

```
./Gui [OPTIONS] [config.json]
  --headless         No GUI, sim only (no GL context needed)
  -s                 Start simulating immediately
  -e, --export       Export each frame
  --exportlast       Export only the final frame
  -n N               Stop after N frames
  -o PATH            Output prefix (per-frame) or full path (--exportlast)
  --bcc              Export as BCC instead of OBJ
  --fiber            Fiber-level mesh (slow, large)
  --twist            Twist animation
  --pull             Pull animation
  --fps N            (default 30)
  --exit             Quit when export limit reached
```

## Available scenes

In `configs/`:
- `cable_work_pattern.json` — default cable knit
- `flame_ribbing_pattern.json`, `openwork_trellis_pattern.json` — other patterns
- `letterA.json` … `letterS.json` — letter sculptures
- `relax0.json`, `relax1.json` — **broken** (reference `.bcc` files not in repo)

## WSL2 / WSLg notes

WSLg's GL stack (Mesa + llvmpipe or D3D12) doesn't expose NVIDIA's
CUDA-GL interop bridge. At startup the engine prints which GL stack it
got, e.g.:

```
GL: Mesa | llvmpipe (LLVM 20.1.2, 256 bits) | 4.5 (Core Profile) Mesa 25.2.8
```

When the renderer string contains `D3D12` or `llvmpipe`, the engine
automatically bypasses CUDA-GL interop with a host-side roundtrip
(~1–2 ms/frame at 65k verts). On native NVIDIA stacks (Windows, native
Linux with the proprietary driver) the original fast interop path runs
unchanged.

Visualization frame rate on llvmpipe is software-rasterizer bound
(~10–15 FPS for the cable pattern); the simulator itself stays
GPU-fast. For research / batch work, prefer headless export.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `recursive directory iterator cannot open directory: KittenEngine/shaders` | You ran from the repo root. `cd build` first. |
| `Cannot create window` | No display. `export DISPLAY=:0` (WSL2) or use `--headless`. |
| `GPUassert: context is destroyed` | Stale binary, or running on a non-NV GL stack without the bypass. Pull, rebuild, retry. |
| `nvcc not found` after install | Add `/usr/local/cuda-12.8/bin` to PATH. |
| `CUDA architectures: 52` printed at configure | nvcc < 12.8 detected. Re-run `scripts/install-linux-deps.sh`. |

## Inspecting OBJ output

Each frame is a polyline mesh of yarn centerlines (~4 MB at 65k verts).
Open in Blender (`File → Import → Wavefront`) or MeshLab. For
animations, batch-import or use a "frame sequence" addon.

## Migration summary

The Linux build touches 18 source files plus 4 new top-level files.
Every change is either platform-neutral or behind runtime detection;
the Windows `.sln`/`.vcxproj` build is unaffected.

| Area | Files | Why |
|---|---|---|
| **Build system** | `CMakeLists.txt`, `scripts/install-linux-deps.sh` | Mirror `.vcxproj` ItemGroup; auto-detect CUDA arch; FetchContent ImGui+CLI11; auto-generate GLAD; resource staging |
| **MSVC-isms** | `Mesh.cpp` | `fopen_s` → `fopen` |
| **GCC strictness** | `SymMat.h`, `cosserat.cu` | Anonymous struct in union → named substruct (`s_t s`) |
| **Type aliases** | `StopWatch.cpp`, `Timer.cpp` | `high_resolution_clock` → `steady_clock` (libstdc++ vs MSVC differ) |
| **Template lookup** | `Algo.h` | Qualify `glm::mix` to bypass `Kitten::mix` overload hiding |
| **Path handling** | `KittenAssets.cpp`, `Mesh.cpp`, `Font.cpp`, `Shader.cpp`, `Gizmos.cpp`, `KittenRendering.cpp`, `render.cpp` | Forward slashes in literals; `generic_string()` for resource-map keys; normalize Assimp Windows separators |
| **Latent bug fix** | `KittenPreprocessor.cpp` | `regex::multiline` so shader `#include` directives beyond line 1 actually inline |
| **WSLg GL** | `KittenInit.cpp` | GL 4.6 + 16xMSAA primary, fall back to 4.5 + 0xMSAA |
| **WSLg CUDA-GL interop** | `ComputeBuffer.h` | Detect Mesa-on-D3D12/llvmpipe; bypass interop with host roundtrip |

Full audit and rationale: [LINUX_MIGRATION_PLAN.md](LINUX_MIGRATION_PLAN.md).
