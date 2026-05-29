---
abstract: "Linux: ./scripts/install-linux-deps.sh once, then cmake -B build &&
           ninja -C build, run from build/. Windows: open YarnBall.sln in VS.
           No test suite. Full reference: README_LINUX.md."
---

# How to do things

There is no test suite or lint config. Build = correctness; behavior
verification is by smoke-running headless export.

## Linux build (one-time setup)

```bash
./scripts/install-linux-deps.sh
```

Apt deps + CUDA 12.8 toolkit (auto-skipped if already adequate) +
git-lfs pull + GLAD generation. Re-runnable; idempotent.

## Linux build (per change)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

`build/Gui` and `build/libYarnBall.a` are the artifacts. Resources
(configs/, resources/, KittenEngine/shaders/, KittenEngine/fonts/) get
copied next to the binary by the post-build step.

To target a specific GPU: `-DCMAKE_CUDA_ARCHITECTURES=86` (3090) /
`89` (4090) / `120` (5090). Default is auto-detected.

## Windows build

Open `YarnBall.sln` in Visual Studio 2022. vcpkg manifest covers
the deps. CUDA 12.8 toolkit must be installed.

## Run smoke tests

Always `cd build` first.

```bash
# Headless export (no GL needed)
./Gui --headless configs/letterS.json -e -n 3 --exit -o /tmp/yt_

# Headless twist scenario
./Gui --headless configs/cable_work_pattern.json --twist -s -e -n 30 --exit -o /tmp/yt_

# GUI (requires DISPLAY; on WSLg uses llvmpipe via the bypass)
DISPLAY=:0 ./Gui configs/cable_work_pattern.json --twist -s
```

After a successful run you should see something like
`Export complete. sim/real ratio Avg 1.7, SD: 0.1, N=31`. Ratio > 1
means faster-than-realtime.

## Verify a source change didn't regress

- `ninja -C build` builds clean (no `FAILED:`, only the harmless
  `Gizmos.h:30` typedef warning)
- Headless letterS exports 3 frames at exit 0
- Headless cable_work_pattern --twist 30 frames exits 0 with 31 OBJs

## Verify CUDA arch auto-detect works

```bash
rm -rf build && cmake -S . -B build -G Ninja
# expect:  -- CUDA architectures: 120     (or your GPU's CC)
cmake -S . -B /tmp/test -DCMAKE_CUDA_ARCHITECTURES=86 .
# expect:  -- CUDA architectures: 86      (override respected)
```

## Useful diagnostic

`build/Gui` prints `GL: <vendor> | <renderer> | <version>` on GUI
startup so you can tell at a glance whether you're on
`NVIDIA GeForce ...` (fast interop path) or
`Mesa | llvmpipe ...` / `Mesa | D3D12 ...` (host-roundtrip bypass).

## Full Linux reference

`README_LINUX.md` has CLI flags, available scenes, troubleshooting,
and the migration summary table.
