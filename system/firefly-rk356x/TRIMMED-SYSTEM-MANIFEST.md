# RK3566 DMS Production System Manifest

Updated: 2026-08-25

## Build Baseline

- Firefly RK356x Linux SDK v1.4.0c
- Linux 5.10.160, one canonical kernel source tree
- Buildroot + glibc
- RKMPP + RGA + DRM/KMS
- No Mali, Vulkan, RKNN, or RKNPU
- One project DTS and one versioned complete-ROM output path

Canonical paths are defined in `build-baseline.env`. The SDK `kernel` link
resolves only to:

```text
/home/jevin/dms-build-20260806/kernel-5.10.160-original-baseline
```

## Required Features Kept

- RK628 MIPI CSI video capture through V4L2
- RK628 audio capture through I2S3 and ALSA
- GStreamer and Rockchip MPP hardware codecs
- RGA and DRM/KMS display
- Ethernet, DHCP, iproute2, ethtool, and Dropbear SSH server
- RS232/RS485, picocom, setserial, and libserialport
- OLED owned only by `distributed-matrix`
- OpenSSL and HTTPS-capable libcurl for the application

## Removed At Source Configuration

- Mali userspace and all Mali kernel families
- Vulkan loader, headers, and tools
- RKNN/RKNPU
- ADB userspace
- Wi-Fi and Bluetooth stacks for this wired product
- Xorg, Wayland, Weston, Mesa, Chromium, Qt, and development/debug packages
- curl CLI, SSH clients, I2C diagnostic tools, lsof, and unused BusyBox applets
- Duplicate OLED executable/service and temporary audio hotfix tools

Trimming is defined by Buildroot, BusyBox, and kernel configuration. It is not
performed by deleting files from a finished ROM.

## Historical Artifact

```text
D:\Distributed Matrix System\artifacts\buildroot-rom\v08\update_v08.img
SHA256 0b4555e80327b55745c36174d03cf3e5daf2bf6d44a86d3c1a0b75db5b37d015
Size   600427082 bytes
```

This V08 image is retained as historical hardware evidence. It is not the
current release name or a second build entry. Current releases must be created
only with `build-rom.sh` and named `hsvj-engine-<version>-H6_M.img`.
The artifact directory also contains `BUILD-MANIFEST.txt`, `VERIFICATION.txt`,
and `SHA256SUMS`. Superseded generated ROMs are removed; the reference ROMs in
`D:\356x` are not project build artifacts and are left untouched.

## Verification Status

- Complete Rockchip update image unpack: pass
- Required partitions present, recovery absent: pass
- Blank misc and direct Buildroot FIT boot policy: pass
- FIT kernel equals the freshly built canonical Linux Image: pass
- FIT DTB equals resource DTB: pass
- RK628 codec is I2S clock master; RK3566 I2S3 is slave: pass
- Rootfs ext4 filesystem check: pass
- Required media, network, serial, ALSA, and OLED runtime files: pass
- Mali/Vulkan/RKNN/ADB exclusion: pass
- Device boot after V08 flash: pass, confirmed on target board 2026-08-25
- Rootfs ext4 extents without incompatible JBD2 journal: pass
- RK628 audio static fix: I2S3 m1 route and RK3568 TX/RX synchronized reset included
