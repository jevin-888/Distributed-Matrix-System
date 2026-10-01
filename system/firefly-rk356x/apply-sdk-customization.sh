#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="/mnt/d/Distributed Matrix System"
CUSTOM_DIR="$PROJECT_DIR/system/firefly-rk356x"
# shellcheck source=build-baseline.env
source "$CUSTOM_DIR/build-baseline.env"

if (( $# != 0 )); then
  echo "This project has one canonical SDK; do not pass an alternate SDK path." >&2
  exit 2
fi
SDK_DIR=$DMS_SDK_DIR
UNIQUE_KERNEL_DIR=$DMS_KERNEL_DIR

if [[ ! -d "$SDK_DIR/buildroot" || ! -d "$SDK_DIR/kernel" ]]; then
  echo "Invalid Firefly SDK directory: $SDK_DIR" >&2
  exit 1
fi
if [[ ! -d "$UNIQUE_KERNEL_DIR" ]] || \
   [[ "$(realpath "$SDK_DIR/kernel")" != "$(realpath "$UNIQUE_KERNEL_DIR")" ]]; then
  echo "SDK kernel must point to the unique Linux 5.10.160 source: $UNIQUE_KERNEL_DIR" >&2
  exit 1
fi
if [[ "$(sed -n 's/^VERSION = //p' "$SDK_DIR/kernel/Makefile")" != "5" ]] || \
   [[ "$(sed -n 's/^PATCHLEVEL = //p' "$SDK_DIR/kernel/Makefile")" != "10" ]] || \
   [[ "$(sed -n 's/^SUBLEVEL = //p' "$SDK_DIR/kernel/Makefile")" != "160" ]]; then
  echo "Refusing non-5.10.160 kernel source" >&2
  exit 1
fi
if [[ ! -f "$PROJECT_DIR/$DMS_DTS_FILE" ]]; then
  echo "Project device tree is missing" >&2
  exit 1
fi
grep -A 4 '^&usbdrd_dwc3' "$PROJECT_DIR/$DMS_DTS_FILE" | \
  grep -Eq 'dr_mode[[:space:]]*=[[:space:]]*"peripheral";' || {
  echo "Project DTS must expose usbdrd_dwc3 in peripheral mode for KVM HID" >&2
  exit 1
}
for symbol in CONFIG_USB_GADGET CONFIG_USB_CONFIGFS CONFIG_USB_CONFIGFS_RNDIS CONFIG_USB_CONFIGFS_F_HID; do
  grep -qx "$symbol=y" "$CUSTOM_DIR/kernel/dms_no_gpu_npu.config" || {
    echo "KVM kernel fragment is missing $symbol=y" >&2
    exit 1
  }
done

# On the RK628-to-RK3566 I2S link the RK628 drives BCLK/LRCK recovered from
# the HDMI source. The simple-card master phandles must therefore reference
# the codec subnode, leaving I2S3 in slave mode.
grep -Eq '^[[:space:]]*rk628_audio_master:[[:space:]]+simple-audio-card,codec[[:space:]]*\{' \
  "$PROJECT_DIR/$DMS_DTS_FILE" || {
  echo "Project DTS has no canonical RK628 codec-master label" >&2
  exit 1
}
for property in bitclock-master frame-master; do
  grep -Eq "^[[:space:]]*simple-audio-card,${property}[[:space:]]*=[[:space:]]*<&rk628_audio_master>;" \
    "$PROJECT_DIR/$DMS_DTS_FILE" || {
    echo "Project DTS ${property} must reference rk628_audio_master" >&2
    exit 1
  }
done

grep -Eq '^[[:space:]]*simple-audio-card,name[[:space:]]*=[[:space:]]*"rk809";' \
  "$PROJECT_DIR/$DMS_DTS_FILE" || {
  echo "Project DTS must expose the RK809 analog output card as rk809" >&2
  exit 1
}
grep -Eq '^[[:space:]]*sound-dai[[:space:]]*=[[:space:]]*<&i2s1_8ch>;' \
  "$PROJECT_DIR/$DMS_DTS_FILE" || {
  echo "Project DTS must connect RK809 to I2S1" >&2
  exit 1
}
grep -A 20 -E '^[[:space:]]*rk809_codec:[[:space:]]+codec[[:space:]]*\{' \
  "$PROJECT_DIR/$DMS_DTS_FILE" | grep -Eq '^[[:space:]]*status[[:space:]]*=[[:space:]]*"okay";' || {
  echo "Project DTS must enable the RK809 codec" >&2
  exit 1
}
if ! grep -R -q 'hw:CARD=rockchiphdmi,DEV=0' "$PROJECT_DIR/config" "$PROJECT_DIR/include" \
    "$PROJECT_DIR/system/firefly-rk356x/package" "$PROJECT_DIR/tests"; then
  echo "Canonical HDMI playback endpoint is missing" >&2
  exit 1
fi
if ! grep -R -q 'hw:CARD=rk809,DEV=0' "$PROJECT_DIR/config" "$PROJECT_DIR/include" \
    "$PROJECT_DIR/system/firefly-rk356x/package" "$PROJECT_DIR/tests"; then
  echo "Canonical RK809 playback endpoint is missing" >&2
  exit 1
fi
grep -q '"audio_output_mode"[[:space:]]*:[[:space:]]*"both"' \
  "$PROJECT_DIR/system/firefly-rk356x/package/distributed-matrix/slave_config.json" || {
  echo "Default audio output mode must be both" >&2
  exit 1
}
grep -q '"hdmi_audio_volume"[[:space:]]*:[[:space:]]*16\.0' \
  "$PROJECT_DIR/system/firefly-rk356x/package/distributed-matrix/slave_config.json" || {
  echo "Verified HDMI capture gain is missing" >&2
  exit 1
}
ROLE_DEFAULTS="$PROJECT_DIR/system/firefly-rk356x/package/distributed-matrix/distributed-matrix.defaults"
INIT_SCRIPT="$PROJECT_DIR/system/firefly-rk356x/package/distributed-matrix/S99distributed-matrix"
MASTER_CONFIG="$PROJECT_DIR/system/firefly-rk356x/package/distributed-matrix/master_config.json"
grep -qx 'ROLE=both' "$ROLE_DEFAULTS" || {
  echo "Controller/player image must start both Master and Slave" >&2
  exit 1
}
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
  grep -Fq "$token" "$INIT_SCRIPT" || {
    echo "Dual-role init policy is missing: $token" >&2
    exit 1
  }
done
grep -Eq '"http_port"[[:space:]]*:[[:space:]]*8080' "$MASTER_CONFIG" || {
  echo "Master HTTP port must be 8080" >&2
  exit 1
}
SLAVE_CONFIG="$PROJECT_DIR/system/firefly-rk356x/package/distributed-matrix/slave_config.json"
grep -Eq '"enabled"[[:space:]]*:[[:space:]]*true' "$SLAVE_CONFIG" &&
grep -Eq '"role"[[:space:]]*:[[:space:]]*"target"' "$SLAVE_CONFIG" || {
  echo "Playback nodes must enable KVM target mode" >&2
  exit 1
}

PACKAGE_SOURCE_LINK=/home/jevin/dms-project
if [[ ! -L "$PACKAGE_SOURCE_LINK" ]] || \
   [[ "$(realpath "$PACKAGE_SOURCE_LINK")" != "$(realpath "$PROJECT_DIR")" ]]; then
  echo "Buildroot package source must be the single project link: $PACKAGE_SOURCE_LINK -> $PROJECT_DIR" >&2
  exit 1
fi
# Remove the obsolete copied source tree. It was not the Buildroot package site
# and could make source auditing ambiguous.
rm -rf /home/jevin/dms-package-source /home/jevin/dms-package-source.tmp

rm -f "$SDK_DIR/buildroot/configs/dms_rk3566_defconfig"

install -D -m 0644 "$CUSTOM_DIR/buildroot/$DMS_BUILDROOT_DEFCONFIG" \
  "$SDK_DIR/buildroot/configs/$DMS_BUILDROOT_DEFCONFIG"
install -D -m 0644 "$CUSTOM_DIR/buildroot/busybox_dms_trim.config" \
  "$SDK_DIR/buildroot/package/busybox/busybox_dms_trim.config"
install -D -m 0644 "$CUSTOM_DIR/buildroot/dms-dropbear-localoptions.h" \
  "$SDK_DIR/buildroot/package/dropbear/dms-localoptions.h"
install -D -m 0644 "$CUSTOM_DIR/device/dms_rk3566_evb2_buildroot_defconfig" \
  "$SDK_DIR/device/rockchip/.chips/rk3566_rk3568/dms_rk3566_evb2_buildroot_defconfig"
install -D -m 0644 "$CUSTOM_DIR/device/parameter-dms-buildroot-fit.txt" \
  "$SDK_DIR/device/rockchip/.chips/rk3566_rk3568/parameter-dms-buildroot-fit.txt"

# cleanall removes SDK/output, including the selected board configuration.
# Recreate it non-interactively from the one canonical DMS board defconfig
# before validating boot policy or starting the Buildroot build.
if [[ ! -f "$SDK_DIR/output/.config" || ! -e "$SDK_DIR/output/defconfig" ]]; then
  (cd "$SDK_DIR" && ./build.sh dms_rk3566_evb2_buildroot_defconfig)
fi
install -D -m 0644 "$CUSTOM_DIR/kernel/dms_no_gpu_npu.config" \
  "$SDK_DIR/kernel/arch/arm64/configs/dms_no_gpu_npu.config"
install -D -m 0644 "$CUSTOM_DIR/kernel/rk628_cru.c" \
  "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.c"
install -D -m 0644 "$CUSTOM_DIR/kernel/rk628_cru.h" \
  "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.h"
install -D -m 0644 "$CUSTOM_DIR/kernel/rk628.c" \
  "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628.c"
install -D -m 0644 "$CUSTOM_DIR/kernel/rk628_hdmirx.c" \
  "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
RK628_CRU_SRC="$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.c"
RK628_CRU_HDR="$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_cru.h"
RK628_CORE_SRC="$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628.c"
RK628_AUDIO_SRC="$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
RK628_CSI_SRC="$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_csi_v4l2.c"
cmp "$CUSTOM_DIR/kernel/rk628_cru.c" "$RK628_CRU_SRC"
cmp "$CUSTOM_DIR/kernel/rk628_cru.h" "$RK628_CRU_HDR"
cmp "$CUSTOM_DIR/kernel/rk628.c" "$RK628_CORE_SRC"
[[ "$(sha256sum "$RK628_CSI_SRC" | awk '{print $1}')" == "$DMS_RK628_CSI_SHA256" ]] || {
  echo "RK628 CSI source differs from the pinned canonical source" >&2
  exit 1
}
grep -q 'int rk628_clk_enable' "$RK628_CRU_HDR" || {
  echo "RK628 HDMI audio clock API is missing" >&2
  exit 1
}
grep -q 'HIWORD_UPDATE(0, CLK_HDMIRX_AUD_GATE_BIT' "$RK628_CRU_SRC" || {
  echo "RK628 HDMI audio gate enable is missing" >&2
  exit 1
}
grep -q 'if (!audio_state->audio_enable)' "$RK628_AUDIO_SRC" || {
  echo "RK628F polling worker must enable AUD_ENABLE after a valid sample rate" >&2
  exit 1
}
grep -q 'rk628_hdmirx_audio_enable(aif);' "$RK628_AUDIO_SRC" || {
  echo "RK628F polling worker audio enable path is missing" >&2
  exit 1
}
grep -q 'regmap_reg_range(HDMI_RX_AUD_CHEXTR_CTRL, HDMI_RX_AUD_SPARE)' \
  "$RK628_CORE_SRC" || {
  echo "RK628 HDMI audio readable range does not include AUD_SPARE" >&2
  exit 1
}
# RK628 is the external BCLK/LRCK master. Install one canonical I2S/TDM
# driver source. Slave mode does not program local MCLK/dividers; RK3568 uses
# synchronized TX/RX reset so a failed FIFO clear cannot leave I2S3 wedged; and
# TRCM keeps the DMA-before-XFER start order. Do not add an HCLK reset, DAI
# prepare hook, or regcache replay.
I2S_TDM_SRC="$SDK_DIR/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c"
I2S_TDM_CANONICAL="$CUSTOM_DIR/kernel/rockchip_i2s_tdm.c"
install -D -m 0644 "$I2S_TDM_CANONICAL" "$I2S_TDM_SRC"
cmp "$I2S_TDM_CANONICAL" "$I2S_TDM_SRC"
if ! sed -n '30,42p' "$I2S_TDM_SRC" | grep -q 'CONFIG_CPU_RK3568' || \
   ! grep -q 'of_device_is_compatible(node, "rockchip,rk3568-i2s-tdm");' "$I2S_TDM_SRC"; then
  echo "RK3568 synchronized TX/RX reset path is missing" >&2
  exit 1
fi
grep -q 'RK3568_I2S3_M1_SEL' "$I2S_TDM_SRC" || {
  echo "RK3568 I2S3 m1 route handling is missing" >&2
  exit 1
}
grep -q 'Keep TRCM enabled for the externally-clocked RK628 link' "$I2S_TDM_SRC" || {
  echo "RK628 I2S3 TRCM slave mode is missing" >&2
  exit 1
}
grep -q 'controller in clock-master mode programs MCLK' "$I2S_TDM_SRC" || {
  echo "RK628 I2S3 slave-mode MCLK guard is missing" >&2
  exit 1
}
grep -q 'rockchip_i2s_tdm_sync_reset(i2s_tdm);' "$I2S_TDM_SRC" || {
  echo "Android-order synchronized TX/RX reset is missing" >&2
  exit 1
}
for obsolete in h_reset slave_reset_prepared rockchip_i2s_tdm_prepare 'RK628 slave reset complete'; do
  if grep -q "$obsolete" "$I2S_TDM_SRC"; then
    echo "Obsolete HCLK/prepare reset code remains: $obsolete" >&2
    exit 1
  fi
done
if grep -q 'SRST_H_I2S3_2CH' "$PROJECT_DIR/$DMS_DTS_FILE" || \
   grep -q 'reset-names = "tx-m", "rx-m", "h"' "$PROJECT_DIR/$DMS_DTS_FILE"; then
  echo "Project DTS still exposes the obsolete I2S3 HCLK reset" >&2
  exit 1
fi
grep -q 'reset-names = "tx-m", "rx-m"' "$PROJECT_DIR/$DMS_DTS_FILE" || {
  echo "Project DTS must expose exactly the I2S3 TX/RX reset pair" >&2
  exit 1
}
grep -q 'rockchip,i2s3-mux-m1;' "$PROJECT_DIR/$DMS_DTS_FILE" || {
  echo "Project DTS must force the physical RK628 link onto I2S3 m1" >&2
  exit 1
}
install -D -m 0644 "$PROJECT_DIR/dummy-codec.c" \
  "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"
install -D -m 0644 "$PROJECT_DIR/$DMS_DTS_FILE" \
  "$SDK_DIR/kernel/arch/arm64/boot/dts/rockchip/rk3566-evb2-lp4x-v10-3566M01-HDMI-1920x1080.dts"
cmp "$PROJECT_DIR/$DMS_DTS_FILE" \
  "$SDK_DIR/kernel/arch/arm64/boot/dts/rockchip/rk3566-evb2-lp4x-v10-3566M01-HDMI-1920x1080.dts"
install -m 0644 "$PROJECT_DIR/assets/logo.bmp" "$SDK_DIR/kernel/logo.bmp"
install -m 0644 "$PROJECT_DIR/assets/logo.bmp" "$SDK_DIR/kernel/logo_kernel.bmp"

# Dedicated Buildroot boot policy for this SDK/board. The stock command first
# enters Android recovery according to misc BCB and does not provide root/CMA.
python3 - "$SDK_DIR/u-boot/include/configs/evb_rk3568.h" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old_settings = r'''#undef ROCKCHIP_DEVICE_SETTINGS
#define ROCKCHIP_DEVICE_SETTINGS \
		"stdin=serial,usbkbd\0" \
		"stdout=serial,vidconsole\0" \
		"stderr=serial,vidconsole\0"
'''
new_settings = r'''#define DMS_LINUX_BOOTARGS \
	"storagemedia=emmc " \
	"androidboot.storagemedia=emmc " \
	"androidboot.mode=normal " \
	"rw rootwait rootfstype=ext4 " \
	"root=PARTUUID=614e0000-0000-4b53-8000-1d28000054a9 " \
	"cma=256M " \
	"earlycon=uart8250,mmio32,0xfe660000 " \
	"console=ttyFIQ0"

#undef ROCKCHIP_DEVICE_SETTINGS
#define ROCKCHIP_DEVICE_SETTINGS \
		"stdin=serial,usbkbd\0" \
		"stdout=serial,vidconsole\0" \
		"stderr=serial,vidconsole\0" \
		"bootargs=" DMS_LINUX_BOOTARGS "\0"
'''
old_boot = '#undef CONFIG_BOOTCOMMAND\n#define CONFIG_BOOTCOMMAND RKIMG_BOOTCOMMAND'
new_boot = r'''#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND \
	"setenv bootargs " DMS_LINUX_BOOTARGS "; " \
	"run rkimg_bootdev; " \
	"boot_fit;"'''
boot_marker = '"setenv bootargs " DMS_LINUX_BOOTARGS'
if boot_marker not in text:
    current_boot = r'''#undef CONFIG_BOOTCOMMAND
#define CONFIG_BOOTCOMMAND \
	"run rkimg_bootdev; " \
	"boot_fit;"'''
    if old_boot in text:
        text = text.replace(old_boot, new_boot, 1)
    elif current_boot in text:
        text = text.replace(current_boot, new_boot, 1)
    else:
        raise SystemExit(f"Cannot find CONFIG_BOOTCOMMAND in {path}")

path.write_text(text)
PY

install -m 0644 "$CUSTOM_DIR/uboot/dms-oled-guard.h" \
  "$SDK_DIR/u-boot/arch/arm/mach-rockchip/rk3568/dms-oled-guard.h"
python3 - "$SDK_DIR/u-boot/arch/arm/mach-rockchip/rk3568/rk3568.c" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
text = text.replace('#if !defined(CONFIG_SPL_BUILD)\n#include "dms-oled-guard.h"\n#endif\n\n', '')
text = text.replace('#include "dms-oled-guard.h"\n', '')
early_hook = 'int arch_cpu_init(void)\n{\n'
if text.count(early_hook) != 1:
    raise SystemExit('Cannot locate unique RK3568 early CPU initialization')
text = text.replace(early_hook,
    '#if !defined(CONFIG_SPL_BUILD)\n#include "dms-oled-guard.h"\n#endif\n\n' + early_hook, 1)
if '\tdms_oled_early_reset();' not in text:
    text = text.replace(early_hook, early_hook +
        '#if !defined(CONFIG_SPL_BUILD)\n\tdms_oled_early_reset();\n#endif\n', 1)
hook = '\tu32 ret = 0;\n\n\tret = uclass_get_device_by_driver(UCLASS_DMC'
if '\tdms_oled_boot_guard();' not in text:
    if hook not in text:
        raise SystemExit('Cannot locate DMC initialization for OLED boot guard')
    text = text.replace(hook, '\tu32 ret = 0;\n\n\tdms_oled_boot_guard();\n\n'
                        '\tret = uclass_get_device_by_driver(UCLASS_DMC', 1)
path.write_text(text)
PY

rm -rf "$SDK_DIR/buildroot/package/distributed-matrix"
cp -a "$CUSTOM_DIR/package/distributed-matrix" "$SDK_DIR/buildroot/package/distributed-matrix"

# Install the maintained FIT packer one-to-one. It rebuilds resource.img from
# the exact DTB embedded in the FIT and refuses mismatched RK628 audio trees.
install -D -m 0755 "$CUSTOM_DIR/device/mk-fitimage-dms.sh" \
  "$SDK_DIR/device/rockchip/common/scripts/mk-fitimage.sh"

# The reduced SDK intentionally omits external/rkscript. Make vendor post-rootfs
# hooks no-ops when that optional source tree is absent instead of logging errors.
for hook in 05-udev.sh 06-async-commit.sh 09-disk.sh; do
  python3 - "$SDK_DIR/device/rockchip/common/post-hooks/$hook" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
guard = '[ -d "$SDK_DIR/external/rkscript" ] || exit 0\n'
if guard not in text:
    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if line.startswith('source '):
            lines.insert(index + 1, '\n' + guard)
            break
    else:
        raise SystemExit(f"Cannot find post-helper source line in {path}")
    path.write_text(''.join(lines))
PY
done

# The Buildroot hook can finish successfully without creating the optional
# post-rootfs log (the log is only created by the Yocto branch). Do not turn a
# valid rootfs build into a failed SDK build just because that optional log is
# absent.
python3 - "$SDK_DIR/device/rockchip/common/build-hooks/30-rootfs.sh" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
old = '\tcat "$RK_LOG_DIR/post-rootfs.log"\n'
new = '\tif [ -f "$RK_LOG_DIR/post-rootfs.log" ]; then\n\t\tcat "$RK_LOG_DIR/post-rootfs.log"\n\tfi\n'
if old in text:
    text = text.replace(old, new, 1)
elif new not in text:
    raise SystemExit(f"Cannot find Buildroot post-rootfs log read in {path}")
path.write_text(text)
PY

# Keep the DMS rootfs partition exactly as declared in parameter.txt. The vendor
# update hook otherwise adds roughly 50 MiB plus inode space and silently changes
# the packed parameter, making it disagree with the maintained partition table.
# Also derive the copied image name from BUILD_DATE instead of the host clock.
python3 - "$SDK_DIR/device/rockchip/common/build-hooks/90-updateimg.sh" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
function_head = 'resize_rootfs_partition()\n{\n'
guard = '''resize_rootfs_partition()
{
\tif [ "${RK_PARAMETER##*/}" = "parameter-dms-buildroot-fit.txt" ]; then
\t\techo "Keeping fixed DMS rootfs partition from parameter.txt"
\t\treturn 0
\tfi
'''
if 'Keeping fixed DMS rootfs partition from parameter.txt' not in text:
    if function_head not in text:
        raise SystemExit(f"Cannot find resize_rootfs_partition in {path}")
    text = text.replace(function_head, guard, 1)
old_day = '\tlocal day=$(date +%y%m%d)\n'
new_day = '\tlocal build_day=${BUILD_DATE:-$(date +%Y%m%d)}\n\tlocal day=${build_day: -6}\n'
if old_day in text:
    text = text.replace(old_day, new_day, 1)
elif new_day not in text:
    raise SystemExit(f"Cannot find image date assignment in {path}")
path.write_text(text)
PY

# Omit the vendor SDK diagnostic bundle (/info, manifest and kernel symbols)
# from the production appliance image.
python3 - "$SDK_DIR/device/rockchip/common/post-hooks/20-info.sh" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
guard = 'rm -rf "$TARGET_DIR/info"\nexit 0\n'
if guard not in text:
    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if line.startswith('source '):
            lines.insert(index + 1, '\n' + guard)
            break
    else:
        raise SystemExit(f"Cannot find post-helper source line in {path}")
    path.write_text(''.join(lines))
PY

python3 - "$SDK_DIR/buildroot/package/Config.in" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text()
line = '\t source "package/distributed-matrix/Config.in"\n'.replace('\t ', '\t')
if 'source "package/distributed-matrix/Config.in"' not in text:
    marker = '\nendmenu\n'
    pos = text.rfind(marker)
    if pos < 0:
        raise SystemExit(f"Cannot find final endmenu in {path}")
    block = '\nmenu "Distributed Matrix"\n\tsource "package/distributed-matrix/Config.in"\nendmenu\n'
    text = text[:pos] + block + text[pos:]
    path.write_text(text)
PY

cd "$SDK_DIR"
./build.sh dms_rk3566_evb2_buildroot_defconfig

# Re-resolve Buildroot from the single checked-in defconfig. Selecting the SDK
# board alone does not rewrite an existing Buildroot output/.config, which would
# otherwise retain stale package choices from an earlier ROM.
make -C "$SDK_DIR/buildroot" \
  O="$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT" \
  $DMS_BUILDROOT_DEFCONFIG

# The package uses a local source directory. Buildroot does not notice changes in
# that directory after the package stamps have been created, so force a clean
# package rebuild on every ROM build. Without this, a new binary/config can be
# copied into the SDK while the old target files are silently reused.
# libcurl feature selection is part of the appliance ABI. Force its package
# rebuild so disabled CLI/protocol features cannot survive in target from an
# older incremental build, then rebuild the application against that library.
TARGET_DIR="$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/target"
rm -f "$TARGET_DIR/usr/bin/lsof" \
  "$TARGET_DIR/usr/sbin/i2cget" "$TARGET_DIR/usr/sbin/i2cset" \
  "$TARGET_DIR/usr/sbin/i2cdump" "$TARGET_DIR/usr/sbin/i2cdetect" \
  "$TARGET_DIR/usr/sbin/i2ctransfer"
make -C "$SDK_DIR/buildroot" \
  O="$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT" \
  busybox-dirclean
make -C "$SDK_DIR/buildroot" \
  O="$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT" \
  libcurl-dirclean
make -C "$SDK_DIR/buildroot" \
  O="$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT" \
  distributed-matrix-dirclean

# Enforce the checked-in trim policy after defconfig resolution. This keeps the
# SDK output one-to-one with the project defconfig and prevents stale defaults
# from silently restoring client/CLI tools.
BR_CONFIG="$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/.config"
grep -qx 'BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES="board/rockchip/common/base/busybox.fragment package/busybox/busybox_dms_trim.config"' "$BR_CONFIG" || { echo "DMS BusyBox trim fragment is not selected" >&2; exit 1; }
for symbol in BR2_PACKAGE_DROPBEAR_CLIENT BR2_PACKAGE_LIBCURL_CURL BR2_PACKAGE_LIBCURL_PROXY_SUPPORT BR2_PACKAGE_LIBCURL_COOKIES_SUPPORT BR2_PACKAGE_LIBCURL_EXTRA_PROTOCOLS_FEATURES BR2_PACKAGE_LIBCURL_TLS_NONE BR2_PACKAGE_ANDROID_ADBD BR2_PACKAGE_I2C_TOOLS BR2_PACKAGE_GDB BR2_PACKAGE_GDB_SERVER BR2_PACKAGE_STRACE BR2_PACKAGE_LSOF BR2_PACKAGE_PROCPS_NG BR2_PACKAGE_HTOP BR2_PACKAGE_FILE; do
  if grep -q "^${symbol}=y" "$BR_CONFIG"; then
    echo "Refusing stale enabled trim symbol: $symbol" >&2
    exit 1
  fi
done
for symbol in BR2_PACKAGE_DROPBEAR BR2_PACKAGE_OPENSSL BR2_PACKAGE_LIBOPENSSL BR2_PACKAGE_LIBCURL BR2_PACKAGE_LIBCURL_OPENSSL BR2_PACKAGE_GSTREAMER1 BR2_PACKAGE_LIBDRM BR2_PACKAGE_LIBV4L BR2_PACKAGE_ROCKCHIP_MPP BR2_PACKAGE_ROCKCHIP_RGA BR2_PACKAGE_PICOCOM BR2_PACKAGE_SETSERIAL BR2_PACKAGE_LIBSERIALPORT BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_RTP BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_RTPMANAGER BR2_PACKAGE_GST1_PLUGINS_GOOD_PLUGIN_UDP; do
  grep -qx "${symbol}=y" "$BR_CONFIG" || { echo "Required DMS symbol missing: $symbol" >&2; exit 1; }
done

# GCC fixincludes copies a rewritten rga/RgaApi.h into the toolchain's
# include-fixed directory but does not bring along the drmrga.h it includes by
# quoted name. Because include-fixed outranks the sysroot in the header search
# order, the SDK's RGA-accelerated pixman then fails with "drmrga.h: No such
# file or directory". The only edit fixincludes makes to that header is a
# comment change, so the copy is pure collateral damage; drop it and let
# <rga/RgaApi.h> resolve to the complete sysroot header set.
for include_fixed in "$SDK_DIR"/buildroot/output/$DMS_BUILDROOT_OUTPUT/host/lib/gcc/aarch64-buildroot-linux-gnu/*/include-fixed; do
  if [[ -d "$include_fixed/rga" ]]; then
    echo "Removing incomplete fixincludes RGA headers: $include_fixed/rga"
    rm -rf "$include_fixed/rga"
  fi
done

# The cleanup above only covers a toolchain that already exists. On a fully
# clean build the toolchain is produced inside "./build.sh buildroot", which
# regenerates the same broken copy after this script has run. Attach the removal
# to pixman itself so it happens immediately before pixman configures, whatever
# order the packages are built in. Doing it pre-configure also keeps the RGA
# feature test honest: a failed test would silently drop 2D acceleration
# instead of failing the build.
python3 - "$SDK_DIR/buildroot/package/pixman/pixman.mk" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
if 'PIXMAN_DMS_DROP_FIXINCLUDES_RGA' not in text:
    marker = '$(eval $(autotools-package))'
    if marker not in text:
        raise SystemExit(f"Cannot find autotools-package eval in {path}")
    block = '''# GCC fixincludes installs an incomplete copy of rga/RgaApi.h into
# include-fixed, which outranks the sysroot and hides the drmrga.h that header
# includes. Drop it before configuring so the RGA feature test and the build
# both see the complete sysroot headers.
define PIXMAN_DMS_DROP_FIXINCLUDES_RGA
\trm -rf $(HOST_DIR)/lib/gcc/$(GNU_TARGET_NAME)/*/include-fixed/rga
endef
PIXMAN_PRE_CONFIGURE_HOOKS += PIXMAN_DMS_DROP_FIXINCLUDES_RGA

'''
    text = text.replace(marker, block + marker, 1)
    path.write_text(text)
PY

# Fail before a long build if the dedicated boot policy was not selected.
grep -qx 'RK_RECOVERY_BASE_CFG=""' "$SDK_DIR/output/.config"
grep -qx 'RK_MISC_IMG="blank-misc.img"' "$SDK_DIR/output/.config"

echo "Applied DMS SDK customization to $SDK_DIR"
