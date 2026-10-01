# Android vs Linux 完整配置差异报告

## 已确认的配置差异

### 1. FIFO 阈值 (0x308004) ❌ 关键差异
**Android**: 0x01000000
- START = 64 (bits 26:18)
- MAX = 0 (bits 17:9)  ← **禁用硬件 overflow 中断**
- MIN = 0 (bits 8:0)   ← **禁用硬件 underflow 中断**

**Linux v09**: 0x01001008
- START = 64
- MAX = 8   ← 启用硬件中断
- MIN = 8   ← 启用硬件中断

**✅ v10 已修复**: 改为 MAX=0, MIN=0

---

### 2. SAO_CTRL (0x308020 / 0x260) ⚠️ 可能不匹配
**Android**: 0x00000100
- bit 8 = 1
- 其他位 = 0

**Linux**: 
```c
I2S_LPCM_BPCUV(0) | I2S_32_16(1) | I2S_DATA_ENABLE_BITS(0xf)
= (0 << 11) | (1 << 0) | (0xf << 5)
= 0x000001e1
```
- bit 0 = 1 (I2S_32_16)
- bits 5-8 = 0xf (I2S_DATA_ENABLE)
- bit 11 = 0 (LPCM_BPCUV)

**差异**:
- Android 只设置 bit 8
- Linux 设置 bits 0,5,6,7,8

**可能的问题**: 
- I2S_32_16(1) 设置了 bit 0，但 Android 没有
- I2S_DATA_ENABLE 可能配置不对

---

### 3. PAO_CTRL (0x308024 / 0x264) ⚠️ 不匹配
**Android**: 0x00000f00
- bits 8-11 = 0xf

**Linux**: 
```c
PAO_RATE(0) = 0 << 16 = 0x00000000
```

**完全不匹配！**

---

### 4. 未知寄存器 0x30800c (offset 0x24c) ❌ 缺失
**Android**: 0x0030c15c
- 这个寄存器在 RK628 头文件中**没有定义**
- Linux 代码**完全没有配置**

**这可能是关键的缺失配置！**

---

### 5. FIFO 状态判断逻辑 ❌ 过于严格
**Linux v09**: 只接受 0x9，拒绝 0xd
**✅ v10 已修复**: 接受 0x9 和 0xd

---

## 需要修复的配置

### 优先级 1: SAO_CTRL 寄存器
**问题**: Linux 配置的值与 Android 不一致

**分析 Android 0x00000100**:
```
bit 8 = 1: 可能是 I2S enable 或某个控制位
其他 = 0: 禁用其他功能
```

**可能的修复**:
```c
// 当前 Linux 代码 (可能错误)
rk628_i2c_write(rk628, HDMI_RX_AUD_SAO_CTRL,
    I2S_LPCM_BPCUV(0) |
    I2S_32_16(1) |
    I2S_DATA_ENABLE_BITS(0xf));

// 应该改为 (匹配 Android)
rk628_i2c_write(rk628, HDMI_RX_AUD_SAO_CTRL, 0x00000100);
```

---

### 优先级 1: PAO_CTRL 寄存器
**问题**: Linux 写入 0，Android 是 0x00000f00

**可能的修复**:
```c
// 当前 Linux 代码
rk628_i2c_write(rk628, HDMI_RX_AUD_PAO_CTRL, PAO_RATE(0));  // = 0

// 应该改为 (匹配 Android)
rk628_i2c_write(rk628, HDMI_RX_AUD_PAO_CTRL, 0x00000f00);
```

---

### 优先级 1: 0x30800c 未知寄存器
**问题**: 完全缺失

**需要添加**:
```c
// 在 rk628_hdmirx_audio_setup() 中添加
rk628_i2c_write(rk628, HDMI_RX_BASE + 0x024c, 0x0030c15c);
```

---

## v10 修复清单

### 已修复 ✅
1. FIFO MAX=0, MIN=0
2. 接受 FIFO 状态 0xd

### 需要添加 ⚠️
3. **SAO_CTRL = 0x00000100** (而不是计算的值)
4. **PAO_CTRL = 0x00000f00** (而不是 0)
5. **0x30800c = 0x0030c15c** (新增寄存器)

---

## 验证方法

修复后在设备上执行:
```bash
# 获取实际寄存器值
cat /sys/kernel/debug/rk628/hdmirx-controller | grep -E "308000|308004|30800c|308020|308024"
```

对比应该得到:
```
308000: 0x10000011 (FIFO_CTRL)
308004: 0x01000000 (FIFO_TH)
30800c: 0x0030c15c (未知)
308020: 0x00000100 (SAO_CTRL)
308024: 0x00000f00 (PAO_CTRL)
```

---

## 结论

**v10 需要额外修复 3 个寄存器配置才能完全匹配 Android！**

当前 v10 只修复了 2 项:
- ✅ FIFO 阈值
- ✅ FIFO 状态判断

还需要修复:
- ⚠️ SAO_CTRL 值
- ⚠️ PAO_CTRL 值  
- ⚠️ 0x30800c 神秘寄存器
