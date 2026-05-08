#!/usr/bin/env bash
# YarnBall — Linux dependency setup.
# Targets Ubuntu 24.04 (and WSL2 thereof). Run from repo root.
#
# Usage: scripts/install-linux-deps.sh
#
# CUDA toolkit install is skipped automatically if a recent enough nvcc
# (>= 12.8) is already on PATH.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

echo "[1/5] Toolchain (build-essential, cmake, ninja, pkg-config, git, git-lfs)"
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config git git-lfs \
                    python3-venv wget ca-certificates

echo "[2/5] Pulling LFS-tracked files (.bcc model assets)"
# `git lfs install` fails if a pre-push hook already exists — use --force to
# overwrite (safe; the hook content is just the canonical LFS pre-push).
git lfs install --force
git lfs pull

echo "[3/5] CUDA Toolkit 12.8 (required for sm_120 / RTX 50-series)"
if ! command -v nvcc >/dev/null || ! nvcc --version | grep -qE "release (12\.([89]|[1-9][0-9])|1[3-9]\.)"; then
    # Pick the right NVIDIA repo: WSL2 forwards the Windows-host driver, so
    # the wsl-ubuntu repo ships a userspace toolkit without a kernel-module
    # package. Native Linux uses the regular ubuntu2404 repo.
    if grep -qiE "(microsoft|wsl)" /proc/version 2>/dev/null; then
        CUDA_REPO_PATH="wsl-ubuntu"
        echo "  → detected WSL2; using wsl-ubuntu CUDA repo"
    else
        CUDA_REPO_PATH="ubuntu2404"
        echo "  → native Linux; using ubuntu2404 CUDA repo"
    fi
    TMPDIR_KEYRING="$(mktemp -d)"
    pushd "$TMPDIR_KEYRING" >/dev/null
    wget -q "https://developer.download.nvidia.com/compute/cuda/repos/${CUDA_REPO_PATH}/x86_64/cuda-keyring_1.1-1_all.deb"
    sudo dpkg -i cuda-keyring_1.1-1_all.deb
    popd >/dev/null
    rm -rf "$TMPDIR_KEYRING"
    sudo apt update
    sudo apt install -y cuda-toolkit-12-8
    echo "  → add /usr/local/cuda-12.8/bin to PATH and /usr/local/cuda-12.8/lib64 to LD_LIBRARY_PATH"
else
    echo "  → nvcc $(nvcc --version | grep release) already adequate, skipping"
fi

echo "[4/5] System libraries (glfw, assimp, freetype, glm, eigen, jsoncpp, stb)"
sudo apt install -y \
    libglfw3-dev libglew-dev libassimp-dev libfreetype-dev \
    libglm-dev libeigen3-dev libjsoncpp-dev libstb-dev

echo "[5/5] Vendoring GLAD (glad1 ABI: <glad/glad.h> + gladLoadGLLoader)"
GLAD_OUT="$REPO_ROOT/third_party/glad"
# Validate both .c and .h to avoid skipping a half-generated tree from a
# previous interrupted run.
if [[ ! -f "$GLAD_OUT/src/glad.c" || ! -f "$GLAD_OUT/include/glad/glad.h" ]]; then
    # Ubuntu 24.04 system Python is externally-managed (PEP 668), so use a venv
    # rather than --user. The venv is throwaway — only needed at setup time.
    GLAD_VENV="$REPO_ROOT/.venv-glad"
    if [[ ! -x "$GLAD_VENV/bin/glad" ]]; then
        python3 -m venv "$GLAD_VENV"
        "$GLAD_VENV/bin/pip" install --quiet glad
    fi
    # Generate into a temp dir on the SAME filesystem as the destination
    # so `mv` is rename(2) (atomic) rather than a copy that can be torn
    # by Ctrl-C. Matters when /tmp is tmpfs and the repo is on disk.
    mkdir -p "$REPO_ROOT/third_party"
    GLAD_TMP="$(mktemp -d "$REPO_ROOT/third_party/.glad.tmp.XXXXXX")"
    "$GLAD_VENV/bin/glad" \
        --generator c \
        --spec gl \
        --profile core \
        --api "gl=4.6" \
        --extensions= \
        --out-path "$GLAD_TMP"
    rm -rf "$GLAD_OUT"
    mv "$GLAD_TMP" "$GLAD_OUT"
    echo "  → generated $GLAD_OUT"
else
    echo "  → $GLAD_OUT already present, skipping"
fi

echo
echo "All dependencies in place. Configure:"
echo "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release"
echo "  ninja -C build"
