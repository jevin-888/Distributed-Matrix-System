# RK3566 eth0 与 I2C OLED 修复验证记录

验证日期：2026-08-14
设备：RK3566 Buildroot，Linux 5.10.160
串口：COM4，1500000 baud

## 1. eth0 根因与修复

根因不只一个：

1. 开机时 `eth0` 保持 DOWN，默认 `S40network` 只执行 `ifup -a`，但系统没有有效的 ifupdown 接口配置。
2. 设备中的 `S41dhcpcd` 把 `$DHCPCD_ARGS` 错写成了字面量 `DHCPCD_ARGS`，启动日志为 `DHCPCD_ARGS: interface not found`。
3. 原脚本使用 `/var/run/dhcpcd.pid`，实际 dhcpcd 9.4.1 PID 文件是 `/run/dhcpcd/pid`，导致 stop/restart 无法正确管理进程。

源码修复文件：

- `system/firefly-rk356x/package/distributed-matrix/S40network`
- `system/firefly-rk356x/package/distributed-matrix/S41dhcpcd`
- `system/firefly-rk356x/package/distributed-matrix/dhcpcd.conf`
- `system/firefly-rk356x/package/distributed-matrix/distributed-matrix.mk`

安装顺序固定为：

1. `S40network` 执行 `ip link set eth0 up` 并等待 PHY carrier。
2. `S41dhcpcd` 启动唯一 dhcpcd 实例，仅管理 eth0/IPv4。

## 2. 网卡设备验证结果

冷启动/重启日志已经确认：

```text
network: eth0 up, waiting for link...
YT8512B Ethernet PHY
Link is Up - 100Mbps/Full
network: eth0 link up
Starting dhcpcd...
eth0: leased 192.168.2.101 for 7200 seconds
```

启动后状态：

```text
eth0 UP, LOWER_UP
carrier=1
IPv4=192.168.2.101/24
dhcpcd: [manager] [ip4]
```

PC 到设备连续 ping 结果：3/3 成功，时延 2-3 ms。

## 3. I2C OLED 验证结果

硬件与代码参数一一对应：

```text
I2C bus: /dev/i2c-1
address: 0x3C
controller: SSD1306
resolution: 128x32
status layout: four 128x8-pixel text rows
display polarity: white background with black text (inverse mode, 0xA7)
```

OLED status text uses all four rows of the 128x32 panel. Each source 6x8 glyph
uses its native 6x8 advance without horizontal stretching, giving 21 uniform
characters per row: `SN:`, `ID:`, `IP:`, and live `eth0` throughput formatted
as `TX:<value>RX:<value>Mbps`.
The node ID is zero-padded to at least three digits, for example `ID:001`.
Periodic status checks run every five seconds. The driver caches each successfully
written 128-byte page and only transfers changed pages; unchanged SN, ID and IP
remain stable while the throughput row changes. Failed page writes disable the
display immediately and force a complete initialization before the next retry.

The panel drawing specifies a 7.58 mm viewing-area height but only a 5.58 mm
active-area height. The remaining glass border is outside the 128x32 pixel
matrix and cannot be removed by changing SSD1306 row data or display offset.

0.91 英寸屏按 128x32 初始化。后续取得的裸屏规格书
`SZCLX091-2832TSWFG02-H14 Ver A` 明确要求：multiplex 使用 `0xA8,0x1F`，
COM pins 使用 `0xDA,0x00`，contrast 使用 `0x81,0x8F`，pre-charge 使用
`0xD9,0x1F`。HVIDEO 小屏使用内部 DC/DC 接法。

2026-09-18 实物接口核验推翻此前 GPIO 假设：主板连接器为 I2C_TP，
RST=GPIO0_B6（14）。拉低 GPIO14 后屏幕熄灭、释放后仍保持关闭；
原来的 GPIO3_D5（125）无此效果。GPIO0_B5（13，INT）拉低后屏幕仍亮，
供电使能输出未验证有效，程序不驱动它。此前 124/125 的定义属于 DSI 接口。
当前应用、S05、Linux pinctrl、U-Boot 均统一控制 GPIO14 复位，移除旧的
VBAT GPIO 控制；保持复位等待 200ms，释放后等待 100ms，在显示关闭状态下
配置控制器并清理全部显存，待升压稳定后开启显示。安装修正应用及 S05 后，用户
按断电后操作主板重启按钮放电、再上电的方式验证，确认第二行居中提示及设备信息
均正常、无花屏；随后复测反馈刚上电短暂花屏、再正常显示。因此应用修正后仍未通过
上电全程无花屏验收。板上 U-Boot/kernel 尚未更新，v20 早期复位保护待整包刷机验证，
且 U-Boot 执行前的上电窗口仍需另行确认，不能提前宣称已彻底解决。

