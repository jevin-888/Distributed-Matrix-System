import 'dart:ui';

class MatrixNode {
  const MatrixNode({
    required this.nodeId,
    required this.ip,
    required this.resolution,
    required this.status,
    required this.playState,
    required this.cpu,
    required this.memory,
    required this.masterClockSynchronized,
    required this.masterClockOffsetMs,
    required this.lastSeen,
    this.nodeRole = '',
    this.deviceType = '',
    this.ipMode = 'auto',
    this.kvmEnabled = false,
    this.kvmRole = 'disabled',
    this.kvmPort = 0,
    this.kvmSessionId = 0,
    this.kvmState = 'disabled',
    this.kvmLastError = '',
    this.signalSourceType = 'none',
    this.signalSourceConfigured = false,
    this.signalSourceActive = false,
    this.signalSourceEndpoint = '',
    this.signalSourceWidth = 0,
    this.signalSourceHeight = 0,
    this.signalSourceFramerateNumerator = 0,
    this.signalSourceFramerateDenominator = 1,
    this.signalSourcePixelFormat = 'auto',
  });

  final int nodeId;
  final String ip;
  final String resolution;
  final String status;
  final String playState;
  final double cpu;
  final double memory;
  final bool masterClockSynchronized;
  final int masterClockOffsetMs;
  final int lastSeen;
  final String nodeRole;
  final String deviceType;
  final String ipMode;
  final bool kvmEnabled;
  final String kvmRole;
  final int kvmPort;
  final int kvmSessionId;
  final String kvmState;
  final String kvmLastError;
  final String signalSourceType;
  final bool signalSourceConfigured;
  final bool signalSourceActive;
  final String signalSourceEndpoint;
  final int signalSourceWidth;
  final int signalSourceHeight;
  final int signalSourceFramerateNumerator;
  final int signalSourceFramerateDenominator;
  final String signalSourcePixelFormat;

  bool get isOnline => status.toLowerCase() == 'online';
  bool get canKvmTarget =>
      kvmEnabled && (kvmRole == 'target' || kvmRole == 'both');
  Uri? get signalPreviewUri {
    if (ip.isEmpty || kvmPort <= 0 || kvmPort >= 65535) return null;
    return Uri(
      scheme: 'http',
      host: ip,
      port: kvmPort + 1,
      path: '/kvm/preview.jpg',
    );
  }

  String get displayNodeId => nodeId.toString().padLeft(3, '0');
  String get displayResolution {
    if (signalSourceConfigured &&
        signalSourceWidth > 0 &&
        signalSourceHeight > 0) {
      return '${signalSourceWidth}x$signalSourceHeight';
    }
    return resolution;
  }

  String get signalSourceLabel => switch (signalSourceType) {
    'capture' => 'HDMI 采集',
    'stream' => '流媒体',
    'network_camera' => '网络摄像头',
    _ => '无信号源',
  };

  /// Only input-capable nodes are available to open as client windows.
  /// The configured node role is authoritative; deviceType is a fallback for
  /// older discovery payloads.
  bool get isInputDevice {
    final role = nodeRole.trim().toLowerCase();
    if (role.isNotEmpty) return role == 'encode' || role == 'codec';
    final type = deviceType.trim().toLowerCase();
    if (type.isNotEmpty) return type == 'input' || type == 'input/output';
    return true;
  }

  /// Output-capable nodes render the physical output wall. The configured
  /// role is authoritative; deviceType keeps legacy discovery payloads usable.
  bool get isOutputDevice {
    final role = nodeRole.trim().toLowerCase();
    if (role.isNotEmpty) return role == 'decode' || role == 'codec';
    final type = deviceType.trim().toLowerCase();
    if (type.isNotEmpty) return type == 'output' || type == 'input/output';
    return false;
  }

