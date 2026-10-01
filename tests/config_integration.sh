#!/usr/bin/env bash
set -euo pipefail
binary="${1:?binary required}"
web_root="${2:?web root required}"
base="$(mktemp -d)"
pids=()
cleanup() {
  for pid in "${pids[@]:-}"; do kill -TERM "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; done
  rm -rf -- "$base"
}
trap cleanup EXIT

master_cache="$base/master-cache"
master_log="$base/master.log"
master_config="$base/master.json"
cat >"$master_config" <<JSON
{
  "network": {
    "ip_mode": "manual",
    "local_ip": "127.0.0.1",
    "multicast_address": "239.1.1.11",
    "multicast_port": 19101,
    "heartbeat_address": "239.1.1.12",
    "heartbeat_port": 19102,
    "discovery_port": 19103,
    "discovery_networks": ["127.0.0.1/32"],
    "http_port": 18081
  },
  "video": {"cache_directory": "$master_cache", "max_cache_size_mb": 2},
  "web": {"root_directory": "$web_root", "regions_file": "$base/regions.json"},
  "sync": {"sync_delay_ms": 25},
  "layout": {"rows": 1, "cols": 2, "screen_width": 640, "screen_height": 480},
  "logging": {"level": "DEBUG", "output": "$master_log"}
}
JSON
"$binary" --role master --config "$master_config" &
master_pid=$!
pids+=("$master_pid")
for _ in $(seq 1 50); do curl -fsS http://127.0.0.1:18081/api/status >/dev/null 2>&1 && break; sleep .1; done
curl -fsS http://127.0.0.1:18081/api/screens | grep -q '"screenWidth":640'
[[ -s "$master_log" ]]
kill -TERM "$master_pid"
wait "$master_pid"
pids=()

slave_cache="$base/slave-cache"
slave_log="$base/slave.log"
slave_config="$base/slave.json"
cat >"$slave_config" <<JSON
{
  "node_id": 9,
  "network": {
    "multicast_address": "239.1.1.21",
    "multicast_port": 19201,
    "heartbeat_address": "239.1.1.22",
    "heartbeat_port": 19202,
    "discovery_port": 19203
  },
  "video": {"cache_directory": "$slave_cache", "max_cache_size_mb": 2},
  "playback": {
    "connector_id": 0, "plane_id": 0, "overlay_plane_ids": [78, 79],
    "fullscreen": false, "startup_timeout_ms": 50,
    "idle_image_path": "",
    "capture": {
      "device": "/dev/video0", "pixel_format": "auto",
      "width": 0, "height": 0,
      "framerate_numerator": 0, "framerate_denominator": 1,
      "colorimetry": "bt709",
      "audio_enabled": false,
      "audio_capture_device": "hw:CARD=hdmiin,DEV=0",
      "audio_output_mode": "both",
      "hdmi_audio_device": "hw:CARD=rockchiphdmi,DEV=0",
      "hdmi_audio_volume": 16.0,
      "analog_audio_device": "hw:CARD=rk809,DEV=0",
      "analog_audio_volume": 1.0
    },
    "startup_animation_path": "", "startup_animation_timeout_ms": 50
  },
  "logging": {"level": "INFO", "output": "$slave_log"}
}
JSON
set +e
"$binary" --role slave --config "$slave_config" >"$base/slave.out" 2>&1 &
slave_pid=$!
pids+=("$slave_pid")
sleep .4
if kill -0 "$slave_pid" 2>/dev/null; then
  [[ -s "$slave_log" ]]
  kill -TERM "$slave_pid"
  wait "$slave_pid"
  slave_status=0
else
  wait "$slave_pid"
  slave_status=$?
  grep -Eq 'GStreamer|plugin' "$base/slave.out"
fi
set -e
pids=()
[[ "$slave_status" -eq 0 || "$slave_status" -eq 1 ]]

printf '{"node_id":9,"playback":{"player":"mpv"}}\n' >"$base/legacy-playback.json"
if "$binary" --role slave --config "$base/legacy-playback.json" >"$base/legacy-playback.out" 2>&1; then
  echo 'legacy playback configuration was accepted' >&2
  exit 1
