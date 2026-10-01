import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:distributed_playback_system/src/api/device_discovery.dart';
import 'package:distributed_playback_system/src/theme/app_theme.dart';
import 'package:distributed_playback_system/src/widgets/connection_dialog.dart';

Future<void> openDialog(
  WidgetTester tester,
  DeviceDiscovery discovery,
  Future<String?> Function(String) connect,
) async {
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(),
      home: Builder(
        builder: (context) => Scaffold(
          body: TextButton(
            onPressed: () => showDialog<void>(
              context: context,
              builder: (_) => ConnectionDialog(
                serverAddress: 'http://10.0.0.1:8080',
                discovery: discovery,
                connect: connect,
              ),
            ),
            child: const Text('连接'),
          ),
        ),
      ),
    ),
  );
  await tester.tap(find.text('连接'));
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 400));
}

DeviceDiscovery scanner({bool online = true}) => DeviceDiscovery(
  useUdp: false,
  loadNetworks: () async => [DiscoveryNetwork('10.0.0.1', 30)],
  probe: (uri) async => online && uri.host == '10.0.0.2',
);

void main() {
  testWidgets('opening searches automatically; one tap connects and closes', (
    tester,
  ) async {
    final connected = <String>[];
    await openDialog(tester, scanner(), (address) async {
      connected.add(address);
      return null;
    });
    expect(find.text('发现 1 台在线设备'), findsOneWidget);
    expect(find.text('连接并同步'), findsNothing);
    await tester.tap(find.byKey(const ValueKey('connect-device-10.0.0.2')));
    await tester.pumpAndSettle();
    expect(connected, ['http://10.0.0.2:8080']);
    expect(find.byType(ConnectionDialog), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('failure stays visible and allows another attempt', (
    tester,
  ) async {
    var calls = 0;
    await openDialog(tester, scanner(), (_) async {
      calls++;
      return '设备连接失败，请重试';
    });
    await tester.tap(find.byKey(const ValueKey('connect-device-10.0.0.2')));
    await tester.pumpAndSettle();
    expect(find.text('设备连接失败，请重试'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('connect-device-10.0.0.2')));
    await tester.pumpAndSettle();
    expect(calls, 2);
    await tester.tap(find.text('取消'));
    await tester.pumpAndSettle();
  });

  testWidgets('empty search supports retry and manual connection', (
    tester,
  ) async {
    final connected = <String>[];
    await openDialog(tester, scanner(online: false), (address) async {
      connected.add(address);
      return null;
    });
    expect(find.textContaining('未发现在线设备'), findsOneWidget);
    await tester.tap(find.text('重新搜索'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('手动输入地址'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField), 'http://10.9.0.7:8181');
    await tester.ensureVisible(find.text('连接此地址'));
    await tester.tap(find.text('连接此地址'));
    await tester.pumpAndSettle();
    expect(connected, ['http://10.9.0.7:8181']);
  });

  testWidgets('closing during discovery ignores late results', (tester) async {
    final pending = Completer<bool>();
    final discovery = DeviceDiscovery(
      useUdp: false,
      loadNetworks: () async => [],
      probe: (_) => pending.future,
    );
    await openDialog(tester, discovery, (_) async => null);
    await tester.tap(find.text('取消'));
    await tester.pumpAndSettle();
    pending.complete(true);
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(discovery.devices, isEmpty);
  });

  testWidgets('device selection remains usable on a narrow phone', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(390, 844);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await openDialog(tester, scanner(), (_) async => null);
    expect(find.text('发现 1 台在线设备'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await tester.tap(find.byKey(const ValueKey('connect-device-10.0.0.2')));
    await tester.pumpAndSettle();
    expect(find.byType(ConnectionDialog), findsNothing);
  });
}