  factory MatrixNode.fromJson(Map<String, dynamic> json) {
    return MatrixNode(
      nodeId: _int(json, 'nodeId'),
      ip: _string(json, 'ip'),
      resolution: _string(json, 'resolution'),
      status: _string(json, 'status'),
      playState: _string(json, 'playState'),
      cpu: _number(json, 'cpu'),
      memory: _number(json, 'memory'),
      masterClockSynchronized: _bool(json, 'masterClockSynchronized'),
      masterClockOffsetMs: _int(json, 'masterClockOffsetMs'),
      lastSeen: _int(json, 'lastSeen'),
      nodeRole: _optionalString(json, 'nodeRole', ''),
      deviceType: _optionalString(json, 'deviceType', ''),
      ipMode: _optionalString(json, 'ipMode', 'auto'),
      kvmEnabled: _optionalBool(json, 'kvmEnabled'),
      kvmRole: _optionalString(json, 'kvmRole', 'disabled'),
      kvmPort: _optionalInt(json, 'kvmPort'),
      kvmSessionId: _optionalInt(json, 'kvmSessionId'),
      kvmState: _optionalString(json, 'kvmState', 'disabled'),
      kvmLastError: _optionalString(json, 'kvmLastError', ''),
      signalSourceType: _optionalString(json, 'signalSourceType', 'none'),
      signalSourceConfigured: _optionalBool(json, 'signalSourceConfigured'),
      signalSourceActive: _optionalBool(json, 'signalSourceActive'),
      signalSourceEndpoint: _optionalString(json, 'signalSourceEndpoint', ''),
      signalSourceWidth: _optionalInt(json, 'signalSourceWidth'),
      signalSourceHeight: _optionalInt(json, 'signalSourceHeight'),
      signalSourceFramerateNumerator: _optionalInt(
        json,
        'signalSourceFramerateNumerator',
      ),
      signalSourceFramerateDenominator: _optionalInt(
        json,
        'signalSourceFramerateDenominator',
        1,
      ),
      signalSourcePixelFormat: _optionalString(
        json,
        'signalSourcePixelFormat',
        'auto',
      ),
    );
  }
}

class KvmSessionStatus {
  const KvmSessionStatus({
    required this.sessionId,
    required this.targetNodeId,
    required this.targetNodeLabel,
    required this.state,
    required this.acquiredAt,
    required this.expiresAt,
  });

  final int sessionId;
  final int targetNodeId;
  final String targetNodeLabel;
  final String state;
  final int acquiredAt;
  final int expiresAt;

  bool get isActive => sessionId != 0 && state == 'acquired';

  factory KvmSessionStatus.fromJson(Map<String, dynamic> json) {
    return KvmSessionStatus(
      sessionId: _int(json, 'sessionId'),
      targetNodeId: _int(json, 'targetNodeId'),
      targetNodeLabel: _string(json, 'targetNodeLabel'),
      state: _string(json, 'state'),
      acquiredAt: _int(json, 'acquiredAt'),
      expiresAt: _int(json, 'expiresAt'),
    );
  }
}

class KvmSession extends KvmSessionStatus {
  const KvmSession({
    required super.sessionId,
    required super.targetNodeId,
    required super.targetNodeLabel,
    required super.state,
    required super.acquiredAt,
    required super.expiresAt,
    required this.targetIp,
    required this.targetPort,
    required this.sessionToken,
  });

  final String targetIp;
  final int targetPort;
  final int sessionToken;

  Uri get previewUri => Uri(
    scheme: 'http',
    host: targetIp,
    port: targetPort + 1,
    path: '/kvm/preview.jpg',
    queryParameters: <String, String>{
      'sessionId': sessionId.toString(),
      'token': sessionToken.toString(),
    },
  );

  factory KvmSession.fromJson(Map<String, dynamic> json) {
    final sessionId = _int(json, 'sessionId');
    final targetNodeId = _int(json, 'targetNodeId');
    final state = _string(json, 'state');
    final targetIp = _string(json, 'targetIp');
    final targetPort = _int(json, 'targetPort');
    final sessionToken = _int(json, 'sessionToken');
    if (sessionId <= 0 || targetNodeId <= 0 || state != 'acquired') {
      throw const FormatException('KVM acquired session is invalid');
    }
    if (targetIp.isEmpty || targetPort < 1 || targetPort >= 65535) {
      throw const FormatException('KVM target connection is invalid');
    }
    if (sessionToken <= 0) {
      throw const FormatException('KVM sessionToken must be positive');
    }
    return KvmSession(
      sessionId: sessionId,
      targetNodeId: targetNodeId,
      targetNodeLabel: _string(json, 'targetNodeLabel'),
      state: state,
      acquiredAt: _int(json, 'acquiredAt'),
      expiresAt: _int(json, 'expiresAt'),
      targetIp: targetIp,
      targetPort: targetPort,
      sessionToken: sessionToken,
    );
  }
}