fi
grep -q 'Unknown configuration key: playback.player' "$base/legacy-playback.out"

printf '{"node_id":9,"playback":{"hdmi_capture_device":"/dev/video0"}}\n' >"$base/legacy-capture.json"
if "$binary" --role slave --config "$base/legacy-capture.json" >"$base/legacy-capture.out" 2>&1; then
  echo 'legacy capture configuration was accepted' >&2
  exit 1
fi
grep -q 'Unknown configuration key: playback.hdmi_capture_device' "$base/legacy-capture.out"

printf '{"node_id":9,"playback":{"capture":{"audio_playback_devices":["hw:0"]}}}\n' >"$base/legacy-audio.json"
if "$binary" --role slave --config "$base/legacy-audio.json" >"$base/legacy-audio.out" 2>&1; then
  echo 'legacy audio playback devices configuration was accepted' >&2
  exit 1
fi
grep -q 'Unknown configuration key: playback.capture.audio_playback_devices' "$base/legacy-audio.out"

printf '{"node_id":9,"playback":{"capture":{"audio_output_mode":"invalid"}}}\n' >"$base/invalid-audio-mode.json"
if "$binary" --role slave --config "$base/invalid-audio-mode.json" >"$base/invalid-audio-mode.out" 2>&1; then
  echo 'invalid audio output mode was accepted' >&2
  exit 1
fi
grep -q 'audio_output_mode must be hdmi, analog, or both' "$base/invalid-audio-mode.out"

printf '{"node_id":9,"playback":{"overlay_plane_ids":[78,78]}}\n' >"$base/duplicate-overlay-planes.json"
if "$binary" --role slave --config "$base/duplicate-overlay-planes.json" >"$base/duplicate-overlay-planes.out" 2>&1; then
  echo 'duplicate DRM overlay plane IDs were accepted' >&2
  exit 1
fi
grep -q 'DRM overlay plane IDs must be non-zero and unique' "$base/duplicate-overlay-planes.out"

printf '{"node_id":9,"kvm":{"enabled":true,"role":"controller","listen_port":9101,"input_devices":["/dev/input/event0"],"keyboard_hid_device":"/dev/hidg0","mouse_hid_device":"/dev/hidg1"}}\n' >"$base/valid-kvm.json"
set +e
"$binary" --role slave --config "$base/valid-kvm.json" >"$base/valid-kvm.out" 2>&1
valid_kvm_status=$?
set -e
[[ "$valid_kvm_status" -eq 0 || "$valid_kvm_status" -eq 1 ]]
if grep -q 'Configuration error' "$base/valid-kvm.out"; then
  echo 'valid KVM configuration was rejected' >&2
  exit 1
fi

for case_name in unknown-field invalid-role disabled-role empty-input bad-port; do
  case "$case_name" in
    unknown-field) payload='{"node_id":9,"kvm":{"extra":true}}' ;;
    invalid-role) payload='{"node_id":9,"kvm":{"enabled":true,"role":"invalid"}}' ;;
    disabled-role) payload='{"node_id":9,"kvm":{"enabled":true,"role":"disabled"}}' ;;
    empty-input) payload='{"node_id":9,"kvm":{"enabled":true,"role":"controller","input_devices":[]}}' ;;
    bad-port) payload='{"node_id":9,"kvm":{"listen_port":65536}}' ;;
  esac
  printf '%s\n' "$payload" >"$base/kvm-$case_name.json"
  if "$binary" --role slave --config "$base/kvm-$case_name.json" >"$base/kvm-$case_name.out" 2>&1; then
    echo "invalid KVM configuration was accepted: $case_name" >&2
    exit 1
  fi
done

printf '{"node_id":9,"playback":{"startup_video_path":"/tmp/boot.mp4"}}\n' >"$base/startup-alias.json"
if "$binary" --role slave --config "$base/startup-alias.json" >"$base/startup-alias.out" 2>&1; then
  echo 'startup animation alias was accepted' >&2
  exit 1
fi

printf '{"node_id":65}\n' >"$base/node-id-too-large.json"
if "$binary" --role slave --config "$base/node-id-too-large.json" >"$base/node-id-too-large.out" 2>&1; then
  echo "node_id values above the protocol limit must be rejected" >&2
  exit 1
