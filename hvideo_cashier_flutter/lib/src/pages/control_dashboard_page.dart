import 'package:flutter/material.dart';

import '../controllers/site_controller.dart';
import '../widgets/connection_dialog.dart';
import '../platform/admin_console_launcher.dart';
import '../theme/app_theme.dart';
import '../widgets/control_deck.dart';
import '../widgets/matrix_wall.dart';
import '../widgets/node_panel.dart';
import '../widgets/scene_panel.dart';
import '../widgets/tech_background.dart';
import '../widgets/tech_components.dart';

class ControlDashboardPage extends StatelessWidget {
  const ControlDashboardPage({
    super.key,
    required this.controller,
    this.adminConsoleLauncher,
  });

  final SiteController controller;
  final AdminConsoleLauncher? adminConsoleLauncher;

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: controller,
      builder: (context, _) {
        return Scaffold(
          body: TechGridBackground(
            child: SafeArea(
              child: LayoutBuilder(
                builder: (context, constraints) {
                  final wide = constraints.maxWidth >= 1160;
                  return Column(
                    children: <Widget>[
                      _TopBar(
                        controller: controller,
                        compact: !wide,
                        adminConsoleLauncher: adminConsoleLauncher,
                      ),
                      _ModeBar(controller: controller),
                      Expanded(
                        child: wide
                            ? _WideWorkspace(controller: controller)
                            : _CompactWorkspace(controller: controller),
                      ),
                    ],
                  );
                },
              ),
            ),
          ),
        );
      },
    );
  }
}

class _TopBar extends StatelessWidget {
  const _TopBar({
    required this.controller,
    required this.compact,
    required this.adminConsoleLauncher,
  });

  final SiteController controller;
  final bool compact;
  final AdminConsoleLauncher? adminConsoleLauncher;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: EdgeInsets.fromLTRB(compact ? 14 : 22, 12, compact ? 14 : 22, 8),
      child: Row(
        children: <Widget>[
          _BrandMark(compact: compact),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: <Widget>[
                Text(
                  '分布式播放系统',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(
                    fontSize: compact ? 19 : 25,
                    fontWeight: FontWeight.w800,
                    letterSpacing: .8,
                  ),
                ),
                if (!compact)
                  const Padding(
                    padding: EdgeInsets.only(top: 4),
                    child: Text(
                      '现场控制台 / DISTRIBUTED PLAYBACK SYSTEM',
                      style: TextStyle(
                        color: mutedText,
                        fontSize: 10,
                        letterSpacing: 1.8,
                      ),
                    ),
                  ),
              ],
            ),
          ),
          StatusPill(
            label: switch (controller.connectionState) {
              MatrixConnectionState.connected => '主节点已连接',
              MatrixConnectionState.connecting => '正在连接',
              MatrixConnectionState.error => '连接异常',
              MatrixConnectionState.disconnected => '未连接',
            },
            color: switch (controller.connectionState) {
              MatrixConnectionState.connected => mint,
              MatrixConnectionState.connecting => amber,
              MatrixConnectionState.error => danger,
              MatrixConnectionState.disconnected => amber,
            },
            pulsing:
                controller.connectionState == MatrixConnectionState.connecting,
          ),
          const SizedBox(width: 8),
          AnimatedActionButton(
            label: compact ? '' : '连接',
            icon: Icons.link_rounded,
            compact: true,
            accent: cyan,
            onPressed: controller.busy
                ? null
                : () => showConnectionDialog(context, controller),
            tooltip: '连接主节点',
          ),
          const SizedBox(width: 5),
          AnimatedActionButton(
            key: const ValueKey('admin-console-button'),
            label: '',
            icon: Icons.settings_rounded,
            compact: true,
            accent: mutedText,
            onPressed: () =>
                _openAdminConsole(context, controller, adminConsoleLauncher),
            tooltip: '后台管理',
          ),
        ],
      ),
    );
  }
}

Future<void> _openAdminConsole(
  BuildContext context,
  SiteController controller,
  AdminConsoleLauncher? launcher,
) async {
  try {
    final uri = Uri.parse(controller.serverAddress);
    final opened = await (launcher ?? openAdminConsole)(uri);
    if (opened || !context.mounted) return;
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(const SnackBar(content: Text('未找到可用的浏览器')));
  } catch (error) {
    if (!context.mounted) return;
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text('无法打开后台管理：$error')));
  }
}

class _BrandMark extends StatelessWidget {
  const _BrandMark({required this.compact});

  final bool compact;

