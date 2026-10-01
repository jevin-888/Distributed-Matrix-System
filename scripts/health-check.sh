#!/usr/bin/env bash
set -u

ROLE=${ROLE:-master}
MASTER_URL=${MASTER_URL:-http://127.0.0.1:8080}
CACHE_DIR=${CACHE_DIR:-/tmp/distributed_matrix/video_cache}
PASS=0
WARN=0
FAIL=0

usage() {
    cat <<'EOF'
Usage: health-check.sh [--role master|slave] [--master-url URL] [--cache-dir DIR]

Checks only the production Distributed Matrix System video path:
HTTP status, process, cache, GStreamer/RKMPP and DRM/KMS.
EOF
}

while (($#)); do
    case $1 in
        --role) [[ $# -ge 2 ]] || { echo "--role requires a value" >&2; exit 2; }; ROLE=$2; shift 2 ;;
        --master-url) [[ $# -ge 2 ]] || { echo "--master-url requires a value" >&2; exit 2; }; MASTER_URL=$2; shift 2 ;;
        --cache-dir) [[ $# -ge 2 ]] || { echo "--cache-dir requires a value" >&2; exit 2; }; CACHE_DIR=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

pass() { PASS=$((PASS + 1)); printf 'PASS: %s\n' "$1"; }
warn() { WARN=$((WARN + 1)); printf 'WARN: %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf 'FAIL: %s\n' "$1"; }

[[ "${ROLE}" == master || "${ROLE}" == slave ]] && pass "role=${ROLE}" || fail "role must be master or slave"
if command -v distributed-matrix >/dev/null 2>&1; then
    DMS_BINARY=$(command -v distributed-matrix)
elif [[ -x /opt/distributed-matrix/bin/distributed-matrix ]]; then
    DMS_BINARY=/opt/distributed-matrix/bin/distributed-matrix
else
    DMS_BINARY=
fi
[[ -n "${DMS_BINARY}" ]] && pass "distributed-matrix installed: ${DMS_BINARY}" || fail "distributed-matrix binary not found"
[[ -d "${CACHE_DIR}" && -w "${CACHE_DIR}" ]] && pass "cache directory writable: ${CACHE_DIR}" || fail "cache directory unavailable: ${CACHE_DIR}"

for factory in filesrc parsebin mppvideodec mppjpegdec v4l2src watchdog videocrop videoconvert kmssink; do
    if command -v gst-inspect-1.0 >/dev/null 2>&1 && gst-inspect-1.0 "${factory}" >/dev/null 2>&1; then
        pass "GStreamer factory: ${factory}"
    else
        fail "GStreamer factory missing: ${factory}"
    fi
done
if command -v gst-inspect-1.0 >/dev/null 2>&1 &&
   gst-inspect-1.0 kmssink 2>/dev/null | grep -q 'render-rectangle'; then
    pass "kmssink render-rectangle property available for hardware windows"
else
    fail "kmssink render-rectangle property missing; hardware multi-window output unavailable"
fi
for factory in mpph264enc rtph264pay rtph264depay rtpjitterbuffer udpsrc udpsink; do
    if command -v gst-inspect-1.0 >/dev/null 2>&1 && gst-inspect-1.0 "${factory}" >/dev/null 2>&1; then
        pass "Optional RTP factory: ${factory}"
    else
        warn "Optional RTP factory missing: ${factory}; hardware MPP-JPEG transport fallback remains active"
    fi
done

# KVM preview is optional at runtime, but production target images should carry
# the app sink and JPEG encoder used by the real signal return path.
for factory in appsink jpegenc; do
    if command -v gst-inspect-1.0 >/dev/null 2>&1 && gst-inspect-1.0 "${factory}" >/dev/null 2>&1; then
        pass "KVM preview factory: ${factory}"
    else
        warn "KVM preview factory missing: ${factory}"
    fi
done

[[ -e /dev/dri/card0 ]] && pass "/dev/dri/card0 present" || fail "/dev/dri/card0 missing"
[[ -e /dev/mpp_service ]] && pass "/dev/mpp_service present" || warn "/dev/mpp_service missing or named differently"
if [[ "${ROLE}" == slave ]]; then
    [[ -c /dev/video0 ]] && pass "/dev/video0 capture device present" || fail "/dev/video0 capture device missing"
fi

if pgrep -af distributed-matrix >/dev/null 2>&1; then
    pass "distributed-matrix process running"
else
    warn "distributed-matrix process not detected"
fi

if [[ "${ROLE}" == master ]]; then
    if command -v curl >/dev/null 2>&1 && curl --fail --silent --max-time 3 "${MASTER_URL}/api/status" >/dev/null; then
        pass "master REST status responds: ${MASTER_URL}/api/status"
    else
        fail "master REST status unavailable: ${MASTER_URL}/api/status"
    fi
fi

printf '\nSummary: PASS=%d WARN=%d FAIL=%d\n' "${PASS}" "${WARN}" "${FAIL}"
((FAIL == 0))
