# 通过串口修复 SSH 连接

## 在串口终端直接执行

请在你的串口终端（PuTTY/MobaXterm）中复制粘贴以下命令：

```bash
# 1. 检查 SSH 服务状态
echo "=== Check SSH Service ==="
ps aux | grep sshd | grep -v grep

# 2. 启动/重启 SSH 服务
echo "=== Start SSH Service ==="
/etc/init.d/S50sshd restart

# 3. 检查 SSH 配置
echo "=== Check SSH Config ==="
grep -E 'PermitRootLogin|PasswordAuthentication|PubkeyAuthentication' /etc/ssh/sshd_config | grep -v "^#"

# 4. 修复 SSH 配置（允许 root 登录和密码认证）
echo "=== Fix SSH Config ==="
sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' /etc/ssh/sshd_config
sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config
sed -i 's/^#*PubkeyAuthentication.*/PubkeyAuthentication yes/' /etc/ssh/sshd_config

# 5. 重启 SSH 服务使配置生效
echo "=== Restart SSH ==="
/etc/init.d/S50sshd restart
sleep 2

# 6. 验证 SSH 端口监听
echo "=== Check SSH Port ==="
netstat -tlnp | grep :22

echo "=== SSH Fix Done ==="
echo "Now try: ssh root@192.168.2.101"
```

---

## Windows 端清理 known_hosts

在 Windows PowerShell 中执行：

```powershell
# 清理旧的 host key
ssh-keygen -R 192.168.2.101

# 重新连接（会提示接受新密钥，输入 yes）
ssh root@192.168.2.101
```

---

## 如果还是连接失败

### 方案 1: 使用 -o 选项忽略 host key 检查

```powershell
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.2.101
```

### 方案 2: 检查防火墙

在串口终端执行：
```bash
# 检查防火墙规则
iptables -L -n | grep 22

# 如果 SSH 被阻止，清除防火墙规则
iptables -F
```

### 方案 3: 生成新的 SSH host key

在串口终端执行：
```bash
# 重新生成 SSH host keys
rm -f /etc/ssh/ssh_host_*
ssh-keygen -A
/etc/init.d/S50sshd restart
```

---

## 测试 SSH 连接

修复后测试：

```powershell
# 测试连接
ssh root@192.168.2.101 "echo 'SSH OK'; uname -a"
```

如果成功，应该看到：
```
SSH OK
Linux distributed-matrix 5.10.160 ...
```

---

## 同时完成音频测试

SSH 修复后，顺便测试一下音频（一石二鸟）：

```bash
ssh root@192.168.2.101 << 'EOF'
echo "=== Audio Test ==="
/etc/init.d/S99distributed-matrix stop
killall arecord
sleep 1

timeout 3 arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 2 /tmp/t48.wav 2>&1 | grep -v FIFO
echo "48kHz: $(stat -c%s /tmp/t48.wav 2>/dev/null || echo 0) bytes"

timeout 3 arecord -D hw:1,0 -f S16_LE -r 44100 -c 2 -d 2 /tmp/t44.wav 2>&1 | grep -v FIFO
echo "44.1kHz: $(stat -c%s /tmp/t44.wav 2>/dev/null || echo 0) bytes"

/etc/init.d/S99distributed-matrix start
EOF
```

---

**请先在串口执行 SSH 修复命令，然后告诉我结果！**
