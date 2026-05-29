---
abstract: "Why CMake-alongside-MSBuild instead of replacing it; FetchContent
           for ImGui/CLI11 vs distro packages; auto-generate GLAD vs commit
           it; runtime CUDA-GL interop bypass via renderer-string detection.
           + (proposed) port the de-collisioned Cosserat solver to NVIDIA
           Warp for batched Isaac Lab RL."
---

# Decisions

Each entry: a date, a decision, a status, a short rationale. Statuses:
`proposed` (open question / future plan), `accepted` (decided + in effect),
`superseded` (replaced — link to the replacement), `rejected` (considered + dropped).

When this file passes ~400 lines or you find yourself wanting to link to a
specific decision from a commit, split into `decisions/NNNN-slug.md`.

---

### 2026-05-08 — CMake build alongside the existing MSBuild path
- **Status**: accepted
- **Context**: Linux support was needed for a WSL2-based dev box.
  Replacing the `.vcxproj` would have broken Windows users; the original
  Windows authors continue using Visual Studio.
- **Decision**: Add a top-level `CMakeLists.txt` and `scripts/install-linux-deps.sh`
  next to the existing `.sln`/`.vcxproj`. Both build paths exist;
  neither references the other. Source files are platform-neutral so
  both build the same code.
- **Why**: Lower risk, zero regression on Windows, and `.vcxproj` /
  CMake have hostile semantics to keep in sync (RDC modes, OpenMP flag
  spelling, resource staging steps).
- **Rejected**: (a) replace `.vcxproj` with CMake → breaks Windows
  workflow; (b) hand-rolled Makefile → loses CUDA-aware language
  integration, no cross-platform.

### 2026-05-08 — FetchContent for ImGui + CLI11; system packages elsewhere
- **Status**: accepted
- **Context**: distro packages on Ubuntu 24.04 are inconsistent:
  `libimgui-dev` (where it exists) doesn't ship the `glfw` + `opengl3`
  backends the source uses; `libcli11-dev` is fine but version-pinning
  is desirable; system `libglfw3-dev`, `libassimp-dev`,
  `libfreetype-dev`, `libglm-dev`, `libeigen3-dev`, `libjsoncpp-dev`,
  `libstb-dev` are all functional and stable.
- **Decision**: ImGui and CLI11 via `FetchContent_Declare` pinned to
  versioned tags (`v1.91.5`, `v2.4.2`). Everything else from apt.
- **Why**: ImGui needs explicit backend TUs (`imgui_impl_glfw.cpp`,
  `imgui_impl_opengl3.cpp`) compiled in a specific way that distro
  packages don't reliably ship. CLI11 is header-only and small, but
  pinning a known-good version is cheaper than tracking distro
  packaging drift.
- **Rejected**: vcpkg-on-Linux (works but introduces a second package
  manager and longer first-build); vendor everything (hundreds of MB
  of third-party code in-tree).

### 2026-05-08 — Auto-generate GLAD via Python `glad` venv at configure time
- **Status**: accepted
- **Context**: The source uses the glad1 ABI (`<glad/glad.h>` +
  `gladLoadGLLoader`). Mainline `Dav1dde/glad` defaults to glad2 which
  is not drop-in. There's no Ubuntu `libglad-dev` package.
- **Decision**: CMake checks for `third_party/glad/src/glad.c` +
  `third_party/glad/include/glad/glad.h`. If missing, creates
  `.venv-glad/`, `pip install glad`, runs the generator into a
  same-filesystem temp dir, then atomically renames into place. Same
  recipe in `scripts/install-linux-deps.sh` step 5. `third_party/glad/`
  is gitignored.
- **Why**: Self-bootstrapping fresh clone with no committed generated
  source. Atomic rename survives Ctrl-C mid-generation. `.venv-glad/`
  isolates Python tooling from system Python (which is PEP 668 locked
  on Ubuntu 24.04).
- **Rejected**: (a) commit `third_party/glad/` — works but pollutes
  diffs and fixes the GL version at commit time; (b) `Dav1dde/glad`
  CMake helper — defaults to glad2, requires output-mode flags that
  vary across tag versions; (c) require user to install GLAD manually
  — defeats the "fresh clone, run script, build" promise.

### 2026-05-08 — Runtime CUDA-GL interop bypass via renderer-string detection
- **Status**: accepted
- **Context**: WSLg's GL stack (Mesa-on-D3D12 or llvmpipe) doesn't
  expose NVIDIA's CUDA-GL interop bridge.
  `cudaGraphicsGLRegisterBuffer` destroys the CUDA context, surfacing
  later as `"context is destroyed"` errors in unrelated CUDA calls.
  GUI mode is unusable on WSL2 without a fix.
- **Decision**: At first `cudaWriteGL`/`cudaReadGL` call, probe
  `glGetString(GL_RENDERER)` for substring `"D3D12"` or `"llvmpipe"`.
  Cache the result. On match, replace interop with a host roundtrip
  (`cudaMemcpy DtoH` → `glBufferSubData` for write; symmetric for
  read). Native NVIDIA renderers fail both substrings → existing fast
  interop path runs unchanged.
