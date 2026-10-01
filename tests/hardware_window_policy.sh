#!/usr/bin/env bash
set -euo pipefail

source_file="${1:?MediaPlayer.cpp path required}"
header_file="${2:?MediaPlayer.h path required}"

grep -Fq 'showWindowLayoutHardwareUnlocked' "$source_file"
grep -Fq 'mppvideodec fast-mode=true dma-feature=true' "$source_file"
grep -Fq 'video/x-raw,format=BGRx' "$source_file"
grep -Fq 'jpegdec ! videoconvert' "$source_file"
grep -Fq 'render-rectangle=' "$source_file"
grep -Fq 'planeIds.push_back(m_options.overlayPlaneIds.front())' "$source_file"
grep -Fq 'planes=%s' "$source_file"
if grep -Eq 'dms_window_compositor|Software window compositor|windowCompositeLoop|std::vector<std::vector<uint8_t>>' \
    "$source_file" "$header_file"; then
    echo 'CPU window compositor implementation remains' >&2
    exit 1
fi

echo 'hardware_window_policy passed'
