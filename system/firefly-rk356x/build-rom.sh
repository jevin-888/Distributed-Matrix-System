#!/usr/bin/env bash
set -euo pipefail
export PATH="$HOME/.local/miniconda2/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

PROJECT_DIR="/mnt/d/Distributed Matrix System"
CUSTOM_DIR="$PROJECT_DIR/system/firefly-rk356x"
# shellcheck source=build-baseline.env
source "$CUSTOM_DIR/build-baseline.env"

SDK_DIR=$DMS_SDK_DIR
OUT_ROOT="$PROJECT_DIR/artifacts/buildroot-rom"
mkdir -p "$OUT_ROOT"
if (( $# > 1 )); then
  echo "Usage: $0 [version]" >&2
  exit 2
fi
PROJECT_VERSION=$(sed 's/\r$//' "$PROJECT_DIR/CMakeLists.txt" | sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)[[:space:]]*$/\1/p')
if [[ -z "$PROJECT_VERSION" ]]; then
  echo "Could not read project version from $PROJECT_DIR/CMakeLists.txt" >&2
  exit 2
fi
ROM_VERSION=${1:-${DMS_RELEASE_VERSION:-$PROJECT_VERSION}}
if [[ "$ROM_VERSION" != "$PROJECT_VERSION" ]]; then
  echo "Release version must match CMake project version $PROJECT_VERSION" >&2
  exit 2
fi
if [[ ! "$ROM_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Invalid release version '$ROM_VERSION'; expected major.minor.patch" >&2
  exit 2
fi
ROM_NAME="hsvj-engine-${ROM_VERSION}-H6_M.img"
OUT_DIR="$OUT_ROOT/$ROM_VERSION"
case "$OUT_DIR" in
  "$OUT_ROOT"/*) ;;
  *) echo "Refusing unsafe output directory: $OUT_DIR" >&2; exit 1 ;;
esac
if [[ -e "$OUT_DIR" ]]; then
  echo "ROM version already exists and will not be overwritten: $OUT_DIR" >&2
  exit 1
fi

"$CUSTOM_DIR/apply-sdk-customization.sh"
# This is a full ROM build. Clean only generated outputs so removed files
# (especially old oled_test binaries and old package sources) cannot survive
# in the next rootfs. Do not call SDK cleanall here: it deletes output/.config
# and output/defconfig, which makes the vendor script enter an interactive
# defconfig picker and can select the wrong board. The canonical SDK/kernel
# source tree is never deleted.
rm -rf "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT"
rm -rf \
  "$SDK_DIR/rockdev" \
  "$SDK_DIR/output/firmware" \
  "$SDK_DIR/output/security-firmware" \
  "$SDK_DIR/output/buildroot" \
  "$SDK_DIR/output/rootfs"
mkdir -p "$SDK_DIR/rockdev" "$SDK_DIR/output/firmware" "$SDK_DIR/output/security-firmware"
KERNEL_DTS="$SDK_DIR/kernel/arch/arm64/boot/dts/rockchip/${DMS_DTS_FILE%.dtsi}.dts"
cmp -s "$PROJECT_DIR/$DMS_DTS_FILE" "$KERNEL_DTS" || {
  echo "Project DTS and kernel build DTS are not identical" >&2
  exit 1
}
cd "$SDK_DIR"

./build.sh buildroot
./build.sh kernel
./build.sh uboot
./build.sh firmware
rm -f "$SDK_DIR/rockdev/update.img"
./build.sh updateimg

mkdir -p "$OUT_DIR"
cp -f rockdev/update.img "$OUT_DIR/$ROM_NAME"
# Component images are temporary verification inputs. Only the complete update
# image is retained after verification to avoid wasting disk space.
cp -f rockdev/boot.img "$OUT_DIR/boot.img"
cp -f rockdev/rootfs.img "$OUT_DIR/rootfs.img"

{
  printf 'BUILD_DATE=%s\n' "$(date -Iseconds)"
  printf 'ROM_VERSION=%s\n' "$ROM_VERSION"
  printf 'ROM_FILE=%s\n' "$ROM_NAME"
  printf 'BUILD_MODE=clean-full\n'
  printf 'SDK=%s\n' "$(realpath "$SDK_DIR")"
  printf 'KERNEL=%s\n' "$(realpath "$SDK_DIR/kernel")"
  printf 'KERNEL_VERSION=%s\n' "$DMS_KERNEL_VERSION"
  printf 'BUILDROOT_DEFCONFIG=%s\n' "$DMS_BUILDROOT_DEFCONFIG"
  printf 'DTS=%s\n' "$PROJECT_DIR/$DMS_DTS_FILE"
  printf '%s\n' '--- source hashes ---'
  sha256sum "$PROJECT_DIR/$DMS_DTS_FILE"
  sha256sum "$KERNEL_DTS"
  sha256sum "$CUSTOM_DIR/kernel/rk628.c"
  sha256sum "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628.c"
  sha256sum "$CUSTOM_DIR/kernel/rk628_hdmirx.c"
  sha256sum "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_hdmirx.c"
  sha256sum "$SDK_DIR/kernel/drivers/media/i2c/rk628/rk628_csi_v4l2.c"
  sha256sum "$CUSTOM_DIR/kernel/rockchip_i2s_tdm.c"
  sha256sum "$SDK_DIR/kernel/sound/soc/rockchip/rockchip_i2s_tdm.c"
  sha256sum "$PROJECT_DIR/dummy-codec.c"
  sha256sum "$SDK_DIR/kernel/sound/soc/codecs/dummy-codec.c"
  sha256sum "$SDK_DIR/kernel/.config"
  sha256sum "$CUSTOM_DIR/buildroot/$DMS_BUILDROOT_DEFCONFIG"
  sha256sum "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/.config"
  sha256sum "$CUSTOM_DIR/device/dms_rk3566_evb2_buildroot_defconfig"
  sha256sum "$SDK_DIR/output/.config"
  sha256sum "$SDK_DIR/kernel/resource.img"
  sha256sum "$CUSTOM_DIR/uboot/dms-oled-guard.h"
  sha256sum "$SDK_DIR/buildroot/output/$DMS_BUILDROOT_OUTPUT/target/usr/bin/distributed-matrix"
} > "$OUT_DIR/BUILD-MANIFEST.txt"
(
  cd "$PROJECT_DIR"
  find src include web config system/firefly-rk356x/package/distributed-matrix \
    -type f -print0 | sort -z | xargs -0 sha256sum
  sha256sum CMakeLists.txt
) > "$OUT_DIR/SOURCE-SHA256SUMS"
(
  cd "$OUT_DIR"
  sha256sum boot.img rootfs.img "$ROM_NAME" > SHA256SUMS.verification
)
{
  printf '%s\n' '--- artifact hashes ---'
  cat "$OUT_DIR/SHA256SUMS.verification"
} >> "$OUT_DIR/BUILD-MANIFEST.txt"
"$CUSTOM_DIR/verify-rom.sh" "$SDK_DIR" "$OUT_DIR" \
  "$OUT_DIR/$ROM_NAME"
rm -f "$OUT_DIR/boot.img" "$OUT_DIR/rootfs.img" "$OUT_DIR/SHA256SUMS.verification"
(
  cd "$OUT_DIR"
  sha256sum "$ROM_NAME" > SHA256SUMS
)

printf 'ROM_VERSION=%s\n' "$ROM_VERSION"
printf 'ROM_FILE=%s\n' "$OUT_DIR/$ROM_NAME"
printf 'ROM_DIR=%s\n' "$OUT_DIR"
ls -lh "$OUT_DIR"
cat "$OUT_DIR/SHA256SUMS"
