# HSVJ Engine

HSVJ Engine 是面向 RK3566 播放器节点的 Linux 分布式矩阵系统。主节点提供 HTTP 控制与媒体服务，节点通过 UDP 心跳和发现协议加入矩阵，播放器负责 HDMI 输入、视频解码和显示。

## 网络发现

- UDP 9002：节点心跳与在线状态。
- UDP 9003：主动发现请求和响应。
- 服务端监听 INADDR_ANY，组播加入使用通配地址，并扫描每个活动 IPv4 网卡的本地网段。
- 客户端按实际 IPv4 网卡计算广播地址，同时执行广播和单播探测，因此发现不依赖回环地址，也不会限制在单一网卡。

## 构建入口

项目只有一个 Linux 源码和一个 Buildroot ROM 基线：

```text
Linux 应用测试/构建       ./build.sh
Linux RK3566 ROM          system/firefly-rk356x/build-rom.sh
Android PAD 客户端        hvideo_cashier_flutter/scripts/build-release.ps1
```

ROM 构建使用 system/firefly-rk356x/build-baseline.env 中的固定 SDK、内核、Buildroot 配置和 DTS。ROM 始终执行完整清理构建，不提供增量或备用构建分支。

## 交付物命名

版本源为根目录 CMakeLists.txt 的项目版本；PAD 版本源为 Flutter pubspec.yaml。命名格式如下：

```text
Linux ROM：   hsvj-engine-<版本>-H6_M.img
Android PAD： hsvj-engine-<版本>-H6_M.apk
```

.apk 是 Android 安装包，不是 Linux 安装包。Linux 播放器通过 Buildroot 生成 .img，例如 hsvj-engine-1.0.0-H6_M.img。禁止使用不带版本号的 hsvj-engine-hw81stable-release.apk 等名称。

## 验证

```bash
./build.sh
ctest --test-dir build/linux-app --output-on-failure
```

Flutter 发布前运行 flutter analyze、flutter test，再执行 scripts/build-release.ps1。构建产物与校验清单位于 artifacts/buildroot-rom/<版本>/；临时构建目录不属于交付物。

## 项目记录

每次构建或发布变更记录在 docs/Progress-Log.md 和 docs/Build-Release.md。硬件诊断证据保留在 artifacts/，临时测试目录应在验证后删除。
