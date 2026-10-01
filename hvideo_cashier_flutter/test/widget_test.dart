import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

import 'package:distributed_playback_system/src/app.dart';
import 'package:distributed_playback_system/src/api/matrix_api_client.dart';
import 'package:distributed_playback_system/src/controllers/site_controller.dart';
import 'package:distributed_playback_system/src/kvm/kvm_transport.dart';
import 'package:distributed_playback_system/src/models/matrix_models.dart';
import 'package:distributed_playback_system/src/pages/control_dashboard_page.dart';
import 'package:distributed_playback_system/src/pages/kvm_takeover_page.dart';
import 'package:distributed_playback_system/src/theme/app_theme.dart';
import 'package:distributed_playback_system/src/widgets/control_deck.dart';
import 'package:distributed_playback_system/src/widgets/live_jpeg_preview.dart';
import 'package:distributed_playback_system/src/widgets/matrix_wall.dart';
import 'package:distributed_playback_system/src/widgets/node_panel.dart';
import 'package:distributed_playback_system/src/widgets/scene_panel.dart';

void main() {
  test(
    'controller starts without demo nodes and targets the configured master',
    () {
      final controller = SiteController(persistPresetState: false);
      addTearDown(controller.dispose);

      expect(controller.serverAddress, 'http://192.168.2.101:8080');
      expect(controller.nodes, isEmpty);
      expect(controller.message, '未连接矩阵主节点');
    },
  );

  testWidgets('desktop control dashboard boots without overflow', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(const DistributedPlaybackSystemApp());
    await tester.pump(const Duration(milliseconds: 300));

    expect(find.text('分布式播放系统'), findsOneWidget);
    expect(find.text('输入节点'), findsOneWidget);
    expect(find.text('屏幕墙实时视图'), findsNothing);
    expect(find.text('在线节点'), findsNothing);
    expect(find.byKey(const ValueKey('clear-canvas-button')), findsOneWidget);
    expect(
      find.byKey(const ValueKey('fullscreen-layout-button')),
      findsOneWidget,
    );
    expect(
      tester.getRect(find.byKey(const ValueKey('clear-canvas-button'))).right,
      lessThan(
        tester
            .getRect(find.byKey(const ValueKey('fullscreen-layout-button')))
            .left,
      ),
    );
    expect(find.text('输入开窗布局'), findsOneWidget);
    expect(find.text('保存预设'), findsOneWidget);
    expect(find.byKey(const ValueKey('kvm-takeover-button')), findsNothing);
    expect(tester.takeException(), isNull);
  });

  testWidgets('settings button opens the current backend console', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final openedUris = <Uri>[];
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );

    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: ControlDashboardPage(
          controller: controller,
          adminConsoleLauncher: (uri) async {
            openedUris.add(uri);
            return true;
          },
        ),
      ),
    );
    await tester.tap(find.byKey(const ValueKey('admin-console-button')));
    await tester.pump();

    expect(openedUris, <Uri>[Uri.parse('http://192.168.2.101:8080')]);
    expect(find.text('连接主节点'), findsNothing);
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets('quick layout dropdown only displays grid dimensions', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1600, 1000);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(const DistributedPlaybackSystemApp());
    await tester.pump(const Duration(milliseconds: 300));

    expect(find.text('2×2 · 0 个节点'), findsNothing);
    await tester.tap(find.byKey(const ValueKey('quick-layout-menu')));
    await tester.pumpAndSettle();

    final labels = tester
        .widgetList<Text>(
          find.descendant(
            of: find.byType(PopupMenuItem<MatrixLayout>),
            matching: find.byType(Text),
          ),
        )
        .map((text) => text.data)
        .toList(growable: false);

    expect(labels, <String>['1×1', '2×2', '1×3', '2×3', '3×3', '1×4']);
  });

  testWidgets('region selector is centered without connection summary', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1200, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    controller.debugLoadRegions(const <MatrixRegion>[
      MatrixRegion(
        name: '主舞台',
        savedAt: 1,
        layout: MatrixLayout(
          rows: 1,
          cols: 1,
          screenWidth: 1920,
          screenHeight: 1080,
        ),
        placements: <MatrixScreenPlacement>[],
      ),
    ]);

    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: ControlDashboardPage(controller: controller),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('主舞台'), findsOneWidget);
    expect(find.textContaining('已连接 ·'), findsNothing);
    expect(
      tester.getCenter(find.byKey(const ValueKey('region-chip-主舞台'))).dx,
      closeTo(600, 1),
    );
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets('Android portrait layout stays usable without overflow', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(412, 915);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(const DistributedPlaybackSystemApp());
    await tester.pump(const Duration(milliseconds: 300));

    expect(find.text('分布式播放系统'), findsOneWidget);
    expect(find.text('输入节点'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  test('left panel exposes input nodes independently of output regions', () {
    final controller = SiteController(persistPresetState: false);
    addTearDown(controller.dispose);

    controller.debugLoadNodes(const <MatrixNode>[
      MatrixNode(
        nodeId: 1,
        ip: '192.168.2.103',
        resolution: '1280×720',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: false,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        nodeRole: 'encode',
        deviceType: 'input',
      ),
      MatrixNode(
        nodeId: 2,
        ip: '192.168.2.104',
        resolution: '1920×1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: false,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        nodeRole: 'decode',
        deviceType: 'output',
      ),
      MatrixNode(
        nodeId: 3,
        ip: '192.168.2.105',
        resolution: '1920×1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: false,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        nodeRole: 'decode',
        deviceType: 'output',
      ),
    ]);

    controller.debugLoadRegions(const <MatrixRegion>[
      MatrixRegion(
        name: 'A区',
        savedAt: 1,
        inputNodeIds: <int>[1],
        layout: MatrixLayout(
          rows: 1,
          cols: 1,
          screenWidth: 1920,
          screenHeight: 1080,
        ),
        placements: <MatrixScreenPlacement>[
          MatrixScreenPlacement(nodeId: 2, frame: Rect.fromLTWH(0, 0, 1, 1)),
        ],
      ),
    ]);

    expect(controller.selectedRegionName, 'A区');
    expect(controller.visibleNodes.map((node) => node.nodeId), <int>[1]);
    expect(controller.visibleOutputNodes.map((node) => node.nodeId), <int>[
      2,
      3,
    ]);
    expect(controller.placementForNode(1), isNull);
  });

  testWidgets(
    'node panel can search output nodes without making them canvas sources',
    (tester) async {
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
      );
      controller.debugLoadNodes(const <MatrixNode>[
        MatrixNode(
          nodeId: 1,
          ip: '192.168.2.101',
          resolution: '1280x720',
          status: 'online',
          playState: 'idle',
          cpu: 0,
          memory: 0,
          masterClockSynchronized: true,
          masterClockOffsetMs: 0,
          lastSeen: 1,
          nodeRole: 'decode',
          deviceType: 'output',
        ),
      ]);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Scaffold(body: NodePanel(controller: controller)),
        ),
      );
      await tester.tap(find.text('输出').last);
      await tester.pump();

      expect(find.text('输出节点'), findsOneWidget);
      expect(find.text('192.168.2.101'), findsOneWidget);
      expect(find.byType(Draggable<int>), findsNothing);
      expect(controller.canPlaceNodeOnCanvas(1), isFalse);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
    },
  );

  test('selected region limits left panel to its configured input nodes', () {
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    addTearDown(controller.dispose);

    controller.debugLoadNodes(const <MatrixNode>[
      MatrixNode(
        nodeId: 1,
        ip: '192.168.2.101',
        resolution: '1920×1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: false,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        nodeRole: 'encode',
        deviceType: 'input',
      ),
      MatrixNode(
        nodeId: 2,
        ip: '192.168.2.102',
        resolution: '1920×1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: false,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        nodeRole: 'encode',
        deviceType: 'input',
      ),
    ]);
    controller.debugLoadRegions(const <MatrixRegion>[
      MatrixRegion(
        name: 'A区',
        savedAt: 1,
        inputNodeIds: <int>[2],
        layout: MatrixLayout(
          rows: 1,
          cols: 1,
          screenWidth: 1920,
          screenHeight: 1080,
        ),
        placements: <MatrixScreenPlacement>[],
      ),
    ]);

    expect(controller.visibleNodes.map((node) => node.nodeId), <int>[2]);
  });

  test('dragging a node into the 2x2 canvas snaps to a layout container', () {
    final controller = SiteController(persistPresetState: false);
    addTearDown(controller.dispose);

    controller.debugLoadNodes(const <MatrixNode>[
      MatrixNode(
        nodeId: 1,
        ip: '192.168.2.101',
        resolution: '1920×1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: true,
        masterClockOffsetMs: 0,
        lastSeen: 1,
      ),
    ]);

    expect(controller.layoutCellAt(const Offset(.1, .1))?.id, 'R1-C1');
    expect(controller.layoutCellAt(const Offset(.75, .75))?.id, 'R2-C2');

    controller.placeNodeOnCanvas(1, const Offset(.75, .75));
    final placement = controller.placementForNode(1);

    expect(placement, isNotNull);
    expect(
      placement!.frame,
      controller.frameForLayoutCell(const LayoutCell(row: 1, col: 1)),
    );
    expect(controller.layoutContainerIdForPlacement(placement), 'R2-C2');
  });

  test(
    'refresh keeps canvas placements when the backend has no regions',
    () async {
      final client = MockClient((request) async {
        final body = switch (request.url.path) {
          MatrixApiSpec.nodes => <String, dynamic>{
            'nodes': <Map<String, dynamic>>[
              <String, dynamic>{
                'nodeId': 1,
                'ip': '192.168.2.101',
                'resolution': '1920x1080',
                'status': 'online',
                'playState': 'idle',
                'cpu': 0,
                'memory': 0,
                'masterClockSynchronized': true,
                'masterClockOffsetMs': 0,
                'lastSeen': 1,
              },
            ],
          },
          MatrixApiSpec.regions => <String, dynamic>{'regions': <dynamic>[]},
          MatrixApiSpec.screens => <String, dynamic>{
            'layout': <String, dynamic>{
              'rows': 2,
              'cols': 2,
              'screenWidth': 1920,
              'screenHeight': 1080,
            },
            'screens': <dynamic>[],
          },
          MatrixApiSpec.status => <String, dynamic>{
            'running': false,
            'state': 'idle',
            'videoUrl': '',
            'syncTimestamp': 0,
            'updatedAt': 0,
            'http': <String, dynamic>{
              'activeConnections': 0,
              'totalRequests': 0,
              'totalBytesSent': 0,
            },
          },
          MatrixApiSpec.audioOutput => <String, dynamic>{'mode': 'both'},
          MatrixApiSpec.audioVolume => <String, dynamic>{'volumePercent': 100},
          _ => throw StateError('Unexpected request: ${request.url.path}'),
        };
        return http.Response(jsonEncode(body), 200);
      });
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
        apiClient: MatrixApiClient('http://127.0.0.1:8080', client: client),
      );
      addTearDown(controller.dispose);
      controller.debugLoadNodes(<MatrixNode>[_testNode(1)]);
      final placementId = controller.placeNodeOnCanvas(
        1,
        const Offset(.75, .75),
      );

      await controller.refresh(silent: true);

      expect(controller.connectionState, MatrixConnectionState.connected);
      expect(controller.canvasPlacements.single.placementId, placementId);
      expect(controller.selectedRegionName, isNull);
    },
  );

  test('output screen mapping is not imported into the input canvas', () async {
    final client = MockClient((request) async {
      final body = switch (request.url.path) {
        MatrixApiSpec.nodes => <String, dynamic>{
          'nodes': <Map<String, dynamic>>[
            <String, dynamic>{
              'nodeId': 1,
              'ip': '192.168.2.103',
              'resolution': '1280x720',
              'status': 'online',
              'playState': 'idle',
              'cpu': 0,
              'memory': 0,
              'masterClockSynchronized': false,
              'masterClockOffsetMs': 0,
              'lastSeen': 1,
              'kvmEnabled': true,
              'kvmRole': 'target',
              'kvmPort': 9101,
              'kvmSessionId': 0,
              'kvmState': 'idle',
              'kvmLastError': '',
            },
          ],
        },
        MatrixApiSpec.regions => <String, dynamic>{'regions': <dynamic>[]},
        MatrixApiSpec.screens => <String, dynamic>{
          'layout': <String, dynamic>{
            'rows': 2,
            'cols': 2,
            'screenWidth': 1920,
            'screenHeight': 1080,
          },
          'screens': <Map<String, dynamic>>[
            <String, dynamic>{
              'nodeId': 1,
              'row': 0,
              'col': 0,
              'width': 1920,
              'height': 1080,
            },
          ],
        },
        MatrixApiSpec.status => <String, dynamic>{
          'running': false,
          'state': 'idle',
          'videoUrl': '',
          'syncTimestamp': 0,
          'updatedAt': 0,
          'http': <String, dynamic>{
            'activeConnections': 0,
            'totalRequests': 0,
            'totalBytesSent': 0,
          },
        },
        MatrixApiSpec.audioOutput => <String, dynamic>{'mode': 'both'},
        MatrixApiSpec.audioVolume => <String, dynamic>{'volumePercent': 100},
        _ => throw StateError('Unexpected request: ${request.url.path}'),
      };
      return http.Response(jsonEncode(body), 200);
    });
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
      apiClient: MatrixApiClient('http://127.0.0.1:8080', client: client),
    );
    addTearDown(controller.dispose);

    await controller.refresh(silent: true);

    expect(controller.canvasPlacements, isEmpty);
    expect(controller.selectedCanvasNode, isNull);
    expect(controller.canTakeoverSelectedNode, isFalse);
  });

  test(
    'output placements stay separate from local input canvas edits',
    () async {
      final screenWrites = <Map<String, dynamic>>[];
      final client = MockClient((request) async {
        if (request.method == 'PUT' &&
            request.url.path == MatrixApiSpec.screens) {
          screenWrites.add(jsonDecode(request.body) as Map<String, dynamic>);
          return http.Response('{"success":true}', 200);
        }
        final body = switch (request.url.path) {
          MatrixApiSpec.nodes => <String, dynamic>{
            'nodes': <Map<String, dynamic>>[
              <String, dynamic>{
                'nodeId': 7,
                'ip': '192.168.2.107',
                'resolution': '1920x1080',
                'status': 'online',
                'playState': 'idle',
                'cpu': 0,
                'memory': 0,
                'masterClockSynchronized': true,
                'masterClockOffsetMs': 0,
                'lastSeen': 1,
              },
            ],
          },
          MatrixApiSpec.regions => <String, dynamic>{'regions': <dynamic>[]},
          MatrixApiSpec.screens => <String, dynamic>{
            'layout': <String, dynamic>{
              'rows': 2,
              'cols': 2,
              'screenWidth': 1920,
              'screenHeight': 1080,
            },
            'screens': <dynamic>[],
            'placements': <Map<String, dynamic>>[
              <String, dynamic>{
                'nodeId': 7,
                'x': .5,
                'y': 0,
                'width': .5,
                'height': .5,
              },
            ],
          },
          MatrixApiSpec.status => <String, dynamic>{
            'running': false,
            'state': 'idle',
            'videoUrl': '',
            'syncTimestamp': 0,
            'updatedAt': 0,
            'http': <String, dynamic>{
              'activeConnections': 0,
              'totalRequests': 0,
              'totalBytesSent': 0,
            },
          },
          MatrixApiSpec.audioOutput => <String, dynamic>{'mode': 'both'},
          MatrixApiSpec.audioVolume => <String, dynamic>{'volumePercent': 100},
          _ => throw StateError('Unexpected request: ${request.url.path}'),
        };
        return http.Response(jsonEncode(body), 200);
      });
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
        apiClient: MatrixApiClient('http://127.0.0.1:8080', client: client),
      );
      addTearDown(controller.dispose);

      await controller.refresh(silent: true);

      expect(controller.canvasPlacements, isEmpty);
      controller.placeNodeOnCanvas(7, const Offset(.75, .25));
      controller.moveCanvasNode(
        controller.canvasPlacements.single.placementId,
        const Offset(-.1, .1),
      );
      await Future<void>.delayed(const Duration(milliseconds: 260));

      expect(controller.canvasPlacements.single.nodeId, 7);
      expect(screenWrites, isEmpty);
    },
  );

  testWidgets(
    'dragging a node card into the canvas keeps the placement visible',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 800);
      tester.view.devicePixelRatio = 1;
      debugDefaultTargetPlatformOverride = TargetPlatform.windows;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
      );
      controller.debugLoadNodes(<MatrixNode>[_testNode(1)]);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Scaffold(
            body: Row(
              children: <Widget>[
                SizedBox(width: 280, child: NodePanel(controller: controller)),
                Expanded(child: MatrixWall(controller: controller)),
              ],
            ),
          ),
        ),
      );
      await tester.pumpAndSettle();

      final source = tester.getCenter(find.text(_testNode(1).ip));
      final canvas = tester.getRect(
        find.byKey(const ValueKey('canvas-content')),
      );
      expect(
        find.byWidgetPredicate(
          (widget) => widget is Draggable || widget is LongPressDraggable,
        ),
        findsOneWidget,
      );
      await tester.dragFrom(
        source,
        canvas.center - source,
        kind: ui.PointerDeviceKind.mouse,
      );
      await tester.pumpAndSettle();
      debugDefaultTargetPlatformOverride = null;

      expect(controller.placementCountForNode(1), 1);
      expect(find.byKey(const ValueKey('canvas-node-box-1')), findsOneWidget);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
    },
  );

  test('the same node can occupy multiple independent layout windows', () {
    final controller = SiteController(persistPresetState: false);
    addTearDown(controller.dispose);

    controller.debugLoadNodes(const <MatrixNode>[
      MatrixNode(
        nodeId: 1,
        ip: '192.168.2.101',
        resolution: '1920×1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: true,
        masterClockOffsetMs: 0,
        lastSeen: 1,
      ),
    ]);

    final firstId = controller.placeNodeOnCanvas(1, const Offset(.25, .25));
    final secondId = controller.placeNodeOnCanvas(1, const Offset(.75, .75));

    expect(firstId, isNotNull);
    expect(secondId, isNotNull);
    expect(secondId, isNot(firstId));
    expect(controller.placementCountForNode(1), 2);
    expect(
      controller.canvasPlacements
          .map(controller.layoutContainerIdForPlacement)
          .toSet(),
      <String>{'R1-C1', 'R2-C2'},
    );

    controller.removeNodeFromCanvas(firstId!);
    expect(controller.placementCountForNode(1), 1);
    expect(controller.canvasPlacements.single.placementId, secondId);
  });

  test('each input node can occupy at most 16 canvas windows', () {
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    addTearDown(controller.dispose);

    controller.debugLoadNodes(<MatrixNode>[_testNode(1), _testNode(2)]);

    for (var index = 0; index < SiteController.maxPlacementsPerNode; index++) {
      expect(controller.placeNodeOnCanvas(1, const Offset(.5, .5)), isNotNull);
    }
    expect(
      controller.placementCountForNode(1),
      SiteController.maxPlacementsPerNode,
    );
    expect(controller.canPlaceNodeOnCanvas(1), isFalse);
    expect(controller.placeNodeOnCanvas(1, const Offset(.5, .5)), isNull);
    expect(
      controller.placementCountForNode(1),
      SiteController.maxPlacementsPerNode,
    );

    // The limit is per input node, so another node still has all 16 slots.
    expect(controller.canPlaceNodeOnCanvas(2), isTrue);
    expect(controller.placeNodeOnCanvas(2, const Offset(.5, .5)), isNotNull);
    expect(controller.placementCountForNode(2), 1);
  });

  test(
    'canvas priorities move adjacent layers and preserve same-level order',
    () {
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
      );
      controller.debugLoadNodes(<MatrixNode>[_testNode(1)]);

      final firstId = controller.placeNodeOnCanvas(1, const Offset(.2, .2))!;
      final secondId = controller.placeNodeOnCanvas(1, const Offset(.5, .5))!;
      final thirdId = controller.placeNodeOnCanvas(1, const Offset(.8, .8))!;

      expect(
        controller.canvasPlacements.map((placement) => placement.priorityLabel),
        <String>['001', '002', '003'],
      );

      controller.changeSelectedPriority(CanvasPriorityAction.decrease);
      expect(
        controller.canvasPlacements.map((placement) => placement.placementId),
        <int>[firstId, thirdId, secondId],
      );
      expect(
        controller.canvasPlacements.map((placement) => placement.priority),
        <int>[1, 2, 3],
      );

      controller.changeSelectedPriority(CanvasPriorityAction.increase);
      expect(
        controller.canvasPlacements.map((placement) => placement.placementId),
        <int>[firstId, secondId, thirdId],
      );
      expect(
        controller.canvasPlacements.map((placement) => placement.priority),
        <int>[1, 2, 3],
      );

      controller.changeSelectedPriority(CanvasPriorityAction.lowest);
      expect(
        controller.canvasPlacements.map((placement) => placement.placementId),
        <int>[thirdId, firstId, secondId],
      );
      controller.changeSelectedPriority(CanvasPriorityAction.highest);
      expect(
        controller.canvasPlacements.map((placement) => placement.placementId),
        <int>[firstId, secondId, thirdId],
      );
      expect(controller.selectedCanvasPlacement?.priorityLabel, '002');

      controller.selectCanvasPlacement(secondId);
      controller.changeSelectedPriority(CanvasPriorityAction.increase);
      expect(
        controller.canvasPlacements.map((placement) => placement.placementId),
        <int>[firstId, thirdId, secondId],
      );
      expect(
        controller.canvasPlacements.map((placement) => placement.priority),
        <int>[1, 2, 2],
      );

      controller.clearCanvas();
      controller.placeNodeOnCanvas(1, const Offset(.5, .5));
      expect(controller.canvasPlacements.single.priorityLabel, '001');
    },
  );

  test('all four canvas edges resize independently', () {
    final controller = SiteController(persistPresetState: false);
    addTearDown(controller.dispose);
    controller.debugLoadNodes(<MatrixNode>[_testNode(1)]);
    final placementId = controller.placeNodeOnCanvas(
      1,
      const Offset(.5, .5),
      snapToLayout: false,
    )!;

    var before = controller.canvasPlacements.single.frame;
    controller.resizeCanvasNode(
      placementId,
      CanvasResizeHandle.top,
      const Offset(0, .03),
    );
    var after = controller.canvasPlacements.single.frame;
    expect(after.top, closeTo(before.top + .03, .000001));
    expect(after.left, before.left);
    expect(after.right, before.right);
    expect(after.bottom, before.bottom);

    before = after;
    controller.resizeCanvasNode(
      placementId,
      CanvasResizeHandle.right,
      const Offset(.03, 0),
    );
    after = controller.canvasPlacements.single.frame;
    expect(after.right, closeTo(before.right + .03, .000001));
    expect(after.left, before.left);
    expect(after.top, before.top);
    expect(after.bottom, before.bottom);

    before = after;
    controller.resizeCanvasNode(
      placementId,
      CanvasResizeHandle.bottom,
      const Offset(0, .03),
    );
    after = controller.canvasPlacements.single.frame;
    expect(after.bottom, closeTo(before.bottom + .03, .000001));
    expect(after.left, before.left);
    expect(after.top, before.top);
    expect(after.right, before.right);

    before = after;
    controller.resizeCanvasNode(
      placementId,
      CanvasResizeHandle.left,
      const Offset(-.03, 0),
    );
    after = controller.canvasPlacements.single.frame;
    expect(after.left, closeTo(before.left - .03, .000001));
    expect(after.top, before.top);
    expect(after.right, before.right);
    expect(after.bottom, before.bottom);
  });

  testWidgets('canvas exposes priority controls and stays inside its bounds', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1200, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    controller.debugLoadNodes(<MatrixNode>[_testNode(1)]);
    final placementId = controller.placeNodeOnCanvas(
      1,
      const Offset(.5, .5),
      snapToLayout: false,
    )!;

    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(body: MatrixWall(controller: controller)),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.byKey(const ValueKey('priority-menu')), findsOneWidget);
    expect(find.byKey(const ValueKey('kvm-takeover-button')), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('kvm-takeover-button')));
    await tester.pumpAndSettle();
    expect(find.text('请先连接矩阵主节点'), findsOneWidget);
    expect(find.text('优先级'), findsOneWidget);
    expect(find.text('001'), findsOneWidget);
    expect(find.text('优先级 001'), findsOneWidget);
    expect(find.text('图层ID'), findsNothing);
    expect(find.text('图层 001'), findsNothing);
    for (final handle in CanvasResizeHandle.values) {
      expect(
        find.byKey(ValueKey('canvas-resize-${handle.name}-$placementId')),
        findsOneWidget,
      );
    }

    await tester.tap(find.byKey(const ValueKey('priority-menu')));
    await tester.pumpAndSettle();
    expect(find.text('上一层'), findsOneWidget);
    expect(find.text('下一层'), findsOneWidget);
    expect(find.text('置顶'), findsOneWidget);
    expect(find.text('置底'), findsOneWidget);

    await tester.tapAt(const Offset(10, 10));
    controller.toggleCanvasNodeMaximized(placementId);
    await tester.pumpAndSettle();
    final canvasRect = tester.getRect(
      find.byKey(const ValueKey('canvas-content')),
    );
    final nodeRect = tester.getRect(
      find.byKey(ValueKey('canvas-node-box-$placementId')),
    );
    final selectionRect = tester.getRect(
      find.byKey(ValueKey('canvas-selection-outline-$placementId')),
    );
    expect(nodeRect.left, greaterThanOrEqualTo(canvasRect.left));
    expect(nodeRect.top, greaterThanOrEqualTo(canvasRect.top));
    expect(nodeRect.right, lessThanOrEqualTo(canvasRect.right));
    expect(nodeRect.bottom, lessThanOrEqualTo(canvasRect.bottom));
    expect(selectionRect.left, greaterThan(nodeRect.left));
    expect(selectionRect.top, greaterThan(nodeRect.top));
    expect(selectionRect.right, lessThan(nodeRect.right));
    expect(selectionRect.bottom, lessThan(nodeRect.bottom));
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets(
    'selected outline stays above overlapping higher-priority nodes',
    (tester) async {
      tester.view.physicalSize = const Size(1200, 800);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
      );
      controller.debugLoadNodes(<MatrixNode>[_testNode(1), _testNode(2)]);
      final lowerPlacementId = controller.placeNodeOnCanvas(
        1,
        const Offset(.5, .5),
        snapToLayout: false,
      )!;
      final upperPlacementId = controller.placeNodeOnCanvas(
        2,
        const Offset(.5, .5),
        snapToLayout: false,
      )!;
      controller.selectCanvasPlacement(lowerPlacementId);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: Scaffold(body: MatrixWall(controller: controller)),
        ),
      );
      await tester.pumpAndSettle();

      final selection = find.byKey(
        ValueKey('canvas-selection-outline-$lowerPlacementId'),
      );
      final lowerNode = find.byKey(
        ValueKey('canvas-node-box-$lowerPlacementId'),
      );
      final upperNode = find.byKey(
        ValueKey('canvas-node-box-$upperPlacementId'),
      );
      expect(tester.getRect(lowerNode), tester.getRect(upperNode));
      expect(selection, findsOneWidget);
      expect(
        find.ancestor(
          of: selection,
          matching: find.byKey(ValueKey('canvas-node-$lowerPlacementId')),
        ),
        findsNothing,
      );
      expect(
        find.ancestor(
          of: selection,
          matching: find.byKey(ValueKey('canvas-node-$upperPlacementId')),
        ),
        findsNothing,
      );
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
    },
  );

  testWidgets('configured canvas node renders its live preview frame', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1200, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final previewBytes = File('assets/branding/app_logo.png').readAsBytesSync();
    final previewClient = MockClient((request) async {
      expect(request.url.path, '/kvm/preview.jpg');
      expect(request.url.queryParameters['stream'], '1');
      return http.Response.bytes(
        previewBytes,
        HttpStatus.ok,
        headers: const <String, String>{'content-type': 'image/jpeg'},
      );
    });
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    controller.debugLoadNodes(<MatrixNode>[
      MatrixNode(
        nodeId: 1,
        ip: InternetAddress.loopbackIPv4.address,
        resolution: '1920x1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: true,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        kvmEnabled: true,
        kvmRole: 'target',
        kvmPort: 9101,
        signalSourceType: 'capture',
        signalSourceConfigured: true,
        signalSourceActive: true,
        signalSourceWidth: 1920,
        signalSourceHeight: 1080,
      ),
    ]);
    controller.placeNodeOnCanvas(1, const Offset(.5, .5), snapToLayout: false);

    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: MatrixWall(
            controller: controller,
            previewClient: previewClient,
          ),
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));

    expect(
      find.byKey(const ValueKey('canvas-preview-frame-1')),
      findsOneWidget,
    );
    expect(
      find.byKey(const ValueKey('canvas-background-layer')),
      findsOneWidget,
    );
    expect(find.text('等待 HDMI 采集 画面'), findsNothing);
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets('each input window shows the complete source preview', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1200, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final previewBytes = File('assets/branding/app_logo.png').readAsBytesSync();
    final previewClient = MockClient(
      (request) async => http.Response.bytes(
        previewBytes,
        HttpStatus.ok,
        headers: const <String, String>{'content-type': 'image/jpeg'},
      ),
    );
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    controller.debugLoadNodes(const <MatrixNode>[
      MatrixNode(
        nodeId: 1,
        ip: '127.0.0.1',
        resolution: '1920x1080',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: true,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        kvmEnabled: true,
        kvmRole: 'target',
        kvmPort: 9101,
        signalSourceType: 'capture',
        signalSourceConfigured: true,
        signalSourceActive: true,
        signalSourceWidth: 1920,
        signalSourceHeight: 1080,
      ),
    ]);
    const positions = <Offset>[
      Offset(.25, .25),
      Offset(.75, .25),
      Offset(.25, .75),
      Offset(.75, .75),
    ];
    final placementIds = positions
        .map((position) => controller.placeNodeOnCanvas(1, position)!)
        .toList(growable: false);

    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: MatrixWall(
            controller: controller,
            previewClient: previewClient,
          ),
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));

    final nodeRect = tester.getRect(
      find.byKey(ValueKey('canvas-node-box-${placementIds.first}')),
    );
    for (final placementId in placementIds) {
      final placement = find.byKey(ValueKey('canvas-node-$placementId'));
      final preview = find.descendant(
        of: placement,
        matching: find.byType(LiveJpegPreview),
      );
      expect(preview, findsOneWidget);
      final previewRect = tester.getRect(preview);
      expect(previewRect.width, closeTo(nodeRect.width, 1));
      expect(previewRect.height, closeTo(nodeRect.height, 1));
    }
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets('configured node card renders its live capture thumbnail', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 620);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final previewBytes = File('assets/branding/app_logo.png').readAsBytesSync();
    final previewClient = MockClient((request) async {
      expect(request.url.path, '/kvm/preview.jpg');
      return http.Response.bytes(
        previewBytes,
        HttpStatus.ok,
        headers: const <String, String>{'content-type': 'image/jpeg'},
      );
    });
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    controller.debugLoadNodes(<MatrixNode>[
      MatrixNode(
        nodeId: 1,
        ip: InternetAddress.loopbackIPv4.address,
        resolution: '1280x720',
        status: 'online',
        playState: 'idle',
        cpu: 0,
        memory: 0,
        masterClockSynchronized: true,
        masterClockOffsetMs: 0,
        lastSeen: 1,
        kvmEnabled: true,
        kvmRole: 'target',
        kvmPort: 9101,
        signalSourceType: 'capture',
        signalSourceConfigured: true,
        signalSourceActive: true,
        signalSourceWidth: 1920,
        signalSourceHeight: 1080,
      ),
    ]);

    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: NodePanel(controller: controller, previewClient: previewClient),
        ),
      ),
    );
    await tester.pump(const Duration(milliseconds: 450));

    expect(find.byKey(const ValueKey('node-preview-1')), findsOneWidget);
    expect(find.text('节点 1 · 1920x1080'), findsOneWidget);
    expect(tester.takeException(), isNull);

    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets('fullscreen KVM page connects and releases exactly once', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1280, 720);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final channel = _FakeKvmChannel();
    var releases = 0;

    await tester.pumpWidget(
      MaterialApp(
        home: KvmTakeoverPage(
          session: _testKvmSession,
          connector: (_) async => channel,
          onRelease: (sessionId) async {
            expect(sessionId, 9);
            releases++;
          },
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.textContaining('192.168.2.101 · 001'), findsOneWidget);
    expect(find.byKey(const ValueKey('kvm-live-preview')), findsOneWidget);
    expect(find.text('Offline'), findsNothing);
    expect(find.textContaining('HID 已接管'), findsOneWidget);
    expect(tester.takeException(), isNull);

    await tester.tap(find.byKey(const ValueKey('exit-kvm-button')));
    await tester.pumpAndSettle();
    expect(channel.closeCount, 1);
    expect(releases, 1);
  });

  testWidgets('KVM takeover page fits Android portrait viewport', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(412, 915);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final channel = _FakeKvmChannel();

    await tester.pumpWidget(
      MaterialApp(
        home: KvmTakeoverPage(
          session: _testKvmSession,
          connector: (_) async => channel,
          onRelease: (_) async {},
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('左键'), findsOneWidget);
    expect(find.text('右键'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await tester.tap(find.byKey(const ValueKey('exit-kvm-button')));
    await tester.pumpAndSettle();
  });

  test('layout presets can be deleted without changing the active layout', () {
    final controller = SiteController(persistPresetState: false);
    addTearDown(controller.dispose);

    final preset = controller.presets.firstWhere((item) => item.name == '九宫格');
    controller.removePreset(preset);

    expect(controller.presets.any((item) => item.name == '九宫格'), isFalse);
    expect(controller.layout.rows, 2);
    expect(controller.layout.cols, 2);
  });

  test('custom layout presets are saved locally and updated by name', () async {
    final directory = await Directory.systemTemp.createTemp(
      'distributed-playback-presets-',
    );
    final stateFile = File('${directory.path}/layout_presets.json');
    final controller = SiteController(
      presetStateFile: stateFile,
      refreshOnStart: false,
    );
    addTearDown(() => directory.delete(recursive: true));
    addTearDown(controller.dispose);

    await controller.saveLayoutPreset(
      '主舞台布局',
      const MatrixLayout(
        rows: 2,
        cols: 3,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
    );
    await controller.saveLayoutPreset(
      '主舞台布局',
      const MatrixLayout(
        rows: 3,
        cols: 3,
        screenWidth: 1280,
        screenHeight: 720,
      ),
    );

    final matching = controller.presets.where(
      (preset) => preset.name == '主舞台布局',
    );
    expect(matching, hasLength(1));
    expect(matching.single.layout.rows, 3);
    expect(matching.single.layout.cols, 3);

    final stored = jsonDecode(await stateFile.readAsString());
    expect(stored['customPresets'], isA<List<dynamic>>());
    expect(stored['customPresets'], contains(containsPair('name', '主舞台布局')));
  });

  test(
    'saved presets restore duplicate node windows and their frames',
    () async {
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
      );
      addTearDown(controller.dispose);
      controller.debugLoadNodes(const <MatrixNode>[
        MatrixNode(
          nodeId: 1,
          ip: '192.168.2.101',
          resolution: '1920×1080',
          status: 'online',
          playState: 'idle',
          cpu: 0,
          memory: 0,
          masterClockSynchronized: true,
          masterClockOffsetMs: 0,
          lastSeen: 1,
        ),
      ]);
      controller.placeNodeOnCanvas(1, const Offset(.25, .25));
      controller.placeNodeOnCanvas(1, const Offset(.75, .75));
      controller.changeSelectedPriority(CanvasPriorityAction.lowest);
      final expectedFrames = controller.canvasPlacements
          .map((placement) => placement.frame)
          .toList(growable: false);
      final expectedPriorities = controller.canvasPlacements
          .map((placement) => placement.priorityLabel)
          .toList(growable: false);
      expect(expectedPriorities, <String>['001', '001']);

      await controller.saveLayoutPreset('双窗口预设', controller.layout);
      expect(controller.activePresetName, '双窗口预设');
      final preset = controller.presets.firstWhere(
        (item) => item.name == '双窗口预设',
      );
      controller.clearCanvas();
      expect(controller.activePresetName, isNull);
      await controller.applyPreset(preset);

      expect(controller.activePresetName, '双窗口预设');
      expect(controller.placementCountForNode(1), 2);
      expect(
        controller.canvasPlacements.map((placement) => placement.frame),
        expectedFrames,
      );
      expect(
        controller.canvasPlacements.map((placement) => placement.priorityLabel),
        expectedPriorities,
      );
      controller.moveCanvasNode(
        controller.canvasPlacements.first.placementId,
        const Offset(.01, 0),
      );
      expect(controller.activePresetName, isNull);
    },
  );

  testWidgets('save preset dialog defaults the name to preset one', (
    tester,
  ) async {
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: Scaffold(
          body: Builder(
            builder: (context) => TextButton(
              onPressed: () => showSavePresetDialog(context, controller),
              child: const Text('打开保存预设'),
            ),
          ),
        ),
      ),
    );

    await tester.tap(find.text('打开保存预设'));
    await tester.pumpAndSettle();

    final nameField = tester.widget<TextField>(
      find.byKey(const ValueKey('current-layout-preset-name')),
    );
    expect(nameField.controller?.text, '预设1');

    await tester.tap(find.text('取消'));
    await tester.pumpAndSettle();
    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets('saving a custom layout adds it to the preset panel', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(900, 760);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final controller = SiteController(
      persistPresetState: false,
      refreshOnStart: false,
    );

    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        home: ListenableBuilder(
          listenable: controller,
          builder: (context, _) => Scaffold(
            body: Column(
              children: <Widget>[
                Builder(
                  builder: (context) => TextButton(
                    onPressed: () => showLayoutDialog(context, controller),
                    child: const Text('打开自定义布局'),
                  ),
                ),
                Expanded(child: ScenePanel(controller: controller)),
              ],
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('打开自定义布局'));
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const ValueKey('layout-preset-name')),
      '会议室预设',
    );
    await tester.enterText(find.byKey(const ValueKey('layout-rows')), '2');
    await tester.enterText(find.byKey(const ValueKey('layout-cols')), '3');
    await tester.tap(find.byKey(const ValueKey('save-layout-preset')));
    await tester.pumpAndSettle();

    expect(find.text('会议室预设'), findsOneWidget);
    expect(find.text('当前启用'), findsOneWidget);
    expect(controller.presets.first.name, '会议室预设');
    expect(controller.layout.rows, 2);
    expect(controller.layout.cols, 3);
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    controller.dispose();
  });

  testWidgets(
    'control deck uses live signal progress and restores muted volume',
    (tester) async {
      tester.view.physicalSize = const Size(1000, 360);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      var serverVolume = 65;
      final volumeWrites = <int>[];
      final client = MockClient((request) async {
        if (request.method == 'PUT' &&
            request.url.path == MatrixApiSpec.audioVolume) {
          serverVolume =
              (jsonDecode(request.body)
                      as Map<String, dynamic>)['volumePercent']
                  as int;
          volumeWrites.add(serverVolume);
          return http.Response(
            jsonEncode(<String, dynamic>{'volumePercent': serverVolume}),
            200,
          );
        }
        final body = switch (request.url.path) {
          MatrixApiSpec.nodes => <String, dynamic>{
            'nodes': <Map<String, dynamic>>[
              <String, dynamic>{
                'nodeId': 1,
                'ip': '192.168.2.101',
                'resolution': '1920x1080',
                'status': 'online',
                'playState': 'idle',
                'cpu': 0,
                'memory': 0,
                'masterClockSynchronized': true,
                'masterClockOffsetMs': 0,
                'lastSeen': 1,
                'signalSourceType': 'capture',
                'signalSourceConfigured': true,
                'signalSourceActive': true,
              },
            ],
          },
          MatrixApiSpec.regions => <String, dynamic>{'regions': <dynamic>[]},
          MatrixApiSpec.screens => <String, dynamic>{
            'layout': <String, dynamic>{
              'rows': 1,
              'cols': 1,
              'screenWidth': 1920,
              'screenHeight': 1080,
            },
            'screens': <dynamic>[],
          },
          MatrixApiSpec.status => <String, dynamic>{
            'running': false,
            'state': 'idle',
            'videoUrl': '',
            'syncTimestamp': 0,
            'updatedAt': 0,
            'http': <String, dynamic>{
              'activeConnections': 0,
              'totalRequests': 0,
              'totalBytesSent': 0,
            },
          },
          MatrixApiSpec.audioOutput => <String, dynamic>{'mode': 'both'},
          MatrixApiSpec.audioVolume => <String, dynamic>{
            'volumePercent': serverVolume,
          },
          _ => throw StateError('Unexpected request: ${request.url.path}'),
        };
        return http.Response(jsonEncode(body), 200);
      });
      final controller = SiteController(
        persistPresetState: false,
        refreshOnStart: false,
        apiClient: MatrixApiClient('http://127.0.0.1:8080', client: client),
      );
      await controller.refresh(silent: true);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(),
          home: ListenableBuilder(
            listenable: controller,
            builder: (context, _) =>
                Scaffold(body: ControlDeck(controller: controller)),
          ),
        ),
      );

      expect(find.text('尚未选择媒体'), findsNothing);
      expect(find.text('选择媒体'), findsNothing);
      expect(find.byKey(const ValueKey('volume-mute-button')), findsOneWidget);
      expect(find.byKey(const ValueKey('volume-down-button')), findsOneWidget);
      expect(find.byKey(const ValueKey('volume-slider')), findsOneWidget);
      expect(find.byKey(const ValueKey('volume-up-button')), findsOneWidget);
      expect(find.byKey(const ValueKey('live-progress')), findsOneWidget);
      expect(find.text('LIVE'), findsOneWidget);
      expect(
        tester
            .widget<LinearProgressIndicator>(
              find.byKey(const ValueKey('live-progress')),
            )
            .value,
        1,
      );

      await tester.tap(find.byKey(const ValueKey('volume-mute-button')));
      await tester.pump(const Duration(milliseconds: 20));
      expect(volumeWrites, <int>[0]);
      expect(controller.isMuted, isTrue);

      await tester.tap(find.byKey(const ValueKey('volume-mute-button')));
      await tester.pump(const Duration(milliseconds: 20));
      expect(volumeWrites, <int>[0, 65]);
      expect(controller.audioVolumePercent, 65);
      expect(tester.takeException(), isNull);

      await tester.pumpWidget(const SizedBox.shrink());
      controller.dispose();
    },
  );
}

