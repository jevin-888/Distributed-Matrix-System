#!/bin/bash -e

TARGET_IMG="$1"
ITS="$CHIP_DIR/$2"
KERNEL_IMG="$3"
RAMDISK_IMG="$4"
KERNEL_DTB="$RK_KERNEL_DTB"
RESOURCE_IMG=kernel/resource.img
FDTGET=$(command -v fdtget || true)

if [ ! -f "$ITS" ]; then
	echo "$ITS not exists!"
	exit 1
fi

for required in "$KERNEL_DTB" kernel/logo_kernel.bmp kernel/logo.bmp; do
	[ -s "$required" ] || { echo "Missing FIT resource input: $required" >&2; exit 1; }
done
[ -n "$FDTGET" ] || { echo "fdtget is required to validate the FIT device tree" >&2; exit 1; }

verify_rk628_i2s_master()
{
	local dtb="$1"
	local sound=/rk628-hdmi-rx-sound
	local codec="$sound/simple-audio-card,codec"
	local cpu="$sound/simple-audio-card,cpu"
	local codec_phandle bitclock_master frame_master cpu_phandle

	codec_phandle=$($FDTGET -t x "$dtb" "$codec" phandle)
	bitclock_master=$($FDTGET -t x "$dtb" "$sound" simple-audio-card,bitclock-master)
	frame_master=$($FDTGET -t x "$dtb" "$sound" simple-audio-card,frame-master)
	[ "$bitclock_master" = "$codec_phandle" ] || {
		echo "FIT FDT bitclock-master does not reference the RK628 codec" >&2
		exit 1
	}
	[ "$frame_master" = "$codec_phandle" ] || {
		echo "FIT FDT frame-master does not reference the RK628 codec" >&2
		exit 1
	}
	cpu_phandle=$($FDTGET -t x "$dtb" "$cpu" phandle 2>/dev/null || true)
	[ -z "$cpu_phandle" ] || [ "$bitclock_master" != "$cpu_phandle" ] || {
		echo "FIT FDT incorrectly makes RK3566 I2S3 the clock master" >&2
		exit 1
	}
}

verify_rk809_analog()
{
	local dtb="$1"
	local sound=/rk809-sound
	local codec=/i2c@fdd40000/pmic@20/codec
	local cpu="$sound/simple-audio-card,cpu"
	local cpu_dai i2s1_phandle

	[ "$($FDTGET "$dtb" "$sound" simple-audio-card,name)" = "rk809" ] || {
		echo "FIT FDT RK809 card name is not rk809" >&2
		exit 1
	}
	[ "$($FDTGET "$dtb" "$codec" status)" = "okay" ] || {
		echo "FIT FDT RK809 codec is not enabled" >&2
		exit 1
	}
	cpu_dai=$($FDTGET -t x "$dtb" "$cpu" sound-dai)
	i2s1_phandle=$($FDTGET -t x "$dtb" /i2s@fe410000 phandle)
	[ "$cpu_dai" = "$i2s1_phandle" ] || {
		echo "FIT FDT RK809 card is not connected to I2S1" >&2
		exit 1
	}
}

TMP_ITS=$(mktemp)
RESOURCE_SYNC_DIR=$(mktemp -d)
cleanup()
{
	rm -f "$TMP_ITS"
	rm -rf "$RESOURCE_SYNC_DIR"
}
trap cleanup EXIT

RESOURCE_TOOL=$(realpath rkbin/tools/resource_tool)
install -m 0644 "$KERNEL_DTB" "$RESOURCE_SYNC_DIR/rk-kernel.dtb"
install -m 0644 kernel/logo_kernel.bmp "$RESOURCE_SYNC_DIR/logo_kernel.bmp"
install -m 0644 kernel/logo.bmp "$RESOURCE_SYNC_DIR/logo.bmp"
(
	cd "$RESOURCE_SYNC_DIR"
	"$RESOURCE_TOOL" rk-kernel.dtb logo_kernel.bmp logo.bmp >/dev/null
)
install -m 0644 "$RESOURCE_SYNC_DIR/resource.img" "$RESOURCE_IMG"
mkdir -p "$RESOURCE_SYNC_DIR/verify"
(
	cd "$RESOURCE_SYNC_DIR/verify"
	"$RESOURCE_TOOL" --unpack --image="$RESOURCE_SYNC_DIR/resource.img" >/dev/null
)
cmp "$KERNEL_DTB" "$RESOURCE_SYNC_DIR/verify/out/rk-kernel.dtb"
verify_rk628_i2s_master "$KERNEL_DTB"
verify_rk628_i2s_master "$RESOURCE_SYNC_DIR/verify/out/rk-kernel.dtb"
verify_rk809_analog "$KERNEL_DTB"
verify_rk809_analog "$RESOURCE_SYNC_DIR/verify/out/rk-kernel.dtb"
for node in rk628-audio-codec rk628-hdmi-rx-sound hdmiin i2s-enable-default; do
	strings "$KERNEL_DTB" | grep -q "$node" || { echo "FIT FDT missing $node" >&2; exit 1; }
	strings "$RESOURCE_SYNC_DIR/verify/out/rk-kernel.dtb" | grep -q "$node" || { echo "resource DTB missing $node" >&2; exit 1; }
done

cp "$ITS" "$TMP_ITS"
if [ "$RK_SECURITY" ]; then
	echo "Security boot enabled, removing uboot-ignore ..."
	sed -i "/uboot-ignore/d" "$TMP_ITS"
fi

sed -i -e "s~@KERNEL_DTB@~$(realpath -q "$KERNEL_DTB")~" -e "s~@KERNEL_IMG@~$(realpath -q "$KERNEL_IMG")~" -e "s~@RAMDISK_IMG@~$(realpath -q "$RAMDISK_IMG")~" -e "s~@RESOURCE_IMG@~$(realpath -q "$RESOURCE_IMG")~" "$TMP_ITS"

rkbin/tools/mkimage -f "$TMP_ITS" -E -p 0x800 "$TARGET_IMG"
