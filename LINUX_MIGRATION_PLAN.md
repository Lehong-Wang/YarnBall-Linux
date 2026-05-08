# YarnBall — Linux Migration Plan (v2)

Consolidated after fresh-Claude and Codex independent reviews. v1 was directionally
correct but had three build-blocking gaps and several runtime gotchas the reviewers
caught; everything below is the corrected version.

Repo root: `/home/daizi/Research/YarnBall`
Target host: WSL2 Ubuntu 24.04, RTX 5090 (sm_120), GCC 13.3
Goal: build and run `Gui` and the `YarnBall` static library on Linux without losing
Windows compatibility.

---

## 1. Audit findings

### 1.1 Already portable

- No `Windows.h`, `_MSC_VER`, `__forceinline`, `__declspec`, Win32 API.
- No `system()` / `popen` / `CreateProcess`.
- Timing already uses `<chrono>` (`StopWatch.h`, `Timer.h`).
- `Common.h` already guards CUDA includes with `__has_include("cuda_runtime.h")`.
- All third-party libs are cross-platform (assimp, eigen3, stb, glad, glfw,
  imgui, glm, jsoncpp, cli11, freetype, thrust).
- KittenGpuLBVH submodule is portable.
- `KittenEngine/KittenEngine/embree/MeshCCD.{h,cpp}` is dead code — not in the
  `.vcxproj` `<ItemGroup>`. Leave on disk; do not add to CMake.

### 1.2 Concrete Windows-isms to fix

**Build system (largest item)**
- `YarnBall.sln`, `KittenEngine/YarnBall.vcxproj`, `KittenEngine/Gui.vcxproj`
  use MSBuild + a CUDA 12.8 props/targets pair (`BuildCustomizations\CUDA 12.8.{props,targets}`).
- Vendored Win32 `libs/` and `includes/` paths in the vcxproj — not used on Linux.
- CUDA codegen hardcoded to `compute_86,sm_86`.
- OpenMP wired as MSVC `-openmp` via `<AdditionalCompilerOptions>` to NVCC.

**Source-level (~22+ literals + 5 runtime concatenations + 5 `fopen_s`)**

