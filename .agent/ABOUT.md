---
abstract: "GPU yarn simulator (Stable Cosserat Rods, SIGGRAPH 2025): CUDA C++
           sim library + GLFW/ImGui viewer. Originally Windows MSBuild only;
           now also Linux via CMake. Both build paths live side by side."
---

# About this project and agent

## Project

YarnBall is a massively parallel GPU yarn simulator implementing
"Stable Cosserat Rods" (SIGGRAPH 2025). Two artifacts:

- `YarnBall` — static library with the simulation core (5 `.cu` files +
  ~25 host `.cpp`). Public surface in `KittenEngine/YarnBall/YarnBall.h`.
- `Gui` — executable that loads a JSON scene and either renders
  interactively (GLFW + GLAD + ImGui) or simulates headless and exports
  OBJ/BCC frames.

Submodule: `KittenEngine/KittenEngine/KittenGpuLBVH` (jerry060599/KittenGpuLBVH)
provides the LBVH spatial structure. **Has its own stripped 158-line
copy of `Common.h` next to the parent's full 798-line version** — see
NOTES.md.

Stack: CUDA 12.8, OpenGL 4.5/4.6 core, GLFW, GLAD (glad1 ABI), ImGui,
glm, Eigen, Thrust, OpenMP, Assimp, freetype, jsoncpp, CLI11, stb.

Two build systems coexist:
- `YarnBall.sln` + `KittenEngine/*.vcxproj` — original Windows / MSBuild / vcpkg
- `CMakeLists.txt` + `scripts/install-linux-deps.sh` — Linux (apt + system libs +
  FetchContent ImGui/CLI11 + auto-generated GLAD)

## Agent

- Role: research-engineer maintaining the sim + ports
- Specialty: CUDA, OpenGL, cross-platform C++ build systems
- Goals: keep both build paths functional; preserve simulator behavior
  byte-for-byte across platforms; avoid scope creep into refactors

## Rules

### Always
- Keep `.sln` / `.vcxproj` (Windows MSBuild) functional alongside any
  CMake change. Verify by reasoning through MSVC's behavior on each
  source change.
- When patching engine sources, prefer changes that are platform-neutral
  (forward-slash paths, `generic_string()` keys, `std::replace` for
  separators) over `#ifdef _WIN32` branches.
- Run from `build/` for engine asset loading — paths are cwd-relative.

### Never
- Glob source files in CMake. The `KittenEngine/embree/` directory and
  the submodule's `KittenGpuLBVH/main.cpp` will get sucked in. Always
  enumerate.
- Add submodule include paths blindly. The submodule's stripped
  `Common.h` (158 lines) silently shadows the parent's (798 lines) if
  it ends up earlier in `-I` order.
- Refactor public API of `YarnBall::Sim` as part of a build fix.
  Pre-existing UB (e.g. missing return) gets the minimal fix
  (`return 0;`), not a signature change.

### Code style
C++17. snake_case for free functions, PascalCase for types. Existing
code uses tabs for indentation.

## Conventions

- Engine asset paths use forward slashes everywhere (works on both
  OSes). Resource-map keys go through `path.generic_string()` so
  Windows native `\` and Linux native `/` produce the same key.
- CUDA arch is auto-detected via `${CUDAToolkit_BIN_DIR}/__nvcc_device_query`
  with a multi-arch fallback list `75;86;89;120`. Override with
  `-DCMAKE_CUDA_ARCHITECTURES=NN`.
- GLAD is auto-generated into `third_party/glad/` at first configure
  via Python `glad` package in a throwaway `.venv-glad/`. The generated
  tree is gitignored (regenerated on demand).
- LFS-tracked `.bcc` files in `KittenEngine/configs/models/`. A fresh
  clone needs `git lfs install --force && git lfs pull` (the install
  script handles this).
