import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'matrix_api_client.dart';

class DiscoveryNetwork {
  DiscoveryNetwork(this.address, this.prefixLength) {
    final ip = InternetAddress.tryParse(address);
    if (ip == null ||
        ip.type != InternetAddressType.IPv4 ||
        prefixLength < 1 ||
        prefixLength > 32) {
      throw const FormatException('无效的 IPv4 网络');
    }
  }

  final String address;
  final int prefixLength;
  int get _ip =>
      InternetAddress(address).rawAddress.fold(0, (a, b) => (a << 8) | b);
  int get _mask => (0xffffffff << (32 - prefixLength)) & 0xffffffff;
  int get _first => _ip & _mask;
  int get _last => _first | (0xffffffff ^ _mask);
  int get hostCount =>
      prefixLength >= 31 ? _last - _first + 1 : _last - _first - 1;
  String get broadcast => _format(_last);

  Iterable<String> get hosts sync* {
    final first = prefixLength >= 31 ? _first : _first + 1;
    final last = prefixLength >= 31 ? _last : _last - 1;
    for (var value = first; value <= last; value++) {
      yield _format(value);
    }
  }

  static String _format(int value) =>
      [24, 16, 8, 0].map((shift) => (value >> shift) & 255).join('.');
}

class DiscoveredDevice {
  const DiscoveredDevice({
    required this.address,
    required this.name,
    this.serverUri,
  });

  final String address;
  final String name;
  final Uri? serverUri;
  bool get canConnect => serverUri != null;
}

/// Finds devices before a Master connection exists. Uses the existing UDP v1
/// discovery request and validates the existing REST APIs, without API aliases.
class DeviceDiscovery extends ChangeNotifier {
  DeviceDiscovery({
    Future<List<DiscoveryNetwork>> Function()? loadNetworks,
    Future<bool> Function(Uri)? probe,
    this.useUdp = true,
    this.discoveryPort = 9003,
    this.timeout = const Duration(milliseconds: 1200),
  }) : _loadNetworks = loadNetworks ?? _platformNetworks,
       _probeOverride = probe;

  final Future<List<DiscoveryNetwork>> Function() _loadNetworks;
  final Future<bool> Function(Uri)? _probeOverride;
  final bool useUdp;
  final int discoveryPort;
  final Duration timeout;
  final Map<String, DiscoveredDevice> _devices = {};
  final Set<MatrixApiClient> _clients = {};
  final List<RawDatagramSocket> _sockets = [];
  int _generation = 0;
  bool _disposed = false;
  bool searching = false;
  int checked = 0;
  int total = 0;
  String? error;
  String? notice;

  List<DiscoveredDevice> get devices => _devices.values.toList()
    ..sort((a, b) {
      if (a.canConnect != b.canConnect) return a.canConnect ? -1 : 1;
      return a.address.compareTo(b.address);
    });

  static Future<List<DiscoveryNetwork>> _platformNetworks() async {
    const channel = MethodChannel(
      'distributed_playback_system/network_discovery',
    );
    final values = await channel.invokeListMethod<dynamic>('ipv4Networks');
    return (values ?? [])
        .map(
          (value) => DiscoveryNetwork(
            value['address'] as String,
            value['prefixLength'] as int,
          ),
        )
        .toList();
  }

  bool _active(int generation) => !_disposed && generation == _generation;
  void _changed() {
    if (!_disposed) notifyListeners();
  }

  Future<bool> _probe(Uri uri) async {
    if (_probeOverride != null) return _probeOverride(uri);
    final api = MatrixApiClient(uri.toString());
    _clients.add(api);
    try {
      return await (() async {
        final status = await api.getStatus();
        if (!status.running) return false;
        await api.getNodes();
        return true;
      })().timeout(timeout);
    } catch (_) {
      return false;
    } finally {
      api.close();
      _clients.remove(api);
    }
  }

