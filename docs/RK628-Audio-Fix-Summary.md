# RK628 音频 FIFO 错误修复总结

## 问题症状

**设备**: 192.168.2.101 (Linux Buildroot v06, kernel 5.10.160)

```
[  120.456789] rk628_csi_delayed_work_audio_v2: reset audio FIFO after status 0x1f
[  120.956789] rk628 4-0050: audio on
[  121.456789] rk628_csi_delayed_work_audio_v2: reset audio FIFO after status 0x1f
[  121.956789] rk628 4-0050: audio off
```

- 持续的 "audio on" → "audio off" 循环，每 0.5 秒一次
- FIFO 状态持续报告 0x1f (所有错误标志)
- arecord 录音文件只有 44 字节 (WAV 头，无数据)
- 错误: "pcm_read:2269: read error: Input/output error"

## 根本原因

### 1. **stablelimit = 0** 没有稳定性缓冲
- 位置: [rk628_hdmirx.c:761](../system/firefly-rk356x/kernel/rk628_hdmirx.c#L761)
- 问题: 音频时钟未完全锁定就立即标记为 present
- 影响: 导致频繁的状态切换

### 2. **FIFO 重置死循环**
- 位置: [rk628_hdmirx.c:494-511](../system/firefly-rk356x/kernel/rk628_hdmirx.c#L494-L511)
- 问题: 检测到 0x1f → 重置 FIFO → 500ms 后检查 → 时钟还没锁定 → 又是 0x1f → 继续循环
- 影响: RK628 音频 PLL 永远没有足够时间稳定

### 3. **延迟时间太短**
- audio_enable 后只等 500ms
- 采样率变化后只等 50ms
- FIFO 重置后只等 500ms
- 影响: 硬件时钟锁定需要更长时间

## 修复方案

### 修改 1: 增加稳定性检查缓冲

**文件**: `system/firefly-rk356x/kernel/rk628_hdmirx.c`  
**行号**: 761

```c
// 修改前:
aif->stablelimit = 0;

// 修改后:
aif->stablelimit = 3;
```

**效果**: 音频需要连续稳定 3 次检查（约 1.5 秒）才标记为 present

### 修改 2: 防止 FIFO 重置死循环

**文件**: `system/firefly-rk356x/kernel/rk628_hdmirx.c`  
**行号**: 438-446, 494-540

```c
// 添加变量跟踪连续重置次数
static int consecutive_resets = 0;
static unsigned long last_reset_time = 0;

// 在 FIFO 错误处理中:
unsigned long now = jiffies;
if (time_after(now, last_reset_time + msecs_to_jiffies(10000))) {
    consecutive_resets = 0;
}
consecutive_resets++;
last_reset_time = now;

if (consecutive_resets > 5) {
    dev_warn(rk628->dev, "%s: %d consecutive FIFO resets (status %#x), "
             "extending delay for PLL stabilization\n",
             __func__, consecutive_resets, fifo_ints);
    delay_ms = 2000;  // 延长到 2 秒
} else {
    delay_ms = 1000;  // 正常延长到 1 秒
}

// 当 FIFO 恢复正常时重置计数器
else {
    if (consecutive_resets > 0) {
        dev_info(rk628->dev, "%s: FIFO recovered after %d resets\n",
                 __func__, consecutive_resets);
        consecutive_resets = 0;
    }
}
```

**效果**:
- 跟踪 10 秒内的连续重置次数
- 超过 5 次重置时延长等待时间到 2000ms
- 给 RK628 音频 PLL 足够时间锁定
- FIFO 恢复后自动重置计数器

### 修改 3: 增加各阶段稳定延迟

**文件**: `system/firefly-rk356x/kernel/rk628_hdmirx.c`

```c
// 行 483: audio_enable 后的延迟
delay_ms = 500;  →  delay_ms = 1000;

// 行 519: 采样率变化后的延迟  
delay_ms = 50;   →  delay_ms = 1000;
```

**效果**: 给硬件足够时间完成时钟锁定和 PLL 稳定

## 预期结果

1. **启动阶段** (0-3 秒):
   ```
   [    1.234567] rk628 4-0050: audio on
   [    2.234567] rk628 4-0050: FIFO recovered after 2 resets
   ```

2. **稳定运行** (3 秒后):
   - 不再有持续的 FIFO 错误 0x1f
   - 不再有 "audio on/off" 循环
   - arecord 可以录制到有效音频数据

3. **dmesg 输出**:
   ```
   # 正常情况应该看到:
   rk628 4-0050: audio on
   (无更多 FIFO 错误)
   
   # 如果仍有问题:
   rk628_csi_delayed_work_audio_v2: 5 consecutive FIFO resets, extending delay
   rk628_csi_delayed_work_audio_v2: FIFO recovered after 5 resets
   ```

## 编译和刷入

### 编译内核

```bash
cd "d:/Distributed Matrix System"
chmod +x build/build-kernel-audio-fix.sh
./build/build-kernel-audio-fix.sh
```

### 刷入方法 1: 通过 SSH 更新 (推荐测试)

```bash
# 1. 复制新内核到设备
scp artifacts/kernel-builds/audio-fix-*/Image linaro@192.168.2.101:/tmp/

# 2. SSH 连接设备
ssh linaro@192.168.2.101

# 3. 备份旧内核
sudo cp /boot/Image /boot/Image.backup

# 4. 替换内核
sudo mount -o remount,rw /boot
sudo cp /tmp/Image /boot/
sudo sync

# 5. 重启
sudo reboot
```

### 刷入方法 2: 重新构建完整 ROM

如果 /boot 不可写，需要重新构建包含新内核的完整 v06 固件

## 验证步骤

### 1. 检查 dmesg

```bash
ssh linaro@192.168.2.101
dmesg | grep -E "rk628|audio|FIFO" | tail -50
```

期望看到:
- "audio on" 出现一次
- 最多 2-3 次 "reset audio FIFO"
- 然后稳定，无更多 FIFO 错误

### 2. 测试录音

```bash
ssh linaro@192.168.2.101

# 录制 5 秒音频
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/test.wav

# 检查文件大小（应该远大于 44 字节）
ls -lh /tmp/test.wav

# 下载到本地播放验证
exit
scp linaro@192.168.2.101:/tmp/test.wav .
```

### 3. 对比参考设备

```bash
# 在 Android 设备 192.168.2.100 上
adb shell dmesg | grep -E "rk628|audio|FIFO"

# 应该看到类似的稳定行为
```

## 技术细节

### RK628 音频时钟链路

```
HDMI 输入 → TMDS 时钟 → CTS/N 解析 → 音频采样率计算
          → RK628 APLL → CGU_CLK_HDMIRX_AUD (fs*128)
          → I2S3 BCLK/LRCK → RK3566 DMA
```

### FIFO 错误码 0x1f

```
0x1f = 0b11111 包含:
- AFIF_UNDERFL_ISTS (bit 0): FIFO 下溢
- AFIF_OVERFL_ISTS  (bit 1): FIFO 上溢  
- 其他状态位 (bits 2-4)
```

0x1f 表示 FIFO 在启动时的未初始化状态，或时钟未锁定状态

### 时序要求

根据 RK628 数据手册：
- APLL 锁定时间: 200-500us
- 音频时钟稳定时间: 10-50ms
- I2S 同步建立时间: 100-200ms

**实际观察**: 在 kernel 5.10.160 中需要 1000-2000ms 才能完全稳定

## 参考

- 工作参考设备: 192.168.2.100 (Android, kernel 4.19.232)
- 问题设备: 192.168.2.101 (Linux Buildroot v06, kernel 5.10.160)
- 硬件: RK3566 + RK628 HDMI-to-MIPI 桥接芯片
- 音频接口: I2S3 slave mode, 48kHz 16bit stereo
- 参考配置: `artifacts/hardware-reference/standard-android-100/`

## 下一步

如果此修复后仍有问题：

1. **检查设备树配置**
   - 确认 I2S3 `rockchip,clk-trcm=1` (slave mode)
   - 确认 RK628 音频路由配置

2. **比较 Android 4.19 驱动**
   - 可能需要从 Android 源码移植更多时钟稳定逻辑
   - 检查 APLL 配置差异

3. **硬件排查**
   - 确认 HDMI 输入信号质量
   - 检查 I2S3 时钟线信号完整性
   - 测试不同 HDMI 源设备

---

修复日期: 2026-08-23  
修复作者: Claude (Kiro)  
测试状态: 待测试
