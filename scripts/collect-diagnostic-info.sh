#!/usr/bin/env bash
set -u

OUTPUT_DIR=${OUTPUT_DIR:-"./diagnostic-$(date +%Y%m%d-%H%M%S)"}
ARCHIVE=${ARCHIVE:-1}

usage() {
    cat <<'EOF'
Usage: collect-diagnostic-info.sh [--output DIR] [--no-archive]

Collects non-destructive Distributed Matrix System video-path diagnostics.
It does not modify configuration, credentials, services or hardware settings.
EOF
}

while (($#)); do
    case $1 in
        --output) [[ $# -ge 2 ]] || { echo "--output requires a directory" >&2; exit 2; }; OUTPUT_DIR=$2; shift 2 ;;
        --no-archive) ARCHIVE=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

mkdir -p -- "${OUTPUT_DIR}"

capture() {
    local file=$1
    shift
    {
        printf '$'
        printf ' %q' "$@"
        printf '\n'
        "$@" 2>&1 || true
    } > "${OUTPUT_DIR}/${file}"
}

capture 01_uname.txt uname -a
if [[ -r /etc/os-release ]]; then cp -- /etc/os-release "${OUTPUT_DIR}/02_os-release.txt"; fi
capture 03_cpu.txt sh -c 'grep -E "^(processor|model name|Hardware|Features)" /proc/cpuinfo; command -v lscpu >/dev/null && lscpu || true'
capture 04_memory.txt sh -c 'free -h; printf "\n/proc/meminfo\n"; cat /proc/meminfo'
capture 05_disk.txt sh -c 'df -hT; printf "\nMounts\n"; mount'
capture 06_network.txt sh -c 'ip -details addr; printf "\nRoutes\n"; ip route; printf "\nMulticast memberships\n"; ip maddr; printf "\nSockets\n"; ss -tulpn 2>/dev/null || true'
capture 07_processes.txt sh -c 'pgrep -a distributed-matrix || true; printf "\nTop memory processes\n"; ps aux --sort=-%mem | head -20'
capture 08_video_devices.txt sh -c 'ls -l /dev/mpp* /dev/rga /dev/dri/* /dev/fb* 2>/dev/null || true; printf "\nRelevant modules\n"; lsmod 2>/dev/null | grep -E "mpp|rkvdec|rkvenc|rga|drm|vop" || true'
capture 09_gstreamer.txt sh -c 'gst-inspect-1.0 --version 2>/dev/null || true; for f in filesrc parsebin mppvideodec mppjpegdec mpph264enc mppjpegenc videocrop videoconvert capsfilter kmssink appsink jpegenc rtph264pay rtph264depay rtpjitterbuffer udpsrc udpsink; do printf "\n===== %s =====\n" "$f"; gst-inspect-1.0 "$f" 2>&1 || true; done'
capture 10_logs.txt sh -c 'journalctl --no-pager -n 400 2>/dev/null | grep -E "distributed-matrix|gstreamer|gst|mpp|rkvdec|drm|kms" || dmesg | tail -400'

for dir in /etc/distributed_matrix /usr/local/etc/distributed_matrix /opt/distributed-matrix/etc; do
    if [[ -d "${dir}" ]]; then
        mkdir -p "${OUTPUT_DIR}/config"
        cp -a -- "${dir}/." "${OUTPUT_DIR}/config/" 2>/dev/null || true
    fi
done

if [[ "${ARCHIVE}" == 1 ]]; then
    tar -czf "${OUTPUT_DIR}.tar.gz" -- "${OUTPUT_DIR}"
    printf 'Diagnostic archive: %s.tar.gz\n' "${OUTPUT_DIR}"
else
    printf 'Diagnostic directory: %s\n' "${OUTPUT_DIR}"
fi
