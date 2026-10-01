#!/usr/bin/env bash
set -euo pipefail

package_dir="${1:?package directory required}"
defaults="$package_dir/distributed-matrix.defaults"
init_script="$package_dir/S99distributed-matrix"
master_config="$package_dir/master_config.json"
slave_config="$package_dir/slave_config.json"
network_config="$package_dir/network.conf"
gadget_script="$package_dir/S10usb-gadget"
network_init_script="$package_dir/S40network"
dhcpcd_init_script="$package_dir/S41dhcpcd"

bash -n "$init_script"
bash -n "$gadget_script"
bash -n "$network_init_script"
bash -n "$dhcpcd_init_script"
bash -n "$package_dir/S05oled-guard"
grep -Fq '/usr/bin/distributed-matrix --oled-boot' "$package_dir/S05oled-guard"
grep -Fq 'set_gpio_low 14' "$package_dir/S05oled-guard"
if grep -Eq 'set_gpio_low (13|124|125)([^0-9]|$)' "$package_dir/S05oled-guard"; then
  echo 'OLED boot script drives an unverified power/DSI GPIO' >&2
  exit 1
fi
[[ "$(grep -c '^ROLE=' "$defaults")" -eq 1 ]]
grep -qx 'ROLE=both' "$defaults"
grep -Eq '"http_port"[[:space:]]*:[[:space:]]*8080' "$master_config"
grep -Eq '"audio_output_mode"[[:space:]]*:[[:space:]]*"both"' "$slave_config"
grep -Eq '"node_id"[[:space:]]*:[[:space:]]*1' "$slave_config"
grep -Eq '"node_role"[[:space:]]*:[[:space:]]*"decode"' "$slave_config"
grep -Eq '"enabled"[[:space:]]*:[[:space:]]*true' "$slave_config"
grep -Eq '"role"[[:space:]]*:[[:space:]]*"target"' "$slave_config"
grep -Eq '"overlay_plane_ids"[[:space:]]*:[[:space:]]*\[[[:space:]]*56[[:space:]]*,[[:space:]]*78[[:space:]]*,[[:space:]]*92[[:space:]]*,[[:space:]]*106[[:space:]]*\]' "$slave_config"
grep -Eq '"input_devices"[[:space:]]*:[[:space:]]*\[[[:space:]]*\]' "$slave_config"
grep -qx 'IP_MODE=auto' "$network_config"
grep -qx 'PREFIX_LENGTH=0' "$network_config"

for token in \
  'ip link set eth0 up' \
  'ip -4 addr flush dev eth0 scope global' \
  'ip -4 route replace default via "$GATEWAY" dev eth0'; do
  grep -Fq "$token" "$dhcpcd_init_script"
done

grep -Fq 'ip link set eth0 up' "$network_init_script"

if [[ -e "$package_dir/S50oled" ]]; then
  echo 'duplicate OLED init script remains' >&2
  exit 1
fi

for token in \
  'MASTER_PIDFILE=/var/run/distributed-matrix-master.pid' \
  'SLAVE_PIDFILE=/var/run/distributed-matrix-slave.pid' \
  'both)' \
  'ip -4 addr show dev eth0 scope global' \
  'for attempt in 1 2 3' \
  'rm -f "$pidfile"' \
  'process_matches_role "$pid" "$role"' \
  'for proc in /proc/[0-9]*' \
  'stop_role slave "$SLAVE_PIDFILE"' \
  'stop_role master "$MASTER_PIDFILE"' \
  'continuing with slave' \
  'start_role master' \
  'start_role slave'; do
  grep -Fq "$token" "$init_script"
done

for token in \
  'functions/hid.keyboard' \
  'functions/hid.mouse' \
  'RNDIS+HID' \
  'echo 8 > $GADGET/functions/hid.keyboard/report_length' \
  'echo 4 > $GADGET/functions/hid.mouse/report_length'; do
  grep -Fq "$token" "$gadget_script"
done

if grep -Eq 'ffs\.adb|FunctionFS|RNDIS\+ADB' "$gadget_script"; then
  echo 'obsolete ADB FunctionFS remains in USB Gadget configuration' >&2
  exit 1
fi

if grep -q 'PIDFILE=/var/run/distributed-matrix.pid' "$init_script"; then
  echo 'legacy single-role PID file remains' >&2
  exit 1
fi

echo 'package_policy passed'
