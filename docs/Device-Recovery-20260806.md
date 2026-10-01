# RK3566 设备恢复记录

日期：2026-08-06。本文是历史恢复记录，不是新的构建入口。

## 基线

恢复使用 Linux 5.10.160 原始基线内核和 RK356x Buildroot SDK。当前正式基线由 system/firefly-rk356x/build-baseline.env 指定，ROM 必须通过 build-rom.sh 生成。

## 恢复原则

- 保留固定内核、Buildroot defconfig、DTS 和 U-Boot 配置的对应关系。
- 不把临时 update.img、单独 kernel 或测试二进制作为正式发布物。
- ROM 刷写后验证启动服务、网络发现、HDMI 输入和播放器进程。

## 网络验证

设备启动后应在实际网卡上获得 IPv4 地址，并能接收 UDP 9002 心跳、响应 UDP 9003 发现请求。验证时不能只使用 127.0.0.1。

## 当前交付

完整 ROM 使用 hsvj-engine-<版本>-H6_M.img 命名。APK 仅属于 Android PAD 客户端；Linux 设备不安装 APK。
