import 'dart:convert';

import 'package:http/http.dart' as http;

import '../config/app_config.dart';
import '../models/matrix_models.dart';

abstract final class MatrixApiSpec {
  static const String nodes = '/api/nodes';
  static const String nodeNetwork = '/api/node-network';
  static const String nodeSource = '/api/node-source';
  static const String discoverNodes = '/api/nodes/discover';
  static const String regions = '/api/regions';
  static const String screens = '/api/screens';
  static const String windows = '/api/windows';
  static const String status = '/api/status';
  static const String audioOutput = '/api/audio-output';
  static const String audioVolume = '/api/audio-volume';
  static const String kvmStatus = '/api/kvm/status';
  static const String kvmAcquire = '/api/kvm/acquire';
  static const String kvmRelease = '/api/kvm/release';
  static const String play = '/api/play';
  static const String pause = '/api/pause';
  static const String resume = '/api/resume';
  static const String stop = '/api/stop';
  static const String preload = '/api/preload';
}

class NodeDiscoveryResult {
  const NodeDiscoveryResult({
    required this.networks,
    required this.addressesProbed,
    required this.nodesFound,
    required this.totalNodes,
    required this.elapsedMs,
  });

  factory NodeDiscoveryResult.fromJson(Map<String, dynamic> json) {
    final networks = json['networks'];
    if (json['success'] != true ||
        networks is! List ||
        networks.any((value) => value is! String) ||
        json['addressesProbed'] is! int ||
        json['nodesFound'] is! int ||
        json['totalNodes'] is! int ||
        json['elapsedMs'] is! int) {
      throw const FormatException('device discovery response is invalid');
    }
    return NodeDiscoveryResult(
      networks: networks.cast<String>(),
      addressesProbed: json['addressesProbed'] as int,
      nodesFound: json['nodesFound'] as int,
      totalNodes: json['totalNodes'] as int,
      elapsedMs: json['elapsedMs'] as int,
    );
  }

  final List<String> networks;
  final int addressesProbed;
  final int nodesFound;
  final int totalNodes;
  final int elapsedMs;
}

class MatrixApiException implements Exception {
  const MatrixApiException(this.message, {this.statusCode});

  final String message;
  final int? statusCode;

  @override
  String toString() =>
      statusCode == null ? message : 'HTTP $statusCode: $message';
}

class MatrixApiClient {
  MatrixApiClient(String serverAddress, {http.Client? client})
    : _baseUri = _normalizeServerAddress(serverAddress),
      _client = client ?? http.Client();

  final Uri _baseUri;
  final http.Client _client;

  String get serverAddress =>
      _baseUri.toString().replaceFirst(RegExp(r'/$'), '');

  Future<List<MatrixNode>> getNodes() async {
    final json = await _request('GET', MatrixApiSpec.nodes);
    final rawNodes = json['nodes'];
    if (rawNodes is! List) throw const FormatException('nodes must be a list');
    return rawNodes
        .map((item) => MatrixNode.fromJson(_asMap(item)))
        .toList(growable: false);
  }

  Future<void> setNodeNetwork({
    required int nodeId,
    required String mode,
    required String address,
    required int prefixLength,
    required String gateway,
    required List<String> dnsServers,
  }) async {
    await _request(
      'PUT',
      MatrixApiSpec.nodeNetwork,
      body: <String, dynamic>{
        'nodeId': nodeId,
        'mode': mode,
        'address': address,
        'prefixLength': prefixLength,
        'gateway': gateway,
        'dnsServers': dnsServers,
      },
    );
  }

  Future<void> setNodeSource({
    required int nodeId,
    required String sourceType,
    required String endpoint,
    required int width,
    required int height,
    required int framerateNumerator,
    required int framerateDenominator,
    required String pixelFormat,
  }) async {
    await _request(
      'PUT',
      MatrixApiSpec.nodeSource,
      body: <String, dynamic>{
        'nodeId': nodeId,
        'sourceType': sourceType,
        'endpoint': endpoint,
        'width': width,
        'height': height,
        'framerateNumerator': framerateNumerator,
        'framerateDenominator': framerateDenominator,
        'pixelFormat': pixelFormat,
      },
    );
  }

