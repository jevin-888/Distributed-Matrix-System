import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui';

import 'package:flutter/foundation.dart';

import '../api/matrix_api_client.dart';
import '../config/app_config.dart';
import '../models/matrix_models.dart';

enum MatrixConnectionState { disconnected, connecting, connected, error }

class SiteController extends ChangeNotifier {
  /// Maximum number of independent client-canvas windows backed by one input node.
  static const int maxPlacementsPerNode = 16;

  SiteController({
    this.persistPresetState = true,
    this.refreshOnStart = true,
    File? presetStateFile,
    MatrixApiClient? apiClient,
  }) : _presetStateFileOverride = presetStateFile,
       _api = apiClient ?? MatrixApiClient(AppConfig.defaultServerAddress) {
    _pollTimer = Timer.periodic(const Duration(seconds: 8), (_) {
      if (connectionState == MatrixConnectionState.connected && !_busy) {
        refresh(silent: true);
      }
    });
    if (persistPresetState) unawaited(_loadPresetState());
    if (refreshOnStart) unawaited(refresh(silent: true));
  }

  static const String _deletedPresetsKey = 'deletedPresetKeys';
  static const String _customPresetsKey = 'customPresets';

  MatrixApiClient _api;
  final bool persistPresetState;
  final bool refreshOnStart;
  final File? _presetStateFileOverride;
  late final Timer _pollTimer;
  Timer? _windowSyncTimer;
  Future<void>? _windowSyncFuture;
  List<MatrixWindowPlacement>? _pendingWindowSync;
  List<MatrixNode> _nodes = const [];
  List<MatrixRegion> _regions = const [];
  MatrixLayout _layout = const MatrixLayout(
    rows: 2,
    cols: 2,
    screenWidth: 1920,
    screenHeight: 1080,
  );
  PlaybackStatus? _playback;
  KvmSession? _activeKvmSession;
  MatrixConnectionState _connectionState = MatrixConnectionState.disconnected;
  bool _busy = false;
  bool _kvmBusy = false;
  bool _disposed = false;
  int? _selectedNodeId;
  int? _selectedPlacementId;
  String? _activePresetName;
  int _nextPlacementId = 1;
  int _nextPriority = 1;
  String? _selectedRegionName;
  String _message = '未连接矩阵主节点';
  AudioOutputMode _audioOutputMode = AudioOutputMode.both;
  int _audioVolumePercent = 100;
  int _audioVolumeBeforeMute = 100;
  final Map<int, CanvasNodePlacement> _canvasPlacements =
      <int, CanvasNodePlacement>{};
  final List<LayoutPreset> _presets = List<LayoutPreset>.from(_defaultPresets);

