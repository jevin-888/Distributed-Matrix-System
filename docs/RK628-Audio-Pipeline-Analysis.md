# RK628 HDMI 音频链路分析

本文记录 Android 参考设备与 Linux RK3566 播放器之间的音频链路差异，内容来自寄存器采样和驱动源码。未确认的寄存器位保持为“待确认”，不能直接作为修复结论。

## 链路

```text
HDMI Source -> RK628 HDMIRX -> RK628 I2S TX -> RK3566 I2S3 RX -> DMA -> ALSA PCM
```

## 已记录寄存器

- HDMI 音频检测：HDMI_RX_PDEC_AUDIODET_CTRL。
- 音频时钟恢复：HDMI_RX_AUDPLL_GEN_CTS、HDMI_RX_AUDPLL_GEN_N。
- FIFO：HDMI_RX_AUD_FIFO_CTRL、HDMI_RX_AUD_FIFO_TH、HDMI_RX_AUD_FIFO_ISTS。
- Android 参考 I2S3：RXCR 0x01c8000f、CKR 0x18003f3f、DMACR 0x000f0010。

## 待确认项

FIFO 状态 0x9 和 0xd 的具体位定义、I2S 主从时钟极性、DMA 启动时序以及 RK628 APLL 锁定状态，必须用同一设备的 Android/Linux 实时寄存器和日志对照确认。未获得这些证据前，不应因单个状态值增加第二套音频初始化路径。

## 建议采样

```bash
cat /sys/kernel/debug/regmap/fe410000.i2s-8ch/registers
dmesg | grep -E 'pll|clock|i2s|audio'
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 test.wav
```

采样文件属于诊断临时文件，验证后应移出交付目录或删除。
