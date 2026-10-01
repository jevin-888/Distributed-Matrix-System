import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:distributed_playback_system/src/api/device_discovery.dart';

void main() {
  test('uses the real prefix including /23 and point-to-point networks', () {
    final network = DiscoveryNetwork('192.168.3.7', 23);
    expect(network.hosts.first, '192.168.2.1');
    expect(network.hosts.last, '192.168.3.254');
    expect(network.hostCount, 510);
    expect(network.broadcast, '192.168.3.255');
    expect(DiscoveryNetwork('10.0.0.0', 31).hosts, ['10.0.0.0', '10.0.0.1']);
    expect(DiscoveryNetwork('10.0.0.1', 32).hosts, ['10.0.0.1']);
    expect(() => DiscoveryNetwork('10.0.0.1', 0), throwsFormatException);
  });

  test(
    'scans every interface, deduplicates, and keeps the custom port',
    () async {
      final probed = <Uri>[];
      final discovery = DeviceDiscovery(
        useUdp: false,
        loadNetworks: () async => [
          DiscoveryNetwork('10.1.0.1', 30),
          DiscoveryNetwork('10.1.0.2', 30),
          DiscoveryNetwork('10.2.0.1', 30),
        ],
        probe: (uri) async {
          probed.add(uri);
          return uri.host == '10.2.0.2' && uri.port == 8181;
        },
      );
      addTearDown(discovery.dispose);
      await discovery.search('http://10.1.0.1:8181');
      expect(probed.toSet().length, probed.length);
      expect(probed.length, 8);
      expect(
        discovery.devices.single.serverUri.toString(),
        'http://10.2.0.2:8181',
      );
      expect(discovery.checked, discovery.total);
      expect(discovery.searching, isFalse);
    },
  );

  test('bounds concurrency and ignores results after cancellation', () async {
    final gate = Completer<bool>();
    var calls = 0;
    final discovery = DeviceDiscovery(
      useUdp: false,
      loadNetworks: () async => [DiscoveryNetwork('10.0.0.1', 24)],
      probe: (_) {
        calls++;
        return gate.future;
      },
    );
    addTearDown(discovery.dispose);
    final scan = discovery.search('http://10.0.0.1:8080');
    await Future<void>.delayed(Duration.zero);
    expect(calls, 64);
    discovery.cancel();
    gate.complete(true);
    await scan;
    expect(discovery.devices, isEmpty);
    expect(discovery.searching, isFalse);
    expect(calls, 64);
  });

  test('new searches clear stale devices and supersede old results', () async {
    final stale = Completer<bool>();
    final discovery = DeviceDiscovery(
      useUdp: false,
      loadNetworks: () async => [],
      probe: (uri) =>
          uri.host == '10.0.0.1' ? stale.future : Future.value(true),
    );
    addTearDown(discovery.dispose);
    final first = discovery.search('http://10.0.0.1:8080');
    await Future<void>.delayed(Duration.zero);
    await discovery.search('http://10.0.0.2:8080');
    stale.complete(true);
    await first;
    expect(discovery.devices.map((d) => d.address), ['10.0.0.2']);
  });

  test(
    'validates the REST contract and rejects unrelated HTTP servers',
    () async {
      final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
      addTearDown(() => server.close(force: true));
      var valid = true;
      final paths = <String>[];
      server.listen((request) async {
        paths.add(request.uri.path);
        request.response.headers.contentType = ContentType.json;
        request.response.write(
          jsonEncode(
            !valid
                ? {'running': true}
                : request.uri.path == '/api/nodes'
                ? {'nodes': []}
                : {
                    'running': true,
                    'state': 'idle',
                    'videoUrl': '',
                    'syncTimestamp': 0,
                    'updatedAt': 0,
                    'http': {
                      'activeConnections': 0,
                      'totalRequests': 0,
                      'totalBytesSent': 0,
                    },
                  },
          ),
        );
        await request.response.close();
      });
      final discovery = DeviceDiscovery(
        useUdp: false,
        loadNetworks: () async => [],
      );
      addTearDown(discovery.dispose);
      await discovery.search('http://127.0.0.1:${server.port}');
      expect(discovery.devices.single.canConnect, isTrue);
      expect(paths, ['/api/status', '/api/nodes']);
      valid = false;
      await discovery.search('http://127.0.0.1:${server.port}');
      expect(discovery.devices, isEmpty);
    },
  );

  test(
    'UDP request matches server protocol and uses reply source IP',
    () async {
      final socket = await RawDatagramSocket.bind(
        InternetAddress.loopbackIPv4,
        0,
      );
      addTearDown(socket.close);
      final requests = <Map<String, dynamic>>[];
      socket.listen((event) {
        if (event != RawSocketEvent.read) return;
        Datagram? packet;
        while ((packet = socket.receive()) != null) {
          requests.add(
            jsonDecode(utf8.decode(packet!.data)) as Map<String, dynamic>,
          );
          socket.send(
            utf8.encode(
              jsonEncode({
                'type': 'heartbeat',
                'nodeId': 1,
                'status': 'online',
                'ip': '192.168.99.99',
                'deviceName': '现场设备',
              }),
            ),
            packet.address,
            packet.port,
          );
        }
      });
      final discovery = DeviceDiscovery(
        discoveryPort: socket.port,
        loadNetworks: () async => [DiscoveryNetwork('127.0.0.1', 32)],
        probe: (_) async => false,
      );
      addTearDown(discovery.dispose);
      await discovery.search('http://127.0.0.1:8080');
      expect(requests, isNotEmpty);
      expect(requests.first.keys.toSet(), {'type', 'version', 'requestId'});
      expect(requests.first['type'], 'discovery_request');
      expect(requests.first['version'], 1);
      expect(requests.first['requestId'], greaterThan(0));
      expect(discovery.devices.single.address, '127.0.0.1');
      expect(discovery.devices.single.name, '现场设备');
      expect(discovery.devices.single.canConnect, isFalse);
    },
  );

  test(
    'a stalled HTTP endpoint times out without blocking the search',
    () async {
      final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
      server.listen((_) {});
      addTearDown(() => server.close(force: true));
      final discovery = DeviceDiscovery(
        useUdp: false,
        loadNetworks: () async => [],
        timeout: const Duration(milliseconds: 50),
      );
      addTearDown(discovery.dispose);
      await discovery.search('http://127.0.0.1:${server.port}');
      expect(discovery.searching, isFalse);
      expect(discovery.devices, isEmpty);
    },
  );
}