  Future<NodeDiscoveryResult> discoverNodes() async {
    return NodeDiscoveryResult.fromJson(
      await _request(
        'POST',
        MatrixApiSpec.discoverNodes,
        body: const <String, dynamic>{},
      ),
    );
  }

  Future<List<MatrixRegion>> getRegions() async {
    final json = await _request('GET', MatrixApiSpec.regions);
    final rawRegions = json['regions'];
    if (rawRegions is! List) {
      throw const FormatException('regions must be a list');
    }
    return rawRegions
        .map((item) => MatrixRegion.fromJson(_asMap(item)))
        .toList(growable: false);
  }

  Future<List<MatrixRegion>> setRegions(List<MatrixRegion> regions) async {
    final json = await _request(
      'PUT',
      MatrixApiSpec.regions,
      body: <String, dynamic>{
        'regions': regions.map((region) => region.toJson()).toList(),
      },
    );
    final rawRegions = json['regions'];
    if (rawRegions is! List) {
      throw const FormatException('regions must be a list');
    }
    return rawRegions
        .map((item) => MatrixRegion.fromJson(_asMap(item)))
        .toList(growable: false);
  }

  Future<ScreenSnapshot> getScreens() async {
    return ScreenSnapshot.fromJson(
      await _request('GET', MatrixApiSpec.screens),
    );
  }

  Future<void> setScreens(
    MatrixLayout layout, {
    List<MatrixScreenPlacement> placements = const <MatrixScreenPlacement>[],
  }) async {
    await _request(
      'PUT',
      MatrixApiSpec.screens,
      body: <String, dynamic>{
        ...layout.toPutJson(),
        'placements': placements
            .map((placement) => placement.toJson())
            .toList(),
      },
    );
  }

  Future<List<MatrixWindowPlacement>> getWindows() async {
    final json = await _request('GET', MatrixApiSpec.windows);
    final rawWindows = json['windows'];
    if (rawWindows is! List) {
      throw const FormatException('windows must be a list');
    }
    return rawWindows
        .map((item) => MatrixWindowPlacement.fromJson(_asMap(item)))
        .toList(growable: false);
  }

  Future<void> setWindows(List<MatrixWindowPlacement> windows) async {
    await _request(
      'PUT',
      MatrixApiSpec.windows,
      body: <String, dynamic>{
        'windows': windows.map((window) => window.toJson()).toList(),
      },
    );
  }

  Future<PlaybackStatus> getStatus() async {
    return PlaybackStatus.fromJson(await _request('GET', MatrixApiSpec.status));
  }

  Future<AudioOutputMode> getAudioOutput() async {
    final json = await _request('GET', MatrixApiSpec.audioOutput);
    return AudioOutputMode.fromWireValue(json['mode']?.toString() ?? '');
  }

  Future<AudioOutputMode> setAudioOutput(AudioOutputMode mode) async {
    final json = await _request(
      'PUT',
      MatrixApiSpec.audioOutput,
      body: <String, dynamic>{'mode': mode.wireValue},
    );
    return AudioOutputMode.fromWireValue(json['mode']?.toString() ?? '');
  }

  Future<int> getAudioVolumePercent() async {
    final json = await _request('GET', MatrixApiSpec.audioVolume);
    return _volumePercent(json['volumePercent']);
  }

  Future<int> setAudioVolumePercent(int volumePercent) async {
    final json = await _request(
      'PUT',
      MatrixApiSpec.audioVolume,
      body: <String, dynamic>{'volumePercent': volumePercent},
    );
    return _volumePercent(json['volumePercent']);
  }

  Future<KvmSessionStatus> getKvmStatus() async {
    return KvmSessionStatus.fromJson(
      await _request('GET', MatrixApiSpec.kvmStatus),
    );
  }

  Future<KvmSession> acquireKvm({
    required int targetNodeId,
    int leaseMs = 300000,
  }) async {
    return KvmSession.fromJson(
      await _request(
        'POST',
        MatrixApiSpec.kvmAcquire,
        body: <String, dynamic>{
          'targetNodeId': targetNodeId,
          'leaseMs': leaseMs,
        },
      ),
    );
  }

