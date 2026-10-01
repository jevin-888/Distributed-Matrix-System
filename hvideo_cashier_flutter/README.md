# 分布式播放系统客户端

本工程为分布式播放系统现场客户端，采用 Flutter 构建 Windows 与 Android 双平台应用。

## 支持平台

- Windows 10/11 x64（EXE）
- Android 设备（APK，支持局域网 HTTP 主节点）
- Flutter 3.44.8 / Dart 3.12.2

## 主要界面

- 顶部：加载后台区域并切换当前区域
- 左侧：显示可用的输入/编解码节点
- 中央：按输入开窗数量组织的客户端画布
- 底部：播放、暂停、停止、媒体选择与音量交互
- 右侧：单窗、2×2、横向三联、2×3 等输入开窗预设
- 窄屏：自动切换为可滚动的平板/手机布局

## 中央画布交互

- 从左侧输入节点卡片拖入中央画布；Android 使用长按拖动。
- 每个输入窗口独立显示该节点的完整输入画面，不按窗口位置裁剪或拼接输入源。
- 拖入后可直接拖动节点调整位置，选中节点后使用四角控制点调整大小。
- 双击节点可铺满中央画布，再次双击可恢复到原来的位置和尺寸。
- 每个输入节点最多可在客户端画布开 16 个独立窗口；不同输入节点分别计数。
- 底部“铺满屏”按钮作用于当前选中的画布节点，“2×2”会重新排列已拖入的节点。
## REST API 一一对应

客户端只调用服务端现有 API，不添加兼容别名：

- `GET /api/nodes`
- `POST /api/nodes/discover`
- `PUT /api/node-network`
- `GET /api/regions`
- `PUT /api/regions`
- `GET /api/screens`
- `PUT /api/screens`
- `GET /api/windows`
- `PUT /api/windows`
- `GET /api/status`
- `GET /api/audio-output`
- `PUT /api/audio-output`
- `GET /api/audio-volume`
- `PUT /api/audio-volume`
- `POST /api/play`
- `POST /api/pause`
- `POST /api/resume`
- `POST /api/stop`
- `POST /api/preload`

默认主节点地址：`http://192.168.2.101:8080`。

## 搜索并连接设备

- 点击右上角“连接”，自动搜索本机各活动网卡所在局域网内的在线设备。
- 列表显示设备名称、IP 和控制服务状态；点击可连接的设备后直接连接并同步，无需再次填写地址。
- 支持重新搜索和折叠的“手动输入地址”入口；连接失败保留列表并显示原因。
- Windows / Android 从系统读取真实 IPv4 子网前缀，使用现有 UDP 9003 发现协议广播和单播搜索；同时探测 HTTP 8080 及当前配置端口，通过现有 `GET /api/status`、`GET /api/nodes` 确认主节点服务。
- 仅运行从节点服务的在线设备也会列出，但不能作为主节点连接。超大网段仅广播搜索，HTTP 扫描最多覆盖 65,536 个 IPv4 地址；跨网段或其他自定义端口可手动连接。
- 搜索采用有限并发；关闭窗口或开始连接后关闭搜索使用的网络连接，迟到响应不会覆盖新一轮搜索结果。

## 开发验证

```powershell
flutter pub get
flutter analyze
flutter test
flutter run -d windows
```

## 构建

```powershell
.\scripts\build-release.ps1
```

脚本会依次执行静态分析、测试、Windows Release 构建和 Android Release 构建，随后清理临时构建目录。所有可交付产物固定保存在 `release`：

仅清理 Flutter 临时构建目录时执行：

```powershell
.\scripts\build-release.ps1 -CleanOnly
```

```text
release\
├─ windows\
│  ├─ distributed_playback_system.exe
│  ├─ flutter_windows.dll
│  └─ data\
└─ android\
   └─ hsvj-engine-1.0.0-H6_M.apk
```

The APK is the Android PAD client. It is not a Linux installation package. The
Linux player is delivered only through the canonical Buildroot ROM flow and uses
`hsvj-engine-<version>-H6_M.img`.

Windows 分发时必须复制整个 `release\windows` 目录，不能只复制 EXE。

## 当前交付文件

Windows 客户端只使用下面这个文件启动：

```text
D:\Distributed Matrix System\hvideo_cashier_flutter\release\windows\distributed_playback_system.exe
```

运行时必须保留它旁边的 `flutter_windows.dll` 和 `data` 目录。`build\windows\x64\runner\Release` 是 Flutter 临时构建目录，不是交付目录；`artifacts` 下的 Linux 二进制是部署到 RK3566 节点的设备程序，也不能在 Windows 上运行。发布脚本完成后会自动清理 Flutter 临时构建目录。
