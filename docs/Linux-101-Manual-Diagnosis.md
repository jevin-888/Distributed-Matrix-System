# Linux 192.168.2.101 音频诊断手册 (v06 固件后)

**日期**: 2026-08-23  
**问题**: 刷入 v06 固件后仍然没有声音  

---

## 快速诊断步骤

### 步骤 1: SSH 连接到设备
```bash
ssh root@192.168.2.101
# 默认密码通常是: firefly 或 rockchip 或无密码
```

### 步骤 2: 检查音频设备
```bash
# 查看录音设备
arecord -l

# 期望输出示例:
# **** List of CAPTURE Hardware Devices ****
# card 1: hdmiin [rockchip,rk628-hdmiin], device 0: fe430000.i2s-dummy-codec dummy-codec-0 [fe430000.i2s-dummy-codec dummy-codec-0]
#   Subdevices: 1/1
#   Subdevice #0: subdevice #0
```

**问题检查点**:
- [ ] 有 `hdmiin` 卡吗？
- [ ] 卡号是多少？（card 1 还是 card 2？）

---

### 步骤 3: 停止占用进程
```bash
# 停止分布式矩阵服务
/etc/init.d/S99distributed-matrix stop

# 确认没有进程占用音频
ps aux | grep -E 'arecord|distributed-matrix'
killall arecord 2>/dev/null || true
```

---

### 步骤 4: 测试基础采集
```bash
# 清理旧文件
rm -f /tmp/test.wav

# 尝试采集 3 秒（假设 hdmiin 是 card 1）
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 3 /tmp/test.wav

# 查看结果
ls -lh /tmp/test.wav
```

**期望结果**:
- 文件大小约 **576044 字节** (576KB)
- 计算: 3秒 × 48000Hz × 2通道 × 2字节/样本 + 44字节WAV头 = 576044

**如果出错，记录错误信息**:
- `Device or resource busy` → 进程占用
- `No such file or directory` → 设备不存在
- `Invalid argument` → 参数不匹配

---

### 步骤 5: 检查 dmesg 错误
```bash
# 查看最近的音频相关错误
dmesg | grep -iE 'failed to clear|FIFO error|audio|i2s3|rk628' | tail -30
```

**关键错误标识**:
- ✗ `failed to clear 3 on slave mode` → I2S 从模式问题
- ✗ `audio FIFO error 0x1f` → RK628 FIFO 错误
- ✗ `audio open error: Device or resource busy` → 设备被占用

---

### 步骤 6: 检查 PCM 状态
```bash
# 查看 PCM 设备状态
cat /proc/asound/card1/pcm0c/sub0/status

# 期望看到（采集时）:
# state: RUNNING
# 或（空闲时）:
# closed
```

---

### 步骤 7: 检查 I2S3 pinmux
```bash
# 需要挂载 debugfs
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true

# 检查 I2S3 引脚配置
grep -E 'pin 147|pin 148|pin 150' /sys/kernel/debug/pinctrl/*/pinmux-pins

# 期望输出:
# pin 147 (gpio4-19): fe430000.i2s (GPIO UNCLAIMED) function i2s3 group i2s3m1-sclk
# pin 148 (gpio4-20): fe430000.i2s (GPIO UNCLAIMED) function i2s3 group i2s3m1-lrck
# pin 150 (gpio4-22): fe430000.i2s (GPIO UNCLAIMED) function i2s3 group i2s3m1-sdi
```

---

### 步骤 8: 检查 RK628 状态
```bash
# 检查 RK628 音频状态
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_present
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate

# 期望输出:
# audio_present: 1
# audio_rate: 48000
```

---

### 步骤 9: 对比 Android 100 的配置

如果有 Android 设备 (192.168.2.100) 可以访问：

```bash
# 在 Android 设备上通过 adb
adb -s 192.168.2.100:5555 shell cat /proc/asound/cards
adb -s 192.168.2.100:5555 shell cat /proc/asound/card*/pcm*c/sub0/status
```

---

## 常见问题排查

### 问题 1: 找不到 hdmiin 设备
```bash
# 检查设备树是否有 rk628 和 dummy-codec
find /sys/firmware/devicetree/base -name "*rk628*" -o -name "*dummy*"

# 检查加载的模块
lsmod | grep -E 'snd|rk628'

# 手动加载模块
modprobe snd_soc_dummy
modprobe snd_soc_rk628
modprobe snd_soc_rockchip_i2s_tdm
```

### 问题 2: HDMI 信号源问题
```bash
# 检查 HDMI 输入是否有信号
v4l2-ctl -d /dev/video0 --get-dv-timings

# 检查 RK628 视频状态
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/video_timing
```

### 问题 3: 硬件差异
**关键问题**:
1. 192.168.2.100 和 192.168.2.101 是**相同型号的板子**吗？
2. 连接的是**同一个 HDMI 信号源**吗？
3. HDMI 线缆质量如何？

### 问题 4: 固件版本确认
```bash
# 检查 Buildroot 版本
cat /etc/os-release
cat /etc/issue

# 检查固件日期
ls -la /etc/

# 检查是否有音频修复的驱动
modinfo snd_soc_rk628 | grep -i version
strings /lib/modules/*/kernel/sound/soc/rockchip/snd-soc-rk628.ko | grep -i audio
```

---

## 收集诊断信息

请执行以下命令并**将完整输出发给我**：

```bash
# 一键诊断脚本
cat > /tmp/audio-diag.sh << 'EOF'
#!/bin/bash
echo "=== 1. 系统信息 ==="
uname -a
cat /etc/os-release

echo -e "\n=== 2. ALSA 设备 ==="
arecord -l
aplay -l

echo -e "\n=== 3. 音频卡 ==="
cat /proc/asound/cards

echo -e "\n=== 4. 测试采集 ==="
/etc/init.d/S99distributed-matrix stop 2>/dev/null || true
killall arecord 2>/dev/null || true
rm -f /tmp/test.wav
timeout 3 arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 3 /tmp/test.wav 2>&1
ls -lh /tmp/test.wav 2>/dev/null || echo "录音失败"

echo -e "\n=== 5. dmesg 错误 ==="
dmesg | grep -iE 'failed|error|rk628|i2s3|audio' | tail -30

echo -e "\n=== 6. RK628 状态 ==="
cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_* 2>/dev/null || echo "RK628 sysfs 不存在"

echo -e "\n=== 7. 加载的模块 ==="
lsmod | grep -E 'snd|rk628'
EOF

chmod +x /tmp/audio-diag.sh
/tmp/audio-diag.sh
```

---

## 下一步

根据诊断结果：

1. **如果 hdmiin 设备不存在** → 设备树问题，固件可能未正确刷入
2. **如果设备存在但采集失败** → 检查 HDMI 信号源和硬件连接
3. **如果有 FIFO 错误** → 驱动版本问题，需要重新编译固件
4. **如果文件大小为 44 字节** → 没有音频数据，RK628 未接收到音频

请执行诊断脚本，并将输出贴给我！
