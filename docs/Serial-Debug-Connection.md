# 通过串口调试端口建立持久连接

**日期**: 2026-08-23  
**目的**: 通过串口避免 SSH 超时，建立稳定的调试连接  

---

## 硬件连接

### RK3566 串口引脚

**UART2 调试串口** (通常是 DEBUG 接口):
- **TX**: GPIO0_D1
- **RX**: GPIO0_D0  
- **GND**: 地线
- **波特率**: 1500000 (1.5M) 或 115200

### USB 转 TTL 连接

```
USB-TTL 适配器          RK3566 板子
-----------------      ------------------
  3.3V (不接)    <-->  (不接 VCC)
  TXD            <-->  RXD (接收)
  RXD            <-->  TXD (发送)
  GND            <-->  GND (地)
```

**注意**: 
- ⚠️ **不要连接 VCC/3.3V** - 板子已供电
- ✓ 只连接 TX、RX、GND 三根线
- ✓ 交叉连接：适配器 TX → 板子 RX，适配器 RX → 板子 TX

---

## Windows 串口连接

### 方式 1: PuTTY

1. **下载 PuTTY**: https://www.putty.org/

2. **配置串口**:
   ```
   Connection type: Serial
   Serial line: COM3 (查看设备管理器确认端口号)
   Speed: 1500000 (或 115200)
   ```

3. **连接参数**:
   - Data bits: 8
   - Stop bits: 1
   - Parity: None
   - Flow control: None

4. **点击 Open** 连接

### 方式 2: MobaXterm

```
Session → Serial
Port: COM3
Speed: 1500000
```

### 方式 3: PowerShell (推荐用于自动化)

```powershell
# 安装 SerialPort 模块
Install-Module -Name SerialPort -Force

# 连接串口
$port = New-Object System.IO.Ports.SerialPort COM3,1500000,None,8,One
$port.Open()

# 发送命令
$port.WriteLine("ls -la")

# 读取输出
$output = $port.ReadExisting()
Write-Host $output

# 关闭
$port.Close()
```

---

## 建立自动化连接令牌

### 方案 1: 使用 screen/tmux 保持会话

通过串口登录后：

```bash
# 安装 screen (如果没有)
# 通常 Buildroot 默认没有，需要在固件中编译进去

# 或者使用 nohup 保持后台任务
nohup /tmp/fix-audio.sh > /tmp/fix-audio.log 2>&1 &

# 查看日志
tail -f /tmp/fix-audio.log
```

### 方案 2: 创建自启动修复脚本

将修复脚本添加到启动项：

```bash
# 通过串口连接后执行
cat > /etc/init.d/S98audio-fix << 'EOF'
#!/bin/sh

case "$1" in
    start)
        echo "检查音频修复..."
        # 等待系统完全启动
        sleep 5
        
        # 检查音频是否正常
        if ! arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 1 /tmp/audio-check.wav 2>/dev/null; then
            echo "音频异常，执行修复..."
            
            # 重新加载模块
            rmmod snd_soc_rockchip_i2s_tdm 2>/dev/null
            rmmod snd_soc_rk628 2>/dev/null
            rmmod snd_soc_dummy 2>/dev/null
            sleep 1
            modprobe snd_soc_dummy
            modprobe snd_soc_rk628
            modprobe snd_soc_rockchip_i2s_tdm
            sleep 2
            
            echo "音频修复完成"
        else
            echo "音频正常"
        fi
        ;;
    *)
        echo "Usage: $0 {start}"
        exit 1
        ;;
esac
EOF

chmod +x /etc/init.d/S98audio-fix
```

### 方案 3: 通过串口远程执行脚本

在 Windows 上创建 PowerShell 脚本：

```powershell
# serial-exec.ps1
param(
    [string]$Command,
    [string]$Port = "COM3",
    [int]$BaudRate = 1500000
)

$serialPort = New-Object System.IO.Ports.SerialPort $Port, $BaudRate, None, 8, One
$serialPort.Open()
$serialPort.ReadTimeout = 5000

try {
    # 发送命令
    $serialPort.WriteLine($Command)
    Start-Sleep -Milliseconds 500
    
    # 读取输出
    $output = ""
    $timeout = [DateTime]::Now.AddSeconds(30)
    
    while ([DateTime]::Now -lt $timeout) {
        if ($serialPort.BytesToRead -gt 0) {
            $output += $serialPort.ReadExisting()
            Start-Sleep -Milliseconds 100
        } else {
            Start-Sleep -Milliseconds 500
        }
    }
    
    Write-Host $output
} finally {
    $serialPort.Close()
}
```

使用：
```powershell
.\serial-exec.ps1 -Command "arecord -l"
```

---

## 通过串口执行音频修复

### 完整步骤

