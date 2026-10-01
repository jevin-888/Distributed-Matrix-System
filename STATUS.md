# 项目状态

最后核对：2026-10-01。本文只记录可以由代码、测试或设备采样直接确认的状态。

## 当前状态

| 项目 | 状态 | 依据 |
| --- | --- | --- |
| Linux RK3566 播放器 | 已配置 | Buildroot 基线和 systemd 服务位于 system/firefly-rk356x |
| 全网卡设备发现 | 已实现 | UDP 9002 心跳、UDP 9003 广播/单播发现，监听 INADDR_ANY |
| REST/HTTP 服务 | 已实现 | src/master/HTTPFileServer.cpp 和 HTTP 集成测试 |
| C++ 测试 | 通过 | CTest 9 项测试通过 |
| Flutter 静态检查 | 通过 | flutter analyze 无问题 |
| Flutter 测试 | 通过 | 55 项测试通过 |
| Linux ROM 交付 | 统一 | build-rom.sh 使用固定 SDK，完整清理后输出单一 .img |
| Android PAD 交付 | 统一 | 发布脚本读取 pubspec.yaml 并输出带版本号 .apk |

## 已确认的硬件证据

历史设备记录确认 RK628 HDMI 输入支持 1920x1080p60，V4L2 捕获使用 NV16/NV12，DRM/KMS 使用 plane 78。原始采样和校验值保存在 artifacts/，不应当被当作新的构建入口。

## 仍需设备复核

Power Present V4L2 getter 的 error 25、NTP 同步状态和 HDMI 热插拔恢复需要在目标硬件上复测。它们不影响本地构建和网络发现协议的结论。

## 交付规则

- Linux：hsvj-engine-<版本>-H6_M.img
- Android PAD：hsvj-engine-<版本>-H6_M.apk
- 版本分别来自 CMake 和 Flutter pubspec.yaml，不得手工拼接其他版本。
- 历史 update_v*.img 只作为归档证据，不是正式交付入口。
