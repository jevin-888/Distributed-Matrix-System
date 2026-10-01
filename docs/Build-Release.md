# 构建与发布规范

更新日期：2026-10-01。

## 唯一版本源

- Linux 应用和 ROM 版本：根目录 `CMakeLists.txt` 的 `project(... VERSION major.minor.patch)`。
- Android PAD 版本：`hvideo_cashier_flutter/pubspec.yaml` 的 `version: major.minor.patch+build`，命名只取前三段版本。

## 唯一构建入口

| 目标 | 命令 | 输出 |
| --- | --- | --- |
| Linux 应用 | `./build.sh` | `build/linux-app/distributed-matrix` |
| RK3566 Linux ROM | `system/firefly-rk356x/build-rom.sh` | `artifacts/buildroot-rom/<版本>/hsvj-engine-<版本>-H6_M.img` |
| Android PAD | `hvideo_cashier_flutter/scripts/build-release.ps1` | `hvideo_cashier_flutter/release/android/hsvj-engine-<版本>-H6_M.apk` |

ROM 构建读取 `build-baseline.env`，清理 Buildroot、kernel 输出和打包临时目录后重新构建。禁止用旧的 `update_v*.img`、独立 SDK 或环境变量指定的二进制作为正式交付物。

## 格式边界

APK 是 Android PAD 安装包；Linux 播放器使用 Buildroot 生成的 `.img` ROM。两者不能互换，也不能用不含版本号的文件名交付。

## 发布检查

1. 确认 CMake/Flutter 版本一致且命名包含版本号和 `H6_M`。
2. 运行 CTest、`flutter analyze` 和 `flutter test`。
3. 为 ROM 生成 `BUILD-MANIFEST.txt`、`SOURCE-SHA256SUMS` 和 `SHA256SUMS`。
4. 发布后删除 `.dart_tool`、Flutter `build`、CMake 临时测试目录和 ROM 的 boot/rootfs 中间副本。
5. 保留硬件诊断证据和正式校验清单，不保留散落测试输出。
