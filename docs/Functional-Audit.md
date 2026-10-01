# 功能审核记录

核对日期：2026-10-01。

## 通过项

| 范围 | 结果 | 证据 |
| --- | --- | --- |
| Master/Slave 进程 | PASS | systemd 服务配置和组件测试 |
| 全网卡 UDP 发现 | PASS | 心跳绑定 INADDR_ANY；活动网卡广播和单播扫描 |
| REST API | PASS | HTTP 集成测试覆盖方法和路径，非法方法返回 405，未知路径返回 404 |
| HTTP Range/缓存 | PASS | cache/http 集成测试 |
| H.264/H.265 播放 | PASS | MPP/KMS 播放路径测试 |
| crop、EOS、watchdog | PASS | MediaPlayer 和组件测试 |
| RK628 1080p60 | PASS | 设备采样：1920x1080、148.5 MHz、60 fps |
| NV16/NV12 | PASS | 设备采样与 KMS plane 78 格式验证 |
| C++ 自动化测试 | PASS | CTest 9/9 |
| Flutter 检查 | PASS | analyze 无问题，测试 55 项通过 |

## API 约束

HTTP API 的方法、路径和状态码必须一对一对应。新增接口必须同时更新 docs/API-Reference.md、服务器路由和集成测试；不得通过模糊路径或多个方法别名绕过规范。

## 构建审核

Linux 应用使用 build.sh，Linux ROM 使用 system/firefly-rk356x/build-rom.sh，Android PAD 使用 Flutter 发布脚本。ROM 构建固定 SDK/内核/Buildroot 基线并执行完整清理，不允许另设构建分支。

## 待复核项

目标硬件仍需复测 Power Present getter、NTP 同步和 HDMI 热插拔恢复。历史诊断文件保存在 artifacts/，不作为当前实现的唯一依据。
