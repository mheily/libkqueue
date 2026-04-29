#!/bin/bash
#
# Run one Linux CI matrix entry inside a docker container.  Mirrors
# .github/workflows/ci-linux.yml without the musl row.
#
# Usage:
#   tools/ci-linux-local.sh                       # run all 4 variants
#   tools/ci-linux-local.sh release-gcc           # one specific variant
#   tools/ci-linux-local.sh release-clang debug-asan
#
# Variants:
#   release-gcc       gcc-14, Release
#   release-clang     clang-20, Release
#   debug-asan        clang-20, Debug, ASAN+LSAN+UBSAN
#   debug-tsan        clang-20, Debug, TSAN
#
# The container caches apt artefacts in tools/ci-linux-cache/ so
# re-runs don't reinstall toolchains every time.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CACHE="$REPO/tools/ci-linux-cache"
IMAGE="ubuntu:24.04"

ALL=(release-gcc release-clang debug-asan debug-tsan)

if [ "${INSIDE_CONTAINER:-0}" = "1" ]; then
    # ------------------ inside-container path ------------------
    variant="$1"
    case "$variant" in
        release-gcc)   CC=gcc;   BUILD_TYPE=Release; ASAN=NO; LSAN=NO; UBSAN=NO; TSAN=NO ;;
        release-clang) CC=clang; BUILD_TYPE=Release; ASAN=NO; LSAN=NO; UBSAN=NO; TSAN=NO ;;
        debug-asan)    CC=clang; BUILD_TYPE=Debug;   ASAN=YES; LSAN=YES; UBSAN=YES; TSAN=NO ;;
        debug-tsan)    CC=clang; BUILD_TYPE=Debug;   ASAN=NO; LSAN=NO; UBSAN=NO; TSAN=YES ;;
        *) echo "unknown variant: $variant" >&2; exit 2 ;;
    esac

    export DEBIAN_FRONTEND=noninteractive

    apt-get update -qq
    apt-get install -y --no-install-recommends \
        ca-certificates curl gpg lsb-release \
        build-essential debhelper devscripts dh-make fakeroot \
        cmake make >/dev/null

    if [ "$CC" = "clang" ]; then
        if ! command -v clang-20 >/dev/null; then
            curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key \
                | gpg --dearmor > /usr/share/keyrings/llvm.gpg
            REL=$(lsb_release -c | awk '{print $2}')
            echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/${REL}/ llvm-toolchain-${REL}-20 main" \
                > /etc/apt/sources.list.d/llvm.list
            apt-get update -qq
            apt-get install -y --no-install-recommends \
                clang-20 llvm-20 libclang-rt-20-dev gdb >/dev/null
            update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 60
            update-alternatives --install /usr/bin/llvm-symbolizer llvm-symbolizer /usr/bin/llvm-symbolizer-20 60
        fi
    elif [ "$CC" = "gcc" ]; then
        if ! command -v gcc-14 >/dev/null; then
            apt-get install -y --no-install-recommends software-properties-common >/dev/null
            add-apt-repository -y ppa:ubuntu-toolchain-r/test
            apt-get update -qq
            apt-get install -y --no-install-recommends gcc-14 gdb >/dev/null
            update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 9999
        fi
    fi

    BUILD_DIR="/build/$variant"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    CC="$CC" cmake /src \
        -G "Unix Makefiles" \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_VERBOSE_MAKEFILE:BOOL=OFF \
        -DENABLE_TESTING=YES \
        -DENABLE_ASAN="$ASAN" \
        -DENABLE_LSAN="$LSAN" \
        -DENABLE_UBSAN="$UBSAN" \
        -DENABLE_TSAN="$TSAN" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" 2>&1 | tail -20

    make -j2

    export ASAN_OPTIONS="symbolize=1 detect_leaks=1 detect_stack_use_after_return=1"
    export LSAN_OPTIONS="fast_unwind_on_malloc=0:malloc_context_size=50"
    export TSAN_OPTIONS="suppressions=/src/tools/tsan.supp"
    export KQUEUE_DEBUG=1
    export M_PERTURB="0x42"

    test/libkqueue-test
    exit 0
fi

# ------------------ outside-container orchestrator ------------------
mkdir -p "$CACHE"

variants=("${@:-${ALL[@]}}")

# Validate variant names early.
for v in "${variants[@]}"; do
    found=0
    for a in "${ALL[@]}"; do [ "$a" = "$v" ] && found=1; done
    if [ "$found" = "0" ]; then
        echo "unknown variant: $v" >&2
        echo "valid: ${ALL[*]}" >&2
        exit 2
    fi
done

results=()
for v in "${variants[@]}"; do
    echo "================================================================"
    echo "  variant: $v"
    echo "================================================================"
    if docker run --rm \
            -e INSIDE_CONTAINER=1 \
            -v "$REPO:/src:ro" \
            -v "$CACHE/$v:/build" \
            -v "$CACHE/apt-$v:/var/cache/apt/archives" \
            "$IMAGE" \
            bash /src/tools/ci-linux-local.sh "$v"; then
        results+=("PASS  $v")
    else
        results+=("FAIL  $v")
    fi
done

echo "================================================================"
echo "  summary"
echo "================================================================"
for r in "${results[@]}"; do echo "  $r"; done

# Non-zero exit if any failed.
for r in "${results[@]}"; do
    [[ "$r" == FAIL* ]] && exit 1
done
exit 0