MatrixNode _testNode(int nodeId) => MatrixNode(
  nodeId: nodeId,
  ip: '192.168.2.${100 + nodeId}',
  resolution: '1920×1080',
  status: 'online',
  playState: 'idle',
  cpu: 0,
  memory: 0,
  masterClockSynchronized: true,
  masterClockOffsetMs: 0,
  lastSeen: 1,
);

const KvmSession _testKvmSession = KvmSession(
  sessionId: 9,
  targetNodeId: 1,
  targetNodeLabel: '001',
  state: 'acquired',
  acquiredAt: 1,
  expiresAt: 2,
  targetIp: '192.168.2.101',
  targetPort: 9101,
  sessionToken: 99,
);

class _FakeKvmChannel implements KvmChannel {
  final Completer<void> _done = Completer<void>();
  var closeCount = 0;

  @override
  bool get isConnected => closeCount == 0;

  @override
  Future<void> get done => _done.future;

  @override
  Object? get lastError => null;

  @override
  Future<void> close() async {
    if (closeCount == 0) {
      closeCount++;
      if (!_done.isCompleted) _done.complete();
    }
  }

  @override
  Future<void> handleKeyEvent(KeyEvent event) async {}

  @override
  Future<void> sendMouse({
    required int buttons,
    int deltaX = 0,
    int deltaY = 0,
    int wheel = 0,
  }) async {}
}