  Future<void> search(String currentAddress) async {
    cancel();
    final generation = _generation;
    searching = true;
    checked = 0;
    total = 0;
    error = null;
    notice = null;
    _devices.clear();
    _changed();
    final targets = <Uri>[];
    final queued = <String>{};
    final seed = Uri.tryParse(
      currentAddress.contains('://')
          ? currentAddress
          : 'http://$currentAddress',
    );
    final ports = <int>{8080, if (seed != null && seed.hasPort) seed.port};
    void enqueue(Uri uri) {
      if (queued.add(uri.toString())) {
        targets.add(uri);
        total = targets.length;
      }
    }

    Future<void> check(Uri uri) async {
      try {
        if (!await _probe(uri) || !_active(generation)) return;
        final old = _devices[uri.host];
        _devices[uri.host] = DiscoveredDevice(
          address: uri.host,
          name: old?.name ?? '矩阵设备',
          serverUri: old?.serverUri ?? uri,
        );
        _changed();
      } catch (_) {
        // A single offline or malformed endpoint does not abort the scan.
      }
    }

    try {
      if (seed != null && seed.host.isNotEmpty) {
        enqueue(
          Uri(
            scheme: seed.scheme,
            host: seed.host,
            port: seed.hasPort ? seed.port : null,
          ),
        );
      }
      List<DiscoveryNetwork> networks = [];
      try {
        networks = await _loadNetworks();
      } catch (_) {
        notice = '无法读取本机网络，请检查网络连接或手动输入地址。';
      }
      if (!_active(generation)) return;
      if (networks.isEmpty) notice ??= '未找到可用的局域网连接。';
      final hosts = <String>{};
      for (final network in networks) {
        if (network.hostCount > 65536 ||
            hosts.length + network.hostCount > 65536) {
          notice = '部分网段过大，仅广播搜索；可手动连接指定地址。';
        } else {
          hosts.addAll(network.hosts);
        }
        if (!useUdp) continue;
        try {
          final socket = await RawDatagramSocket.bind(network.address, 0);
          if (!_active(generation)) {
            socket.close();
            return;
          }
          _sockets.add(socket);
          socket.broadcastEnabled = true;
          socket.listen((event) {
            if (event != RawSocketEvent.read || !_active(generation)) return;
            Datagram? packet;
            while ((packet = socket.receive()) != null) {
              try {
                final data = jsonDecode(utf8.decode(packet!.data));
                if (data is! Map ||
                    data['type'] != 'heartbeat' ||
                    data['status'] != 'online' ||
                    data['nodeId'] is! int ||
                    (data['nodeId'] as int) <= 0) {
                  continue;
                }
                // Use the source address rather than a stale IP in the payload.
                final address = packet.address.address;
                final old = _devices[address];
                final name = data['deviceName'];
                _devices[address] = DiscoveredDevice(
                  address: address,
                  name: name is String && name.trim().isNotEmpty
                      ? name
                      : '矩阵节点 ${data['nodeId']}',
                  serverUri: old?.serverUri,
                );
                for (final port in ports) {
                  final uri = Uri(scheme: 'http', host: address, port: port);
                  enqueue(uri);
                }
                _changed();
              } catch (_) {
                /* Ignore unrelated or malformed datagrams. */
              }
            }
          }, onError: (Object _) {});
          final request = utf8.encode(
            jsonEncode({
              'type': 'discovery_request',
              'version': 1,
              'requestId': DateTime.now().microsecondsSinceEpoch,
            }),
          );
          socket.send(
            request,
            InternetAddress(network.broadcast),
            discoveryPort,
          );
          // Unicast also works on networks that filter broadcast packets.
          if (network.hostCount <= 65536) {
            var sent = 0;
            for (final host in network.hosts) {
              if (!_active(generation)) return;
              socket.send(request, InternetAddress(host), discoveryPort);
              if (++sent % 128 == 0) await Future<void>.delayed(Duration.zero);
            }
          }
        } on SocketException {
          /* REST probing remains available. */
        }
      }
      for (final host in hosts) {
        for (final port in ports) {
          enqueue(Uri(scheme: 'http', host: host, port: port));
        }
      }
      total = targets.length;
      _changed();
      var next = 0;
      final udpWindow = Future<void>.delayed(
        useUdp ? const Duration(seconds: 2) : Duration.zero,
      );
      Future<void> worker() async {
        while (_active(generation) && next < targets.length) {
          final uri = targets[next++];
          await check(uri);
          if (!_active(generation)) return;
          checked++;
          _changed();
        }
      }

      await Future.wait(List.generate(64, (_) => worker()));
      await udpWindow;
      if (!_active(generation)) return;
      for (final socket in _sockets) {
        socket.close();
      }
      _sockets.clear();
      await Future.wait(List.generate(64, (_) => worker()));
    } catch (_) {
      if (_active(generation)) error = '搜索失败，请检查网络后重新搜索。';
    } finally {
      if (_active(generation)) {
        for (final socket in _sockets) {
          socket.close();
        }
        _sockets.clear();
        searching = false;
        _changed();
      }
    }
  }

  void cancel() {
    _generation++;
    for (final socket in _sockets) {
      socket.close();
    }
    _sockets.clear();
    for (final client in _clients) {
      client.close();
    }
    _clients.clear();
    searching = false;
  }

  @override
  void dispose() {
    _disposed = true;
    cancel();
    super.dispose();
  }
}
