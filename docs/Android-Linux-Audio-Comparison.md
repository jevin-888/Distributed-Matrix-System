# Android vs Linux RK628 Audio Configuration Comparison

## 已验证对齐的配置

### 1. FIFO 阈值配置 ✅
```c
// Linux (rk628_hdmirx.c)
#define RK628_AUDIO_FIFO_START  64
#define RK628_AUDIO_FIFO_MAX    8
#define RK628_AUDIO_FIFO_MIN    8
```

Android 参考 (rk628-hdmirx-now.txt):
- 0x308004: FIFO_TH = 0x01000000
  - 解析：这个值需要进一步确认是否匹配 64/8/8

### 2. stablelimit ✅
```c
aif->stablelimit = 3;  // 需要连续3次稳定才启用音频
```

### 3. FIFO 重置逻辑 ✅
- 已添加 consecutive_resets 跟踪
- 已增加延迟：1500ms → 2000ms → 3000ms

## 新发现：FIFO 状态判断问题 🔧

### 当前问题
Linux 代码：
```c
if ((fifo_ints != 0x9) &&
    (fifo_ints & (AFIF_UNDERFL_ISTS | AFIF_OVERFL_ISTS))) {
    // 重置 FIFO
}
```

实际状态：
- Android (工作): 可能接受 0x9 和 0xd
- Linux v09 (失败): fifo_ints = 0xd 每秒触发重置

### 修复 (v10)
```c
// 将 0xd 也视为正常状态
if ((fifo_ints != 0x9) && (fifo_ints != 0xd) &&
    (fifo_ints & (AFIF_UNDERFL_ISTS | AFIF_OVERFL_ISTS))) {
```

## 需要验证的配置

### 1. RK628 CRU (时钟) 配置
Android 参考 (rk628-cru-now.txt):
- 0xc00090: 0x03355460 - APLL 配置
- 0xc00094: 0x000080d7 - 时钟分频

**需要检查**: Linux 的时钟配置是否匹配

### 2. I2S3 配置
Android 参考 (i2s3-now.txt):
- 0x00: 0x7200000f - TXCR (TX control)
- 0x04: 0x01c8000f - RXCR (RX control)
- 0x08: 0x18003f3f - CKR (Clock)
- 0x10: 0x000f0010 - DMACR (DMA control)

**需要检查**: RK3566 I2S3 寄存器配置

### 3. HDMIRX 其他音频寄存器
从 Android rk628-hdmirx-now.txt:

| 地址 | Android值 | 说明 | Linux状态 |
|------|-----------|------|-----------|
| 0x308000 | 0x10000011 | AUD_FIFO_CTRL | ✅ 已配置 |
| 0x308004 | 0x01000000 | AUD_FIFO_TH | ⚠️ 需验证阈值编码 |
| 0x308008 | 0x00000001 | AUD_FIFO_ISTS | ⚠️ 中断状态 |
| 0x30800c | 0x0030c15c | ? | ❓ 未知 |
| 0x308020 | 0x00000100 | ? | ❓ 未知 |
| 0x308024 | 0x00000f00 | ? | ❓ 未知 |

### 4. 音频采样率检测
```c
// Linux
if (abs((int)fs_audio - (int)audio_state->fs_audio) > 1000) {
    // 重新初始化
}
```

**需要确认**: Android 的采样率切换逻辑

## 待验证项目

### 高优先级
1. ⚠️ **FIFO 阈值编码方式** - Android 0x01000000 是否等于 START=64, MAX=8, MIN=8?
2. ⚠️ **I2S3 时钟配置** - RK3566 端的 I2S 配置是否正确
3. ⚠️ **DMA 启动时机** - audio_enable 为 true 后，DMA 是否正确启动

### 中优先级
4. ❓ **0x30800c 寄存器** - Android 有配置，Linux 可能缺失
5. ❓ **0x308020/308024 寄存器** - 未知用途

### 低优先级
6. ✅ **APLL 时钟** - 已使用 DEFAULT_AUDIO_CLK = 5644800
7. ✅ **I2S 数据格式** - I2S_32_16(1) 已配置

## v10 修复重点

**核心修复**: 接受 FIFO 状态 0xd 为正常状态，避免无限重置循环

**预期结果**:
- ✅ FIFO 不再每秒重置
- ✅ stablelimit 计数能够达到 3
- ✅ audio_enable 最终被设为 true
- ✅ arecord 能够录制音频

## 如果 v10 仍然失败

需要获取实时寄存器对比：
1. SSH 到设备获取 `/sys/kernel/debug/rk628/hdmirx-controller`
2. 对比 Android 和 Linux 的每个寄存器值
3. 找出配置差异并修复
