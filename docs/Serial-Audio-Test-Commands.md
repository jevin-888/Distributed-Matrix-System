# 串口终端一键测试指令

## 问题现状

192.168.2.101 设备刷入 v06 固件后仍然无声音，现象：
- ✓ 音频设备存在: `card 1: hdmiin`
- ✗ 持续 FIFO 错误: `reset audio FIFO after status 0x1f`
- ✗ I2S 从模式错误: `failed to clear 3 on slave mode`
- ✗ RK628 sysfs 不存在

## 直接在串口终端执行

**请在你的串口终端（PuTTY/MobaXterm）中直接复制粘贴以下完整命令块：**

```bash
#!/bin/bash
echo "=== AUDIO TEST START ==="
/etc/init.d/S99distributed-matrix stop 2>/dev/null; killall arecord 2>/dev/null; sleep 1
rm -f /tmp/t*.wav

echo "Test 48kHz..."
timeout 3 arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 2 /tmp/t48.wav >/dev/null 2>&1
S48=$(stat -c%s /tmp/t48.wav 2>/dev/null || echo 0)
echo "48kHz: $S48 bytes"

echo "Test 44.1kHz..."
timeout 3 arecord -D hw:1,0 -f S16_LE -r 44100 -c 2 -d 2 /tmp/t44.wav >/dev/null 2>&1
S44=$(stat -c%s /tmp/t44.wav 2>/dev/null || echo 0)
echo "44.1kHz: $S44 bytes"

echo "Test 32kHz..."
timeout 3 arecord -D hw:1,0 -f S16_LE -r 32000 -c 2 -d 2 /tmp/t32.wav >/dev/null 2>&1
S32=$(stat -c%s /tmp/t32.wav 2>/dev/null || echo 0)
echo "32kHz: $S32 bytes"

echo "=== RESULTS ==="
if [ $S48 -gt 1000 ]; then echo "OK: 48kHz works"; elif [ $S44 -gt 1000 ]; then echo "OK: 44.1kHz works"; elif [ $S32 -gt 1000 ]; then echo "OK: 32kHz works"; else echo "FAILED: No audio"; fi
echo "=== TEST END ==="

/etc/init.d/S99distributed-matrix start 2>/dev/null
```

---

## 预期结果

### 如果成功（文件大小 > 1000 字节）：
```
48kHz: 384044 bytes
OK: 48kHz works
```
→ 说明音频采集正常工作

### 如果失败（文件大小 = 0 或 44 字节）：
```
48kHz: 44 bytes
44.1kHz: 44 bytes
32kHz: 44 bytes
FAILED: No audio
```
→ 说明没有音频数据

---

## 失败原因分析

如果所有格式都失败（文件只有 44 字节的 WAV 头），问题是：

### 1. HDMI 信号源音频格式问题 (最可能)

**HDMI 源可能输出压缩音频格式**，RK628 只支持 PCM：

**检查方法**：
- 在 HDMI 信号源设备的音频设置中查看
- 如果是机顶盒/播放器，通常在：设置 → 音频 → HDMI 输出

**解决方法**：
- 修改为 **PCM** 或 **立体声** 或 **2.0 声道**
- ✗ 不要选 "Bitstream" / "源码输出" / "AC3" / "DTS" / "Dolby"

### 2. HDMI 信号源无音频输出

**检查方法**：
```bash
# 在串口执行
v4l2-ctl -d /dev/video0 --get-dv-timings
```

查看是否有视频信号，但可能视频有、音频无。

### 3. HDMI 线缆不支持音频

某些廉价 HDMI 线只传输视频，不传输音频。

**解决方法**：
- 更换一根标准 HDMI 1.4+ 线缆
- 或者直接用 192.168.2.100 的线缆测试

### 4. 硬件批次差异

虽然说硬件相同，但可能：
- I2S 引脚接法不同
- RK628 芯片版本不同（RK628D vs RK628F）

---

## 快速验证方法

**交换 HDMI 信号源测试**（最快的验证方法）：

1. 拔下 192.168.2.100 的 HDMI 线
2. 接到 192.168.2.101 上
3. 重新执行上面的测试命令
4. 如果有声音 → 确认是原 HDMI 源的问题
5. 如果还是没声音 → 101 设备本身有问题

---

## 请把结果告诉我

执行完测试命令后，请告诉我：
1. **文件大小**：48kHz/44.1kHz/32kHz 的字节数
2. **HDMI 信号源型号**：连接到 101 的是什么设备？
3. **是否可以交换测试**：能否把 100 的 HDMI 线接到 101？

这样我才能给出精准的解决方案！
