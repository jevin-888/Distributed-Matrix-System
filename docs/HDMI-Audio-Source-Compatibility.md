# HDMI 音频源兼容性问题排查

**日期**: 2026-08-23  
**设备**: 192.168.2.101 (v06 固件)  
**问题**: 刷入 v06 后仍无声音  
**新发现**: 101 和 100 连接不同 HDMI 信号源  

---

## 问题分析

### 已知信息
- ✓ 硬件完全相同 (RK3566 + RK628)
- ✓ 固件已更新到 v06 (包含音频修复)
- ✓ 两个 HDMI 信号源都有声音输出
- ✗ 192.168.2.101 采集无声音
- ✓ 192.168.2.100 采集正常

### 可能原因
**不同 HDMI 信号源的音频格式差异**:
1. **音频编码格式** - PCM vs 压缩音频 (AC3/DTS)
2. **采样率** - 48kHz vs 44.1kHz vs 32kHz
3. **位深度** - 16bit vs 24bit vs 32bit
4. **声道布局** - 立体声 vs 多声道
5. **音频时钟** - 不同的 HDMI 时钟可能导致 RK628 PLL 锁定失败

---

## 诊断步骤

### 1. 检查 RK628 是否检测到音频

```bash
ssh root@192.168.2.101

# 检查音频存在标志
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_present
# 期望: 1

# 检查检测到的采样率
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate
# 期望: 48000 或 44100
```

**如果 `audio_present` 是 0**:
- RK628 根本没有检测到音频信号
- 可能原因: HDMI 源发送的是压缩音频 (非 PCM)

**如果 `audio_rate` 不是 48000**:
- 需要调整 arecord 的采样率参数

---

### 2. 测试不同音频格式

```bash
# 自动化测试脚本
./build/diagnose-hdmi-audio-source.sh

# 或手动测试不同采样率
for rate in 48000 44100 32000; do
    echo "测试 ${rate}Hz..."
    arecord -D hw:1,0 -f S16_LE -r $rate -c 2 -d 2 /tmp/test-${rate}.wav 2>&1
    ls -lh /tmp/test-${rate}.wav
done
```

---

### 3. 检查 HDMI 音频格式声明 (EDID)

HDMI 信号源会根据 EDID 决定发送什么格式的音频。

```bash
# 查看 EDID 中的音频能力声明
v4l2-ctl -d /dev/video0 --get-edid | edid-decode | grep -A10 "Audio"
```

**关键信息**:
- 支持的音频格式 (PCM, AC3, DTS...)
- 支持的采样率 (32/44.1/48 kHz)
- 支持的位深度 (16/20/24 bit)

---

### 4. 对比两个 HDMI 信号源

| 项目 | 192.168.2.100 源 | 192.168.2.101 源 |
|------|-----------------|-----------------|
| 信号源设备型号 | ? | ? |
| audio_present | 1 | ? |
| audio_rate | 48000 | ? |
| 测试 48kHz 采集 | ✓ 成功 | ✗ 失败 |
| 测试 44.1kHz 采集 | ? | ? |

---

## 解决方案

### 方案 1: 调整采样率匹配源

如果 `audio_rate` 显示的不是 48000:

```bash
# 假设检测到 44100Hz
RATE=$(cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate)
arecord -D hw:1,0 -f S16_LE -r $RATE -c 2 -d 3 /tmp/test.wav

# 更新配置文件
vi /opt/distributed-matrix/share/distributed_matrix/config/slave_config.json
# 修改 audio_capture_device 参数
```

### 方案 2: 强制 HDMI 源输出 PCM 48kHz

**如果 HDMI 源是可配置的设备** (如机顶盒、播放器):
1. 进入音频设置
2. 选择 "PCM" 或 "立体声" (而不是 "自动" 或 "源码输出")
3. 设置采样率为 48kHz

### 方案 3: 修改 RK628 驱动配置

如果 `audio_present = 0`，可能需要修改 RK628 驱动的音频检测逻辑：

```bash
# 检查 dmesg 中 RK628 的音频检测日志
dmesg | grep -i 'rk628.*audio'

# 可能看到:
# "audio format not supported" 
# "compressed audio detected"
```

**临时解决** (需要重新编译):
修改 `system/firefly-rk356x/kernel/rk628_hdmirx.c` 中的音频检测逻辑，强制接受更多音频格式。

### 方案 4: 交换 HDMI 信号源测试

**最简单的验证方法**:
1. 将 192.168.2.100 的 HDMI 源接到 192.168.2.101
2. 测试 101 是否有声音
3. 如果有声音 → 确认是信号源兼容性问题
4. 如果还是没声音 → 101 设备本身有问题

---

## 常见 HDMI 音频格式问题

### 压缩音频格式
某些设备默认输出压缩音频 (需要特殊解码器):
- **AC3 (Dolby Digital)** - 5.1 环绕声
- **DTS** - 另一种环绕声格式
- **E-AC3 / TrueHD** - 高清音频

**RK628 只支持 PCM 格式**，不支持压缩音频。

### 检查方法
```bash
# 如果 dmesg 中有这些关键词，说明是压缩音频:
dmesg | grep -iE 'non-pcm|compressed|ac3|dts|dolby'
```

### 解决
在 HDMI 源设备设置中:
- ✓ 选择 "PCM" / "立体声" / "2.0声道"
- ✗ 不要选择 "Bitstream" / "源码输出" / "环绕声"

---

## 快速诊断命令

```bash
# 一键诊断脚本
ssh root@192.168.2.101 << 'EOF'
echo "=== RK628 音频检测 ==="
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_present
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate

echo -e "\n=== 测试采集 ==="
/etc/init.d/S99distributed-matrix stop
RATE=$(cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate)
echo "检测到采样率: $RATE Hz"
arecord -D hw:1,0 -f S16_LE -r $RATE -c 2 -d 2 /tmp/test.wav 2>&1
ls -lh /tmp/test.wav

echo -e "\n=== dmesg 音频 ==="
dmesg | grep -iE 'rk628.*audio|audio.*present' | tail -10
EOF
```

---

## 下一步

1. **执行诊断脚本**: `./build/diagnose-hdmi-audio-source.sh`
2. **查看 `audio_present` 和 `audio_rate` 的值**
3. **根据结果选择对应方案**
4. **如果仍然失败，考虑交换 HDMI 源验证**

请把诊断脚本的输出发给我！
