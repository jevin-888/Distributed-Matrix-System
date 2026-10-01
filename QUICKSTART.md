# RK3566 真机快速开始

## 1. 设备连接

```text
Host: 192.168.1.103
User: linaro
Password: linaro
Architecture: aarch64
OS: Debian 11
Formal directory: /opt/distributed-matrix
Web: http://192.168.1.103:8080/（无需登录）
```

优先使用已安装的 SSH key：

```powershell
$key="$HOME\.ssh\dms_device_ed25519"
ssh -i $key -o BatchMode=yes -o IdentitiesOnly=yes linaro@192.168.1.103
```

## 2. 设备生产依赖检查

```bash
uname -m
systemctl is-active distributed-matrix-master.service
gst-inspect-1.0 mppvideodec
gst-inspect-1.0 mppjpegdec
gst-inspect-1.0 rtph264depay
gst-inspect-1.0 rtpjitterbuffer
gst-inspect-1.0 videocrop
gst-inspect-1.0 videoconvert
gst-inspect-1.0 kmssink
gst-inspect-1.0 kmssink | grep render-rectangle
ls -l /dev/dri/card0 /dev/mpp_service
curl -fsS http://127.0.0.1:8080/api/status
```

## 3. 从当前项目归档并在设备构建

仅在源码有新变更时执行。归档不包含构建目录、内核产物或历史日志，也不依赖已清理的临时隐藏脚本：

```powershell
Set-Location "D:\Distributed Matrix System"
tar.exe -czf dms-device-source.tar.gz `
  --exclude=dms-device-source.tar.gz --exclude=artifacts --exclude=build --exclude=.git .
scp -i $key -o BatchMode=yes -o IdentitiesOnly=yes `
  dms-device-source.tar.gz `
  linaro@192.168.1.103:/home/linaro/dms-device-source.tar.gz
```

在设备上构建并先跑完整测试：

```bash
rm -rf -- /home/linaro/dms-device-src
mkdir -p /home/linaro/dms-device-src
tar -xzf /home/linaro/dms-device-source.tar.gz -C /home/linaro/dms-device-src
cmake -S /home/linaro/dms-device-src -B /home/linaro/dms-device-src/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build /home/linaro/dms-device-src/build --parallel "$(getconf _NPROCESSORS_ONLN)"
ctest --test-dir /home/linaro/dms-device-src/build --output-on-failure
```

## 4. 正式安装与回收临时文件

只有测试全部通过后才安装。设备当前同时运行 Master 和 node-id 1 的 Slave：

```bash
printf '%s\n' linaro | sudo -S systemctl stop \
  distributed-matrix-slave.service distributed-matrix-master.service
printf '%s\n' linaro | sudo -S cmake --install /home/linaro/dms-device-src/build \
  --prefix /opt/distributed-matrix
printf '%s\n' linaro | sudo -S systemctl start \
  distributed-matrix-master.service distributed-matrix-slave.service
systemctl is-enabled distributed-matrix-master.service distributed-matrix-slave.service
systemctl is-active distributed-matrix-master.service distributed-matrix-slave.service
curl -fsS http://127.0.0.1:8080/api/status
curl -fsS http://127.0.0.1:8080/api/nodes
rm -rf -- /home/linaro/dms-device-src
rm -f -- /home/linaro/dms-device-source.tar.gz
```

Windows 端确认上传和安装完成后删除本地归档：

```powershell
Remove-Item -LiteralPath "D:\Distributed Matrix System\dms-device-source.tar.gz"
```

## Hardware multi-window requirement

Set `playback.overlay_plane_ids` to the real DRM overlay IDs, ordered from the
lowest to the highest hardware z position:

```json
"playback": {
  "plane_id": 78,
  "background_plane_id": 56,
  "overlay_plane_ids": [56, 78, 92, 106]
}
```

Use `modetest -p` to obtain IDs supported by the board. Every window layer needs
one distinct plane. The runtime decodes each RTP/H.264 source with MPP and sends
each layer directly to `kmssink render-rectangle=...`; there is no CPU compositor
fallback. If the list is too short or a plane cannot accept the rectangle, the
layout is rejected without a CPU compositor; the slave keeps the current
playback state or shows its configured idle image. HTTP-MJPEG is retained only
as a transport fallback and uses `mppjpegdec` before KMS.

On the verified RK3566 VOP2 target, keep Smart0 windows in physical priority
order (`56, 78, 92, 106`). The idle/background image initially owns plane 56;
when a window scene starts it is released and the fullscreen/first live window
takes over that same plane. The driver reports these windows with the same
normalized z-position, so placing the first live window on plane 78 can leave a
valid decoded framebuffer hidden behind fbcon even though `kmssink` is in
PLAYING state.

## 5. 真机验收

- H.264：播放、暂停、恢复、停止，状态依次应为 `preparing → playing → paused → playing → idle`。
- H.265：播放测试媒体自然结束，节点必须从 `playing` 回到 `idle`。
- EOS 日志必须同时出现：`Media playback reached end of stream`、`Media player reached end of stream`。
- EOS 后再次播放 H.264，确认 connector 113 / plane 78 可复用。
- 统计必须保持：prime import errors=0、caps reject=0、GStreamer ERROR=0、not-negotiated=0。

设备测试媒体只用于验收，完成后删除；保留 `/tmp/distributed_matrix/video_cache` 目录本身。