class MatrixLayout {
  const MatrixLayout({
    required this.rows,
    required this.cols,
    required this.screenWidth,
    required this.screenHeight,
    this.totalWidth,
    this.totalHeight,
  });

  final int rows;
  final int cols;
  final int screenWidth;
  final int screenHeight;
  final int? totalWidth;
  final int? totalHeight;

  int get outputWidth => totalWidth ?? cols * screenWidth;
  int get outputHeight => totalHeight ?? rows * screenHeight;

  Map<String, dynamic> toPutJson() => <String, dynamic>{
    'rows': rows,
    'cols': cols,
    'screenWidth': screenWidth,
    'screenHeight': screenHeight,
  };

  Map<String, dynamic> toPlayJson() => toPutJson();

  factory MatrixLayout.fromJson(Map<String, dynamic> json) {
    return MatrixLayout(
      rows: _int(json, 'rows'),
      cols: _int(json, 'cols'),
      screenWidth: _int(json, 'screenWidth'),
      screenHeight: _int(json, 'screenHeight'),
      totalWidth: json['totalWidth'] is num
          ? (json['totalWidth'] as num).toInt()
          : null,
      totalHeight: json['totalHeight'] is num
          ? (json['totalHeight'] as num).toInt()
          : null,
    );
  }
}

class LayoutCell {
  const LayoutCell({required this.row, required this.col});

  final int row;
  final int col;

  String get id => 'R${row + 1}-C${col + 1}';
}

class MatrixScreen {
  const MatrixScreen({
    required this.nodeId,
    required this.row,
    required this.col,
    required this.width,
    required this.height,
  });

  final int nodeId;
  final int row;
  final int col;
  final int width;
  final int height;

  factory MatrixScreen.fromJson(Map<String, dynamic> json) {
    return MatrixScreen(
      nodeId: _int(json, 'nodeId'),
      row: _int(json, 'row'),
      col: _int(json, 'col'),
      width: _int(json, 'width'),
      height: _int(json, 'height'),
    );
  }
}

class MatrixScreenPlacement {
  const MatrixScreenPlacement({required this.nodeId, required this.frame});

  final int nodeId;
  // Normalized coordinates relative to the complete display wall.
  final Rect frame;

  factory MatrixScreenPlacement.fromJson(Map<String, dynamic> json) {
    final nodeId = _int(json, 'nodeId');
    if (nodeId <= 0) {
      throw const FormatException('screen placement nodeId must be positive');
    }
    final frame = Rect.fromLTWH(
      _number(json, 'x'),
      _number(json, 'y'),
      _number(json, 'width'),
      _number(json, 'height'),
    );
    if (frame.left < 0 ||
        frame.top < 0 ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.right > 1 ||
        frame.bottom > 1) {
      throw const FormatException(
        'screen placement must stay inside the 0-1 canvas',
      );
    }
    return MatrixScreenPlacement(nodeId: nodeId, frame: frame);
  }

  Map<String, dynamic> toJson() => <String, dynamic>{
    'nodeId': nodeId,
    'x': frame.left,
    'y': frame.top,
    'width': frame.width,
    'height': frame.height,
  };
}

enum CanvasResizeHandle {
  top,
  topRight,
  right,
  bottomRight,
  bottom,
  bottomLeft,
  left,
  topLeft,
}

enum CanvasPriorityAction { increase, decrease, highest, lowest }

class CanvasNodePlacement {
  const CanvasNodePlacement({
    required this.placementId,
    required this.priority,
    required this.nodeId,
    required this.frame,
    this.restoreFrame,
  });

  final int placementId;
  final int priority;
  final int nodeId;
  final Rect frame;
  final Rect? restoreFrame;

  bool get isMaximized => restoreFrame != null;
  String get priorityLabel => priority.toString().padLeft(3, '0');

  CanvasNodePlacement copyWith({
    int? priority,
    Rect? frame,
    Rect? restoreFrame,
    bool clearRestoreFrame = false,
  }) {
    return CanvasNodePlacement(
      placementId: placementId,
      priority: priority ?? this.priority,
      nodeId: nodeId,
      frame: frame ?? this.frame,
      restoreFrame: clearRestoreFrame
          ? null
          : (restoreFrame ?? this.restoreFrame),
    );
  }
}

