#!/usr/bin/env bash
set -euo pipefail

if (( $# != 3 )); then
  echo "usage: verify-rom.sh <sdk-dir> <rom-dir> <rom-img>" >&2
  exit 2
fi
SDK_DIR=$1
ROM_DIR=$2
ROM_IMG=$3
CUSTOM_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=build-baseline.env
source "$CUSTOM_DIR/build-baseline.env"
if [[ "$(realpath "$SDK_DIR")" != "$(realpath "$DMS_SDK_DIR")" ]]; then
  echo "Refusing non-canonical SDK: $SDK_DIR" >&2
  exit 1
fi
REPORT="$ROM_DIR/VERIFICATION.txt"
TOOLS="$SDK_DIR/tools/linux/Linux_Pack_Firmware/rockdev"
FDTGET=$(command -v fdtget || true)

[[ -f "$ROM_IMG" ]] || { echo "ROM not found: $ROM_IMG" >&2; exit 1; }
[[ -f "$SDK_DIR/device/rockchip/common/images/blank-misc.img" ]] || { echo "blank misc not found" >&2; exit 1; }
[[ -n "$FDTGET" ]] || { echo "fdtget is required for ROM verification" >&2; exit 1; }

verify_rk628_i2s_master() {
  local dtb=$1
  local sound=/rk628-hdmi-rx-sound
  local codec="$sound/simple-audio-card,codec"
  local cpu="$sound/simple-audio-card,cpu"
  local i2s3=/i2s@fe430000
  local codec_phandle bitclock_master frame_master cpu_phandle properties

  codec_phandle=$($FDTGET -t x "$dtb" "$codec" phandle)
  bitclock_master=$($FDTGET -t x "$dtb" "$sound" simple-audio-card,bitclock-master)
  frame_master=$($FDTGET -t x "$dtb" "$sound" simple-audio-card,frame-master)
  [[ "$bitclock_master" == "$codec_phandle" ]] || {
    echo 'FAIL bitclock-master does not reference the RK628 codec'; return 1;
  }
  [[ "$frame_master" == "$codec_phandle" ]] || {
    echo 'FAIL frame-master does not reference the RK628 codec'; return 1;
  }
  cpu_phandle=$($FDTGET -t x "$dtb" "$cpu" phandle 2>/dev/null || true)
  [[ -z "$cpu_phandle" || "$bitclock_master" != "$cpu_phandle" ]] || {
    echo 'FAIL RK3566 I2S3 is incorrectly configured as clock master'; return 1;
  }
  echo "PASS RK628 codec drives I2S BCLK/LRCK (phandle $codec_phandle); RK3566 I2S3 is slave"

  properties=$($FDTGET -p "$dtb" "$i2s3")
  grep -Fxq 'rockchip,i2s3-mux-m1' <<<"$properties" || {
    echo 'FAIL RK3568 I2S3 is not forced onto the physical m1 pins'; return 1;
  }
  echo 'PASS RK3568 I2S3 is forced onto m1 for the RK628 SCLK/LRCK/SDI link'
}

verify_rk809_analog() {
  local dtb=$1
  local cpu_dai i2s1_phandle
  [[ "$(fdtget "$dtb" /rk809-sound simple-audio-card,name)" == rk809 ]] || {
    echo 'FAIL RK809 analog card name is not rk809'; return 1;
  }
  [[ "$(fdtget "$dtb" /i2c@fdd40000/pmic@20/codec status)" == okay ]] || {
    echo 'FAIL RK809 codec is disabled in FIT FDT'; return 1;
  }
  cpu_dai=$(fdtget -t x "$dtb" /rk809-sound/simple-audio-card,cpu sound-dai)
  i2s1_phandle=$(fdtget -t x "$dtb" /i2s@fe410000 phandle)
  [[ "$cpu_dai" == "$i2s1_phandle" ]] || {
    echo 'FAIL RK809 analog card is not connected to I2S1'; return 1;
  }
  echo 'PASS RK809 analog card is enabled on I2S1'
}

verify_kvm_usb_gadget() {
  local dtb=$1
  local controller=/usbdrd/dwc3@fcc00000
  if ! $FDTGET -p "$dtb" "$controller" >/dev/null 2>&1; then
    echo 'FAIL USB DRD controller is missing from FIT FDT'; return 1;
  fi
  [[ "$($FDTGET "$dtb" "$controller" dr_mode)" == peripheral ]] || {
    echo 'FAIL USB DRD controller is not in peripheral mode'; return 1;
  }
  echo 'PASS USB DRD controller is in peripheral mode for KVM HID'
}

verify_oled_reset_pin() {
  local dtb=$1
  local pins
  read -ra pins <<<"$($FDTGET -t u "$dtb" /pinctrl/oled/oled-boot-pins rockchip,pins)"
  [[ ${#pins[@]} -eq 4 && ${pins[0]} == 0 && ${pins[1]} == 14 && ${pins[2]} == 0 ]] || {
    echo 'FAIL OLED reset must use the verified I2C_TP GPIO0_B6 only'; return 1;
  }
  echo 'PASS OLED boot pin is I2C_TP RST GPIO0_B6; no DSI/INT GPIO driven'
}

TMP=$(mktemp -d /tmp/dms-rom-verify.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/outer" "$TMP/pkg"

{
  echo "DMS RK3566 Buildroot ROM verification"
  echo "Date: $(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "ROM: $ROM_IMG"
  echo
  echo "[canonical kernel sources]"
  cmp "$CUSTOM_DIR/kernel/rk628_cru.c" \
      "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.c"
  cmp "$CUSTOM_DIR/kernel/rk628_cru.h" \
      "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.h"
  cmp "$CUSTOM_DIR/kernel/rk628.c" \
      "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628.c"
  cmp "$CUSTOM_DIR/kernel/rk628_hdmirx.c" \
      "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  [[ "$(sha256sum "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_csi_v4l2.c" | awk '{print $1}')" == "$DMS_RK628_CSI_SHA256" ]] || {
    echo 'FAIL RK628 CSI source differs from the pinned canonical source'; exit 1;
  }
  grep -q 'int rk628_clk_enable' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.h"
  grep -q 'HIWORD_UPDATE(0, CLK_HDMIRX_AUD_GATE_BIT' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.c"
  grep -q 'regmap_reg_range(HDMI_RX_AUD_CHEXTR_CTRL, HDMI_RX_AUD_SPARE)' \
      "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628.c"
  grep -q 'rk628_clk_enable(rk628, CGU_CLK_HDMIRX_AUD)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  cmp "$CUSTOM_DIR/../../dummy-codec.c" \
      "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"
  cmp "$CUSTOM_DIR/kernel/rockchip_i2s_tdm.c" \
      "$SDK_DIR/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c"
  if grep -Eq 'h_reset|slave_reset_prepared|rockchip_i2s_tdm_prepare|RK628 slave reset complete' "$SDK_DIR/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c"; then
    echo 'FAIL obsolete I2S3 HCLK/prepare reset code remains'; exit 1
  fi
  sed -n '30,42p' "$SDK_DIR/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c" | grep -q 'CONFIG_CPU_RK3568'
  grep -q 'of_device_is_compatible(node, "rockchip,rk3568-i2s-tdm");' \
      "$SDK_DIR/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c"
  grep -q 'RK3568_I2S3_M1_SEL' "$SDK_DIR/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c"
  grep -q 'reset-names = "tx-m", "rx-m"' "$CUSTOM_DIR/../../$DMS_DTS_FILE"
  grep -q 'rockchip,i2s3-mux-m1;' "$CUSTOM_DIR/../../$DMS_DTS_FILE"
  if grep -q 'SRST_H_I2S3_2CH' "$CUSTOM_DIR/../../$DMS_DTS_FILE"; then
    echo 'FAIL obsolete I2S3 HCLK reset remains in DTS'; exit 1
  fi
  grep -Eq '#define RK628_AUDIO_FIFO_MAX[[:space:]]+8' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -Eq '#define RK628_AUDIO_FIFO_MIN[[:space:]]+8' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'AFIF_TH_MAX(RK628_AUDIO_FIFO_MAX)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'AFIF_TH_MIN(RK628_AUDIO_FIFO_MIN)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'AFIF_SUBPACKETS(1)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'AUD_MUTE_SEL(0)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'AUDS_MAS_SAMPLE_FLAT' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'if (fifo_ints != 0x9 &&' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'rk628_hdmirx_audio_fifo_initd(aif);' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'PAO_RATE(0)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  if grep -Eq '0x308008|0x30824c|0x10000011|0x00000f00' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"; then
    echo 'FAIL stale HDMI2.0/SCDC values are still written as audio registers'; exit 1
  fi
  grep -q 'I2S_DATA_ENABLE_BITS(0xf)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c" &&
  grep -q 'I2S_DATA_ENABLE_BITS(0)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c" || {
    echo 'FAIL RK628 I2S control does not preserve BCLK/LRCK while gating data lanes'; exit 1
  }
  if grep -q 'I2S_ENABLE_BITS(0x3f)' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"; then
    echo 'FAIL RK628 audio control still disables BCLK/LRCK with I2S_ENABLE_BITS'; exit 1
  fi
  grep -Eq 'RK628F/H.*polled' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'rk628_csi_delayed_work_audio_v2' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'stablelimit' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'audio_clk_ppm_inc' "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  grep -q 'channels_min = 2' "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"
  grep -q 'channels_max = 2' "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"
  grep -q 'SNDRV_PCM_RATE_48000' "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"
  grep -q 'SNDRV_PCM_FMTBIT_S16_LE' "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"
  if grep -q '\.playback' "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"; then
    echo 'FAIL dummy codec still exposes a playback API'; exit 1
  fi
  grep -q 'BR2_PACKAGE_DROPBEAR_LOCALOPTIONS_FILE="package/dropbear/dms-localoptions.h"' \
    "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/.config"
  grep -q '#undef HAVE_GETRANDOM' "$SDK_DIR/buildroot/package/dropbear/dms-localoptions.h"
  grep -q '#undef HAVE_GETRANDOM' \
    "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/build/dropbear-2022.82/localoptions.h"
  echo 'PASS Dropbear SSH startup does not block on unseeded kernel CRNG'
  echo 'PASS one canonical RK628 audio path and capture-only 48 kHz/S16_LE/stereo codec API'

  echo "[package]"
  "$TOOLS/rkImageMaker" -unpack "$ROM_IMG" "$TMP/outer" >/dev/null
  cd "$TMP/pkg"
  "$TOOLS/afptool" -unpack "$TMP/outer/firmware.img" . >/dev/null
  cat package-file
  for required in MiniLoaderAll.bin Image/uboot.img Image/misc.img Image/boot.img Image/oem.img Image/rootfs.img Image/userdata.img; do
    [[ -s "$required" ]] || { echo "FAIL missing $required"; exit 1; }
    echo "PASS $required $(stat -c %s "$required") bytes"
  done
  if grep -q '^recovery[[:space:]]' package-file; then
    echo 'FAIL recovery image is packaged'; exit 1
  fi
  echo 'PASS recovery image is not packaged'

  echo
  echo "[boot policy]"
  cmp Image/misc.img "$SDK_DIR/device/rockchip/common/images/blank-misc.img"
  if strings Image/misc.img | grep -E 'boot-recovery|--wipe_all|^recovery$' >/dev/null; then
    echo 'FAIL recovery BCB found in misc.img'; exit 1
  fi
  echo 'PASS misc.img is blank and has no forced recovery BCB'
  strings Image/uboot.img | grep -F 'setenv bootargs ' | head -1
  strings Image/uboot.img | grep -F 'run rkimg_bootdev; boot_fit;' | head -1
  strings Image/uboot.img | grep -F 'root=PARTUUID=614e0000-0000-4b53-8000-1d28000054a9' | head -1
  strings Image/uboot.img | grep -F 'cma=256M' | head -1
  echo 'PASS U-Boot forces Buildroot bootargs before direct FIT boot, including root PARTUUID and CMA=256M'

  echo
  echo "[image integrity]"
  cmp Image/boot.img "$ROM_DIR/boot.img"
  cmp Image/rootfs.img "$ROM_DIR/rootfs.img"
  echo 'PASS extracted boot.img/rootfs.img equal published component images'
  "$SDK_DIR/u-boot/tools/dumpimage" -l Image/boot.img | grep -E '^(FIT description:| Image [0-9]| Default Configuration:| Configuration [0-9]|  Hash algo:|  Hash value:)' || true

  BOOT_VERIFY="$TMP/boot-verify"
  mkdir -p "$BOOT_VERIFY/resource"
  "$SDK_DIR/u-boot/tools/dumpimage" -i Image/boot.img -T flat_dt -p 0 -o "$BOOT_VERIFY/fdt.dtb" fdt >/dev/null
  "$SDK_DIR/u-boot/tools/dumpimage" -i Image/boot.img -T flat_dt -p 1 -o "$BOOT_VERIFY/Image" kernel >/dev/null
  "$SDK_DIR/u-boot/tools/dumpimage" -i Image/boot.img -T flat_dt -p 2 -o "$BOOT_VERIFY/resource.img" resource >/dev/null
  (
    cd "$BOOT_VERIFY/resource"
    "$SDK_DIR/rkbin/tools/resource_tool" --unpack --image="$BOOT_VERIFY/resource.img" >/dev/null
  )
  cmp "$BOOT_VERIFY/Image" "$SDK_DIR/kernel/arch/arm64/boot/Image"
  echo 'PASS FIT kernel payload equals the canonical freshly built Linux Image'
  cmp "$BOOT_VERIFY/fdt.dtb" "$BOOT_VERIFY/resource/out/rk-kernel.dtb"
  verify_rk628_i2s_master "$BOOT_VERIFY/fdt.dtb"
  verify_rk628_i2s_master "$BOOT_VERIFY/resource/out/rk-kernel.dtb"
  verify_rk809_analog "$BOOT_VERIFY/fdt.dtb"
  verify_rk809_analog "$BOOT_VERIFY/resource/out/rk-kernel.dtb"
  verify_kvm_usb_gadget "$BOOT_VERIFY/fdt.dtb"
  verify_kvm_usb_gadget "$BOOT_VERIFY/resource/out/rk-kernel.dtb"
  verify_oled_reset_pin "$BOOT_VERIFY/fdt.dtb"
  verify_oled_reset_pin "$BOOT_VERIFY/resource/out/rk-kernel.dtb"
  for node in rk628-audio-codec rk628-hdmi-rx-sound hdmiin i2s-enable-default; do
    # Search the DTB directly. With `set -o pipefail`, `strings | grep -q`
    # can report failure when grep exits early and strings receives SIGPIPE.
    grep -aFq "$node" "$BOOT_VERIFY/fdt.dtb" || { echo "FAIL FIT FDT missing $node"; exit 1; }
    grep -aFq "$node" "$BOOT_VERIFY/resource/out/rk-kernel.dtb" || { echo "FAIL resource DTB missing $node"; exit 1; }
  done
  echo 'PASS FIT FDT and resource.img/rk-kernel.dtb are identical and contain RK628 HDMI input audio nodes'

  e2fsck -fn Image/rootfs.img
  echo 'PASS rootfs ext4 filesystem check'
  if tune2fs -l Image/rootfs.img | grep -E '^Filesystem features:.*(^|[[:space:]])has_journal([[:space:]]|$)' >/dev/null; then
    echo 'FAIL rootfs still contains the JBD2 journal that fails on the target kernel'; exit 1
  fi
  tune2fs -l Image/rootfs.img | grep -E '^Filesystem features:.*(^|[[:space:]])extent([[:space:]]|$)' >/dev/null || {
    echo 'FAIL rootfs is not ext4 with extents'; exit 1;
  }
  echo 'PASS rootfs uses ext4 extents without the incompatible JBD2 journal'

  echo
  echo "[runtime]"
  for path in \
    /usr/bin/distributed-matrix \
    /usr/sbin/dropbear \
    /etc/init.d/S99distributed-matrix \
    /etc/init.d/S10usb-gadget \
    /usr/bin/v4l2-ctl \
    /usr/bin/gst-launch-1.0 \
    /usr/bin/picocom \
    /usr/bin/setserial \
    /usr/bin/amixer \
    /usr/bin/aplay \
    /usr/lib/librockchip_mpp.so.0 \
    /usr/lib/librga.so.2.1.0 \
    /usr/lib/libdrm.so.2.4.0 \
    /usr/lib/libserialport.so.0.1.0; do
    debugfs -R "stat $path" Image/rootfs.img 2>/dev/null | grep -q '^Inode:' || { echo "FAIL missing $path"; exit 1; }
    echo "PASS $path"
  done
  if debugfs -R "stat /usr/bin/curl" Image/rootfs.img 2>/dev/null | grep -q '^Inode:'; then
    echo 'FAIL curl CLI remains in rootfs'; exit 1
  fi
  if debugfs -R "stat /usr/bin/dbclient" Image/rootfs.img 2>/dev/null | grep -q '^Inode:'; then
    echo 'FAIL Dropbear dbclient remains in rootfs'; exit 1
  fi
  if debugfs -R "stat /usr/bin/ssh" Image/rootfs.img 2>/dev/null | grep -q '^Inode:'; then
    echo 'FAIL Dropbear ssh client remains in rootfs'; exit 1
  fi
  for path in /usr/bin/lsof /usr/sbin/i2cget /usr/sbin/i2cset /usr/sbin/i2cdump /usr/sbin/i2cdetect /usr/sbin/i2ctransfer; do
    if debugfs -R "stat $path" Image/rootfs.img 2>/dev/null | grep -q '^Inode:'; then
      echo "FAIL trimmed BusyBox applet remains in rootfs: $path"; exit 1
    fi
  done
  echo 'PASS curl CLI, SSH clients and trimmed BusyBox diagnostic applets absent from rootfs'
  for dir in /usr/bin /usr/sbin /usr/lib /etc/init.d; do
    if debugfs -R "ls -l $dir" Image/rootfs.img 2>&1 | grep -Eiq '(^|[[:space:]/])(mali|vulkan|rknn|adb)([[:space:]/.-]|$)'; then
      echo "FAIL prohibited GPU/NPU/ADB name under $dir"; exit 1
    fi
  done
  echo 'PASS no Mali/Vulkan/RKNN/ADB names in key rootfs directories'

  if debugfs -R "stat /usr/bin/oled_test" Image/rootfs.img 2>/dev/null | grep -q '^Inode:'; then
    echo 'FAIL duplicate OLED executable remains in rootfs'; exit 1
  fi
  if debugfs -R "stat /etc/init.d/S50oled" Image/rootfs.img 2>/dev/null | grep -q '^Inode:'; then
    echo 'FAIL duplicate OLED init script remains in rootfs'; exit 1
  fi
  grep -q 'DEFAULT_I2C_BUS = 1' "$CUSTOM_DIR/../../include/oled/OledDisplay.h"
  grep -q '0xA8,0x1F,.*32 rows' "$CUSTOM_DIR/../../src/oled/OledDisplay.cpp"
  grep -q '0xDA,0x00,.*panel vendor' "$CUSTOM_DIR/../../src/oled/OledDisplay.cpp"
  grep -q '0x81,0x8F,.*panel vendor contrast' "$CUSTOM_DIR/../../src/oled/OledDisplay.cpp"
  grep -q '0xD9,0x1F,.*panel vendor pre-charge' "$CUSTOM_DIR/../../src/oled/OledDisplay.cpp"
  grep -q '0xA7,.*white background, black pixels' "$CUSTOM_DIR/../../src/oled/OledDisplay.cpp" || {
    echo 'FAIL OLED display polarity is not the verified white-background mode'; exit 1;
  }
  echo 'PASS distributed-matrix is the only OLED owner on /dev/i2c-1'
  ROOTFS_DMS_BINARY="$TMP/rootfs-distributed-matrix"
  debugfs -R "dump -p /usr/bin/distributed-matrix $ROOTFS_DMS_BINARY" Image/rootfs.img >/dev/null 2>&1
  debugfs -R "dump /etc/init.d/S05oled-guard $TMP/S05oled-guard" Image/rootfs.img >/dev/null 2>&1
  cmp "$CUSTOM_DIR/package/distributed-matrix/S05oled-guard" "$TMP/S05oled-guard"
  grep -Fq -- '--oled-boot' "$TMP/S05oled-guard"
  grep -aq 'GPIO14 reset complete; RAM cleared before display-on' "$ROOTFS_DMS_BINARY"
  grep -aq 'GPIO preparation failed; keeping display disabled' "$ROOTFS_DMS_BINARY"
  grep -aq 'DMS OLED: GPIO14 held low; early guard in arch_cpu_init' "$SDK_DIR/u-boot/u-boot"
  python3 - "$SDK_DIR/u-boot/arch/arm/mach-rockchip/rk3568/rk3568.c" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text()
body = text.split('int arch_cpu_init(void)\n{\n', 1)[1]
assert body.startswith('#if !defined(CONFIG_SPL_BUILD)\n\tdms_oled_early_reset();\n#endif\n'), 'OLED reset is not the first CPU init operation'
assert text.count('\tdms_oled_early_reset();') == 1, 'duplicate early OLED hook'
PY
  cmp Image/uboot.img "$SDK_DIR/rockdev/uboot.img"
  cmp "$ROOTFS_DMS_BINARY" \
    "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/target/usr/bin/distributed-matrix"
  echo 'PASS OLED boot guard, fail-closed initialization and freshly built application are packaged'
  for directory in src include web config; do
    diff -qr "$CUSTOM_DIR/../../$directory" \
      "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/build/distributed-matrix-1.0.0/$directory"
  done
  cmp "$CUSTOM_DIR/../../CMakeLists.txt" \
    "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/build/distributed-matrix-1.0.0/CMakeLists.txt"
  echo 'PASS packaged application build sources match the current project one-to-one'

  ROOTFS_SLAVE_CONFIG="$TMP/rootfs-slave_config.json"
  ROOTFS_MASTER_CONFIG="$TMP/rootfs-master_config.json"
  ROOTFS_DEFAULTS="$TMP/rootfs-distributed-matrix.defaults"
  debugfs -R "dump -p /etc/distributed_matrix/slave_config.json $ROOTFS_SLAVE_CONFIG" Image/rootfs.img >/dev/null 2>&1
  debugfs -R "dump -p /etc/distributed_matrix/master_config.json $ROOTFS_MASTER_CONFIG" Image/rootfs.img >/dev/null 2>&1
  debugfs -R "dump -p /etc/default/distributed-matrix $ROOTFS_DEFAULTS" Image/rootfs.img >/dev/null 2>&1
  for token in \
    'Cannot acknowledge KVM session handshake' \
    'KVM target handshake failed'; do
    grep -aFq "$token" "$ROOTFS_DMS_BINARY" || {
      echo "FAIL rootfs KVM ACK handshake is missing: $token"; exit 1;
    }
  done
  echo 'PASS rootfs KVM transport requires target ACK before active takeover'
  cmp "$ROOTFS_SLAVE_CONFIG" "$SDK_DIR/buildroot/package/distributed-matrix/slave_config.json"
  cmp "$ROOTFS_MASTER_CONFIG" "$SDK_DIR/buildroot/package/distributed-matrix/master_config.json"
  cmp "$ROOTFS_DEFAULTS" "$SDK_DIR/buildroot/package/distributed-matrix/distributed-matrix.defaults"
  grep -qx 'ROLE=both' "$ROOTFS_DEFAULTS" || {
    echo 'FAIL rootfs does not start both Master and Slave by default'; exit 1;
  }
  grep -Eq '"http_port"[[:space:]]*:[[:space:]]*8080' "$ROOTFS_MASTER_CONFIG" || {
    echo 'FAIL rootfs Master HTTP port is not 8080'; exit 1;
  }
  grep -q '"capture"' "$ROOTFS_SLAVE_CONFIG"
  if grep -q 'hdmi_capture_device' "$ROOTFS_SLAVE_CONFIG"; then
    echo 'FAIL legacy hdmi_capture_device remains in rootfs configuration'; exit 1
  fi
  grep -q 'hw:CARD=rockchiphdmi,DEV=0' "$ROOTFS_SLAVE_CONFIG" || {
    echo 'FAIL rootfs audio playback is not routed to the HDMI card'; exit 1;
  }
  grep -q 'hw:CARD=rk809,DEV=0' "$ROOTFS_SLAVE_CONFIG" || {
    echo 'FAIL rootfs audio playback is not routed to the RK809 analog card'; exit 1;
  }
  grep -q '"audio_output_mode"[[:space:]]*:[[:space:]]*"both"' "$ROOTFS_SLAVE_CONFIG" || {
    echo 'FAIL rootfs default audio output mode is not both'; exit 1;
  }
  grep -q '"hdmi_audio_volume"[[:space:]]*:[[:space:]]*16\.0' "$ROOTFS_SLAVE_CONFIG" || {
    echo 'FAIL rootfs HDMI playback does not include the verified 16x gain'; exit 1;
  }
  grep -q '"enabled"[[:space:]]*:[[:space:]]*true' "$ROOTFS_SLAVE_CONFIG" &&
  grep -q '"role"[[:space:]]*:[[:space:]]*"target"' "$ROOTFS_SLAVE_CONFIG" || {
    echo 'FAIL rootfs does not enable KVM target mode'; exit 1;
  }
  if grep -q 'audio_playback_devices' "$ROOTFS_SLAVE_CONFIG"; then
    echo 'FAIL obsolete audio_playback_devices remains in rootfs configuration'; exit 1;
  fi
  if grep -q 'audio_playback_device"' "$ROOTFS_SLAVE_CONFIG"; then
    echo 'FAIL legacy single audio playback device remains in rootfs configuration'; exit 1;
  fi
  ROOTFS_INIT_SCRIPT="$TMP/rootfs-S99distributed-matrix"
  debugfs -R "dump -p /etc/init.d/S99distributed-matrix $ROOTFS_INIT_SCRIPT" Image/rootfs.img >/dev/null 2>&1
  for token in \
    'MASTER_PIDFILE=/var/run/distributed-matrix-master.pid' \
    'SLAVE_PIDFILE=/var/run/distributed-matrix-slave.pid' \
    'both)' \
    'ip -4 addr show dev eth0 scope global' \
    'for attempt in 1 2 3' \
    'rm -f "$pidfile"' \
    'continuing with slave' \
    'start_role master' \
    'start_role slave'; do
    grep -Fq "$token" "$ROOTFS_INIT_SCRIPT" || {
      echo "FAIL rootfs dual-role init policy is missing: $token"; exit 1;
    }
  done
  if grep -q 'PIDFILE=/var/run/distributed-matrix.pid' "$ROOTFS_INIT_SCRIPT"; then
    echo 'FAIL legacy single-role PID file remains in rootfs init'; exit 1
  fi
  grep -q 'AUDIO_CARD=rk809' "$ROOTFS_INIT_SCRIPT" || {
    echo 'FAIL rootfs audio init does not select the canonical RK809 card'; exit 1;
  }
  grep -q "cset name='Playback Path' HP" "$ROOTFS_INIT_SCRIPT" || {
    echo 'FAIL rootfs audio init does not enable the 3.5mm headphone path'; exit 1;
  }
  strings "$ROOTFS_DMS_BINARY" | grep -F 'HDMI capture locked: source=%ux%u capture=%ux%u format=%s' >/dev/null || {
    echo 'FAIL rootfs binary does not contain the authoritative HDMI lock probe'; exit 1;
  }
  strings "$ROOTFS_DMS_BINARY" | grep -F 'Capture device produced no frame before timeout' >/dev/null || {
    echo 'FAIL rootfs binary does not require a real V4L2 frame before switching display'; exit 1;
  }
  strings "$ROOTFS_DMS_BINARY" | grep -F 'playback.capture' >/dev/null
  strings "$ROOTFS_DMS_BINARY" | grep -F '/api/audio-volume' >/dev/null || {
    echo 'FAIL rootfs binary does not expose the audio volume API'; exit 1;
  }
  strings "$ROOTFS_DMS_BINARY" | grep -F 'set_audio_volume' >/dev/null || {
    echo 'FAIL rootfs binary does not contain the audio volume command'; exit 1;
  }
  strings "$ROOTFS_DMS_BINARY" | grep -F 'dms_capture_audio_volume_' >/dev/null || {
    echo 'FAIL rootfs binary does not contain runtime GStreamer volume controls'; exit 1;
  }
  echo 'PASS rootfs starts Master:8080 and Slave capture together with runtime audio volume API'

  echo
  echo "[configuration]"
  KCFG="$SDK_DIR/kernel/.config"
  BCFG="$SDK_DIR/buildroot/output/rockchip_dms_rk3566/.config"
  grep -qx 'BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES="board/rockchip/common/base/busybox.fragment package/busybox/busybox_dms_trim.config"' "$BCFG" || { echo "FAIL DMS BusyBox trim fragment not selected"; exit 1; }
  if grep -Eiq '^CONFIG_.*(MALI|RKNPU|RKNN|PANFROST)=[ym]' "$KCFG"; then
    echo 'FAIL prohibited Mali/NPU kernel symbol enabled'; exit 1
  fi
  if grep -Eiq '^BR2_PACKAGE_.*(MALI|VULKAN|RKNPU|RKNN|ANDROID_ADBD)=[ym]' "$BCFG"; then
    echo 'FAIL prohibited Buildroot package enabled'; exit 1
  fi
  for symbol in CONFIG_DRM_ROCKCHIP CONFIG_VIDEO_V4L2 CONFIG_VIDEO_ROCKCHIP_ISP CONFIG_SERIAL_8250 CONFIG_SND_SOC_ROCKCHIP CONFIG_SND_SOC_ROCKCHIP_I2S_TDM CONFIG_SND_SIMPLE_CARD CONFIG_SND_SOC_DUMMY_CODEC CONFIG_USB_GADGET CONFIG_USB_CONFIGFS CONFIG_USB_CONFIGFS_RNDIS CONFIG_USB_CONFIGFS_F_HID; do
    grep -q "^$symbol=[ym]" "$KCFG" || { echo "FAIL required $symbol not enabled"; exit 1; }
    echo "PASS $symbol"
  done
  if grep -q '^BR2_PACKAGE_DROPBEAR_CLIENT=y' "$BCFG"; then
    echo 'FAIL Dropbear client was not trimmed'; exit 1
  fi
  if grep -q '^BR2_PACKAGE_LIBCURL_CURL=y' "$BCFG"; then
    echo 'FAIL curl CLI was not trimmed'; exit 1
  fi
  for symbol in BR2_PACKAGE_LIBCURL_PROXY_SUPPORT BR2_PACKAGE_LIBCURL_COOKIES_SUPPORT BR2_PACKAGE_LIBCURL_EXTRA_PROTOCOLS_FEATURES BR2_PACKAGE_LIBCURL_TLS_NONE BR2_PACKAGE_ANDROID_ADBD BR2_PACKAGE_I2C_TOOLS BR2_PACKAGE_GDB BR2_PACKAGE_GDB_SERVER BR2_PACKAGE_STRACE BR2_PACKAGE_LSOF BR2_PACKAGE_PROCPS_NG BR2_PACKAGE_HTOP BR2_PACKAGE_FILE; do
    if grep -q "^${symbol}=y" "$BCFG"; then
      echo "FAIL unused libcurl feature remains enabled: $symbol"; exit 1
    fi
  done
  echo 'PASS development tools, Dropbear client, curl CLI, ADB and unused libcurl features excluded; production server/HTTPS libcurl retained'
  for symbol in BR2_PACKAGE_DROPBEAR BR2_PACKAGE_OPENSSL BR2_PACKAGE_LIBOPENSSL BR2_PACKAGE_LIBCURL BR2_PACKAGE_LIBCURL_OPENSSL BR2_PACKAGE_ROCKCHIP_MPP BR2_PACKAGE_ROCKCHIP_RGA BR2_PACKAGE_LIBDRM BR2_PACKAGE_LIBV4L BR2_PACKAGE_PICOCOM BR2_PACKAGE_SETSERIAL BR2_PACKAGE_LIBSERIALPORT BR2_PACKAGE_GSTREAMER1 BR2_PACKAGE_GSTREAMER1_ROCKCHIP BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_KMS BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_RTP BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_RTPMANAGER BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_UDP; do
    grep -q "^$symbol=y" "$BCFG" || { echo "FAIL required $symbol not enabled"; exit 1; }
    echo "PASS $symbol"
  done
  for plugin in "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/target/usr/lib/gstreamer-1.0/libgstudp.so" \
                "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/target/usr/lib/gstreamer-1.0/libgstrtp.so" \
                "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/target/usr/lib/gstreamer-1.0/libgstrtpmanager.so"; do
    [[ -s "$plugin" ]] || { echo "FAIL required RTP plugin missing: $plugin"; exit 1; }
    echo "PASS $plugin"
  done
  grep -q 'mppjpegdec' "$ROOTFS_DMS_BINARY" || {
    echo 'FAIL rootfs binary has no hardware JPEG decoder path'; exit 1;
  }
  grep -q 'render-rectangle' "$ROOTFS_DMS_BINARY" || {
    echo 'FAIL rootfs binary has no DRM render-rectangle hardware window path'; exit 1;
  }
  if grep -aq 'dms_window_compositor\|Software window compositor' "$ROOTFS_DMS_BINARY"; then
    echo 'FAIL CPU window compositor fallback remains in rootfs binary'; exit 1;
  fi
  echo 'PASS Mali/Vulkan/RKNN/ADB excluded and required media/network/serial options selected'

  echo
  echo "[hashes]"
  sha256sum "$ROM_IMG" "$ROM_DIR/boot.img" "$ROM_DIR/rootfs.img"
} | tee "$REPORT"

echo "Verification report: $REPORT"