  static const List<LayoutPreset> _defaultPresets = <LayoutPreset>[
    LayoutPreset(
      name: '单屏聚焦',
      layout: MatrixLayout(
        rows: 1,
        cols: 1,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
      accent: 0xFF20D6E8,
    ),
    LayoutPreset(
      name: '标准 2×2',
      layout: MatrixLayout(
        rows: 2,
        cols: 2,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
      accent: 0xFF2789FF,
    ),
    LayoutPreset(
      name: '横向三联',
      layout: MatrixLayout(
        rows: 1,
        cols: 3,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
      accent: 0xFF38E6B2,
    ),
    LayoutPreset(
      name: '指挥中心',
      layout: MatrixLayout(
        rows: 2,
        cols: 3,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
      accent: 0xFFFFB454,
    ),
    LayoutPreset(
      name: '九宫格',
      layout: MatrixLayout(
        rows: 3,
        cols: 3,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
      accent: 0xFFB77BFF,
    ),
    LayoutPreset(
      name: '横向四联',
      layout: MatrixLayout(
        rows: 1,
        cols: 4,
        screenWidth: 1920,
        screenHeight: 1080,
      ),
      accent: 0xFF4CC9F0,
    ),
  ];

  String get serverAddress => _api.serverAddress;
  List<MatrixNode> get nodes => List.unmodifiable(_nodes);
  List<MatrixRegion> get regions => List.unmodifiable(_regions);
  String? get selectedRegionName => _selectedRegionName;
  MatrixRegion? get selectedRegion {
    for (final region in _regions) {
      if (region.name == _selectedRegionName) return region;
    }
    return null;
  }

  List<MatrixNode> get visibleNodes {
    final selected = selectedRegion;
    final regionInputNodeIds = selected?.inputNodeIds.toSet();
    final hasRegionInputBinding = selected != null;
    return List.unmodifiable(
      _nodes.where(
        (node) =>
            node.isInputDevice &&
            (!hasRegionInputBinding ||
                regionInputNodeIds!.contains(node.nodeId)),
      ),
    );
  }

  /// Keep every discovered output-capable node visible for discovery. Region
  /// bindings control output-wall commands and must not hide a device here.
  List<MatrixNode> get visibleOutputNodes {
    return List.unmodifiable(_nodes.where((node) => node.isOutputDevice));
  }

  MatrixLayout get layout => _layout;
  PlaybackStatus? get playback => _playback;
  MatrixConnectionState get connectionState => _connectionState;
  bool get busy => _busy;
  int? get selectedNodeId => _selectedNodeId;
  int? get selectedPlacementId => _selectedPlacementId;
  CanvasNodePlacement? get selectedCanvasPlacement =>
      _selectedPlacementId == null
      ? null
      : _canvasPlacements[_selectedPlacementId];
  String? get activePresetName => _activePresetName;
  String get message => _message;
  AudioOutputMode get audioOutputMode => _audioOutputMode;
  int get audioVolumePercent => _audioVolumePercent;
  bool get isMuted => _audioVolumePercent == 0;
  bool get hasConfiguredSignal =>
      visibleNodes.any((node) => node.signalSourceConfigured);
  bool get hasActiveSignal =>
      visibleNodes.any((node) => node.signalSourceActive);
  List<CanvasNodePlacement> get canvasPlacements {
    final insertionOrder = <int, int>{};
    var index = 0;
    for (final placementId in _canvasPlacements.keys) {
      insertionOrder[placementId] = index++;
    }
    final placements = _canvasPlacements.values.toList(growable: false)
      ..sort((left, right) {
        final byPriority = left.priority.compareTo(right.priority);
        if (byPriority != 0) return byPriority;
        return insertionOrder[left.placementId]!.compareTo(
          insertionOrder[right.placementId]!,
        );
      });
    return List<CanvasNodePlacement>.unmodifiable(placements);
  }

  int get placedNodeCount => _canvasPlacements.length;
  bool get isConnected => _connectionState == MatrixConnectionState.connected;
  bool get isPlaying {
    if (_playback?.running != true) return false;
    return switch (_playback?.state.toLowerCase()) {
      'scheduled' || 'playing' => true,
      _ => false,
    };
  }

  int get visibleOnlineCount =>
      visibleNodes.where((node) => node.isOnline).length;
  KvmSession? get activeKvmSession => _activeKvmSession;
  MatrixNode? get selectedCanvasNode {
    final placement = selectedCanvasPlacement;
    return placement == null ? null : nodeById(placement.nodeId);
  }

  bool get canTakeoverSelectedNode {
    final node = selectedCanvasNode;
    return isConnected &&
        !_kvmBusy &&
        _activeKvmSession == null &&
        node != null &&
        node.isOnline &&
        node.canKvmTarget;
  }

  String get kvmTakeoverUnavailableReason {
    final node = selectedCanvasNode;
    if (node == null) return '请先选择画布节点';
    if (!isConnected) return '请先连接矩阵主节点';
    if (!node.isOnline) return '节点 ${node.displayNodeId} 当前离线';
    if (!node.canKvmTarget) return '节点 ${node.displayNodeId} 未启用 KVM 目标能力';
    if (_activeKvmSession != null) return '已有 KVM 接管会话';
    if (_kvmBusy) return '正在处理 KVM 会话';
    return 'KVM 接管';
  }

  void previewAudioVolume(double value) {
    _audioVolumePercent = (value * 100).round().clamp(0, 100);
    if (_audioVolumePercent > 0) {
      _audioVolumeBeforeMute = _audioVolumePercent;
    }
    notifyListeners();
  }

  List<LayoutPreset> get presets => List<LayoutPreset>.unmodifiable(_presets);

  Future<void> saveLayoutPreset(String name, MatrixLayout layout) async {
    final normalizedName = name.trim();
    if (normalizedName.isEmpty) {
      throw const FormatException('预设名称不能为空');
    }
    if (layout.rows < 1 ||
        layout.rows > 12 ||
        layout.cols < 1 ||
        layout.cols > 12 ||
        layout.screenWidth < 1 ||
        layout.screenHeight < 1) {
      throw const FormatException('布局参数超出允许范围');
    }

    final preset = LayoutPreset(
      name: normalizedName,
      layout: layout,
      accent: 0xFF38E6B2,
      placements: List<LayoutPresetPlacement>.unmodifiable(
        canvasPlacements.map(
          (placement) => LayoutPresetPlacement(
            nodeId: placement.nodeId,
            frame: placement.frame,
            priority: placement.priority,
          ),
        ),
      ),
    );
    final existingIndex = _presets.indexWhere(
      (item) => item.name == normalizedName,
    );
    if (existingIndex >= 0) {
      _presets[existingIndex] = preset;
    } else {
      _presets.insert(0, preset);
    }
    _activePresetName = normalizedName;
    _message = existingIndex >= 0
        ? '预设布局已更新：$normalizedName'
        : '预设布局已保存：$normalizedName';
    notifyListeners();
    if (persistPresetState) await _savePresetState();
  }

  void removePreset(LayoutPreset preset) {
    final index = _presets.indexWhere((item) => identical(item, preset));
    if (index < 0) return;
    final removed = _presets.removeAt(index);
    if (_activePresetName == removed.name) _activePresetName = null;
    _message = '预设布局已删除：${removed.name}';
    if (persistPresetState) unawaited(_savePresetState());
    notifyListeners();
  }

  Future<void> _loadPresetState() async {
    try {
      final file = _presetStateFile();
      if (!await file.exists()) return;
      final decoded = jsonDecode(await file.readAsString());
      if (decoded is! Map<String, dynamic>) return;
      final rawDeleted = decoded[_deletedPresetsKey];
      final deleted = rawDeleted is List
          ? rawDeleted.whereType<String>().toSet()
          : const <String>{};
      _presets.removeWhere(
        (preset) => deleted.contains(_presetStorageKey(preset)),
      );

      final rawCustomPresets = decoded[_customPresetsKey];
      final customPresets = rawCustomPresets is List
          ? rawCustomPresets
                .map(_decodeStoredPreset)
                .whereType<LayoutPreset>()
                .toList(growable: false)
          : const <LayoutPreset>[];
      for (final preset in customPresets) {
        _presets.removeWhere((item) => item.name == preset.name);
      }
      _presets.insertAll(0, customPresets);
      if (!_disposed) notifyListeners();
    } catch (_) {
      // Presets remain usable in memory if local preference storage is unavailable.
    }
  }

  Future<void> _savePresetState() async {
    try {
      final activeKeys = _presets.map(_presetStorageKey).toSet();
      final deleted = _defaultPresets
          .where((preset) => !activeKeys.contains(_presetStorageKey(preset)))
          .map(_presetStorageKey)
          .toList(growable: false);
      final defaultKeys = _defaultPresets.map(_presetStorageKey).toSet();
      final customPresets = _presets
          .where((preset) => !defaultKeys.contains(_presetStorageKey(preset)))
          .map(_encodeStoredPreset)
          .toList(growable: false);
      final file = _presetStateFile();
      await file.parent.create(recursive: true);
      await file.writeAsString(
        jsonEncode(<String, dynamic>{
          _deletedPresetsKey: deleted,
          _customPresetsKey: customPresets,
        }),
        flush: true,
      );
    } catch (_) {
      // Deletion still applies to the current session.
    }
  }

  String _presetStorageKey(LayoutPreset preset) {
    final layout = preset.layout;
    return '${preset.name}|${layout.rows}|${layout.cols}|'
        '${layout.screenWidth}|${layout.screenHeight}';
  }

  Map<String, dynamic> _encodeStoredPreset(LayoutPreset preset) {
    return <String, dynamic>{
      'name': preset.name,
      'rows': preset.layout.rows,
      'cols': preset.layout.cols,
      'screenWidth': preset.layout.screenWidth,
      'screenHeight': preset.layout.screenHeight,
      'accent': preset.accent,
      if (preset.placements != null)
        'placements': preset.placements!
            .map(
              (placement) => <String, dynamic>{
                'nodeId': placement.nodeId,
                'priority': placement.priority,
                'left': placement.frame.left,
                'top': placement.frame.top,
                'width': placement.frame.width,
                'height': placement.frame.height,
              },
            )
            .toList(growable: false),
    };
  }

  LayoutPreset? _decodeStoredPreset(dynamic value) {
    if (value is! Map) return null;
    final map = value.map((key, item) => MapEntry(key.toString(), item));
    final name = map['name'];
    final rows = map['rows'];
    final cols = map['cols'];
    final screenWidth = map['screenWidth'];
    final screenHeight = map['screenHeight'];
    final accent = map['accent'];
    final rawPlacements = map['placements'];
    if (name is! String ||
        name.trim().isEmpty ||
        rows is! num ||
        cols is! num ||
        screenWidth is! num ||
        screenHeight is! num) {
      return null;
    }
    final layout = MatrixLayout(
      rows: rows.toInt(),
      cols: cols.toInt(),
      screenWidth: screenWidth.toInt(),
      screenHeight: screenHeight.toInt(),
    );
    if (layout.rows < 1 ||
        layout.rows > 12 ||
        layout.cols < 1 ||
        layout.cols > 12 ||
        layout.screenWidth < 1 ||
        layout.screenHeight < 1) {
      return null;
    }
    return LayoutPreset(
      name: name.trim(),
      layout: layout,
      accent: accent is num ? accent.toInt() : 0xFF38E6B2,
      placements: rawPlacements is List
          ? List<LayoutPresetPlacement>.unmodifiable(
              rawPlacements
                  .map(_decodeStoredPresetPlacement)
                  .whereType<LayoutPresetPlacement>(),
            )
          : null,
    );
  }

  LayoutPresetPlacement? _decodeStoredPresetPlacement(dynamic value) {
    if (value is! Map) return null;
    final map = value.map((key, item) => MapEntry(key.toString(), item));
    final nodeId = map['nodeId'];
    final priority = map['priority'] ?? map['layerId'];
    final left = map['left'];
    final top = map['top'];
    final width = map['width'];
    final height = map['height'];
    if (nodeId is! num ||
        left is! num ||
        top is! num ||
        width is! num ||
        height is! num) {
      return null;
    }
    final frame = Rect.fromLTWH(
      left.toDouble(),
      top.toDouble(),
      width.toDouble(),
      height.toDouble(),
    );
    if (frame.left < 0 ||
        frame.top < 0 ||
        frame.width <= 0 ||
        frame.height <= 0 ||
        frame.right > 1 ||
        frame.bottom > 1) {
      return null;
    }
    return LayoutPresetPlacement(
      nodeId: nodeId.toInt(),
      frame: frame,
      priority: priority is num && priority > 0 ? priority.toInt() : 0,
    );
  }

  File _presetStateFile() {
    final override = _presetStateFileOverride;
    if (override != null) return override;
    if (Platform.isWindows) {
      final base =
          Platform.environment['LOCALAPPDATA'] ??
          Platform.environment['APPDATA'] ??
          Directory.systemTemp.path;
      return File(
        '$base${Platform.pathSeparator}DistributedPlaybackSystem'
        '${Platform.pathSeparator}layout_presets.json',
      );
    }
    if (Platform.isAndroid) {
      final filesDirectory = Directory(
        '${Directory.systemTemp.parent.path}${Platform.pathSeparator}files',
      );
      return File(
        '${filesDirectory.path}${Platform.pathSeparator}layout_presets.json',
      );
    }
    final home = Platform.environment['HOME'] ?? Directory.systemTemp.path;
    return File(
      '$home${Platform.pathSeparator}.distributed_playback_system'
      '${Platform.pathSeparator}layout_presets.json',
    );
  }

  MatrixNode? nodeById(int id) {
    for (final node in _nodes) {
      if (node.nodeId == id) return node;
    }
    return null;
  }

  void selectNode(int? nodeId) {
    _selectedNodeId = nodeId;
    _selectedPlacementId = null;
    notifyListeners();
  }

  void selectCanvasPlacement(int placementId) {
    final placement = _canvasPlacements[placementId];
    if (placement == null) return;
    _selectedPlacementId = placementId;
    _selectedNodeId = placement.nodeId;
    notifyListeners();
  }

  void selectRegion(String name) {
    if (_selectedRegionName == name) return;
    MatrixRegion? target;
    for (final region in _regions) {
      if (region.name == name) target = region;
    }
    if (target == null) return;
    final selectedTarget = target;
    _selectedRegionName = selectedTarget.name;
    _activePresetName = null;
    if (_selectedNodeId == null ||
        !visibleNodes.any((node) => node.nodeId == _selectedNodeId)) {
      _selectedNodeId = visibleNodes.isEmpty ? null : visibleNodes.first.nodeId;
    }
    _message =
        '已切换区域：${selectedTarget.name} · ${selectedTarget.outputNodeIds.length} 个输出节点';
    notifyListeners();
    unawaited(_applyRegionOutputLayout(selectedTarget));
  }

  Future<void> _applyRegionOutputLayout(MatrixRegion region) async {
    if (!isConnected || _disposed) return;
    try {
      await _api.setScreens(region.layout, placements: region.placements);
      if (!_disposed) {
        _message = '区域输出墙已启用：${region.layout.rows}×${region.layout.cols}';
        notifyListeners();
      }
    } catch (error) {
      if (!_disposed) {
        _connectionState = MatrixConnectionState.error;
        _message = _friendlyError(error);
        notifyListeners();
      }
    }
  }

  @visibleForTesting
  void debugLoadNodes(List<MatrixNode> nodes) {
    _nodes = List<MatrixNode>.unmodifiable(nodes);
    _selectedNodeId = nodes.isEmpty ? null : nodes.first.nodeId;
    notifyListeners();
  }

  @visibleForTesting
  void debugLoadRegions(List<MatrixRegion> regions) {
    _regions = List<MatrixRegion>.unmodifiable(regions);
    _selectedRegionName = regions.isEmpty ? null : regions.first.name;
    _activePresetName = null;
    notifyListeners();
  }

  bool isNodePlaced(int nodeId) =>
      _canvasPlacements.values.any((placement) => placement.nodeId == nodeId);

  int placementCountForNode(int nodeId) => _canvasPlacements.values
      .where((placement) => placement.nodeId == nodeId)
      .length;

  bool canPlaceNodeOnCanvas(int nodeId) {
    return visibleNodes.any((node) => node.nodeId == nodeId) &&
        placementCountForNode(nodeId) < maxPlacementsPerNode;
  }

  CanvasNodePlacement? placementForNode(int nodeId) {
    for (final placement in _canvasPlacements.values) {
      if (placement.nodeId == nodeId) return placement;
    }
    return null;
  }

  bool get canIncreaseSelectedPriority {
    final placements = canvasPlacements;
    final selectedIndex = _selectedCanvasPlacementIndex(placements);
    return selectedIndex >= 0 && selectedIndex < placements.length - 1;
  }

  bool get canDecreaseSelectedPriority {
    final placements = canvasPlacements;
    final selectedIndex = _selectedCanvasPlacementIndex(placements);
    return selectedIndex > 0;
  }

  int get _highestPriority {
    if (_canvasPlacements.isEmpty) return 1;
    return canvasPlacements.last.priority;
  }

  int get _lowestPriority {
    if (_canvasPlacements.isEmpty) return 1;
    return canvasPlacements.first.priority;
  }

  int _selectedCanvasPlacementIndex(List<CanvasNodePlacement> placements) {
    final selectedId = _selectedPlacementId;
    if (selectedId == null) return -1;
    return placements.indexWhere(
      (placement) => placement.placementId == selectedId,
    );
  }

  void _replaceCanvasPlacementOrder(List<CanvasNodePlacement> placements) {
    _canvasPlacements
      ..clear()
      ..addEntries(
        placements.map(
          (placement) => MapEntry(placement.placementId, placement),
        ),
      );
  }

  void changeSelectedPriority(CanvasPriorityAction action) {
    final selectedId = _selectedPlacementId;
    if (selectedId == null) return;
    final selected = _canvasPlacements[selectedId];
    if (selected == null) return;

    final placements = canvasPlacements.toList(growable: true);
    final selectedIndex = _selectedCanvasPlacementIndex(placements);
    if (selectedIndex < 0) return;

    late final CanvasNodePlacement updated;
    switch (action) {
      case CanvasPriorityAction.increase:
      case CanvasPriorityAction.decrease:
        final targetIndex = action == CanvasPriorityAction.increase
            ? selectedIndex + 1
            : selectedIndex - 1;
        if (targetIndex < 0 || targetIndex >= placements.length) return;

        final adjacent = placements[targetIndex];
        updated = selected.copyWith(priority: adjacent.priority);
        placements[selectedIndex] = adjacent.copyWith(
          priority: selected.priority,
        );
        placements[targetIndex] = updated;
        break;
      case CanvasPriorityAction.highest:
        if (selectedIndex == placements.length - 1) return;
        placements.removeAt(selectedIndex);
        updated = selected.copyWith(priority: _highestPriority);
        placements.add(updated);
        break;
      case CanvasPriorityAction.lowest:
        if (selectedIndex == 0) return;
        placements.removeAt(selectedIndex);
        updated = selected.copyWith(priority: _lowestPriority);
        placements.insert(0, updated);
        break;
    }

    // Map insertion order is the stable tie-breaker for equal priorities.
    _replaceCanvasPlacementOrder(placements);
    _activePresetName = null;
    final actionLabel = switch (action) {
      CanvasPriorityAction.increase => '已上调',
      CanvasPriorityAction.decrease => '已下调',
      CanvasPriorityAction.highest => '已设为最高优先级',
      CanvasPriorityAction.lowest => '已设为最低优先级',
    };
    _message = '优先级 ${updated.priorityLabel} $actionLabel';
    notifyListeners();
    _scheduleWindowSync();
  }

  LayoutCell? layoutCellAt(Offset normalizedPosition) {
    final rows = _layout.rows.clamp(1, 12).toInt();
    final cols = _layout.cols.clamp(1, 12).toInt();
    if (rows <= 0 || cols <= 0) return null;
    final x = normalizedPosition.dx.clamp(0.0, .999999).toDouble();
    final y = normalizedPosition.dy.clamp(0.0, .999999).toDouble();
    return LayoutCell(row: (y * rows).floor(), col: (x * cols).floor());
  }

  Rect frameForLayoutCell(LayoutCell cell) {
    return _frameForLayoutCell(_layout, cell);
  }

  Rect _frameForLayoutCell(MatrixLayout layout, LayoutCell cell) {
    final rows = layout.rows.clamp(1, 12).toInt();
    final cols = layout.cols.clamp(1, 12).toInt();
    final cellWidth = 1 / cols;
    final cellHeight = 1 / rows;
    return Rect.fromLTWH(
      cell.col * cellWidth,
      cell.row * cellHeight,
      cellWidth,
      cellHeight,
    );
  }

  String layoutContainerIdForPlacement(CanvasNodePlacement placement) {
    final cell = layoutCellAt(
      Offset(placement.frame.center.dx, placement.frame.center.dy),
    );
    return cell?.id ?? '--';
  }

  int? placeNodeOnCanvas(
    int nodeId,
    Offset normalizedPosition, {
    bool snapToLayout = true,
  }) {
    if (!visibleNodes.any((node) => node.nodeId == nodeId)) return null;
    if (placementCountForNode(nodeId) >= maxPlacementsPerNode) {
      _message = '节点 $nodeId 已达到每节点 $maxPlacementsPerNode 个输入窗口上限';
      notifyListeners();
      return null;
    }
    const defaultWidth = .38;
    const defaultHeight = .34;
    final cell = snapToLayout ? layoutCellAt(normalizedPosition) : null;
    final snappedFrame = cell == null ? null : frameForLayoutCell(cell);
    final width = snappedFrame?.width ?? defaultWidth;
    final height = snappedFrame?.height ?? defaultHeight;
    final left = (normalizedPosition.dx - width / 2)
        .clamp(0.0, 1.0 - width)
        .toDouble();
    final top = (normalizedPosition.dy - height / 2)
        .clamp(0.0, 1.0 - height)
        .toDouble();
    final placementId = _nextPlacementId++;
    _canvasPlacements[placementId] = CanvasNodePlacement(
      placementId: placementId,
      priority: _nextPriority++,
      nodeId: nodeId,
      frame: snappedFrame ?? Rect.fromLTWH(left, top, width, height),
    );
    _selectedNodeId = nodeId;
    _selectedPlacementId = placementId;
    _activePresetName = null;
    _message = cell == null
        ? '节点 $nodeId 已加入画布，可拖动、缩放或双击铺满'
        : '节点 $nodeId 已吸附到输入窗口 ${cell.id}';
    notifyListeners();
    _scheduleWindowSync();
    return placementId;
  }

  void snapCanvasNodeToLayout(int placementId) {
    final placement = _canvasPlacements[placementId];
    if (placement == null || placement.isMaximized) return;
    final cell = layoutCellAt(
      Offset(placement.frame.center.dx, placement.frame.center.dy),
    );
    if (cell == null) return;
    _canvasPlacements[placementId] = placement.copyWith(
      frame: frameForLayoutCell(cell),
    );
    _selectedNodeId = placement.nodeId;
    _selectedPlacementId = placementId;
    _activePresetName = null;
    _message = '节点 ${placement.nodeId} 已吸附到输入窗口 ${cell.id}';
    notifyListeners();
    _scheduleWindowSync();
  }

  void moveCanvasNode(int placementId, Offset normalizedDelta) {
    final placement = _canvasPlacements[placementId];
    if (placement == null || placement.isMaximized) return;
    final frame = placement.frame;
    final left = (frame.left + normalizedDelta.dx)
        .clamp(0.0, 1.0 - frame.width)
        .toDouble();
    final top = (frame.top + normalizedDelta.dy)
        .clamp(0.0, 1.0 - frame.height)
        .toDouble();
    _canvasPlacements[placementId] = placement.copyWith(
      frame: Rect.fromLTWH(left, top, frame.width, frame.height),
    );
    _selectedNodeId = placement.nodeId;
    _selectedPlacementId = placementId;
    _activePresetName = null;
    notifyListeners();
    _scheduleWindowSync();
  }

  void resizeCanvasNode(
    int placementId,
    CanvasResizeHandle handle,
    Offset normalizedDelta,
  ) {
    final placement = _canvasPlacements[placementId];
    if (placement == null || placement.isMaximized) return;
    const minWidth = .18;
    const minHeight = .18;
    var left = placement.frame.left;
    var top = placement.frame.top;
    var right = placement.frame.right;
    var bottom = placement.frame.bottom;

    switch (handle) {
      case CanvasResizeHandle.top:
        top = (top + normalizedDelta.dy)
            .clamp(0.0, bottom - minHeight)
            .toDouble();
        break;
      case CanvasResizeHandle.topLeft:
        left = (left + normalizedDelta.dx)
            .clamp(0.0, right - minWidth)
            .toDouble();
        top = (top + normalizedDelta.dy)
            .clamp(0.0, bottom - minHeight)
            .toDouble();
        break;
      case CanvasResizeHandle.topRight:
        right = (right + normalizedDelta.dx)
            .clamp(left + minWidth, 1.0)
            .toDouble();
        top = (top + normalizedDelta.dy)
            .clamp(0.0, bottom - minHeight)
            .toDouble();
        break;
      case CanvasResizeHandle.right:
        right = (right + normalizedDelta.dx)
            .clamp(left + minWidth, 1.0)
            .toDouble();
        break;
      case CanvasResizeHandle.bottom:
        bottom = (bottom + normalizedDelta.dy)
            .clamp(top + minHeight, 1.0)
            .toDouble();
        break;
      case CanvasResizeHandle.left:
        left = (left + normalizedDelta.dx)
            .clamp(0.0, right - minWidth)
            .toDouble();
        break;
      case CanvasResizeHandle.bottomLeft:
        left = (left + normalizedDelta.dx)
            .clamp(0.0, right - minWidth)
            .toDouble();
        bottom = (bottom + normalizedDelta.dy)
            .clamp(top + minHeight, 1.0)
            .toDouble();
        break;
      case CanvasResizeHandle.bottomRight:
        right = (right + normalizedDelta.dx)
            .clamp(left + minWidth, 1.0)
            .toDouble();
        bottom = (bottom + normalizedDelta.dy)
            .clamp(top + minHeight, 1.0)
            .toDouble();
        break;
    }

    _canvasPlacements[placementId] = placement.copyWith(
      frame: Rect.fromLTRB(left, top, right, bottom),
    );
    _selectedNodeId = placement.nodeId;
    _selectedPlacementId = placementId;
    _activePresetName = null;
    notifyListeners();
    _scheduleWindowSync();
  }

  void toggleCanvasNodeMaximized(int placementId) {
    final placement = _canvasPlacements[placementId];
    if (placement == null) return;
    if (placement.isMaximized) {
      _canvasPlacements[placementId] = placement.copyWith(
        frame: placement.restoreFrame,
        clearRestoreFrame: true,
      );
      _message = '节点 ${placement.nodeId} 已恢复到原来的位置和尺寸';
    } else {
      _canvasPlacements[placementId] = placement.copyWith(
        frame: const Rect.fromLTWH(0, 0, 1, 1),
        restoreFrame: placement.frame,
      );
      _message = '节点 ${placement.nodeId} 已铺满画布，再次双击可恢复';
    }
    _selectedNodeId = placement.nodeId;
    _selectedPlacementId = placementId;
    notifyListeners();
    _scheduleWindowSync();
  }

  void maximizeSelectedNode() {
    final placementId = _selectedPlacementId;
    if (placementId == null || !_canvasPlacements.containsKey(placementId)) {
      _message = '请先把节点拖入画布并选中';
      notifyListeners();
      return;
    }
    toggleCanvasNodeMaximized(placementId);
  }

  void removeNodeFromCanvas(int placementId) {
    final removed = _canvasPlacements.remove(placementId);
    if (removed == null) return;
    if (_selectedPlacementId == placementId) _selectedPlacementId = null;
    _resetPriorityCounterIfEmpty();
    _activePresetName = null;
    _message = '节点 ${removed.nodeId} 的当前窗口已从画布移除';
    notifyListeners();
    _scheduleWindowSync();
  }

  void clearCanvas() {
    if (_canvasPlacements.isEmpty) return;
    _canvasPlacements.clear();
    _selectedPlacementId = null;
    _nextPriority = 1;
    _activePresetName = null;
    _message = '画布已清空，请从左侧重新拖入节点';
    notifyListeners();
    _scheduleWindowSync();
  }

  void _arrangeCanvas(MatrixLayout layout) {
    // A built-in grid preset has no saved node list. Populate an empty canvas
    // from the currently available input nodes so selecting a preset is a
    // usable scene switch rather than a no-op.
    if (_canvasPlacements.isEmpty) {
      final capacity =
          layout.rows.clamp(1, 12).toInt() * layout.cols.clamp(1, 12).toInt();
      final candidates = visibleNodes
          .where((node) => node.isOnline && node.isInputDevice)
          .take(capacity)
          .toList(growable: false);
      for (var index = 0; index < candidates.length; index++) {
        final row = index ~/ layout.cols.clamp(1, 12).toInt();
        final col = index % layout.cols.clamp(1, 12).toInt();
        final cell = LayoutCell(row: row, col: col);
        final placementId = _nextPlacementId++;
        _canvasPlacements[placementId] = CanvasNodePlacement(
          placementId: placementId,
          priority: _nextPriority++,
          nodeId: candidates[index].nodeId,
          frame: _frameForLayoutCell(layout, cell),
        );
      }
      return;
    }
    final placements = canvasPlacements;
    final columns = layout.cols.clamp(1, 12).toInt();
    final capacity = layout.rows.clamp(1, 12).toInt() * columns;
    for (var index = 0; index < placements.length; index++) {
      if (index >= capacity) continue;
      final row = index ~/ columns;
      final col = index % columns;
      final placement = placements[index];
      final cell = LayoutCell(row: row, col: col);
      final frame = _frameForLayoutCell(layout, cell);
      _canvasPlacements[placement.placementId] = CanvasNodePlacement(
        placementId: placement.placementId,
        priority: placement.priority,
        nodeId: placement.nodeId,
        frame: frame,
      );
    }
  }

  void _restorePresetPlacements(List<LayoutPresetPlacement> placements) {
    // A saved scene is an explicit node assignment. Do not apply the current
    // sidebar filter here: it is a presentation filter, while the preset is
    // the authoritative list of windows to restore.
    final availableNodeIds = _nodes
        .where((node) => node.isOnline && node.isInputDevice)
        .map((node) => node.nodeId)
        .toSet();
    final restoredCounts = <int, int>{};
    _canvasPlacements.clear();
    _selectedPlacementId = null;
    _nextPriority = 1;
    for (final saved in placements) {
      if (!availableNodeIds.contains(saved.nodeId)) continue;
      final count = restoredCounts[saved.nodeId] ?? 0;
      if (count >= maxPlacementsPerNode) continue;
      restoredCounts[saved.nodeId] = count + 1;
      final placementId = _nextPlacementId++;
      final priority = saved.priority > 0 ? saved.priority : _nextPriority;
      if (_nextPriority <= priority) _nextPriority = priority + 1;
      _canvasPlacements[placementId] = CanvasNodePlacement(
        placementId: placementId,
        priority: priority,
        nodeId: saved.nodeId,
        frame: saved.frame,
      );
    }
  }

  Future<void> connect(String address) async {
    final normalized = address.trim();
    if (normalized.isEmpty) return;
    // A poll may start while the device picker is open. Finish it before
    // replacing its API client, so its response cannot overwrite the selection.
    while (_busy && !_disposed) {
      await Future<void>.delayed(const Duration(milliseconds: 25));
    }
    if (_disposed) return;
    _api.close();
    _api = MatrixApiClient(normalized);
    _clearRemoteState();
    _connectionState = MatrixConnectionState.connecting;
    _message = '正在连接 ${_api.serverAddress}…';
    notifyListeners();
    await refresh();
  }

  Future<KvmSession> acquireSelectedNodeKvm({int leaseMs = 300000}) async {
    final node = selectedCanvasNode;
    if (node == null) throw StateError('请先选择画布节点');
    if (!isConnected) throw StateError('请先连接矩阵主节点');
    if (_kvmBusy) throw StateError('正在处理 KVM 会话');
    if (!node.isOnline) {
      throw StateError('节点 ${node.displayNodeId} 当前离线');
    }
    if (!node.canKvmTarget) {
      throw StateError('节点 ${node.displayNodeId} 未启用 KVM 目标能力');
    }
    if (_activeKvmSession != null) throw StateError('已有 KVM 接管会话');

    _kvmBusy = true;
    _message = '正在申请节点 ${node.displayNodeId} 的 KVM 接管权限…';
    notifyListeners();
    try {
      final session = await _api.acquireKvm(
        targetNodeId: node.nodeId,
        leaseMs: leaseMs,
      );
      if (session.targetNodeId != node.nodeId ||
          session.sessionId <= 0 ||
          session.state != 'acquired') {
        throw const FormatException('KVM acquire response is inconsistent');
      }
      _activeKvmSession = session;
      _message = '节点 ${node.displayNodeId} 的 KVM 会话已建立';
      return session;
    } catch (error) {
      _message = _friendlyError(error);
      rethrow;
    } finally {
      _kvmBusy = false;
      if (!_disposed) notifyListeners();
    }
  }

  Future<void> releaseKvmSession(int sessionId) async {
    final active = _activeKvmSession;
    if (sessionId <= 0) return;
    try {
      await _api.releaseKvm(sessionId);
      _message = 'KVM 接管已释放';
    } catch (error) {
      _message = _friendlyError(error);
      rethrow;
    } finally {
      if (active?.sessionId == sessionId) _activeKvmSession = null;
      if (!_disposed) notifyListeners();
    }
  }

  Future<void> refresh({bool silent = false}) async {
    if (_busy) return;
    _busy = true;
    if (!silent) {
      _connectionState = MatrixConnectionState.connecting;
      _message = '正在同步输入节点、区域和播放状态…';
      notifyListeners();
    }

    try {
      MatrixRegion? regionToActivate;
      final nodes = await _api.getNodes();
      final regions = await _api.getRegions();
      final playback = await _api.getStatus();
      List<MatrixWindowPlacement> remoteWindows =
          const <MatrixWindowPlacement>[];
      try {
        remoteWindows = await _api.getWindows();
      } catch (_) {
        // Older masters do not expose the window scene endpoint yet.
      }
      final audioOutputMode = await _api.getAudioOutput();
      final audioVolumePercent = await _api.getAudioVolumePercent();
      _nodes = nodes;
      _regions = regions;
      if (remoteWindows.isNotEmpty || isConnected) {
        _canvasPlacements
          ..clear()
          ..addEntries(
            remoteWindows.map(
              (window) => MapEntry(
                window.windowId,
                CanvasNodePlacement(
                  placementId: window.windowId,
                  priority: window.zOrder,
                  nodeId: window.sourceNodeId,
                  frame: window.frame,
                ),
              ),
            ),
          );
        _nextPlacementId = remoteWindows.fold<int>(
          1,
          (next, window) =>
              window.windowId >= next ? window.windowId + 1 : next,
        );
        _nextPriority = remoteWindows.fold<int>(
          1,
          (next, window) => window.zOrder >= next ? window.zOrder + 1 : next,
        );
      }
      final nodeIds = nodes.map((node) => node.nodeId).toSet();
      _canvasPlacements.removeWhere(
        (_, placement) => !nodeIds.contains(placement.nodeId),
      );
      _resetPriorityCounterIfEmpty();
      if (regions.isEmpty) {
        _selectedRegionName = null;
      } else {
        final selectedStillExists = regions.any(
          (region) => region.name == _selectedRegionName,
        );
        if (!selectedStillExists) {
          _selectedRegionName = regions.first.name;
          regionToActivate = regions.first;
        }
      }
      if (regionToActivate != null) {
        await _api.setScreens(
          regionToActivate.layout,
          placements: regionToActivate.placements,
        );
      }
      if (!_canvasPlacements.containsKey(_selectedPlacementId)) {
        _selectedPlacementId = null;
      }
      _playback = playback;
      _audioOutputMode = audioOutputMode;
      _audioVolumePercent = audioVolumePercent;
      if (audioVolumePercent > 0) {
        _audioVolumeBeforeMute = audioVolumePercent;
      }
      _connectionState = MatrixConnectionState.connected;
      _message = '已连接 · ${regions.length} 个区域 · ${nodes.length} 个节点';
      if (_selectedNodeId == null ||
          !visibleNodes.any((node) => node.nodeId == _selectedNodeId)) {
        final visible = visibleNodes;
        _selectedNodeId = visible.isEmpty ? null : visible.first.nodeId;
      }
    } catch (error) {
      if (_disposed) return;
      _clearRemoteState();
      _connectionState = MatrixConnectionState.error;
      _message = _friendlyError(error);
    } finally {
      _busy = false;
      if (!_disposed) notifyListeners();
    }
  }

  Future<void> discoverNodes() async {
    if (_busy) return;
    if (!isConnected) {
      _message = '请先连接矩阵主节点';
      notifyListeners();
      return;
    }
    _busy = true;
    _message = '正在搜索路由可达网段中的设备节点…';
    notifyListeners();
    try {
      final result = await _api.discoverNodes();
      _nodes = await _api.getNodes();
      final nodeIds = _nodes.map((node) => node.nodeId).toSet();
      _canvasPlacements.removeWhere(
        (_, placement) => !nodeIds.contains(placement.nodeId),
      );
      _resetPriorityCounterIfEmpty();
      if (_selectedNodeId == null ||
          !visibleNodes.any((node) => node.nodeId == _selectedNodeId)) {
        final visible = visibleNodes;
        _selectedNodeId = visible.isEmpty ? null : visible.first.nodeId;
      }
      _connectionState = MatrixConnectionState.connected;
      _message =
          '搜索完成：发现 ${result.nodesFound} 个节点，已探测 ${result.addressesProbed} 个地址';
    } catch (error) {
      _message = _friendlyError(error);
    } finally {
      _busy = false;
      if (!_disposed) notifyListeners();
    }
  }

  void _clearRemoteState() {
    _nodes = const [];
    _regions = const [];
    _playback = null;
    _activeKvmSession = null;
    _selectedNodeId = null;
    _selectedPlacementId = null;
    _selectedRegionName = null;
    _canvasPlacements.clear();
    _nextPlacementId = 1;
    _nextPriority = 1;
    _activePresetName = null;
  }

  void _resetPriorityCounterIfEmpty() {
    if (_canvasPlacements.isEmpty) _nextPriority = 1;
  }

  Future<void> applyLayout(MatrixLayout layout) async {
    if (_busy) return;
    _activePresetName = null;
    _layout = layout;
    _arrangeCanvas(layout);
    await _syncAppliedLayout();
  }

  Future<void> applyPreset(LayoutPreset preset) async {
    if (_busy) return;
    _activePresetName = preset.name;
    _layout = preset.layout;
    final placements = preset.placements;
    if (placements == null) {
      _arrangeCanvas(preset.layout);
    } else {
      _restorePresetPlacements(placements);
    }
    await _syncAppliedLayout();
  }

  Future<void> _syncAppliedLayout() {
    if (_disposed) return Future<void>.value();
    _windowSyncTimer?.cancel();
    _windowSyncTimer = null;
    // Keep only the newest snapshot. A drag or a scene switch can generate
    // several edits before the previous HTTP request has completed.
    _pendingWindowSync = canvasPlacements
        .map(
          (placement) => MatrixWindowPlacement(
            windowId: placement.placementId,
            sourceNodeId: placement.nodeId,
            frame: placement.frame,
            zOrder: placement.priority,
          ),
        )
        .toList(growable: false);
    final running = _windowSyncFuture;
    if (running != null) return running;
    final completer = Completer<void>();
    _windowSyncFuture = completer.future;
    unawaited(_drainWindowSync(completer));
    return completer.future;
  }

  Future<void> _drainWindowSync(Completer<void> completer) async {
    String? lastError;
    try {
      while (!_disposed) {
        final snapshot = _pendingWindowSync;
        if (snapshot == null) break;
        _pendingWindowSync = null;
        if (!isConnected) continue;
        try {
          await _api.setWindows(snapshot);
          lastError = null;
        } catch (error) {
          lastError = '输入画布同步失败：$error';
        }
      }
      if (!_disposed) {
        _message =
            lastError ??
            (isConnected
                ? '输入画布布局已应用：${_layout.rows}×${_layout.cols}'
                : '已切换本地输入画布：${_layout.rows}×${_layout.cols}');
        notifyListeners();
      }
    } finally {
      _windowSyncFuture = null;
      _pendingWindowSync = null;
      if (!completer.isCompleted) completer.complete();
    }
  }

  void _scheduleWindowSync() {
    _windowSyncTimer?.cancel();
    _windowSyncTimer = Timer(const Duration(milliseconds: 180), () {
      _windowSyncTimer = null;
      if (!_disposed) unawaited(_syncAppliedLayout());
    });
  }

  Future<void> pause() async {
    await _execute(action: _api.pause, success: '已发送暂停命令', refreshAfter: true);
  }

  Future<void> resume() async {
    await _execute(action: _api.resume, success: '已发送继续命令', refreshAfter: true);
  }

  Future<void> stop() async {
    await _execute(action: _api.stop, success: '已发送停止命令', refreshAfter: true);
  }

  Future<void> setAudioOutput(AudioOutputMode mode) async {
    await _execute(
      action: () async {
        _audioOutputMode = await _api.setAudioOutput(mode);
      },
      success: 'Audio output: ${mode.wireValue}',
    );
  }

  Future<void> setAudioVolumePercent(int volumePercent) async {
    final clamped = volumePercent.clamp(0, 100);
    await _execute(
      action: () async {
        _audioVolumePercent = await _api.setAudioVolumePercent(clamped);
        if (_audioVolumePercent > 0) {
          _audioVolumeBeforeMute = _audioVolumePercent;
        }
      },
      success: 'Audio volume: $clamped%',
    );
  }

  Future<void> adjustAudioVolume(int delta) async {
    if (delta == 0) return;
    await setAudioVolumePercent((_audioVolumePercent + delta).clamp(0, 100));
  }

  Future<void> toggleMute() async {
    if (isMuted) {
      await setAudioVolumePercent(_audioVolumeBeforeMute.clamp(1, 100));
      return;
    }
    _audioVolumeBeforeMute = _audioVolumePercent.clamp(1, 100);
    await setAudioVolumePercent(0);
  }

  Future<void> togglePlayback() => isPlaying ? pause() : resume();

  Future<void> _execute({
    required Future<void> Function() action,
    required String success,
    bool refreshAfter = false,
  }) async {
    if (_busy) return;
    if (!isConnected) {
      _message = '请先连接矩阵主节点';
      notifyListeners();
      return;
    }
    _busy = true;
    notifyListeners();
    try {
      await action();
      _message = success;
      if (refreshAfter) {
        _playback = await _api.getStatus();
      }
    } catch (error) {
      _message = _friendlyError(error);
      _connectionState = MatrixConnectionState.error;
    } finally {
      _busy = false;
      notifyListeners();
    }
  }

  String _friendlyError(Object error) {
    if (error is MatrixApiException) return error.toString();
    if (error is FormatException) return '服务器数据格式与 API 规范不一致：${error.message}';
    return '操作失败：$error';
  }

  @override
  void dispose() {
    _disposed = true;
    _pollTimer.cancel();
    _windowSyncTimer?.cancel();
    _pendingWindowSync = null;
    _api.close();
    super.dispose();
  }
}