class MatrixWindowPlacement {
  const MatrixWindowPlacement({
    required this.windowId,
    required this.sourceNodeId,
    required this.frame,
    required this.zOrder,
  });

  final int windowId;
  final int sourceNodeId;
  final Rect frame;
  final int zOrder;

  factory MatrixWindowPlacement.fromJson(Map<String, dynamic> json) {
    final windowId = _int(json, 'windowId');
    final sourceNodeId = _int(json, 'sourceNodeId');
    final zOrder = _int(json, 'zOrder');
    final frame = Rect.fromLTWH(
      _number(json, 'x'),
      _number(json, 'y'),
      _number(json, 'width'),
      _number(json, 'height'),
    );
    if (windowId <= 0 ||
        sourceNodeId <= 0 ||
        zOrder <= 0 ||
        frame.left < 0 ||
        frame.top < 0 ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.right > 1 ||
        frame.bottom > 1) {
      throw const FormatException('window placement is invalid');
    }
    return MatrixWindowPlacement(
      windowId: windowId,
      sourceNodeId: sourceNodeId,
      frame: frame,
      zOrder: zOrder,
    );
  }

  Map<String, dynamic> toJson() => <String, dynamic>{
    'windowId': windowId,
    'sourceNodeId': sourceNodeId,
    'x': frame.left,
    'y': frame.top,
    'width': frame.width,
    'height': frame.height,
    'zOrder': zOrder,
  };
}

class ScreenSnapshot {
  const ScreenSnapshot({
    required this.layout,
    required this.screens,
    this.placements,
  });

  final MatrixLayout layout;
  final List<MatrixScreen> screens;
  final List<MatrixScreenPlacement>? placements;

  factory ScreenSnapshot.fromJson(Map<String, dynamic> json) {
    final rawScreens = json['screens'];
    if (rawScreens is! List) {
      throw const FormatException('screens must be a list');
    }
    final rawPlacements = json['placements'];
    if (rawPlacements != null && rawPlacements is! List) {
      throw const FormatException('placements must be a list');
    }
    return ScreenSnapshot(
      layout: MatrixLayout.fromJson(_map(json, 'layout')),
      screens: rawScreens
          .map((item) => MatrixScreen.fromJson(_asMap(item)))
          .toList(growable: false),
      placements: rawPlacements is List
          ? rawPlacements
                .map((item) => MatrixScreenPlacement.fromJson(_asMap(item)))
                .toList(growable: false)
          : null,
    );
  }
}

class PlaybackStatus {
  const PlaybackStatus({
    required this.running,
    required this.state,
    required this.videoUrl,
    required this.syncTimestamp,
    required this.updatedAt,
    required this.activeConnections,
    required this.totalRequests,
    required this.totalBytesSent,
  });

  final bool running;
  final String state;
  final String videoUrl;
  final int syncTimestamp;
  final int updatedAt;
  final int activeConnections;
  final int totalRequests;
  final int totalBytesSent;

  factory PlaybackStatus.fromJson(Map<String, dynamic> json) {
    final http = _map(json, 'http');
    return PlaybackStatus(
      running: _bool(json, 'running'),
      state: _string(json, 'state'),
      videoUrl: _string(json, 'videoUrl'),
      syncTimestamp: _int(json, 'syncTimestamp'),
      updatedAt: _int(json, 'updatedAt'),
      activeConnections: _int(http, 'activeConnections'),
      totalRequests: _int(http, 'totalRequests'),
      totalBytesSent: _int(http, 'totalBytesSent'),
    );
  }
}

class VideoDescriptor {
  const VideoDescriptor({
    required this.width,
    required this.height,
    this.fps = 30,
    this.duration = 0,
  });

  final int width;
  final int height;
  final double fps;
  final double duration;

  Map<String, dynamic> toJson() => <String, dynamic>{
    'width': width,
    'height': height,
    'fps': fps,
    'duration': duration,
  };
}

class MatrixRegion {
  const MatrixRegion({
    required this.name,
    required this.savedAt,
    required this.layout,
    required this.placements,
    this.inputNodeIds = const <int>[],
  });

