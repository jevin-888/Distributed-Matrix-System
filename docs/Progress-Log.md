# 变更记录

本文保留可由代码、测试输出或设备采样确认的记录。旧日志中无法恢复的乱码已删除，历史硬件证据仍保存在 artifacts/。

## 2026-10-01 构建与文档规范

- 明确 Linux 播放器使用 Buildroot ROM，交付格式为 hsvj-engine-<版本>-H6_M.img。
- 明确 Android PAD 才使用 hsvj-engine-<版本>-H6_M.apk；Linux 不安装 APK。
- ROM 版本从 CMakeLists.txt 读取，ROM 构建固定 SDK、内核、Buildroot defconfig 和 DTS，并执行完整清理。
- 删除旧的独立 ROM 构建入口，避免同一源码产生多个正式交付分支。
- NodeDiscovery 和 HTTP 服务保留全网卡监听、广播与单播发现；未改为回环或单网卡限制。
- CTest 9/9、Flutter analyze 和 Flutter 55 项测试已通过。
- Android release APK 已编译并复制为 `hvideo_cashier_flutter/release/android/hsvj-engine-1.0.0-H6_M.apk`，SHA256 为 `9CE2DB9D882FF8488692481D9AD2DE70A30B2C44EABF678EBB64240BD5A73CEF`。
- Flutter 自带 clean 已清理 hvideo_cashier_flutter/build 和 hvideo_cashier_flutter/.dart_tool；系统自动审批拒绝删除根目录 build/review-audit，需手动清理。

## 2026-09-19 HDMI/OLED 设备验证

- 设备日志确认 HDMI 动画顶部花条和动画到背景黑屏问题已修正，实机观察通过。
- OLED 上电瞬间闪花仍需硬件时序复核，未把单次正常启动扩展为完整 ROM 验收。
- 现场验证使用独立 ARM 二进制；整包 ROM 的刷机验证范围按对应构建目录中的记录为准。

## 2026-09-17 客户端自动发现

- PAD 读取各活动网卡真实 IPv4 子网，使用 UDP 9003 广播和单播发现，并按来源地址去重。
- 连接对话框支持自动搜索、选择设备、失败重试和手动地址。
- Flutter analyze 无问题，Flutter 测试 55 项通过；Windows 和 Android 发布构建成功。

## 历史硬件结论

- RK628 输入采样确认 1920x1080p60，V4L2 使用 NV16/NV12，DRM/KMS 使用 plane 78。
- HDMI watchdog、HPD/EDID 和 CSI stream OFF/ON 的诊断证据见 docs/RK628-HDMI-Input-Diagnostic.md。
- Power Present getter、NTP 同步和完全断电后的 OLED 复位仍需要目标硬件复测。
