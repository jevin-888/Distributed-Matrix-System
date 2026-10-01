# RK628 HDMI 输入诊断

本记录整理 2026-08-07 的 RK3566/RK628 设备采样，目标系统为 Linux 5.10.160 aarch64。

## 已确认

- 输入时序为 1920x1080 progressive，pixel clock 148500000 Hz，60 fps。
- /dev/video0 支持 NV16 和 NV12，stepwise 范围从 64x64 到 1920x1080。
- 1920x1080 三帧大小：NV16 为 12,441,600 bytes，NV12 为 9,331,200 bytes。
- KMS plane 78 可接受 NV16 和 NV12；采样 pipeline 使用 v4l2src 与 kmssink。
- HPD/EDID 和 CSI stream OFF/ON 恢复流程已在设备上验证。

## 复核建议

1. 使用 v4l2-ctl -d /dev/video0 --list-formats-ext 确认驱动格式。
2. 使用 modetest 确认 connector 113、plane 78 和像素格式。
3. 记录 watchdog、HDMI capture lost、HDMI capture recovered 日志。
4. 复测热插拔后帧流和显示状态。

## 限制

历史日志曾出现 packed YUV 与 NV16/NV12 之间的格式讨论。当前应用路径以设备实际报告的 NV16/NV12 为准，不应根据旧日志新增第二套播放器 pipeline。
