---
abstract: "Why CMake-alongside-MSBuild instead of replacing it; FetchContent
           for ImGui/CLI11 vs distro packages; auto-generate GLAD vs commit
           it; runtime CUDA-GL interop bypass via renderer-string detection."
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
