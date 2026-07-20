#!/bin/sh

# cross-compile from lin to win
# it will build but asi will not work
# it's mainly for convenience to not boot up win vm xd

set -e

PROJECT_DIR="$(pwd)"

IMAGE="registry.gitlab.com/superewald/llwin:17-debug-wine"
CONFIG="${1:-Release}"

echo "building claudia ($CONFIG) in $PROJECT_DIR..."

docker run --rm \
    -v "$PROJECT_DIR:/src" \
    -w /src \
    "$IMAGE" \
    sh -c "
        wmake -B build/$CONFIG -A Win32
        cmake --build build/$CONFIG -j\$(nproc)
    "

echo ""
echo "build complete: build/$CONFIG/bin/claudia.asi + dinput8.dll"
