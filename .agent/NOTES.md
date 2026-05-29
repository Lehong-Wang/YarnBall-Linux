---
abstract: "Cross-compiler / cross-platform gotchas surfaced during the Linux
           migration: GCC anonymous-aggregate strictness, CMake CMP0104
           auto-init, libstdc++ chrono aliasing, two-phase template lookup,
           WSLg's missing CUDA-GL interop, latent regex bug. + submodule's
           stripped Common.h that silently shadows. + sim-core internals:
           collision is one BVH path (self == inter-rod), contact is fused
           into the Cosserat solver, query() syncs per substep, rods batch
           into one flat array."
---

# Notes

## Things that look like dead code but aren't

- **`KittenEngine/KittenEngine/KittenGpuLBVH/KittenEngine/includes/modules/Common.h`**
  is a 158-line stripped copy of the parent's 798-line `Common.h` (640-line
  delta). The submodule includes it as `<KittenEngine/includes/modules/Common.h>`
  (angle brackets) so resolution is `-I`-order dependent. **Never add the
  submodule's `KittenEngine/includes` or `KittenGpuLBVH/` itself to
  `target_include_directories`** — it shadows the parent. The current
  CMakeLists deliberately uses only `KittenEngine/KittenEngine/includes`
  and the repo-root `KittenEngine/` so `<KittenEngine/...>` resolves to
  the parent's full file.

## Things that look like dead code AND are dead

- `KittenEngine/KittenEngine/embree/MeshCCD.{h,cpp}` — Embree-dependent
  collision code. Not in the `.vcxproj` `<ItemGroup>`. Don't glob
  `**/*.cpp`; explicitly enumerate sources.
- `KittenEngine/KittenEngine/KittenGpuLBVH/main.cpp` — submodule's
  standalone test driver with `void main(int, char**)`. GCC rejects
  under `-pedantic-errors`. Don't pull it into the lib.
- `KittenEngine/KittenEngine/opt/polynomial.h` — unconditionally
  `#include <intrin.h>`. Currently no TU includes it. Footgun if
  anyone ever does on Linux.
- `arithmetic_sse_*.h` — gated by `USE_SSE && __SSE2__ && LBFGS_FLOAT == 64`,
  none of which are defined. Headers never reach the preprocessor.

## Pitfalls

### GCC rejects anonymous aggregates whose members have non-trivial constructors
MSVC accepts `union { float dat[6]; struct { vec3 diag; vec3 upper; }; };`
because GLM's `vec3` has user-defined constructors. GCC errors with
"member ... with constructor not allowed in anonymous aggregate".
`-fms-extensions` doesn't help (it explicitly disallows ctors in anon
structs). Fix: name the inner struct (`struct s_t { ... } s;`), update
call sites to `.s.diag`. See `SymMat.h`.

### `high_resolution_clock` is `steady_clock` on MSVC, `system_clock` on libstdc++
Header field type was `steady_clock::time_point` — assigning from
`high_resolution_clock::now()` works on MSVC, fails to compile on
GCC/libstdc++. Always pick `steady_clock` explicitly for monotonic
timing in cross-platform code.

### Two-phase name lookup hides `glm::mix` in template scope
`namespace Kitten { using namespace glm; ... }`. Then
`Kitten::mix(mat, mat, T)` is declared. Inside a template function in
namespace Kitten, an unqualified `mix(double, double, double)` resolves
to *only* `Kitten::mix` (the directly-declared overload), even though
the file does `using namespace glm`. MSVC's lookup happens to find the
glm overload via the using-directive; GCC's stricter two-phase lookup
doesn't. Fix: qualify as `glm::mix(...)`.

### CMake's CMP0104 auto-initializes `CMAKE_CUDA_ARCHITECTURES = 52`
Under `project(... LANGUAGES CUDA)`, the variable becomes DEFINED with
value `52` even when the user didn't pass `-D`. `if(NOT DEFINED ...)`
never fires. Distinguish auto-init from explicit user-set via the cache
TYPE: auto-init produces `STRING` (helpstring `"CUDA architectures"`),
user `-D` produces `UNINITIALIZED`. Pattern:

```cmake
get_property(_t CACHE CMAKE_CUDA_ARCHITECTURES PROPERTY TYPE)
if(NOT _t STREQUAL "UNINITIALIZED")  # auto-detect; user didn't set it
```

### CMake's `file(RENAME)` fails across filesystems
`${CMAKE_BINARY_DIR}` is often on tmpfs while `${CMAKE_SOURCE_DIR}` is
on disk. Stage GLAD generation under
`${CMAKE_SOURCE_DIR}/third_party/.glad_gen` so the final rename is
same-filesystem. Same applies to shell `mv` after `mktemp -d` (default
template puts the dir under `/tmp`).

### `KittenPreprocessor.cpp:65` regex needs `regex::multiline`
The `^ *#include +...` regex without the multiline flag matches only
at offset 0 of the input string. Shaders start with `#version`, so the
regex never matched anything before this fix — the engine's GLSL
`#include` resolution was a silent no-op. Mesa surfaces this as a hard
GL compile error; native NVIDIA driver tolerates the unresolved
directives somehow. Both platforms now correctly inline because of the
multiline flag.