- **Why**: Zero overhead and zero behavior change on every working
  setup. Targeted, automatic, no env-var or CMake flag plumbing.
  The bypass cost is ~1–2 ms/frame at 65k verts — negligible at
  30 FPS.
- **Rejected**: (a) try-then-fallback (first failed interop call
  destroys the CUDA context, so we can't recover); (b) compile-time
  switch (would require a new CMake option and a way to detect WSLg
  at configure); (c) require a native GL stack (would force users off
  WSL2 entirely).

### 2026-05-08 — Preserve `Sim::advance` as `float`-returning to avoid public-API change
- **Status**: accepted
- **Context**: Pre-existing UB: `float Sim::advance(float)` had no
  `return` statement on the main path. GCC compiles it; some compilers
  warn. The minimal fix is `return 0;`. Briefly changed the signature
  to `void` during round-2 review.
- **Decision**: Reverted to `float advance(float)` returning 0. The
  public header signature stays identical to the original.
- **Why**: External consumers (the C++ interface documented in README)
  could be linking against this header. A `void` change breaks
  source-compat for anyone who took the return value, even if no
  in-tree caller does. Scope was "make it build on Linux", not "fix
  every latent API smell".
- **Supersedes**: an earlier `void` change in the same migration
  (uncommitted, never shipped).

### 2026-05-29 — (Proposed) Port the de-collisioned Cosserat solver to NVIDIA Warp for batched Isaac Lab RL
- **Status**: proposed
- **Context**: A downstream effort (not YarnBall maintenance) wants the
  Cosserat rod solver running inside Isaac Lab for massively-parallel RL —
  ~1000 rod-per-arm envs, max throughput. Locked scope: no collision,
  kinematic-pin coupling only (rod follows the gripper, no reaction force),
  Cosserat fidelity required. Relies on the internals in
  `NOTES.md ## Architecture quirks` (collision is one BVH path; contact is
  fused in the solver; rods already batch into one flat array).
- **Decision**: Port the ~150-line de-collisioned solver to Warp (Python
  JIT-to-CUDA). Warp is Isaac Lab's native kernel layer (auto-batch, CUDA
  graph capture, zero-copy torch interop) and has no per-platform native
  artifact to build.
- **Rough plan**:
  0. *Validation harness first* — run reference YarnBall headless on one rod
     with a scripted pin trajectory; dump per-step pos+quat as ground truth.
  1. State = flat `N*V` Warp arrays (pos/vel/last_vel/dx/last_pos:vec3,
     q/q_rest:quat, inv_mass/k_stretch/l_rest:float, flags:uint32).
  2. Four kernels 1:1 with the solver: `init_itr`, `cosserat_itr` (drop the
     contact loop; replace the shared-mem sector reduction with a direct
     neighbor gather over segments [tid-1,tid] and [tid,tid+1] gated by
     hasPrev/hasNext), `quaternion_itr`, `end_itr`.
  3. Capture `init + num_itr*(cosserat+quaternion) + end` in a
     `wp.ScopedCapture` graph (on a Warp stream — default stream can't be
     captured); `advance(dt)` = `wp.capture_launch` per substep, no host sync.
  4. Validate against the Phase-0 dump; tune `num_itr`; compare on the SAME
     GPU to avoid FP-arch drift.
  5. RL glue kernels: `apply_pins(ee_pose[N,7])` + `reset_envs(env_ids)`
     (zero vel/last_vel/last_pos on reset). Per-vertex params already enable
     domain randomization.
  6. Isaac Lab: subclass `DirectRLEnv`; post-PhysX, `wp.from_torch` the EE
     pose → apply_pins → capture_launch → `wp.to_torch(pos)` as obs. Cache
     the torch↔warp views and use the `from_torch(return_ctype=...)` fast
     path to stay sync-free; headless, no rendering.
- **Two review findings that simplify the port**:
  - With collision gone the position Hessian is `s*I` (all remaining terms —
    inertia, stretch, connection — are isotropic-diagonal; the off-diagonal
    `hess3::outer` was contact-only). The per-vertex 3x3 solve collapses to a
    scalar division `dx += accel_ratio * f / s`.
  - `wp.quat` layout `(i,j,k,w)` with `w` real matches `Kit::Rotor` exactly →
    orientation DOFs port with no re-derivation.
- **Why Warp over the alternatives**: native Isaac Lab integration, no
  device-pointer/stream plumbing, no per-platform binary, future-friendly
  toward Newton (the Warp-based engine now in Isaac Lab). Cost: re-validate
  a rewrite (hence Phase 0).
- **Rejected**: (a) CUDA `.so` binding — reuses validated math but ties you
  to a Linux `.so`/Windows DLL plus manual stream/pointer plumbing against
  Isaac; (b) PhysX capsule rope — free + two-way collision but loses the
  Cosserat twist/bend fidelity that is the reason to use this solver;
  (c) reuse an existing Warp/Newton rod — none ships (Newton has cloth-VBD +
  MPM, not rods).
- **Open**: whether RL later needs self-collision (reintroduces the BVH +
  per-substep sync, and inter-env coupling unless envs are spaced beyond rod
  reach) or two-way coupling (extract the pin reaction force). Platform:
  prototype on WSL2, run real training on native Linux. See user-level
  memory `isaac-rl-rod-port` for the full scope record.