1. **连接串口** (PuTTY/MobaXterm)

2. **按回车进入 shell**

3. **执行修复**:
```bash
# 创建修复脚本
cat > /tmp/fix.sh << 'EOF'
#!/bin/bash
/etc/init.d/S99distributed-matrix stop
killall arecord
rmmod snd_soc_rockchip_i2s_tdm snd_soc_rk628 snd_soc_dummy
sleep 1
modprobe snd_soc_dummy snd_soc_rk628 snd_soc_rockchip_i2s_tdm
sleep 2
PRESENT=$(cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_present 2>/dev/null || echo 0)
RATE=$(cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate 2>/dev/null || echo 48000)
echo "present=$PRESENT rate=$RATE"
if [ "$PRESENT" = "1" ]; then
    arecord -D hw:1,0 -f S16_LE -r $RATE -c 2 -d 2 /tmp/t.wav && ls -lh /tmp/t.wav
fi
/etc/init.d/S99distributed-matrix start
EOF

chmod +x /tmp/fix.sh
/tmp/fix.sh
```

4. **查看结果**

---

## 从 Windows 自动化执行

创建 **build/serial-fix-audio.ps1**:

```powershell
# 串口自动修复脚本
$Port = "COM3"  # 修改为实际端口号
$BaudRate = 1500000

Write-Host "=== 通过串口修复 192.168.2.101 音频 ===" -ForegroundColor Cyan

$serial = New-Object System.IO.Ports.SerialPort $Port, $BaudRate, None, 8, One
$serial.Open()
$serial.ReadTimeout = 2000

function Send-SerialCommand {
    param([string]$cmd)
    $serial.WriteLine($cmd)
    Start-Sleep -Milliseconds 500
}

function Read-SerialOutput {
    $output = ""
    $endTime = [DateTime]::Now.AddSeconds(10)
    while ([DateTime]::Now -lt $endTime -and $serial.BytesToRead -gt 0) {
        $output += $serial.ReadExisting()
        Start-Sleep -Milliseconds 200
    }
    return $output
}

try {
    # 发送回车确保在 shell 提示符
    Send-SerialCommand ""
    Start-Sleep -Seconds 1
    
    # 停止服务
    Write-Host "`n1. 停止服务..." -ForegroundColor Yellow
    Send-SerialCommand "/etc/init.d/S99distributed-matrix stop"
    Start-Sleep -Seconds 2
    
    # 重新加载模块
    Write-Host "2. 重新加载音频模块..." -ForegroundColor Yellow
    Send-SerialCommand "rmmod snd_soc_rockchip_i2s_tdm snd_soc_rk628 snd_soc_dummy"
    Start-Sleep -Seconds 1
    Send-SerialCommand "modprobe snd_soc_dummy && modprobe snd_soc_rk628 && modprobe snd_soc_rockchip_i2s_tdm"
    Start-Sleep -Seconds 3
    
    # 检查状态
    Write-Host "3. 检查 RK628 状态..." -ForegroundColor Yellow
    Send-SerialCommand "cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_present"
    $output = Read-SerialOutput
    Write-Host $output
    
    Send-SerialCommand "cat /sys/devices/platform/fe5a0000.i2c/i2c-1/1-0051/hdmirx/rk628/audio_rate"
    $output = Read-SerialOutput
    Write-Host $output
    
    # 测试采集
    Write-Host "4. 测试音频采集..." -ForegroundColor Yellow
    Send-SerialCommand "arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 2 /tmp/test.wav && ls -lh /tmp/test.wav"
    Start-Sleep -Seconds 4
    $output = Read-SerialOutput
    Write-Host $output
    
    # 重启服务
    Write-Host "5. 重启服务..." -ForegroundColor Yellow
    Send-SerialCommand "/etc/init.d/S99distributed-matrix start"
    Start-Sleep -Seconds 2
    
    Write-Host "`n=== 完成 ===" -ForegroundColor Green
    
} finally {
    $serial.Close()
}
```

---

## 使用串口调试的优势

1. ✓ **不受网络影响** - 即使网络故障也能访问
2. ✓ **无超时限制** - 可以长时间保持连接
3. ✓ **启动早期访问** - 可以看到 bootloader 和内核启动日志
4. ✓ **更底层控制** - 可以进入单用户模式或 recovery
5. ✓ **稳定可靠** - 不会因为 SSH key 变化而中断

---

## 下一步

1. **连接串口线** - USB-TTL 转板子调试口
2. **打开串口工具** - PuTTY 设置 COM 口和波特率
3. **执行修复脚本** - 直接粘贴命令或使用 PowerShell 自动化
4. **查看完整输出** - 不会被 SSH 超时打断

**请告诉我你的串口是哪个 COM 口，我可以帮你创建自动化脚本！**
