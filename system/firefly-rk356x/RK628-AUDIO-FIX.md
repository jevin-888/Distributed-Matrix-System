# RK628 HDMI Capture Audio Baseline

Updated: 2026-08-25

## Clock Domains

There are two separate clock relationships:

1. The PC is the HDMI source clock master and RK628 is the HDMI receiver.
2. RK628 recovers the HDMI audio clock and drives BCLK/LRCK on the board I2S
   link. RK3566 I2S3 receives those clocks and therefore runs in slave mode.

The simple-audio-card `bitclock-master` and `frame-master` properties must
reference the RK628 codec subnode. The verified FIT DTB resolves both values to
codec phandle `0x12a`, not the RK3566 I2S3 CPU subnode.

## Audio Path

```text
PC HDMI source
  -> RK628 HDMI RX
  -> RK628 I2S output (BCLK/LRCK master)
  -> RK3566 I2S3 capture (slave)
  -> ALSA hdmiin
  -> distributed-matrix audio pipeline
```

Video uses the independent RK628 MIPI CSI -> V4L2 path. Audio is not embedded
in the V4L2 video stream.

The production PCM API is capture-only, `S16_LE`, 48 kHz, stereo.

## Canonical Sources

- SDK: `/home/jevin/dms-sdk/rk356x-v1.4.0c-min`
- Kernel: `/home/jevin/dms-build-20260806/kernel-5.10.160-original-baseline`
- DTS: `D:\Distributed Matrix System\rk3566-evb2-lp4x-v10-3566M01-HDMI-1920x1080.dtsi`
- RK628 regmap: `system/firefly-rk356x/kernel/rk628.c`
- RK628 audio driver: `system/firefly-rk356x/kernel/rk628_hdmirx.c`
- I2S driver: `system/firefly-rk356x/kernel/rockchip_i2s_tdm.c`
- Codec API: `dummy-codec.c`

`build-rom.sh` synchronizes these sources into the SDK and rejects any DTS,
FIT, resource DTB, RK628 CSI source, or kernel baseline mismatch.

## Hardware-Verified Failure And Fix

```text
I2S3 pin activity: GPIO4_C3=SCLK, GPIO4_C4=LRCK, GPIO4_C6=SDI
Broken boot route: GRF 0xfdc60310 = 0x00000221 (I2S3 m0)
Working route:     GRF 0xfdc60310 = 0x00004221 (I2S3 m1)
```

The board routes RK628 audio to I2S3 m1, but pinctrl alone did not select the
internal m1 input. The DTS now opts into a board-specific
`rockchip,i2s3-mux-m1` property; the I2S driver accepts it only for RK3568
I2S3 at `0xfe430000` and writes GRF IOMUX bit 14.

After an I2S slave FIFO clear timeout, `I2S_CLR` remained `0x3`. RK3568 had
been excluded from `HAVE_SYNC_RESET`, so the recovery call did nothing. The
driver now maps `rockchip,cru`, resolves only the DTS `tx-m`/`rx-m` reset IDs,
and resets both domains together. The target-board hot test proved this exact
reset (`SRST_M_I2S3_2CH_TX=85`, `RX=86`) preserves configuration and restores
DMA capture.

With m1 plus the synchronized reset, the target board produced:

```text
I2S_CLR       = 0
I2S_RXFIFOLR = 0x0e
DMA IRQ      = 0 -> 16
hw_ptr       = 14312
2-second WAV = 384044 bytes, 377934 non-zero PCM bytes
arecord      = 0
```

The RK628 regmap readable range now also contains `HDMI_RX_AUD_SPARE`
(`0x30268`), eliminating the previous `ret=-5` register read.

## Historical ROM Status

V08 is a historical image containing the audio fixes and the rootfs boot fix.
It passed the package checks and booted normally on the target board after
flashing on 2026-08-25. It is retained as evidence only.

```text
D:\Distributed Matrix System\artifacts\buildroot-rom\v08\update_v08.img
SHA256 0b4555e80327b55745c36174d03cf3e5daf2bf6d44a86d3c1a0b75db5b37d015
Size   600427082 bytes
```

Current ROM delivery uses only `system/firefly-rk356x/build-rom.sh` and the
versioned name `hsvj-engine-<version>-H6_M.img`; historical V08 files are not a
second build branch.

The Android reference is `AUD_FIFO_TH=0x01001008`
(`START=64`, `MAX=8`, `MIN=8`). The RK628 worker accepts the normal `0x9`
startup state and applies the vendor single/double FIFO recovery sequence only
for real underflow/overflow states.
