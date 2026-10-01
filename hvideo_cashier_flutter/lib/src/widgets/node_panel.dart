import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;

import '../controllers/site_controller.dart';
import '../models/matrix_models.dart';
import '../theme/app_theme.dart';
import 'tech_components.dart';
import 'live_jpeg_preview.dart';

class NodePanel extends StatefulWidget {
  const NodePanel({
    super.key,
    required this.controller,
    this.horizontal = false,
    this.previewClient,
  });

  final SiteController controller;
  final bool horizontal;
  final http.Client? previewClient;

  @override
  State<NodePanel> createState() => _NodePanelState();
}

class _NodePanelState extends State<NodePanel> {
  String query = '';
  bool showingOutputs = false;

  @override
  Widget build(BuildContext context) {
    final availableNodes = showingOutputs
        ? widget.controller.visibleOutputNodes
        : widget.controller.visibleNodes;
    final nodes = availableNodes
        .where((node) {
          final term = query.trim().toLowerCase();
          return term.isEmpty ||
              node.ip.toLowerCase().contains(term) ||
              node.nodeId.toString().contains(term) ||
              node.displayNodeId.contains(term);
        })
        .toList(growable: false);

    final list = widget.horizontal
        ? SizedBox(
            height: 126,
            child: ListView.separated(
              scrollDirection: Axis.horizontal,
              itemCount: nodes.length,
              separatorBuilder: (_, _) => const SizedBox(width: 10),
              itemBuilder: (context, index) => SizedBox(
                width: 220,
                child: _NodeCard(
                  controller: widget.controller,
                  node: nodes[index],
                  draggable: !showingOutputs,
                  previewClient: widget.previewClient,
                ),
              ),
            ),
          )
        : Expanded(
            child: ListView.separated(
              itemCount: nodes.length,
              separatorBuilder: (_, _) => const SizedBox(height: 10),
              itemBuilder: (context, index) => _NodeCard(
                controller: widget.controller,
                node: nodes[index],
                draggable: !showingOutputs,
                previewClient: widget.previewClient,
              ),
            ),
          );

    return TechPanel(
      padding: const EdgeInsets.all(14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        mainAxisSize: widget.horizontal ? MainAxisSize.min : MainAxisSize.max,
        children: <Widget>[
          SectionTitle(
            title: showingOutputs ? '输出节点' : '输入节点',
            subtitle: showingOutputs
                ? '${widget.controller.visibleOutputNodes.where((node) => node.isOnline).length}/${widget.controller.visibleOutputNodes.length} 在线'
                : '${widget.controller.visibleOnlineCount}/${widget.controller.visibleNodes.length} 在线',
            icon: showingOutputs
                ? Icons.desktop_windows_rounded
                : Icons.hub_rounded,
            trailing: Row(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                IconButton(
                  key: const ValueKey('toggle-node-kind'),
                  tooltip: showingOutputs ? '查看输入节点' : '查看输出节点',
                  onPressed: () => setState(() {
                    showingOutputs = !showingOutputs;
                    query = '';
                  }),
                  icon: Icon(
                    showingOutputs ? Icons.input_rounded : Icons.output_rounded,
                    size: 19,
                  ),
                ),
                IconButton(
                  tooltip: '搜索网络节点',
                  onPressed: widget.controller.busy
                      ? null
                      : widget.controller.discoverNodes,
                  icon: const Icon(Icons.travel_explore_rounded, size: 19),
                ),
                IconButton(
                  tooltip: '刷新节点状态',
                  onPressed: widget.controller.busy
                      ? null
                      : widget.controller.refresh,
                  icon: AnimatedRotation(
                    turns: widget.controller.busy ? 1 : 0,
                    duration: const Duration(milliseconds: 700),
                    child: const Icon(Icons.refresh_rounded, size: 19),
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          SegmentedButton<bool>(
            key: const ValueKey('node-kind-segmented-control'),
            segments: const <ButtonSegment<bool>>[
              ButtonSegment<bool>(
                value: false,
                label: Text('输入'),
                icon: Icon(Icons.input_rounded, size: 16),
              ),
              ButtonSegment<bool>(
                value: true,
                label: Text('输出'),
                icon: Icon(Icons.output_rounded, size: 16),
              ),
            ],
            selected: <bool>{showingOutputs},
            showSelectedIcon: false,
            style: ButtonStyle(
              visualDensity: VisualDensity.compact,
              textStyle: WidgetStateProperty.all(
                const TextStyle(fontSize: 11, fontWeight: FontWeight.w700),
              ),
            ),
            onSelectionChanged: (selection) => setState(() {
              showingOutputs = selection.single;
              query = '';
            }),
          ),
          const SizedBox(height: 10),
          SizedBox(
            height: 42,
            child: TextField(
              key: ValueKey(
                showingOutputs ? 'output-node-search' : 'input-node-search',
              ),
              onChanged: (value) => setState(() => query = value),
              style: const TextStyle(fontSize: 12),
              decoration: const InputDecoration(
                hintText: '搜索 IP 或节点编号',
                prefixIcon: Icon(Icons.search_rounded, size: 18),
                contentPadding: EdgeInsets.symmetric(horizontal: 12),
              ),
            ),
          ),
          const SizedBox(height: 12),
          list,
        ],
      ),
    );
  }
}

class _NodeCard extends StatefulWidget {
  const _NodeCard({
    required this.controller,
    required this.node,
    required this.draggable,
    this.previewClient,
  });

  final SiteController controller;
  final MatrixNode node;
  final bool draggable;
  final http.Client? previewClient;

  @override
  State<_NodeCard> createState() => _NodeCardState();
}

class _NodeCardState extends State<_NodeCard> {
  bool hovered = false;

  @override
  Widget build(BuildContext context) {
    final mobile =
        !kIsWeb &&
        (defaultTargetPlatform == TargetPlatform.android ||
            defaultTargetPlatform == TargetPlatform.iOS);
    final child = _buildCard(interactive: true);
    final childWhenDragging = Opacity(
      opacity: .34,
      child: _buildCard(interactive: false),
    );
    final feedback = Material(
      color: Colors.transparent,
      child: SizedBox(
        width: 220,
        child: Transform.rotate(
          angle: -.018,
          child: _buildCard(interactive: false, feedback: true),
        ),
      ),
    );

    if (!widget.draggable) {
      return child;
    }
    if (mobile) {
      return LongPressDraggable<int>(
        data: widget.node.nodeId,
        dragAnchorStrategy: pointerDragAnchorStrategy,
        feedback: feedback,
        childWhenDragging: childWhenDragging,
        onDragStarted: () => widget.controller.selectNode(widget.node.nodeId),
        child: child,
      );
    }
    return Draggable<int>(
      data: widget.node.nodeId,
      dragAnchorStrategy: pointerDragAnchorStrategy,
      feedback: feedback,
      childWhenDragging: childWhenDragging,
      onDragStarted: () => widget.controller.selectNode(widget.node.nodeId),
      child: child,
    );
  }

  Widget _buildCard({required bool interactive, bool feedback = false}) {
    final selected = widget.controller.selectedNodeId == widget.node.nodeId;
    final placed = widget.controller.isNodePlaced(widget.node.nodeId);
    final placementCount = widget.controller.placementCountForNode(
      widget.node.nodeId,
    );
    final statusColor = widget.node.isOnline ? mint : mutedText;
    final sourceColor = widget.node.signalSourceConfigured ? cyan : amber;
    final card = AnimatedContainer(
      duration: const Duration(milliseconds: 180),
      curve: Curves.easeOutCubic,
      padding: const EdgeInsets.all(11),
      decoration: BoxDecoration(
        color: selected ? blue.withValues(alpha: .16) : const Color(0xEE132F47),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(
          color: selected || feedback
              ? cyan.withValues(alpha: .86)
              : hovered
              ? cyan.withValues(alpha: .35)
              : Colors.white.withValues(alpha: .07),
          width: feedback ? 1.5 : 1,
        ),
        boxShadow: selected || feedback
            ? <BoxShadow>[
                BoxShadow(
                  color: cyan.withValues(alpha: feedback ? .24 : .11),
                  blurRadius: feedback ? 24 : 18,
                ),
              ]
            : null,
      ),
      child: Row(
        children: <Widget>[
          ClipRRect(
            borderRadius: BorderRadius.circular(7),
            child: Container(
              width: 72,
              height: 48,
              decoration: BoxDecoration(
                gradient: LinearGradient(
                  begin: Alignment.topLeft,
                  end: Alignment.bottomRight,
                  colors: widget.node.isOnline
                      ? const <Color>[Color(0xFF17616D), Color(0xFF11354C)]
                      : const <Color>[Color(0xFF263B4B), Color(0xFF172B3C)],
                ),
                borderRadius: BorderRadius.circular(7),
              ),
              child:
                  widget.draggable &&
                      widget.node.isOnline &&
                      widget.node.signalSourceConfigured &&
                      widget.node.signalPreviewUri != null
                  ? LiveJpegPreview(
                      key: ValueKey('node-preview-${widget.node.nodeId}'),
                      uri: widget.node.signalPreviewUri!,
                      client: widget.previewClient,
                      interval: const Duration(milliseconds: 400),
                      fit: BoxFit.cover,
                      placeholderBuilder: (_, _) => Icon(
                        Icons.videocam_outlined,
                        color: sourceColor,
                        size: 21,
                      ),
                    )
                  : Icon(
                      widget.node.isOnline
                          ? Icons.desktop_windows_rounded
                          : Icons.desktop_access_disabled_rounded,
                      color: statusColor,
                      size: 22,
                    ),
            ),
          ),
          const SizedBox(width: 11),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                Text(
                  widget.node.ip,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                    fontWeight: FontWeight.w700,
                    fontSize: 12.5,
                  ),
                ),
                const SizedBox(height: 4),
                Text(
                  '节点 ${widget.node.nodeId} · ${widget.node.displayResolution}',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(color: mutedText, fontSize: 10.5),
                ),
                const SizedBox(height: 5),
                Text(
                  widget.node.signalSourceConfigured
                      ? widget.node.signalSourceLabel
                      : '默认背景 · 未配置信号源',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: TextStyle(color: sourceColor, fontSize: 10.5),
                ),
                const SizedBox(height: 5),
                Row(
                  children: <Widget>[
                    Container(
                      width: 6,
                      height: 6,
                      decoration: BoxDecoration(
                        color: statusColor,
                        shape: BoxShape.circle,
                      ),
                    ),
                    const SizedBox(width: 5),
                    Flexible(
                      child: Text(
                        placed
                            ? (placementCount >=
                                      SiteController.maxPlacementsPerNode
                                  ? '已放置 $placementCount 个窗口 · 已达每节点上限'
                                  : '已放置 $placementCount 个窗口 · 可继续拖动')
                            : (widget.node.isOnline
                                  ? widget.node.playState
                                  : (widget.draggable
                                        ? '离线 · 拖动预览'
                                        : '输出节点 · ${widget.node.playState}')),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                          color: placed ? cyan : statusColor,
                          fontSize: 10.5,
                        ),
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          const SizedBox(width: 7),
          Icon(
            placed ? Icons.check_circle_rounded : Icons.drag_indicator_rounded,
            color: placed ? cyan : mutedText,
            size: 19,
          ),
        ],
      ),
    );

    if (!interactive) return card;
    return MouseRegion(
      onEnter: (_) => setState(() => hovered = true),
      onExit: (_) => setState(() => hovered = false),
      cursor: SystemMouseCursors.grab,
      child: GestureDetector(
        onTap: () => widget.controller.selectNode(widget.node.nodeId),
        child: Tooltip(
          message: widget.draggable ? '拖动到中央画布' : '输出节点只读状态',
          child: card,
        ),
      ),
    );
  }
}
