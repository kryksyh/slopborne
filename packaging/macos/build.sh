#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ $(uname -s) == Darwin ]] || { echo 'This build script requires macOS.' >&2; exit 1; }
runtime_only=0
test_build=0
bootstrap=0
performance=0
for arg in "$@"; do
    case "$arg" in
        --runtime-only) runtime_only=1 ;;
        --test) test_build=1 ;;
        --bootstrap) bootstrap=1 ;;
        --performance) performance=1 ;;
        *) echo "Usage: bash build.sh [--bootstrap] [--test] [--runtime-only] [--performance]" >&2; exit 1 ;;
    esac
done
for tool in clang cmake ninja python3 pkg-config; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
if [[ $(uname -m) == arm64 ]]; then
    arch -x86_64 /usr/bin/true || { echo 'Rosetta is required to run the x86-64 guest and build tools.' >&2; exit 1; }
fi
prefix="$PWD/out/macos/prefix"
mkdir -p out/macos
if (( bootstrap )); then
    if (( runtime_only )); then
        python3 packaging/macos/bootstrap.py --runtime-only
    else
        python3 packaging/macos/bootstrap.py
    fi
fi
[[ -f third_party/LibAtrac9/C/src/libatrac9.h ]] || git submodule update --init third_party/LibAtrac9
if (( runtime_only || test_build )); then
    cmake -S packaging/macos -B out/macos/runtime -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
    cmake --build out/macos/runtime -j "${BB_JOBS:-6}"
    ctest --test-dir out/macos/runtime --output-on-failure
fi
if (( runtime_only )); then
    echo 'Built out/macos/runtime/bb-probe-cpu (CPU diagnostics only; no game renderer).'
    exit 0
fi
# Host tools and header-only packages may be ARM64. All linked libraries must be Intel.
host_prefix=${BB_MACOS_HOST_PREFIX:-$(brew --prefix)}
intel_prefix=${BB_MACOS_INTEL_PREFIX:-/usr/local}
# bootstrap.py provides MoltenVK when there is no Intel Homebrew.
[[ -z ${BB_MACOS_INTEL_PREFIX:-} && -f $prefix/lib/libMoltenVK.dylib ]] && intel_prefix=$prefix
for lib in "$prefix/lib/libSDL3.dylib" "$prefix/lib/libavcodec.dylib" \
           "$prefix/lib/libcrypto.dylib" "$intel_prefix/lib/libMoltenVK.dylib"; do
    [[ -f $lib ]] || { echo "Missing $lib; see packaging/macos/README.md (or use --bootstrap)." >&2; exit 1; }
    lipo "$lib" -verify_arch x86_64 || { echo "Need an x86_64 library: $lib" >&2; exit 1; }
done
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
      ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:$intel_prefix/lib/pkgconfig:/usr/share/pkgconfig"
# Do not allow an inherited ARM Homebrew search path to override the Intel prefix.
unset PKG_CONFIG_PATH
gpu_build_dir="$PWD/out/macos/gpu"
gpu_build_type=RelWithDebInfo
gpu_lto=${BB_LTO:-OFF}
if (( performance )); then
    gpu_build_dir="$PWD/out/macos/gpu-performance"
    gpu_build_type=Release
    gpu_lto=${BB_LTO:-ON}
fi
cmake -S gpu -B "$gpu_build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$gpu_build_type" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
    -DCMAKE_PREFIX_PATH="$prefix;$intel_prefix;$host_prefix/opt/boost" \
    -DVulkan_INCLUDE_DIR="$host_prefix/include" -DVulkan_LIBRARY="$intel_prefix/lib/libMoltenVK.dylib" \
    -DOPENSSL_INCLUDE_DIR="$prefix/include" -DOPENSSL_CRYPTO_LIBRARY="$prefix/lib/libcrypto.dylib" \
    -DOPENSSL_SSL_LIBRARY="$prefix/lib/libssl.dylib" -DBB_LTO="$gpu_lto" -DBB_PGO=off
if ! cmake --build "$gpu_build_dir" --target bb-probe bb-gpu-capabilities bb-controller-info -j "${BB_JOBS:-6}" > out/macos/gpu-build.log 2>&1; then
    tail -60 out/macos/gpu-build.log >&2
    echo 'Build failed; full log: out/macos/gpu-build.log' >&2
    exit 1
fi
# Keep Linux/Windows build products separate; run-macos.sh uses this directory.
echo "Built $gpu_build_dir/bb-probe"
if (( test_build )); then
    "$gpu_build_dir/bb-probe" --vulkan-only
    cmake --build "$gpu_build_dir" --target robustness-shader-test hle-thread-test dense-arena-test pm4-packet-reuse-test gpu-wait-test -j "${BB_JOBS:-6}"
    python3 tests/test_gpu_wait.py "$gpu_build_dir/gpu-wait-test"
    python3 -c 'import subprocess, sys; subprocess.run([sys.argv[1]], check=True, timeout=30)' "$gpu_build_dir/hle-thread-test"
    python3 -c 'import subprocess, sys; subprocess.run([sys.argv[1]], check=True, timeout=60)' "$gpu_build_dir/dense-arena-test"
    python3 -c 'import subprocess, sys; subprocess.run([sys.argv[1]], check=True, timeout=30)' "$gpu_build_dir/pm4-packet-reuse-test"
    "$gpu_build_dir/robustness-shader-test" out/macos/robustness
    command -v spirv-val >/dev/null || { echo 'Tests require spirv-val (brew install spirv-tools).' >&2; exit 1; }
    for shader in buffers images widths shared dma; do
        spirv-val --target-env vulkan1.3 "out/macos/robustness/$shader.spv"
    done
    BB_TEST_BIN_DIR="$gpu_build_dir" python3 -m unittest discover -s tests -p test_probe.py
fi