  final String name;
  final int savedAt;

  /// Physical output-wall layout selected while creating this region.
  final MatrixLayout layout;
  final List<MatrixScreenPlacement> placements;

  /// Input-capable nodes assigned to this region's client canvas.
  final List<int> inputNodeIds;

  Set<int> get outputNodeIds =>
      placements.map((placement) => placement.nodeId).toSet();

  Map<String, dynamic> toJson() => <String, dynamic>{
    'name': name,
    'layout': layout.toPutJson(),
    'placements': placements.map((placement) => placement.toJson()).toList(),
    'savedAt': savedAt,
    'inputNodeIds': inputNodeIds,
  };

  factory MatrixRegion.fromJson(Map<String, dynamic> json) {
    final layout = MatrixLayout.fromJson(_map(json, 'layout'));
    final rawPlacements = json['placements'];
    if (rawPlacements is! List) {
      throw const FormatException('region placements must be a list');
    }
    final placements = rawPlacements
        .map((item) => MatrixScreenPlacement.fromJson(_asMap(item)))
        .toList(growable: false);
    final inputNodeIds = _positiveIntList(json, 'inputNodeIds');
    return MatrixRegion(
      name: _string(json, 'name'),
      savedAt: _int(json, 'savedAt'),
      layout: layout,
      placements: placements,
      inputNodeIds: inputNodeIds,
    );
  }
}

enum AudioOutputMode {
  hdmi,
  analog,
  both;

  String get wireValue => name;

  static AudioOutputMode fromWireValue(String value) {
    return AudioOutputMode.values.firstWhere(
      (mode) => mode.wireValue == value,
      orElse: () => throw FormatException(
        'audio output mode must be hdmi, analog, or both: $value',
      ),
    );
  }
}

class LayoutPreset {
  const LayoutPreset({
    required this.name,
    required this.layout,
    required this.accent,
    this.placements,
  });

  final String name;
  final MatrixLayout layout;
  final int accent;
  final List<LayoutPresetPlacement>? placements;
}

class LayoutPresetPlacement {
  const LayoutPresetPlacement({
    required this.nodeId,
    required this.frame,
    this.priority = 0,
  });

  final int nodeId;
  final Rect frame;
  final int priority;
}

Map<String, dynamic> _asMap(dynamic value) {
  if (value is Map<String, dynamic>) return value;
  if (value is Map) {
    return value.map((key, item) => MapEntry(key.toString(), item));
  }
  throw const FormatException('JSON value must be an object');
}

Map<String, dynamic> _map(Map<String, dynamic> json, String key) =>
    _asMap(json[key]);

String _string(Map<String, dynamic> json, String key) {
  final value = json[key];
  if (value is String) return value;
  throw FormatException('$key must be a string');
}

int _int(Map<String, dynamic> json, String key) {
  final value = json[key];
  if (value is num) return value.toInt();
  throw FormatException('$key must be an integer');
}

double _number(Map<String, dynamic> json, String key) {
  final value = json[key];
  if (value is num) return value.toDouble();
  throw FormatException('$key must be a number');
}

bool _bool(Map<String, dynamic> json, String key) {
  final value = json[key];
  if (value is bool) return value;
  throw FormatException('$key must be a boolean');
}

String _optionalString(Map<String, dynamic> json, String key, String fallback) {
  if (!json.containsKey(key)) return fallback;
  return _string(json, key);
}

int _optionalInt(Map<String, dynamic> json, String key, [int fallback = 0]) {
  if (!json.containsKey(key)) return fallback;
  return _int(json, key);
}

bool _optionalBool(
  Map<String, dynamic> json,
  String key, [
  bool fallback = false,
]) {
  if (!json.containsKey(key)) return fallback;
  return _bool(json, key);
}

List<int> _positiveIntList(Map<String, dynamic> json, String key) {
  final value = json[key];
  if (value is! List) throw FormatException('$key must be a list');
  final result = <int>[];
  for (final item in value) {
    if (item is! num || item.toInt() != item || item <= 0) {
      throw FormatException('$key must contain positive integers');
    }
    final id = item.toInt();
    if (result.contains(id)) throw FormatException('$key must be unique');
    result.add(id);
  }
  return List<int>.unmodifiable(result);
}
