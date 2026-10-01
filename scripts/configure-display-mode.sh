#!/usr/bin/env bash
set -euo pipefail

export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0

connector=HDMI-1
fallback_mode=1280x720
target_mode=1920x1080
refresh_rate=60

for attempt in $(seq 1 30); do
    if xrandr --query 2>/dev/null | grep -q "^${connector} connected"; then
        # kmssink can change the DRM CRTC behind Xorg. Cycling through a
        # different mode forces Xorg to submit the target mode again instead
        # of accepting its stale cached state as already configured.
        xrandr --output "${connector}" --mode "${fallback_mode}" --rate "${refresh_rate}"
        sleep 1
        xrandr --output "${connector}" --mode "${target_mode}" --rate "${refresh_rate}" --primary
        exit 0
    fi
    sleep 1
done

echo "${connector} did not become available within 30 seconds" >&2
exit 1