`fopen_s` (MSVC Annex K — won't link on glibc):
- `KittenEngine/KittenEngine/src/Mesh.cpp:143, 169, 196, 686, 786`

Hard-coded `\\` in resource path string literals:
- `KittenEngine/KittenEngine/src/Shader.cpp:69, 76`
- `KittenEngine/KittenEngine/src/Gizmos.cpp:303`
- `KittenEngine/KittenEngine/src/KittenRendering.cpp:106, 108, 109, 110, 111, 112, 113, 115, 116`
- `KittenEngine/KittenEngine/src/Font.cpp:34, 50`
- `KittenEngine/YarnBall/io/render.cpp:11, 12, 14, 15, 34`

Runtime `path.string() + "\\"` concatenations (require `std::filesystem::path::operator/`):
- `KittenEngine/KittenEngine/src/KittenAssets.cpp:77`
- `KittenEngine/KittenEngine/src/Mesh.cpp:284, 306, 311, 328`

Do **not** rewrite `KittenEngine/KittenEngine/src/KittenPreprocessor.cpp:65` —
the `\n` there is a regex character class, not a path separator.

**Latent footguns (currently dead, leave alone but document)**
- `KittenEngine/KittenEngine/opt/polynomial.h:5` includes `<intrin.h>`
  unconditionally. No TU currently includes `polynomial.h`. Do not add it
  to the CMake target list.
- `arithmetic_sse_*.h` use `_aligned_malloc` and `<intrin.h>`. The actual
  reason these are dead on Linux is not the MSVC guards (those exist but
  also have a GCC fallback path) — it's that `lbfgs.c` only `#include`s
  them under `#if defined(USE_SSE) && defined(__SSE2__) && LBFGS_FLOAT == 64`,
  and `USE_SSE` is undefined in this build. The headers therefore never
  reach the preprocessor at all.

**Diagnostic strings — leave alone**
- `KittenEngine/KittenEngine/src/Mesh.cpp:333` (now `:330` after the
  `fopen_s` simplification) has a printf format string `(\\%s)` for
  user-visible logging. Not a path separator. Don't sweep it into the
  backslash rewrite.

**Pre-existing (not introduced by migration, surface after build)**
- `configs/relax0.json` and `configs/relax1.json` reference `.bcc` files
  (`kp-garter-rib-4-pattern.txt.bcc`, `frame.bcc`) that do not exist in
  the repository.
- `KittenEngine/YarnBall/io/jsonBuilder.cpp` resolves `curveFile` paths
  against process CWD rather than the config file's directory.

### 1.3 Local-box state

- nvcc 12.0 currently installed. Project pins 12.8. RTX 5090 needs sm_120,
  which only exists in CUDA toolkit 12.8+. **Hard requirement to upgrade
  the toolkit** — see Phase 0.
- `cmake`, `pkg-config`, `libglfw3-dev`, `libassimp-dev`, etc. not installed.
- `libGL.so` and `libfreetype.so` are present.
- WSLg with `/Xwayland` running — OpenGL forwarding available, but see
  WSLg risks below.

---

## 2. Phase 0 — Environment prep

```bash
# Toolchain
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config git git-lfs

# Pull LFS-tracked BCC model files (.gitattributes filters *.bcc through LFS).
# Without this, GUI launch silently fails when the first scene loads.
git lfs install
git lfs pull

# CUDA 12.8 toolkit (required for sm_120 / RTX 5090)
wget https://developer.download.nvidia.com/compute/cuda/repos/wsl-ubuntu/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update
sudo apt install -y cuda-toolkit-12-8
# add /usr/local/cuda-12.8/bin to PATH and /usr/local/cuda-12.8/lib64 to LD_LIBRARY_PATH

# System libraries available on Linux
sudo apt install -y libglfw3-dev libglew-dev libassimp-dev libfreetype-dev \
                    libglm-dev libeigen3-dev libjsoncpp-dev libstb-dev

# ImGui, CLI11, GLAD: handled via FetchContent / vendoring (not apt packages)
```

Verification:
```bash
nvcc --version    # should report 12.8
cmake --version   # >= 3.24 (for CMAKE_CUDA_ARCHITECTURES native)
nvidia-smi        # 5090 visible
```

---

## 3. Phase 1 — CMakeLists.txt

### 3.1 Top-level structure

```cmake
cmake_minimum_required(VERSION 3.24)
project(YarnBall LANGUAGES C CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hard-fail if toolkit is too old for RTX 50-series.
find_package(CUDAToolkit 12.8 REQUIRED)

# Gate native arch detection on toolkit version (12.0 doesn't know sm_120).
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    if(CUDAToolkit_VERSION VERSION_LESS 12.8)
        set(CMAKE_CUDA_ARCHITECTURES 86)
    else()
        set(CMAKE_CUDA_ARCHITECTURES native)
    endif()
endif()

find_package(OpenGL  REQUIRED)
find_package(glfw3   REQUIRED)
find_package(assimp  REQUIRED)
find_package(Freetype REQUIRED)
find_package(glm     REQUIRED)
find_package(Eigen3  REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenMP  REQUIRED)

# jsoncpp packaging is inconsistent — try config first, fall back to pkg-config.
find_package(jsoncpp CONFIG QUIET)
if(NOT jsoncpp_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
endif()
```

### 3.2 ImGui via FetchContent (must explicitly add backends)

```cmake
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.91.5)  # plain master; source uses no docking APIs
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)
```

### 3.3 CLI11 via FetchContent

```cmake
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG v2.4.2)
FetchContent_MakeAvailable(cli11)
```

### 3.4 GLAD — vendor glad1 (do NOT FetchContent glad2)

The source uses `<glad/glad.h>` + `gladLoadGLLoader` (glad1 ABI).
Mainline `Dav1dde/glad` defaults to glad2 (`<glad/gl.h>` + `gladLoadGL`)
which is not drop-in.

Vendor a generated glad1 snapshot:
```
third_party/glad/
    include/glad/glad.h
    include/KHR/khrplatform.h
    src/glad.c
```

```cmake
add_library(glad STATIC third_party/glad/src/glad.c)
target_include_directories(glad PUBLIC third_party/glad/include)
```

(One way to obtain: run https://glad.dav1d.de/ with API=GL 4.6 Core,
Profile=Core, all extensions, generate glad1.)

### 3.5 Source enumeration — explicit, NO globs

Globs would suck in `KittenEngine/KittenEngine/embree/MeshCCD.cpp`
(Embree absent) and `KittenEngine/KittenEngine/KittenGpuLBVH/main.cpp`
(submodule's standalone test driver). Enumerate to match the
`KittenEngine/YarnBall.vcxproj` `<ItemGroup>` exactly:

```cmake
set(YARNBALL_HOST_SOURCES
    # KittenEngine/opt
    KittenEngine/KittenEngine/opt/asa047.cpp
    KittenEngine/KittenEngine/opt/compass_search.cpp
    KittenEngine/KittenEngine/opt/lbfgs.c
    KittenEngine/KittenEngine/opt/math.cpp
    KittenEngine/KittenEngine/opt/praxis.cpp
    KittenEngine/KittenEngine/opt/svd/svd.cpp
    KittenEngine/KittenEngine/opt/toms178.cpp
    # KittenEngine/src
    KittenEngine/KittenEngine/src/Algo.cpp
    KittenEngine/KittenEngine/src/ComputeBuffer.cpp
    KittenEngine/KittenEngine/src/Font.cpp
    KittenEngine/KittenEngine/src/FrameBuffer.cpp
    KittenEngine/KittenEngine/src/Gizmos.cpp
    KittenEngine/KittenEngine/src/KittenAssets.cpp
    KittenEngine/KittenEngine/src/KittenInit.cpp
    KittenEngine/KittenEngine/src/KittenPreprocessor.cpp
    KittenEngine/KittenEngine/src/KittenRendering.cpp
    KittenEngine/KittenEngine/src/Mesh.cpp
    KittenEngine/KittenEngine/src/MeshMoments.cpp
    KittenEngine/KittenEngine/src/Shader.cpp
    KittenEngine/KittenEngine/src/StopWatch.cpp
    KittenEngine/KittenEngine/src/Texture.cpp
    KittenEngine/KittenEngine/src/Timer.cpp
    # YarnBall/io
    KittenEngine/YarnBall/io/fiberMesher.cpp
    KittenEngine/YarnBall/io/reader.cpp
    KittenEngine/YarnBall/io/fileExport.cpp
    KittenEngine/YarnBall/io/jsonBuilder.cpp
    KittenEngine/YarnBall/io/render.cpp
    # YarnBall/sim
    KittenEngine/YarnBall/sim/statistics.cpp
    KittenEngine/YarnBall/sim/step.cpp)

set(YARNBALL_CUDA_SOURCES
    KittenEngine/KittenEngine/KittenGpuLBVH/lbvh.cu
    KittenEngine/YarnBall/sim/iteration.cu
    KittenEngine/YarnBall/sim/cosserat.cu
    KittenEngine/YarnBall/sim/collision.cu
    KittenEngine/YarnBall/YarnBall.cu)

add_library(YarnBall STATIC ${YARNBALL_HOST_SOURCES} ${YARNBALL_CUDA_SOURCES})

target_include_directories(YarnBall PUBLIC
    KittenEngine/KittenEngine/includes
    KittenEngine
    KittenEngine/KittenEngine
    # NOTE: do NOT add KittenEngine/KittenEngine/KittenGpuLBVH/KittenEngine/includes
    # — it shadows the parent Common.h. Submodule code resolves siblings via
    # relative includes already.
)

target_link_libraries(YarnBall PUBLIC
    CUDA::cudart
    OpenMP::OpenMP_CXX
    Threads::Threads
    glm::glm
    Eigen3::Eigen
    glad
    assimp::assimp
    Freetype::Freetype
    OpenGL::GL
    glfw
    imgui)

# jsoncpp link target name varies by distro
if(TARGET JsonCpp::JsonCpp)
    target_link_libraries(YarnBall PUBLIC JsonCpp::JsonCpp)
elseif(TARGET jsoncpp_lib)
    target_link_libraries(YarnBall PUBLIC jsoncpp_lib)
else()
    target_link_libraries(YarnBall PUBLIC PkgConfig::JSONCPP)
endif()

# OpenMP must reach both compile and link of CUDA TUs (lbvh.cu uses
# `#pragma omp parallel for` and `#pragma omp atomic` inside CUDA-compiled code).
target_compile_options(YarnBall PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--extended-lambda>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcudafe=--diag_suppress=esa_on_defaulted_function_ignored>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-fopenmp>
    $<$<COMPILE_LANGUAGE:CXX>:-fopenmp>)

# Gui executable
add_executable(Gui KittenEngine/main.cpp)
target_link_libraries(Gui PRIVATE YarnBall CLI11::CLI11)
```

### 3.6 Resource staging — must include `frames/`

```cmake
add_custom_command(TARGET Gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/configs   $<TARGET_FILE_DIR:Gui>/configs
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/resources $<TARGET_FILE_DIR:Gui>/resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/KittenEngine/shaders
        $<TARGET_FILE_DIR:Gui>/KittenEngine/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/KittenEngine/KittenEngine/fonts
        $<TARGET_FILE_DIR:Gui>/KittenEngine/fonts
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:Gui>/frames)
```

(Optional follow-up: change `KittenInit.cpp` to resolve cwd to
`std::filesystem::canonical("/proc/self/exe").parent_path()` so `./Gui`
works from anywhere, not just the binary's directory.)

---

## 4. Phase 2 — Source patches

### 4.1 `fopen_s` → `fopen` (compile-blocking)

`KittenEngine/KittenEngine/src/Mesh.cpp:143, 169, 196, 686, 786`

Pattern:
```cpp
// before
FILE* file;
fopen_s(&file, p.c_str(), "w");
// after
FILE* file = fopen(p.c_str(), "w");
if (!file) { /* existing error path */ }
```

### 4.2 Backslash literals → forward slashes

Forward slash works on Windows too; this is the universal fix.

Files:
- `KittenEngine/KittenEngine/src/Shader.cpp:69, 76`
- `KittenEngine/KittenEngine/src/Gizmos.cpp:303`
- `KittenEngine/KittenEngine/src/KittenRendering.cpp:106, 108-116`
- `KittenEngine/KittenEngine/src/Font.cpp:34, 50`
- `KittenEngine/YarnBall/io/render.cpp:11, 12, 14, 15, 34`

Do **not** rewrite `KittenPreprocessor.cpp:65` (regex `\n`).

### 4.3 Runtime path concatenation

Change `path.string() + "\\" + foo` to `(path / foo).generic_string()`. Use
`generic_string()` (always forward slashes) rather than `string()` (native
separators) so that resource map keys are stable across platforms — this
preserves Windows compatibility instead of breaking it:

- `KittenEngine/KittenEngine/src/KittenAssets.cpp:77`
- `KittenEngine/KittenEngine/src/Mesh.cpp:284, 306, 311, 328`

### 4.4 Resource-map key construction

The `Kitten::resources` map (in `KittenAssets.cpp`) and the mesh resources
in `Mesh.cpp` use stringified `std::filesystem::path` as keys. On Windows,
`path.string()` returns native form (`\`); on Linux it returns POSIX form
(`/`). If callers ever look up by a manually composed path, the lookup
silently misses. Make all key construction use `generic_string()`:

- `KittenEngine/KittenEngine/src/KittenAssets.cpp` lines:
  `49`, `69`, `81`, `113-114`, `127`, `140`, `144`, `148`, `152`, `156`, `160`
- `KittenEngine/KittenEngine/src/Mesh.cpp` lines:
  `265`, `268`, `331`, `419` (resource keys / canonical lookups)

`std::ifstream(path.string())` calls (e.g. `Mesh.cpp:423, 452, 485, 597, 640`)
do not need rewriting — `ifstream` accepts either form on either OS.

### 4.5 Verification grep after patches

```bash
# Should return only KittenPreprocessor.cpp:65 (regex \n)
grep -rn '\\\\' KittenEngine/ --include='*.cpp' --include='*.cu' --include='*.h'

# Should return nothing
grep -rn 'fopen_s' KittenEngine/
```

---

## 5. Phase 3 — Build

```bash
cd /home/daizi/Research/YarnBall
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Sanity check that OpenMP is linked into the CUDA TUs:
```bash
nm build/CMakeFiles/YarnBall.dir/KittenEngine/KittenEngine/KittenGpuLBVH/lbvh.cu.o | grep GOMP
```

---

## 6. Phase 4 — Smoke tests

### 6.1 Headless first (lowest-risk path)

```bash
cd build
./Gui --headless configs/cable_work_pattern.json --twist -s -e -n 30 --exit -o /tmp/yb_
ls /tmp/yb_*.obj
```

### 6.2 GUI

```bash
cd build
./Gui configs/cable_work_pattern.json
```

If this hangs / black-screens, the underlying issue is most likely
CUDA-GL interop on WSLg (not GL 4.6 itself — see §7).

---

## 7. Risks and known issues

### 7.1 CUDA-GL interop on WSLg (highest GUI risk)

`KittenEngine/KittenEngine/includes/modules/ComputeBuffer.h:12` includes
`cuda_gl_interop.h`. Mesa-on-D3D12 (WSLg's GL stack) may not expose
the NV interop extensions even though it advertises GL 4.6. This breaks
GUI mode on WSLg even with a fully successful build.

Workaround: run `--headless` on WSL2; run GUI on a real Linux box with
NVIDIA's native driver and X11/Wayland.

### 7.2 16x MSAA may be denied

`KittenInit.cpp:68` calls `glfwWindowHint(GLFW_SAMPLES, 16)` and
`glfwCreateWindow` returns null on denial — `KittenInit.cpp:70` calls
`std::exit(1)` with no fallback. If GUI startup hard-exits, lower the
hint to 4 or 0 as a fallback.

### 7.3 No headless library variant

`YarnBall.h` transitively pulls GLAD/GLFW/ImGui via `KittenEngine.h`.
Even `--headless` requires the full GUI dep set at compile time.
Splitting the lib is out of scope for this migration; document it.

### 7.4 Pre-existing data/runtime issues (not caused by migration)

- `configs/relax0.json` and `configs/relax1.json` reference `.bcc` files
  not in the repo. They will fail at runtime regardless of build target.
- `KittenEngine/YarnBall/io/jsonBuilder.cpp` resolves `curveFile` against
  process CWD instead of config file directory. Fix: rebase with
  `std::filesystem::path(configPath).parent_path()`.

### 7.5 nvcc 12.0 is insufficient

CMake configure will hard-fail on a 5090 with nvcc 12.0 because 12.0
does not know about sm_120. The `find_package(CUDAToolkit 12.8 REQUIRED)`
in §3.1 catches this at configure time with a clear error.

### 7.6 ImGui distro packaging not used

Distro `libimgui-dev` (where it exists) does not consistently ship the
glfw + opengl3 backends needed by the source. FetchContent is the
deliberate choice; do not switch to the distro package. The pinned tag
is plain `v1.91.5` (master, not docking) — the source uses only
`Begin/Text/TreeNode/Checkbox/SliderFloat` and no docking APIs.

### 7.7 Submodule include shadowing

`KittenEngine/KittenEngine/KittenGpuLBVH/` ships a stripped 158-line
`KittenEngine/includes/modules/Common.h` next to the parent's full
798-line `Common.h` — a 640-line gap of helper functions. The submodule
includes it as `<KittenEngine/includes/modules/Common.h>` (angle
brackets), so resolution is determined entirely by `-I` ordering.

Two paths must NOT appear in `target_include_directories`:

1. `KittenEngine/KittenEngine/KittenGpuLBVH/KittenEngine/includes`
   (the literal stub include root)
2. `KittenEngine/KittenEngine/KittenGpuLBVH` itself —
   adding it puts `KittenGpuLBVH/KittenEngine/includes/modules/Common.h`
   one directory hop away from a `<KittenEngine/...>` search, which
   would silently shadow the parent depending on `-I` order

The CMakeLists in this repo deliberately uses
`KittenEngine/KittenEngine/includes` and the repo-root `KittenEngine/`
as the only paths that contain a `KittenEngine/` subtree, so
`<KittenEngine/includes/modules/Common.h>` resolves to the parent's
full file.

---

## 8. Phase 5 — README + helper script

- Append a "Linux build" section to `README.md` with the apt commands
  from §2 and the cmake/ninja commands from §5.
- Add `scripts/install-linux-deps.sh` wrapping the apt invocations.
- Keep the existing Windows/vcpkg instructions intact.

---

## 9. Effort estimate

- ~1–2 days for someone fluent in CMake + CUDA.
- The CMakeLists.txt is the bulk of the work.
- Source patches are ~30 minutes mechanical, plus the 5 `fopen_s` rewrites.
- GLAD vendoring is ~10 minutes (visit glad.dav1d.de, drop the files in,
  add to CMake).
- Smoke tests are immediate once build succeeds.

---

## 9.5 Issues encountered during implementation (beyond original audit)

These surfaced once the build was attempted and were fixed inline. They were
not predictable from a static audit because they're either GCC-vs-MSVC
behavior differences or pre-existing latent bugs that MSVC tolerated.

### 9.5.1 `Mesh.cpp` `fopen_s` (5 sites)
MSVC Annex K. Replaced with plain `fopen(p.c_str(), "w")` + null check.
This was caught in the second-pass review. Critical — won't link.

### 9.5.2 `SymMat.h` anonymous struct in union (2 sites)
GCC rejects anonymous-aggregate members with non-trivial constructors,
even with `-fms-extensions`. GLM's `vec3` has user-defined constructors,
so `union { float dat[6]; struct { vec3 diag; vec3 upperTriangle; }; };`
is a hard error.
Fix: name the inner struct `s` and update access from `H.diag` to
`H.s.diag` at the four call sites in `cosserat.cu`. Memory layout
preserved.

### 9.5.3 `StopWatch.cpp` / `Timer.cpp` chrono mismatch
MSVC: `high_resolution_clock` is an alias for `steady_clock`.
libstdc++: it's an alias for `system_clock`. Header field type was
`steady_clock::time_point`; assigning from `high_resolution_clock::now()`
fails on Linux. Fix: use `steady_clock::now()` consistently (matches
header, monotonic semantics).

### 9.5.4 `Algo.h` unqualified `mix(double,double,double)`
`Common.h` declares `Kitten::mix(mat, mat, T)`. In template functions
inside `namespace Kitten`, two-phase name lookup picks up the local
member and stops — `glm::mix` (visible via using-directive) becomes
unreachable. MSVC's lookup order is permissive enough to find it; GCC
isn't. Fix: qualify three call sites as `glm::mix(...)`.

### 9.5.5 `KittenPreprocessor.cpp:65` regex without `multiline`
**This was a latent bug on Windows too.** The shader-include regex
`^ *#include +...` uses `^` which only matches offset 0 by default.
Since shaders start with `#version`, no `#include` directive ever
matched and the engine's preprocessor was a no-op. On Windows the GL
driver tolerated the unresolved `#include` directives somehow (most
likely silently ignoring with warnings or a path the user never
exercised); Mesa's d3d12 GLSL preprocessor surfaces it as a hard error.
Fix: add `regex::multiline` flag.

### 9.5.6 GL 4.6 → 4.5 + MSAA fallback
WSLg's Mesa-on-D3D12 stack maxes at GL 4.5; native NVIDIA on X11 has
4.6. Lowered to 4.5 (no engine feature actually requires 4.6) and added
a fallback path that retries `glfwCreateWindow` without MSAA if the
first attempt fails. Improves robustness on older drivers in addition
to WSLg.

### 9.5.7 `step.cpp::advance` missing return
Pre-existing UB: function returns `float` but had no `return` statement
on the main path. Added `return 0;`.

### 9.5.8 CUDA architecture detection
`CMAKE_CUDA_ARCHITECTURES native` proved unreliable when `nvcc --run`
fails to query the device (CMake silently falls back to `52`). The
CMakeLists now (a) requires CUDAToolkit ≥ 12.8, (b) uses the toolkit's
`__nvcc_device_query` helper to detect the GPU, (c) falls back to a
multi-arch list `86;89;120` if detection fails. Override with
`-DCMAKE_CUDA_ARCHITECTURES=...` for non-default targets.

### 9.5.9 Distro path quirks
- `libstb-dev` installs to `/usr/include/stb/` (subdirectory). Source
  uses `#include "stb_image.h"` (flat). Fix: add `/usr/include/stb`
  to `STB_INCLUDE_DIR` search PATHS.
- `libjsoncpp-dev` installs to `/usr/include/jsoncpp/json/` and ships
  a CMake config. `find_package(jsoncpp CONFIG)` works on Ubuntu 24.04.

### 9.5.10 `git lfs install` collides with existing pre-push hook
Script bailed under `set -euo pipefail` because the pre-push hook
already existed. Fix: use `git lfs install --force` (overwrites with
the canonical LFS hook content).

### 9.5.11 Ubuntu 24.04 PEP 668
`pip install --user` is blocked. The script now creates a throwaway
venv at `.venv-glad/` for the GLAD generator instead.

### 9.5.12 Final-pass review fixes (post-merge to plan)
A second-round review (fresh-Claude + Codex) caught additional issues
fixed inline:

- **`Font.cpp:188`** — cache key built from `path.string()` (native form)
  but lookup uses forward slashes; Windows-only mismatch silently misses
  the cache. Fixed: `generic_string()`.
- **`Mesh.cpp:303`** — Assimp may emit `\` separators in material texture
  paths; concat with `parent_path / "tex\foo.png"` makes Linux treat the
  RHS as one filename. Fixed: replace `\` → `/` before joining.
- **`scripts/install-linux-deps.sh`** — missing `wget` in step 1 (used
  by step 3); CUDA repo URL hardcoded to `wsl-ubuntu` even on native
  Linux (now `grep -qiE "(microsoft|wsl)" /proc/version` to detect
  WSL); GLAD generation made atomic (writes to `mktemp -d` then
  `mv`) to survive Ctrl-C mid-run; added `wget ca-certificates` to
  the apt list.
- **`CMakeLists.txt` GLAD bootstrap** — replaced the `FATAL_ERROR` with
  auto-generation: if `third_party/glad/` is missing, CMake creates the
  venv and runs the glad generator itself. A fresh `cmake -B build` is
  now self-sufficient on any system with `python3-venv` and network
  access.
- **`CMAKE_CUDA_ARCHITECTURES` detection** — CMake's CMP0104 policy
  auto-initializes the variable to `52` during `project(... LANGUAGES
  CUDA)`, so `if(NOT DEFINED ...)` always skipped detection. Fixed by
  treating `STREQUAL "52"` as the unset sentinel.
- **`Sim::advance` return type** — pre-existing UB (declared `float`,
  no return statement). Changed to `void`; callers were already
  ignoring the return.
- **`install-linux-deps.sh` nvcc-version regex** — didn't match CUDA
  12.10+. Fixed.
- **CMake CUDA helper path** — was hardcoded
  `/usr/local/cuda-12.8/bin/__nvcc_device_query`. Switched to
  `${CUDAToolkit_BIN_DIR}/__nvcc_device_query`.

### 9.5.15 CUDA-GL interop bypass (post-merge enhancement)
The §7.1 risk (WSLg's GL stack lacks NVIDIA's CUDA-GL interop bridge,
causing context destruction) is now handled at runtime:

- **`ComputeBuffer.h`**: added `cudaGLInteropBroken()` — checks
  `glGetString(GL_RENDERER)` once, sets a static flag if the substring
  matches `"D3D12"` (WSLg Mesa-on-D3D12) or `"llvmpipe"` (Mesa software
  fallback). Native NVIDIA renderers don't match → fast interop path
  unchanged.
- **`ComputeBuffer::cudaWriteGL` / `cudaReadGL`**: when the flag is set,
  bypass interop with a host roundtrip
  (`cudaMemcpy DtoH` → `glBufferSubData`, and the symmetric direction
  for read). A `thread_local std::vector<uint8_t>` caches the staging
  buffer to avoid per-frame allocation.
- **`KittenInit.cpp`**: after `gladLoadGLLoader`, prints
  `GL: <vendor> | <renderer> | <version>` so the GL stack is visible
  at startup for diagnostics.
- **No API change**: callers still invoke `vertBuffer->cudaWriteGL(...)`;
  the dispatch is internal.
- **Cost on the fallback path**: ~1–2 ms/frame for the YarnBall sim's
  ~2 MB/frame DtoH+upload at 65k verts. Negligible at 30 FPS.
- **Cost on the fast path**: zero — one `strstr` at startup, then a
  cached branch.

Verified: GUI mode on WSLg (llvmpipe) now runs the cable_work_pattern
twist scenario for 12+ seconds without crashing, zero CUDA errors,
zero shader errors. Headless mode unchanged.

### 9.5.13 Pre-existing issues left as-is
Flagged by reviewers but out of scope for this Linux-migration commit:

- **`KittenPreprocessor.cpp:65` regex matches `#include` inside block
  comments** — a commented-out `#include` would be wrongly substituted.
  Has been latent on Windows too (regex never fired before this commit).
- **`jsonBuilder.cpp:8-22` `curveFile` paths resolve against process
  CWD**, not the config file's directory.
- **`relax0.json` / `relax1.json`** reference `.bcc` files not present
  in the repo.
- **`Sim::step(float h)` ignores its `h` parameter** at `step.cpp:58-60`
  (calls `advance(maxH)` instead of `advance(h)`). Pre-existing.
- **Many `cudaMalloc`/`cudaMemcpy*`/`cudaStream*`/`cudaGraph*` return
  values are unchecked** in `YarnBall.cu` and `step.cpp`. The code
  relies on a single `cudaGetLastError()` at the end. Pre-existing.
- **`Mesh.cpp loadMeshFrom` material loop**: `(Texture*)resources[p]` after
  a failed `loadTexture(p)` inserts `nullptr` into the resource map,
  making subsequent loads early-return with the cached null. Renderer
  guards against null tex, so it's a slow leak rather than a crash.
  Pre-existing.

### 9.5.14a Reverts after over-engineering audit
A pristine-vs-current diff against `../YarnBall_2` flagged a few changes
that had drifted beyond what the migration required. Reverted:

- **`Sim::advance` signature** kept as `float advance(float)` returning 0
  (was briefly changed to `void` in §9.5.12 — public-API change beyond
  the scope of "make it build on Linux"; pre-existing UB-by-omission of
  `return` is fixed minimally with `return 0;`).
- **`Mesh.cpp` sub-mesh printf** (line 336): restored original
  `printf("asset: loading sub-mesh %s (\\%s)\n", path.string().c_str(), ...)`
  — the `\\%s` is a log format string, not a path separator. Plan §1.2
  explicitly said leave it alone.
- **`KittenInit.cpp`** GL request order: now tries the original
  4.6 + 16xMSAA first, and only falls back to 4.5 + 0xMSAA on WSLg/older
  drivers. Native Windows + NVIDIA still gets exactly the original
  context.
- **CMakeLists.txt removals**: deleted `CUDA_SEPARABLE_COMPILATION ON`
  and `CUDA_RESOLVE_DEVICE_SYMBOLS ON` (not in the original .vcxproj —
  added `-rdc=true` which has a real linking cost and was never needed
  here, no cross-TU `__device__` calls). Also removed
  `CMAKE_POSITION_INDEPENDENT_CODE ON` (irrelevant for a static lib that
  only feeds an executable).
- **`scripts/install-linux-deps.sh`**: removed `--no-cuda` flag and the
  argparse loop. The CUDA install step already auto-skips when nvcc is
  recent enough — the flag was redundant.

### 9.5.14 Round-3 review fixes
Final pre-merge review surfaced two filesystem hazards and one logic
correction:

- **`CMakeLists.txt` GLAD staging across filesystems** — `_glad_tmp` was
  under `${CMAKE_BINARY_DIR}` which can be on tmpfs while
  `${CMAKE_SOURCE_DIR}/third_party` is on disk. CMake's `file(RENAME)`
  fails across devices, leaving no `glad/` after the prior
  `REMOVE_RECURSE`. Moved staging dir to
  `${CMAKE_SOURCE_DIR}/third_party/.glad_gen` so the rename is
  same-filesystem.
- **`scripts/install-linux-deps.sh` `mktemp -d`** had the same hazard
  (default `/tmp/` is tmpfs). Now uses
  `mktemp -d "$REPO_ROOT/third_party/.glad.tmp.XXXXXX"`.
- **`CMAKE_CUDA_ARCHITECTURES STREQUAL "52"` sentinel** silently
  overrode legitimate `-DCMAKE_CUDA_ARCHITECTURES=52` from users
  targeting Maxwell. Replaced with cache-type probe:
  ```cmake
  get_property(_cuda_arch_type CACHE CMAKE_CUDA_ARCHITECTURES PROPERTY TYPE)
  if(NOT _cuda_arch_type STREQUAL "UNINITIALIZED")
      # auto-detect
  endif()
  ```
  CMake's policy auto-init produces `type=STRING`; user `-D` produces
  `type=UNINITIALIZED`. Both `-D=52` and `-D=86` are now correctly
  respected.
- **Multi-arch fallback list widened** from `"86;89;120"` to
  `"75;86;89;120"` (Turing→Blackwell) so detection failures on older
  hardware still PTX-JIT cleanly without ~10s startup tax.

---

## 10. Out of scope for this migration

- Splitting `YarnBall` into a true headless sim library (no GL deps).
- Resolving cwd to the binary path so `./Gui` works from anywhere.
- Fixing the `jsonBuilder.cpp` cwd-relative `curveFile` resolution.
- Replacing the missing `.bcc` files referenced by `relax0.json` / `relax1.json`.

These are real issues but pre-existing; tackling them belongs in
follow-up work, not the cross-platform migration.
