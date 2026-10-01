import 'package:flutter/material.dart';

import 'controllers/site_controller.dart';
import 'pages/control_dashboard_page.dart';
import 'theme/app_theme.dart';

class DistributedPlaybackSystemApp extends StatefulWidget {
  const DistributedPlaybackSystemApp({super.key});

  @override
  State<DistributedPlaybackSystemApp> createState() =>
      _DistributedPlaybackSystemAppState();
}

class _DistributedPlaybackSystemAppState
    extends State<DistributedPlaybackSystemApp> {
  late final SiteController controller;

  @override
  void initState() {
    super.initState();
    controller = SiteController();
  }

  @override
  void dispose() {
    controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: '分布式播放系统',
      debugShowCheckedModeBanner: false,
      theme: buildAppTheme(),
      home: ControlDashboardPage(controller: controller),
    );
  }
}