fi
grep -Fq 'config.node_id is out of range' "$base/node-id-too-large.out"

if "$binary" --role slave --config "$base/slave.json" --node-id 65 >"$base/node-id-cli-too-large.out" 2>&1; then
  echo "--node-id values above the protocol limit must be rejected" >&2
  exit 1
fi
grep -Fq 'Invalid value for --node-id: 65' "$base/node-id-cli-too-large.out"
grep -q 'Unknown configuration key: playback.startup_video_path' "$base/startup-alias.out"

printf '{"sync":{"ntp_server":"pool.ntp.org"}}\n' >"$base/legacy-ntp.json"
if "$binary" --role master --config "$base/legacy-ntp.json" >"$base/legacy-ntp.out" 2>&1; then
  echo 'legacy NTP configuration was accepted' >&2
  exit 1
fi
grep -q 'Unknown configuration key: sync.ntp_server' "$base/legacy-ntp.out"

printf '{"unknown":true}\n' >"$base/invalid.json"
if "$binary" --role master --config "$base/invalid.json" >"$base/invalid.out" 2>&1; then
  echo 'unknown configuration key was accepted' >&2
  exit 1
fi
grep -q 'Unknown configuration key' "$base/invalid.out"

printf '{"role":"master"}\n' >"$base/legacy.json"
if "$binary" --role master --config "$base/legacy.json" >"$base/legacy.out" 2>&1; then
  echo 'legacy role configuration was accepted' >&2
  exit 1
fi
grep -q 'Unknown configuration key' "$base/legacy.out"

printf '{"network":{"http_port":65536}}
' >"$base/range.json"
if "$binary" --role master --config "$base/range.json" >"$base/range.out" 2>&1; then
  echo 'out-of-range HTTP port was accepted' >&2
  exit 1
fi
grep -q 'network.http_port is out of range' "$base/range.out"

printf '{"layout":{"rows":-1}}
' >"$base/negative.json"
if "$binary" --role master --config "$base/negative.json" >"$base/negative.out" 2>&1; then
  echo 'negative layout value was accepted' >&2
  exit 1
fi
grep -q 'layout.rows must be an unsigned integer' "$base/negative.out"

printf '{"network":{"ip_mode":"manual","local_ip":""}}\n' >"$base/manual-missing-ip.json"
if "$binary" --role master --config "$base/manual-missing-ip.json" >"$base/manual-missing-ip.out" 2>&1; then
  echo 'manual IP configuration without an address was accepted' >&2
  exit 1
fi
grep -q 'network.local_ip is required when network.ip_mode is manual' "$base/manual-missing-ip.out"

printf '{"network":{"ip_mode":"auto","local_ip":"192.168.2.102"}}\n' >"$base/auto-with-ip.json"
if "$binary" --role slave --config "$base/auto-with-ip.json" --node-id 9 >"$base/auto-with-ip.out" 2>&1; then
  echo 'automatic IP configuration with an address was accepted' >&2
  exit 1
fi
grep -q 'network.local_ip must be empty when network.ip_mode is auto' "$base/auto-with-ip.out"

printf '{"network":{"ip_mode":"manual","local_ip":"192.168.2.999"}}\n' >"$base/invalid-ip.json"
if "$binary" --role master --config "$base/invalid-ip.json" >"$base/invalid-ip.out" 2>&1; then
  echo 'invalid manual IPv4 address was accepted' >&2
  exit 1
fi
grep -q 'network.local_ip must be a valid IPv4 address' "$base/invalid-ip.out"

if "$binary" --role master --unknown >"$base/unknown-cli.out" 2>&1; then
  echo 'unknown CLI option was accepted' >&2
  exit 1
fi
grep -q 'Unknown master option' "$base/unknown-cli.out"

"$binary" --help | grep -q -- '--role <master|slave>'
"$binary" --help | grep -q -- '--idle-image <path>'
"$binary" --help | grep -q -- '--ip-mode <auto|manual>'
"$binary" --help | grep -q -- '--ip <address>'

echo 'config_integration passed'