2026-09-18 后续检查确认现场内核仍为 8 月 30 日版本。v21 候选将首次 GPIO14
拉低从 U-Boot 的 `rk_board_init()` 提前到 `arch_cpu_init()`，在 driver model 和
板级电源初始化前执行；先预置低电平，再开启输出和 GPIO 复用。新增寄存器模型
测试后 CTest 9/9 通过。该候选不改厂商 DDR/SPL 加载器，因此不覆盖加载器之前
的上电窗口；需要刷入后结合实际屏幕和串口记录验证，不能只凭编译和读回结果验收。

以下为 2026-09-17 的历史排查，不能作为当前引脚映射或最终验收依据。

2026-09-17 冷启动修复候选（未通过实机验收）：v16 实际代码曾提前打开 POWER_EN。
根据原理图确认 J2 的 VCC_3V3 独立供给 VDD，POWER_EN 仅控制 VBAT，因此恢复为
VBAT 关闭时复位、初始化并清理 SSD1306 全部 8 页 RAM，再打开 VBAT、等待 100ms、
发送 0xAF。GPIO 或 I2C 失败禁止亮屏，丢弃未完成帧并在 5 秒后完整重试。
S05 使用同一生产程序的 `--oled-boot` 模式，在网络启动前显示白底开机提示；
候选 ROM 的 U-Boot 新增 GPIO3_D4/D5 拉低保护，Linux 继续保持相同安全状态。
当前板子仅安装新应用及 S05 脚本，尚未刷入新 U-Boot；用户实测冷启动持续黑屏、
软重启正常。初始化日志和 I²C ACK 不能证明屏幕实际显示成功，仍需继续定位。
候选 v17 已标记不可作为修复完成版交付。下述历史运行结果不代表本次冷启动通过。

2026-09-17 后续实机排查：单独复位及逐字节写入仍花屏，`0xA5` 全亮正常。
将配置与参数合并传输、关闭滚动、使用水平寻址并显式指定列/页窗口后，用户确认
显示恢复正常。该方式已合入生产驱动：清屏覆盖 0..7 页，更新行单独指定该页，
不再使用原来的页地址/列高低位命令。`system login` 改到第二行（row=1）居中
（col=28）。CTest 8/8 通过，新 ARM 程序已安装；用户随后断电约 10 秒再上电，
确认居中提示及后续设备信息均正常、没有花屏，本轮物理冷启动显示验收通过。
板上 U-Boot 仍是 v16，本次实测范围为更新后的应用和 S05 启动脚本。

使用 Buildroot AArch64 工具链交叉编译并在设备运行，结果：

```text
OledDisplay: opened /dev/i2c-1 addr=0x3C
OLED opened OK. Displaying info ...
OLED_RC=0
```

2026-08-26 在 `192.168.2.103` 的 HVIDEO 六针裸屏板重新验证时，程序在
`/dev/i2c-1`、7 位地址 `0x3C` 发送第一个 `0x00,0xAE` 事务即收到
`ENXIO`（地址无 ACK）。同一总线上的 RK628 `0x51` 正常，因此本次不亮发生在
OLED 地址应答之前，不是初始化参数或显存数据导致。应检查 J2 的 3.3V、GND、
LCD_RST、SCL、SDA 及连接器/FPC 连通性；HVIDEO 原理图中的 I2C 上拉 R8/R1 为
NC，也必须确认主板侧上拉实际存在。

测试时写入内容：

```text
distributed-matrix
192.168.2.101
```

生产版 ARM64 `distributed-matrix` 已使用 `BUILD_TESTING=OFF` 完整交叉编译通过。`oled_test` 已改为仅在 `BUILD_TESTING=ON` 时构建，不进入正式 Buildroot rootfs。

## 4. 清理

- 已删除设备 `/tmp/oled_test` 与上传中间文件。
- 已删除 WSL 和 Windows 中的临时 ARM64 测试产物。
- 正式源码只保留一个 S40、一个 S41、一个 dhcpcd 配置及一套 OLED 实现。
- 2026-08-14 验证未生成完整刷机 ROM；2026-09-17 的修复、冷启动验收及 ROM 构建另见 `docs/Progress-Log.md` 对应记录。
