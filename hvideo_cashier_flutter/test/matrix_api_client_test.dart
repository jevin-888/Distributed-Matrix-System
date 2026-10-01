import 'dart:convert';
import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';

import 'package:distributed_playback_system/src/api/matrix_api_client.dart';
import 'package:distributed_playback_system/src/models/matrix_models.dart';

void main() {
  test(
    'client exposes every backend API operation with the exact method',
    () async {
      final requests = <String>[];
      final client = MockClient((request) async {
        requests.add('${request.method} ${request.url.path}');
        final response = switch ('${request.method} ${request.url.path}') {
          'GET /api/nodes' => <String, dynamic>{'nodes': <dynamic>[]},
          'PUT /api/node-network' => <String, dynamic>{
            'success': true,
            'reconnectRequired': true,
          },
          'POST /api/nodes/discover' => <String, dynamic>{
            'success': true,
            'networks': <String>['192.168.2.0/24'],
            'addressesProbed': 254,
            'nodesFound': 1,
            'totalNodes': 1,
            'elapsedMs': 1500,
          },
          'GET /api/regions' => <String, dynamic>{
            'regions': <dynamic>[
              <String, dynamic>{
                'name': '主舞台',
                'inputNodeIds': <dynamic>[],
                'layout': <String, dynamic>{
                  'rows': 1,
                  'cols': 1,
                  'screenWidth': 1920,
                  'screenHeight': 1080,
                },
                'placements': <Map<String, dynamic>>[
                  <String, dynamic>{
                    'nodeId': 1,
                    'x': 0,
                    'y': 0,
                    'width': 1,
                    'height': 1,
                  },
                ],
                'savedAt': 1,
              },
            ],
          },
          'PUT /api/regions' => jsonDecode(request.body),
          'GET /api/screens' => <String, dynamic>{
            'layout': <String, dynamic>{
              'rows': 1,
              'cols': 1,
              'screenWidth': 1920,
              'screenHeight': 1080,
            },
            'screens': <dynamic>[],
          },
          'PUT /api/screens' => <String, dynamic>{'success': true},
          'GET /api/status' => <String, dynamic>{
            'running': true,
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
          'GET /api/audio-output' => <String, dynamic>{'mode': 'both'},
          'PUT /api/audio-output' => jsonDecode(request.body),
          'GET /api/audio-volume' => <String, dynamic>{'volumePercent': 100},
          'PUT /api/audio-volume' => jsonDecode(request.body),
          'GET /api/kvm/status' => <String, dynamic>{
            'sessionId': 0,
            'controllerNodeId': 0,
            'controllerNodeLabel': '',
            'targetNodeId': 0,
            'targetNodeLabel': '',
            'state': 'idle',
            'acquiredAt': 0,
            'expiresAt': 0,
          },
          'POST /api/kvm/acquire' => <String, dynamic>{
            'sessionId': 7,
            'controllerNodeId': 0,
            'controllerNodeLabel': '',
            'targetNodeId': 1,
            'targetNodeLabel': '001',
            'state': 'acquired',
            'acquiredAt': 10,
            'expiresAt': 20,
            'targetIp': '192.168.2.101',
            'targetPort': 9101,
            'sessionToken': 123456789,
          },
          'POST /api/kvm/release' => <String, dynamic>{'success': true},
          'POST /api/play' ||
          'POST /api/pause' ||
          'POST /api/resume' ||
          'POST /api/stop' ||
          'POST /api/preload' => <String, dynamic>{'success': true},
          _ => throw StateError(
            'Unexpected request: ${request.method} ${request.url}',
          ),
        };
        return http.Response(
          jsonEncode(response),
          200,
          headers: const <String, String>{'content-type': 'application/json'},
        );
      });
      final api = MatrixApiClient('http://192.168.2.101:8080', client: client);
      addTearDown(api.close);

      await api.getNodes();
      await api.setNodeNetwork(
        nodeId: 1,
        mode: 'manual',
        address: '192.168.2.103',
        prefixLength: 24,
        gateway: '192.168.2.1',
        dnsServers: const <String>['223.5.5.5', '114.114.114.114'],
      );
      final discovery = await api.discoverNodes();
      expect(discovery.addressesProbed, 254);
      expect(discovery.nodesFound, 1);
      final regions = await api.getRegions();
      expect(regions.single.name, '主舞台');
      expect(regions.single.outputNodeIds, <int>{1});
      expect(regions.single.layout.rows, 1);
      expect(regions.single.placements.single.nodeId, 1);
      await api.setRegions(const <MatrixRegion>[]);
      await api.getScreens();
      await api.setScreens(
        const MatrixLayout(
          rows: 1,
          cols: 1,
          screenWidth: 1920,
          screenHeight: 1080,
        ),
      );
      await api.getStatus();
      expect(await api.getAudioOutput(), AudioOutputMode.both);
      expect(
        await api.setAudioOutput(AudioOutputMode.hdmi),
        AudioOutputMode.hdmi,
      );
      expect(await api.getAudioVolumePercent(), 100);
      expect(await api.setAudioVolumePercent(72), 72);
      expect((await api.getKvmStatus()).state, 'idle');
      final kvm = await api.acquireKvm(targetNodeId: 1);
      expect(kvm.targetNodeLabel, '001');
      expect(kvm.targetPort, 9101);
      expect(kvm.sessionToken, 123456789);
      await api.releaseKvm(kvm.sessionId);
      await api.play(
        videoUrl: '/tmp/demo.mp4',
        video: const VideoDescriptor(width: 1920, height: 1080),
      );
      await api.pause();
      await api.resume();
      await api.stop();
      await api.preload('/tmp/demo.mp4');

      expect(requests, const <String>[
        'GET /api/nodes',
        'PUT /api/node-network',
        'POST /api/nodes/discover',
        'GET /api/regions',
        'PUT /api/regions',
        'GET /api/screens',
        'PUT /api/screens',
        'GET /api/status',
        'GET /api/audio-output',
        'PUT /api/audio-output',
        'GET /api/audio-volume',
        'PUT /api/audio-volume',
        'GET /api/kvm/status',
        'POST /api/kvm/acquire',
        'POST /api/kvm/release',
        'POST /api/play',
        'POST /api/pause',
        'POST /api/resume',
        'POST /api/stop',
        'POST /api/preload',
      ]);
    },
  );

  test(
    'node network request body matches the backend schema exactly',
    () async {
      late Map<String, dynamic> body;
      final client = MockClient((request) async {
        expect(request.method, 'PUT');
        expect(request.url.path, MatrixApiSpec.nodeNetwork);
        body = jsonDecode(request.body) as Map<String, dynamic>;
        return http.Response('{"success":true,"reconnectRequired":true}', 200);
      });
      final api = MatrixApiClient('http://192.168.2.101:8080', client: client);
      addTearDown(api.close);

      await api.setNodeNetwork(
        nodeId: 7,
        mode: 'auto',
        address: '',
        prefixLength: 0,
        gateway: '',
        dnsServers: const <String>[],
      );

      expect(body, <String, dynamic>{
        'nodeId': 7,
        'mode': 'auto',
        'address': '',
        'prefixLength': 0,
        'gateway': '',
        'dnsServers': <dynamic>[],
      });
    },
  );

  test('KVM acquire and release bodies match the API schema exactly', () async {
    final bodies = <String, Map<String, dynamic>>{};
    final client = MockClient((request) async {
      bodies[request.url.path] =
          jsonDecode(request.body) as Map<String, dynamic>;
      if (request.url.path == MatrixApiSpec.kvmAcquire) {
        return http.Response(
          jsonEncode(<String, dynamic>{
            'sessionId': 1,
            'targetNodeId': 1,
            'targetNodeLabel': '001',
            'state': 'acquired',
            'acquiredAt': 1,
            'expiresAt': 2,
            'targetIp': '192.168.2.101',
            'targetPort': 9101,
            'sessionToken': 99,
          }),
          200,
        );
      }
      return http.Response('{"success":true}', 200);
    });
    final api = MatrixApiClient('http://127.0.0.1:8080', client: client);
    addTearDown(api.close);

    await api.acquireKvm(targetNodeId: 1);
    await api.releaseKvm(1);

    expect(bodies[MatrixApiSpec.kvmAcquire], <String, dynamic>{
      'targetNodeId': 1,
      'leaseMs': 300000,
    });
    expect(bodies[MatrixApiSpec.kvmRelease], <String, dynamic>{'sessionId': 1});
  });

  test('device discovery sends exactly an empty JSON object', () async {
    String? body;
    final client = MockClient((request) async {
      body = request.body;
      return http.Response(
        '{"success":true,"networks":[],"addressesProbed":0,"nodesFound":0,"totalNodes":0,"elapsedMs":1}',
        200,
      );
    });
    final api = MatrixApiClient('http://127.0.0.1:8080', client: client);
    addTearDown(api.close);

    await api.discoverNodes();

    expect(jsonDecode(body!), <String, dynamic>{});
  });

  test('KVM acquire rejects a response without direct transport fields', () {
    expect(
      () => KvmSession.fromJson(<String, dynamic>{
        'sessionId': 1,
        'targetNodeId': 1,
        'targetNodeLabel': '001',
        'state': 'acquired',
        'acquiredAt': 1,
        'expiresAt': 2,
      }),
      throwsFormatException,
    );
  });

  test('region request carries its physical output-wall layout', () async {
    late Map<String, dynamic> body;
    final client = MockClient((request) async {
      body = jsonDecode(request.body) as Map<String, dynamic>;
      return http.Response.bytes(utf8.encode(request.body), 200);
    });
    final api = MatrixApiClient('http://127.0.0.1:8080', client: client);
    addTearDown(api.close);

    await api.setRegions(const <MatrixRegion>[
      MatrixRegion(
        name: '主舞台',
        savedAt: 1,
        inputNodeIds: <int>[2, 3],
        layout: MatrixLayout(
          rows: 1,
          cols: 2,
          screenWidth: 1920,
          screenHeight: 1080,
        ),
        placements: <MatrixScreenPlacement>[
          MatrixScreenPlacement(nodeId: 7, frame: Rect.fromLTWH(0, 0, .5, 1)),
          MatrixScreenPlacement(nodeId: 8, frame: Rect.fromLTWH(.5, 0, .5, 1)),
        ],
      ),
    ]);

    final region = (body['regions'] as List).single as Map<String, dynamic>;
    expect(region.keys.toSet(), <String>{
      'name',
      'inputNodeIds',
      'layout',
      'placements',
      'savedAt',
    });
    expect(region['layout'], <String, dynamic>{
      'rows': 1,
      'cols': 2,
      'screenWidth': 1920,
      'screenHeight': 1080,
    });
    expect(region['inputNodeIds'], <int>[2, 3]);
    expect((region['placements'] as List), hasLength(2));
  });

  test('screen layout request carries normalized node placements', () async {
    late Map<String, dynamic> body;
    final client = MockClient((request) async {
      body = jsonDecode(request.body) as Map<String, dynamic>;
      return http.Response('{"success":true}', 200);
    });
    final api = MatrixApiClient('http://127.0.0.1:8080', client: client);
    addTearDown(api.close);

    await api.setScreens(
      const MatrixLayout(
        rows: 2,
        cols: 2,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
      placements: const <MatrixScreenPlacement>[
        MatrixScreenPlacement(nodeId: 7, frame: Rect.fromLTWH(.5, 0, .5, .5)),
      ],
    );

    expect(body['rows'], 2);
    expect(body['placements'], <dynamic>[
      <String, dynamic>{
        'nodeId': 7,
        'x': .5,
        'y': 0.0,
        'width': .5,
        'height': .5,
      },
    ]);
  });

  test('screen snapshot restores explicit placement coordinates', () {
    final snapshot = ScreenSnapshot.fromJson(<String, dynamic>{
      'layout': <String, dynamic>{
        'rows': 2,
        'cols': 2,
        'screenWidth': 1920,
        'screenHeight': 1080,
      },
      'screens': <dynamic>[],
      'placements': <dynamic>[
        <String, dynamic>{
          'nodeId': 7,
          'x': .5,
          'y': 0,
          'width': .5,
          'height': .5,
        },
      ],
    });

    expect(snapshot.placements, hasLength(1));
    expect(snapshot.placements!.single.nodeId, 7);
    expect(
      snapshot.placements!.single.frame,
      const Rect.fromLTWH(.5, 0, .5, .5),
    );
  });
}
