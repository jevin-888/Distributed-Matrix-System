# v10 ROM 完整修复清单

## 修复的配置（完全对齐 Android）

### 1. FIFO 阈值 (0x308004) ✅
```c
// Android: 0x01000000 (START=64, MAX=0, MIN=0)
#define RK628_AUDIO_FIFO_START  64
#define RK628_AUDIO_FIFO_MAX    0   // 改为 0 (v09 是 8)
#define RK628_AUDIO_FIFO_MIN    0   // 改为 0 (v09 是 8)
```
**效果**: 禁用硬件 FIFO 阈值中断，使用软件轮询

---

### 2. FIFO 状态判断逻辑 ✅
```c
// v09: 只接受 0x9
if ((fifo_ints != 0x9) && (fifo_ints & UNDERFLOW/OVERFLOW))

// v10: 接受 0x9 和 0xd  
if ((fifo_ints != 0x9) && (fifo_ints != 0xd) && (fifo_ints & UNDERFLOW/OVERFLOW))
```
**效果**: 不再将 0xd 误判为错误，避免每秒重置循环

---

### 3. SAO_CTRL (0x308020) ✅
```c
// v09: 使用宏计算，结果 = 0x000001e1
rk628_i2c_write(rk628, HDMI_RX_AUD_SAO_CTRL,
    I2S_LPCM_BPCUV(0) | I2S_32_16(1) | I2S_DATA_ENABLE_BITS(0xf));

// v10: 直接匹配 Android 值
rk628_i2c_write(rk628, HDMI_RX_AUD_SAO_CTRL, 0x00000100);
```
**效果**: I2S 输出控制寄存器完全匹配 Android

---

### 4. PAO_CTRL (0x308024) ✅
```c
// v09: 写入 0
rk628_i2c_write(rk628, HDMI_RX_AUD_PAO_CTRL, PAO_RATE(0));

// v10: 匹配 Android 值
rk628_i2c_write(rk628, HDMI_RX_AUD_PAO_CTRL, 0x00000f00);
```
**效果**: Parallel Audio Output 控制匹配 Android

---

### 5. 未知寄存器 0x30824c ✅ 新增
```c
// v09: 完全缺失

// v10: 添加 Android 的配置
rk628_i2c_write(rk628, 0x30824c, 0x0030c15c);
```
**效果**: 配置一个未记录的寄存器，可能是关键控制位

---

### 6. FIFO 重置延迟优化 ✅
```c
// v09: 1000ms
// v10: 1500ms → 2000ms → 3000ms (根据重置次数递增)
```
**效果**: 给 PLL 更多时间稳定

---

## 预期效果

**v09 的问题链**:
1. FIFO MAX/MIN=8 太敏感 → 硬件频繁触发中断
2. 误判 0xd 为错误 → 每秒重置 FIFO
3. stablecount 永远 < 3 → audio_enable 永远 false
4. I2S 数据未启用 → DMA 无数据
5. arecord I/O 错误

**v10 的修复链**:
1. ✅ FIFO MAX/MIN=0 → 禁用硬件中断，软件轮询
2. ✅ 接受 0xd 状态 → FIFO 不再频繁重置
3. ✅ stablecount 能达到 3 → audio_enable = true
4. ✅ SAO_CTRL/PAO_CTRL/0x24c 完全匹配 Android → I2S 输出正确配置
5. ✅ I2S 数据流 → DMA 传输 → arecord 成功

---

## 寄存器配置对比表

| 寄存器 | Android | Linux v09 | Linux v10 | 状态 |
|--------|---------|-----------|-----------|------|
| 0x308004 (FIFO_TH) | 0x01000000 | 0x01001008 | 0x01000000 | ✅ 匹配 |
| 0x308020 (SAO_CTRL) | 0x00000100 | 0x000001e1 | 0x00000100 | ✅ 匹配 |
| 0x308024 (PAO_CTRL) | 0x00000f00 | 0x00000000 | 0x00000f00 | ✅ 匹配 |
| 0x30824c (未知) | 0x0030c15c | (缺失) | 0x0030c15c | ✅ 匹配 |
| FIFO 状态判断 | 接受多种 | 只接受 0x9 | 接受 0x9/0xd | ✅ 修复 |

---

## 构建命令

```bash
wsl bash -c "cd /home/jevin/dms-sdk/rk356x-v1.4.0c-min && \
cp /mnt/d/'Distributed Matrix System'/system/firefly-rk356x/kernel/rk628_hdmirx.c \
   buildroot/output/rockchip_dms_rk3566/build/linux-custom/drivers/media/i2c/rk628/ && \
make dms-audio-fix-v10"
```

---

## 测试验证

刷机后在设备上执行:
```bash
# 1. 检查 FIFO 是否不再频繁重置
dmesg | grep FIFO | tail -20

# 2. 录音测试
arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 5 /tmp/test-v10.wav
ls -lh /tmp/test-v10.wav

# 预期: 文件 > 900KB，有声音
```

---

## 结论

**v10 现在完全对齐了 Android 的关键音频寄存器配置**

所有已知差异都已修复：
- ✅ FIFO 阈值
- ✅ FIFO 状态判断
- ✅ SAO_CTRL
- ✅ PAO_CTRL
- ✅ 0x30824c 未知寄存器
- ✅ FIFO 重置延迟优化

**如果 v10 仍然失败，问题可能在 RK3566 端的 I2S3/DMA 配置**
