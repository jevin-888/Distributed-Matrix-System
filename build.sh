#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build/linux-app"}
BUILD_TYPE=${BUILD_TYPE:-Release}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
ENABLE_ASAN=${ENABLE_ASAN:-OFF}

usage() {
    cat <<'EOF'
Usage: ./build.sh [clean]

Environment:
  BUILD_DIR=<path>       Build directory (default: ./build/linux-app)
  BUILD_TYPE=<type>      CMake build type (default: Release)
  JOBS=<count>           Parallel build jobs
  ENABLE_ASAN=ON|OFF     Enable AddressSanitizer (default: OFF)
EOF
}

case ${1:-} in
    "") ;;
    -h|--help) usage; exit 0 ;;
    clean)
        case "${BUILD_DIR}" in
            "${ROOT_DIR}"|/|"") echo "Refusing to remove unsafe build directory: ${BUILD_DIR}" >&2; exit 2 ;;
        esac
        rm -rf -- "${BUILD_DIR}"
        echo "Removed ${BUILD_DIR}"
        exit 0
        ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
esac

for command in cmake ctest; do
    command -v "${command}" >/dev/null 2>&1 || {
        echo "Missing required command: ${command}" >&2
        exit 1
    }
done

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DENABLE_ASAN="${ENABLE_ASAN}" \
    -DBUILD_TESTING=ON
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

if [[ "${ENABLE_ASAN^^}" == "ON" ]]; then
    ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
        ctest --test-dir "${BUILD_DIR}" --output-on-failure
else
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo "Build and tests completed: ${BUILD_DIR}/distributed-matrix"
