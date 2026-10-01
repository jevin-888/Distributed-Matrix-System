# Android vs Linux 配置差异总结

## 已确认的关键差异

### 1. FIFO 阈值配置 ❌ 不匹配

**Android (工作)**:
```
0x308004 = 0x01000000
  START (bits 26:18) = 64
  MAX   (bits 17:9)  = 0    ← 禁用硬件 overflow 检测
  MIN   (bits 8:0)   = 0    ← 禁用硬件 underflow 检测
```

**Linux v09 (不工作)**:
```
0x308004 = 0x01001008
  START = 64
  MAX   = 8     ← 启用了硬件阈值检测
  MIN   = 8     ← 启用了硬件阈值检测
```

**影响**: 
- MAX/MIN=8 可能过于敏感，导致硬件频繁触发 overflow/underflow 中断
- Android 使用轮询方式（MAX=MIN=0），完全依靠软件监控 FIFO 状态

**修复 v10**: 
```c
#define RK628_AUDIO_FIFO_MAX  0  // 改为 0
#define RK628_AUDIO_FIFO_MIN  0  // 改为 0
```

### 2. FIFO 状态判断逻辑 ❌ 过于严格

**Linux v09**:
```c
if ((fifo_ints != 0x9) && (fifo_ints & UNDERFLOW/OVERFLOW))
    reset_fifo();  // 只接受 0x9，拒绝 0xd
```

**Linux v10 修复**:
```c
if ((fifo_ints != 0x9) && (fifo_ints != 0xd) && (fifo_ints & UNDERFLOW/OVERFLOW))
    reset_fifo();  // 接受 0x9 和 0xd
```

**影响**:
- 0xd 可能是正常工作状态的一部分
- 拒绝 0xd 导致每秒重置循环

### 3. I2S3 配置 ⚠️ 需要验证

**Android I2S3 寄存器**:
- RXCR (0x04): 0x01c8000f
- CKR  (0x08): 0x18003f3f  
- DMACR (0x10): 0x000f0010

**Linux I2S3**: 
- 由设备树和 rockchip-i2s 驱动控制
- 需要检查实际寄存器值

**可能的问题**:
- I2S3 可能配置为 master 而不是 slave
- 时钟分频器不匹配
- DMA 触发阈值不对

### 4. 其他未知寄存器 ⚠️

**Android 配置了，但 Linux 代码中未见**:
- 0x30800c: 0x0030c15c
- 0x308020: 0x00000100
- 0x308024: 0x00000f00

这些可能是关键的配置寄存器！

## v10 修复汇总

### 已修改 ✅
1. **FIFO MAX = 0, MIN = 0** (匹配 Android)
2. **接受 FIFO 状态 0xd** (放宽判断)
3. **FIFO 重置延迟增加** (1500ms → 3000ms)

### 仍需检查 ⚠️
1. **0x30800c/0x308020/0x308024 寄存器** - 可能缺失关键配置
2. **I2S3 slave 模式** - RK3566 端的配置
3. **APLL 时钟锁定** - RK628 音频时钟是否稳定

## 预期结果

**v10 应该能够工作，因为**:
1. FIFO 阈值现在匹配 Android (MAX=0, MIN=0)
2. FIFO 状态判断不再过于严格 (接受 0xd)
3. 这两个改动应该能让 FIFO 稳定，audio_enable 最终为 true

**如果 v10 仍然失败**:
- 需要配置 0x30800c/0x308020/0x308024 这些未知寄存器
- 或者 I2S3/DMA 配置有问题

## 下一步

1. **立即构建 v10 测试** - 优先级最高
2. 如果失败，获取 Linux 实时寄存器 dump 进行深度对比