### WSLg's GL stack lacks CUDA-GL interop
`glGetString(GL_RENDERER)` returns `"Mesa | llvmpipe (...)"` (software)
or `"D3D12 (NVIDIA ...)"` (Mesa-on-D3D12) on WSLg.
`cudaGraphicsGLRegisterBuffer` then destroys the CUDA context — the
crash surfaces in the next CUDA call (e.g. an LBVH kernel) as
`"context is destroyed"`. Detection in `ComputeBuffer.h::cudaGLInteropBroken()`
forces a host-roundtrip path on these renderers; native NVIDIA keeps
the original interop path.

### Engine asset paths are cwd-relative
`Kit::loadDirectory("KittenEngine/shaders")` resolves against process
CWD, not the binary's directory. Must `cd build/` before running. On
Windows, Visual Studio's debugger silently sets CWD to `$(OutDir)` so
this never bites; on Linux it's a paper cut.

### `path.string()` differs by platform; `generic_string()` doesn't
On Windows, `std::filesystem::path::string()` returns native form (`\`).
On Linux, returns POSIX form (`/`). Resource-map keys built from
`path.string()` differ across platforms, so cross-platform lookup by
literal fails. Always use `generic_string()` for keys / lookups; keep
`string()` for I/O calls (file open APIs accept native form).

### Assimp emits Windows-style separators in material texture paths
A `.obj`/`.fbx` with `map_Kd textures\foo.png` returns `aiString` with
`\` even on Linux. Concatenating `parent / "textures\foo.png"` on Linux
treats the whole RHS as one filename. Normalize before joining:
`std::replace(s.begin(), s.end(), '\\', '/')`.

## Architecture quirks

- **No headless library variant.** `YarnBall.h` transitively pulls
  GLAD/GLFW/ImGui via `KittenEngine.h`. Even `--headless` mode requires
  the full GUI dep set at compile time. Splitting would be a real
  refactor; out of scope so far. (That said, the *physics* TUs use no GL
  symbols — only `Kit::Rotor` / `hess3` / `segmentClosestPoints` /
  `pow2` / `length2` / `Bound` (all header-only) + `Kit::LBVH`. GL enters
  solely via the `KittenEngine.h` umbrella include and the
  `ComputeBuffer`/`Mesh` render members of `Sim`. So a headless sim lib
  is mostly drop-the-render-members + swap the umbrella include, not a
  deep refactor.)
- **Sim's GL ComputeBuffers are guarded by `glGetStringi` truthiness**
  (`YarnBall.cu:160, 191`). Headless mode (no GL context) → glad's
  function pointers are null → branches skipped. The sim works without
  a GL context as long as `render()` is never called. `--headless` in
  `main.cpp` skips the render loop entirely.
- **CUDA Graphs are captured once per `numItr` change** in
  `step.cpp::rebuildCUDAGraph`. The first `advance(h)` does the capture
  (slow); subsequent calls just `cudaGraphLaunch`.
- **Many `cudaMalloc`/`cudaMemcpy*`/`cudaStream*` returns are unchecked.**
  The code relies on a single `cudaGetLastError()` at the end of
  `configure()`/`advance()` to surface failures. Fragile but pre-existing.
- **`Sim::step(float h)` ignores its `h` parameter** — it calls
  `advance(maxH)`, not `advance(h)`. Pre-existing bug.
- **Collision is a single code path; there is no "self-collision only"
  toggle.** `detectCollisions` builds one global LBVH over *all*
  segments; `buildCollisionList` (collision.cu) exempts only adjacent
  (`|i-j| ≤ 2`) and glued segments. Self-collision and inter-rod
  collision are therefore the same mechanism. Consequence: one rod per
  `Sim` ⇒ only self-collision by construction; many rods in one `Sim`
  with collisions on ⇒ they all collide with each other. Turn off via
  `detectionPeriod ≤ 0` (the contact loop reads `d_numCols`, which stays
  0, so it's skipped).
- **Contact response is fused into `cosseratItr`, not a separate phase.**
  The IPC barrier + friction energy is evaluated *inside* the solver
  kernel (cosserat.cu:81–146), so it runs `numItr`× per substep; broad/
  narrow-phase detection runs 1× per detection step. Contacts are
  detected once but resolved `numItr` times — the solver kernel
  dominates runtime, and you cannot time "contact response" separately
  from the solver by kernel.
- **`bvh.query()` forces a CPU↔GPU sync every detection step.** It
  returns `std::min((size_t)impl->d_flags[0], resSize)` (lbvh.cu:399/419);
  reading element `[0]` of a `thrust::device_vector` is a synchronizing
  copy. `compute()` reads `d_flags[0]` too (line 379) but only on a full
  rebuild (~every `bvhRebuildPeriod`). So collisions-on ⇒ a sync per
  substep, on top of the one `checkErrors` sync per `advance()`.
  Disabling collision removes the per-substep sync.
- **Independent rods share one flat vertex array.** `createFromCurves`
  packs every curve into one `Sim`, terminating each by
  `verts[last].flags = 0` (reader.cpp:35). The `hasNext`/`hasPrev` flag
  guards in `cosseratItr`/`quaternionLambdaItr` stop force and
  orientation propagation at each boundary, so N rods simulate correctly
  in a single kernel sweep with zero cross-talk when collision is off.
  Batching many rods is a data-layout decision, not a kernel change.