  @override
  Widget build(BuildContext context) {
    final size = compact ? 42.0 : 52.0;
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(size * .31),
        boxShadow: <BoxShadow>[
          BoxShadow(color: cyan.withValues(alpha: .25), blurRadius: 20),
        ],
      ),
      child: Image.asset(
        'assets/branding/app_logo.png',
        width: size,
        height: size,
        filterQuality: FilterQuality.high,
      ),
    );
  }
}

class _ModeBar extends StatelessWidget {
  const _ModeBar({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 3),
      child: LayoutBuilder(
        builder: (context, constraints) {
          final regions = controller.regions.isEmpty
              ? <Widget>[const _EmptyRegionChip()]
              : controller.regions
                    .map(
                      (region) => _RegionChip(
                        controller: controller,
                        name: region.name,
                      ),
                    )
                    .toList(growable: false);
          return SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: ConstrainedBox(
              constraints: BoxConstraints(minWidth: constraints.maxWidth),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: <Widget>[
                  for (
                    var index = 0;
                    index < regions.length;
                    index++
                  ) ...<Widget>[
                    regions[index],
                    if (index < regions.length - 1) const SizedBox(width: 8),
                  ],
                ],
              ),
            ),
          );
        },
      ),
    );
  }
}

class _RegionChip extends StatelessWidget {
  const _RegionChip({required this.controller, required this.name});

  final SiteController controller;
  final String name;

  @override
  Widget build(BuildContext context) {
    final active = controller.selectedRegionName == name;
    return InkWell(
      key: ValueKey('region-chip-$name'),
      borderRadius: BorderRadius.circular(11),
      onTap: () => controller.selectRegion(name),
      child: AnimatedContainer(
        duration: const Duration(milliseconds: 180),
        padding: const EdgeInsets.symmetric(horizontal: 15, vertical: 10),
        decoration: BoxDecoration(
          color: active ? cyan.withValues(alpha: .15) : const Color(0x66122E46),
          borderRadius: BorderRadius.circular(11),
          border: Border.all(
            color: active
                ? cyan.withValues(alpha: .65)
                : Colors.white.withValues(alpha: .06),
          ),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(
              Icons.location_on_rounded,
              size: 16,
              color: active ? cyan : mutedText,
            ),
            const SizedBox(width: 7),
            Text(
              name,
              style: TextStyle(
                color: active ? Colors.white : mutedText,
                fontSize: 12,
                fontWeight: FontWeight.w700,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _EmptyRegionChip extends StatelessWidget {
  const _EmptyRegionChip();

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 38,
      padding: const EdgeInsets.symmetric(horizontal: 14),
      alignment: Alignment.center,
      decoration: BoxDecoration(
        color: const Color(0x66122E46),
        borderRadius: BorderRadius.circular(11),
        border: Border.all(color: Colors.white.withValues(alpha: .06)),
      ),
      child: const Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Icon(Icons.location_off_rounded, size: 16, color: mutedText),
          SizedBox(width: 7),
          Text(
            '后台未配置区域',
            style: TextStyle(
              color: mutedText,
              fontSize: 12,
              fontWeight: FontWeight.w700,
            ),
          ),
        ],
      ),
    );
  }
}

class _WideWorkspace extends StatelessWidget {
  const _WideWorkspace({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 10, 18, 18),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          SizedBox(width: 256, child: NodePanel(controller: controller)),
          const SizedBox(width: 13),
          Expanded(
            child: Column(
              children: <Widget>[
                Expanded(child: MatrixWall(controller: controller)),
                const SizedBox(height: 13),
                ControlDeck(controller: controller),
              ],
            ),
          ),
          const SizedBox(width: 13),
          SizedBox(width: 272, child: ScenePanel(controller: controller)),
        ],
      ),
    );
  }
}

class _CompactWorkspace extends StatelessWidget {
  const _CompactWorkspace({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    return SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(14, 8, 14, 18),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          NodePanel(controller: controller, horizontal: true),
          const SizedBox(height: 12),
          SizedBox(height: 430, child: MatrixWall(controller: controller)),
          const SizedBox(height: 12),
          ControlDeck(controller: controller),
          const SizedBox(height: 12),
          ScenePanel(controller: controller, horizontal: true),
          const SizedBox(height: 8),
          Center(
            child: Text(
              '分布式播放系统 · API v1.2',
              style: TextStyle(
                color: mutedText.withValues(alpha: .65),
                fontSize: 10,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

Future<void> showConnectionDialog(
  BuildContext context,
  SiteController controller,
) => showDialog<void>(
  context: context,
  barrierDismissible: false,
  builder: (context) => ConnectionDialog(
    serverAddress: controller.serverAddress,
    connect: (address) async {
      await controller.connect(address);
      return controller.isConnected ? null : controller.message;
    },
  ),
);
