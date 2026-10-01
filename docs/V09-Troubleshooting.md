## V09 刷机后故障排查步骤

设备 192.168.2.101 刷入 v09 ROM 后：
- ✅ 设备可以 ping 通
- ❌ SSH 连接超时（banner exchange 阶段）
- ❌ 音频仍然没声音

### 立即检查（通过串口）

1. **检查系统是否正常启动**
   ```bash
   # 通过串口登录（root / linaro）
   uname -a
   # 应该显示 Linux 5.10.160
   ```

2. **检查网络和 SSH 服务**
   ```bash
   ip addr show eth0
   # 确认是否有 192.168.2.101 IP
   
   ps aux | grep dropbear
   # 确认 SSH 服务是否运行
   
   netstat -tlnp | grep :22
   # 确认端口 22 是否监听
   ```

3. **检查 dmesg 音频日志**
   ```bash
   dmesg | grep -i rk628 | tail -30
   dmesg | grep -i audio | tail -30
   dmesg | grep -i fifo | tail -30
   ```

4. **检查音频设备**
   ```bash
   cat /proc/asound/cards
   # 应该看到 hdmiin 卡
   
   arecord -l
   # 列出录音设备
   ```

5. **检查 RK628 驱动是否加载修复**
   ```bash
   # 检查驱动文件时间戳
   ls -l /lib/modules/5.10.160/kernel/drivers/media/i2c/rk628/
   
   # 搜索修复代码的特征字符串
   dmesg | grep "consecutive FIFO resets"
   dmesg | grep "FIFO recovered"
   ```

6. **验证 SSH 密钥是否安装**
   ```bash
   ls -la /root/.ssh/
   cat /root/.ssh/authorized_keys
   # 应该看到你的 ssh-ed25519 公钥
   ```

### 可能的问题

#### 问题 1：内核没有包含修复
- 检查：`strings /boot/Image | grep "consecutive FIFO"`
- 原因：内核可能没有重新编译
- 解决：需要确认 kernel 构建步骤

#### 问题 2：SSH 服务配置问题
- 检查：`/etc/init.d/S50dropbear status`
- 检查：`/var/log/messages` 或 `dmesg | grep dropbear`

#### 问题 3：网络配置问题
- 检查：`ip route`
- 检查：防火墙规则

### 请在串口执行并反馈结果

```bash
# 一键诊断脚本
echo "=== System Info ==="
uname -a
uptime

echo -e "\n=== Network ==="
ip addr show eth0
ip route

echo -e "\n=== SSH Service ==="
ps aux | grep dropbear
netstat -tlnp | grep :22

echo -e "\n=== Audio Devices ==="
cat /proc/asound/cards

echo -e "\n=== RK628 FIFO Errors (last 20) ==="
dmesg | grep -E "rk628|FIFO|audio" | tail -20

echo -e "\n=== SSH Keys ==="
ls -la /root/.ssh/
cat /root/.ssh/authorized_keys
```

把输出结果发给我，我来分析问题。
