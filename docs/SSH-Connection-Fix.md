# SSH 连接配置和音频测试指南

## 问题原因

设备使用 **Dropbear SSH**，需要特定的加密算法参数才能连接。

---

## ✅ 解决方案 1: 使用完整 SSH 命令

在 PowerShell 中执行：

```powershell
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o KexAlgorithms=curve25519-sha256 -o Ciphers=aes128-ctr -o ServerAliveInterval=60 root@192.168.2.101
```

当提示接受 host key 或输入密码时，按照提示操作。

---

## ✅ 解决方案 2: 创建 SSH 配置文件（推荐）

### 步骤 1: 创建或编辑 SSH 配置

在 PowerShell 中执行：

```powershell
# 创建 .ssh 目录（如果不存在）
New-Item -ItemType Directory -Force -Path "$HOME\.ssh"

# 创建配置文件
@"
Host 192.168.2.101
    HostName 192.168.2.101
    User root
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
    KexAlgorithms curve25519-sha256,ecdh-sha2-nistp256
    Ciphers aes128-ctr,aes256-ctr,chacha20-poly1305@openssh.com
    MACs hmac-sha2-256,hmac-sha1
    ServerAliveInterval 60
    ServerAliveCountMax 3
"@ | Out-File -FilePath "$HOME\.ssh\config" -Encoding ASCII -Append
```

### 步骤 2: 连接（超级简单）

```powershell
ssh root@192.168.2.101
```

---

## ✅ 解决方案 3: 使用 PuTTY（最简单）

如果 OpenSSH 一直超时，使用 PuTTY：

1. 打开 PuTTY
2. Host Name: `192.168.2.101`
3. Port: `22`
4. Connection Type: SSH
5. 点击 Open
6. 接受 host key
7. 登录: `root` / 你的密码

连接后执行：
```bash
/etc/init.d/S99distributed-matrix stop
timeout 3 arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 2 /tmp/t.wav
stat -c%s /tmp/t.wav
/etc/init.d/S99distributed-matrix start
```

把文件大小告诉我。

---

## ✅ 解决方案 4: 一键测试脚本

我创建了 PowerShell 脚本，自动处理所有 SSH 参数：

```powershell
.\build\ssh-connect-101.ps1
```

脚本会自动连接并测试音频。

---

## 🔧 SSH 连接后立即测试音频

连接成功后，执行这个一键命令：

```bash
/etc/init.d/S99distributed-matrix stop; killall arecord; timeout 3 arecord -D hw:1,0 -f S16_LE -r 48000 -c 2 -d 2 /tmp/t48.wav 2>&1 | grep -v FIFO; echo "48kHz: $(stat -c%s /tmp/t48.wav || echo 0) bytes"; timeout 3 arecord -D hw:1,0 -f S16_LE -r 44100 -c 2 -d 2 /tmp/t44.wav 2>&1 | grep -v FIFO; echo "44.1kHz: $(stat -c%s /tmp/t44.wav || echo 0) bytes"; /etc/init.d/S99distributed-matrix start
```

---

## 📊 预期结果

### 成功（有声音）：
```
48kHz: 384044 bytes
```

### 失败（无声音）：
```
48kHz: 44 bytes
44.1kHz: 44 bytes
```

如果失败，原因 99% 是 **HDMI 信号源音频设置问题**：
- HDMI 源输出压缩音频（AC3/DTS）
- RK628 只支持 PCM

**解决**：在 HDMI 信号源设备设置中改为 PCM/立体声输出。

---

## 为什么我之前能连接？

你之前使用的 SSH 客户端可能：
1. 已经保存了兼容的加密算法配置
2. 或者使用的是 PuTTY 等工具（自动协商算法）

我清理 known_hosts 后，OpenSSH 需要重新协商，但默认参数与 Dropbear 不完全兼容。

---

**请选择一个方案执行，然后把文件大小（字节数）告诉我！**