  Future<void> releaseKvm(int sessionId) async {
    await _request(
      'POST',
      MatrixApiSpec.kvmRelease,
      body: <String, dynamic>{'sessionId': sessionId},
    );
  }

  Future<void> play({
    required String videoUrl,
    required VideoDescriptor video,
    int delayMs = 3000,
    MatrixLayout? layout,
    List<MatrixScreenPlacement>? placements,
  }) async {
    final body = <String, dynamic>{
      'videoUrl': videoUrl,
      'video': video.toJson(),
      'delayMs': delayMs,
      if (layout != null) 'layout': layout.toPlayJson(),
      if (placements != null)
        'placements': placements
            .map((placement) => placement.toJson())
            .toList(),
    };
    await _request('POST', MatrixApiSpec.play, body: body);
  }

  Future<void> pause() => _emptyCommand(MatrixApiSpec.pause);

  Future<void> resume() => _emptyCommand(MatrixApiSpec.resume);

  Future<void> stop() => _emptyCommand(MatrixApiSpec.stop);

  Future<void> preload(String videoUrl) async {
    await _request(
      'POST',
      MatrixApiSpec.preload,
      body: <String, dynamic>{'videoUrl': videoUrl},
    );
  }

  Future<void> _emptyCommand(String path) async {
    await _request('POST', path, body: const <String, dynamic>{});
  }

  Future<Map<String, dynamic>> _request(
    String method,
    String path, {
    Map<String, dynamic>? body,
  }) async {
    final uri = _baseUri.resolve(path);
    final headers = <String, String>{'Accept': 'application/json'};
    if (body != null) headers['Content-Type'] = 'application/json';

    late final http.Response response;
    try {
      response = switch (method) {
        'GET' =>
          await _client
              .get(uri, headers: headers)
              .timeout(const Duration(seconds: 5)),
        'POST' =>
          await _client
              .post(uri, headers: headers, body: jsonEncode(body))
              .timeout(const Duration(seconds: 8)),
        'PUT' =>
          await _client
              .put(uri, headers: headers, body: jsonEncode(body))
              .timeout(const Duration(seconds: 8)),
        _ => throw ArgumentError.value(
          method,
          'method',
          'Unsupported HTTP method',
        ),
      };
    } on Exception catch (error) {
      throw MatrixApiException('无法连接矩阵主节点：$error');
    }

    Map<String, dynamic> json = const <String, dynamic>{};
    if (response.body.trim().isNotEmpty) {
      try {
        json = _asMap(jsonDecode(utf8.decode(response.bodyBytes)));
      } on FormatException catch (error) {
        throw MatrixApiException(
          '服务器返回了无效 JSON：$error',
          statusCode: response.statusCode,
        );
      }
    }

    if (response.statusCode < 200 || response.statusCode >= 300) {
      final message =
          json['error']?.toString() ??
          json['message']?.toString() ??
          response.reasonPhrase ??
          '请求失败';
      throw MatrixApiException(message, statusCode: response.statusCode);
    }
    return json;
  }

  void close() => _client.close();
}

Uri _normalizeServerAddress(String input) {
  var value = input.trim();
  if (value.isEmpty) value = AppConfig.defaultServerAddress;
  if (!value.startsWith('http://') && !value.startsWith('https://')) {
    value = 'http://$value';
  }
  final uri = Uri.parse(value);
  return uri.path.endsWith('/') ? uri : uri.replace(path: '${uri.path}/');
}

Map<String, dynamic> _asMap(dynamic value) {
  if (value is Map<String, dynamic>) return value;
  if (value is Map) {
    return value.map((key, item) => MapEntry(key.toString(), item));
  }
  throw const FormatException('JSON value must be an object');
}

int _volumePercent(dynamic value) {
  if (value is int && value >= 0 && value <= 100) return value;
  throw const FormatException('volumePercent must be an integer from 0 to 100');
}
