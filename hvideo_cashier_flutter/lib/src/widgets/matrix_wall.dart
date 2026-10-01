import 'dart:async';
import 'package:flutter/material.dart';
import 'package:http/http.dart' as http;

import '../controllers/site_controller.dart';
import '../pages/kvm_takeover_page.dart';
import '../models/matrix_models.dart';
import '../theme/app_theme.dart';
import 'scene_panel.dart';
import 'tech_components.dart';
import 'live_jpeg_preview.dart';

const List<MatrixLayout> _quickLayouts = <MatrixLayout>[
  MatrixLayout(rows: 1, cols: 1, screenWidth: 1920, screenHeight: 1080),
  MatrixLayout(rows: 2, cols: 2, screenWidth: 1920, screenHeight: 1080),
  MatrixLayout(rows: 1, cols: 3, screenWidth: 1920, screenHeight: 1080),
  MatrixLayout(rows: 2, cols: 3, screenWidth: 1920, screenHeight: 1080),
  MatrixLayout(rows: 3, cols: 3, screenWidth: 1920, screenHeight: 1080),
  MatrixLayout(rows: 1, cols: 4, screenWidth: 1920, screenHeight: 1080),
];

class MatrixWall extends StatelessWidget {
  const MatrixWall({super.key, required this.controller, this.previewClient});
  final SiteController controller;
  final http.Client? previewClient;

  @override
  Widget build(BuildContext context) {
    return TechPanel(
      padding: const EdgeInsets.all(14),
      accent: mint,
      child: Column(
        children: <Widget>[
          _LayoutToolbar(controller: controller),
          const SizedBox(height: 10),
          Expanded(
            child: _FreeformCanvas(
              controller: controller,
              previewClient: previewClient,
            ),
          ),
        ],
      ),
    );
  }
}

class _LayoutToolbar extends StatelessWidget {
  const _LayoutToolbar({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0x88132F47),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: Colors.white.withValues(alpha: .07)),
      ),
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Wrap(
          alignment: WrapAlignment.center,
          crossAxisAlignment: WrapCrossAlignment.center,
          spacing: 8,
          runSpacing: 8,
          children: <Widget>[
            _PriorityMenu(controller: controller),
            if (controller.selectedCanvasPlacement != null)
              AnimatedActionButton(
                key: const ValueKey('kvm-takeover-button'),
                label: 'KVM 接管',
                icon: Icons.desktop_windows_rounded,
                accent: controller.canTakeoverSelectedNode ? cyan : amber,
                compact: true,
                tooltip: controller.canTakeoverSelectedNode
                    ? '全屏接管当前节点'
                    : controller.kvmTakeoverUnavailableReason,
                onPressed: () => _handleKvmAction(context, controller),
              ),
            _QuickLayoutMenu(
              selected: controller.layout,
              onSelected: controller.applyLayout,
            ),
            AnimatedActionButton(
              label: '输入开窗布局',
              icon: Icons.tune_rounded,
              accent: mint,
              compact: true,
              onPressed: () => showLayoutDialog(context, controller),
            ),
            AnimatedActionButton(
              label: '保存预设',
              icon: Icons.bookmark_add_outlined,
              accent: blue,
              compact: true,
              onPressed: () => showSavePresetDialog(context, controller),
            ),
          ],
        ),
      ),
    );
  }
}

Future<void> _handleKvmAction(
  BuildContext context,
  SiteController controller,
) async {
  if (!controller.canTakeoverSelectedNode) {
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(
        SnackBar(content: Text(controller.kvmTakeoverUnavailableReason)),
      );
    return;
  }
  await _openKvmTakeover(context, controller);
}

Future<void> _openKvmTakeover(
  BuildContext context,
  SiteController controller,
) async {
  try {
    final session = await controller.acquireSelectedNodeKvm();
    if (!context.mounted) {
      await controller.releaseKvmSession(session.sessionId);
      return;
    }
    await Navigator.of(context).push<void>(
      MaterialPageRoute<void>(
        fullscreenDialog: true,
        builder: (_) => KvmTakeoverPage(
          session: session,
          onRelease: controller.releaseKvmSession,
        ),
      ),
    );
  } catch (error) {
    if (!context.mounted) return;
    ScaffoldMessenger.of(
      context,
    ).showSnackBar(SnackBar(content: Text('KVM 接管失败：$error')));
  }
}

class _PriorityMenu extends StatelessWidget {
  const _PriorityMenu({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    final selected = controller.selectedCanvasPlacement;
    return PopupMenuButton<CanvasPriorityAction>(
      key: const ValueKey('priority-menu'),
      enabled: selected != null,
      tooltip: selected == null ? '请先选择节点窗口' : '调整当前节点优先级',
      position: PopupMenuPosition.under,
      offset: const Offset(0, 6),
      padding: EdgeInsets.zero,
      color: const Color(0xFF0A2942),
      surfaceTintColor: Colors.transparent,
      shadowColor: Colors.black.withValues(alpha: .42),
      elevation: 14,
      clipBehavior: Clip.antiAlias,
      constraints: const BoxConstraints.tightFor(width: 168),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(8),
        side: BorderSide(color: cyan.withValues(alpha: .28)),
      ),
      onSelected: controller.changeSelectedPriority,
      itemBuilder: (context) => <PopupMenuEntry<CanvasPriorityAction>>[
        _priorityMenuItem(
          action: CanvasPriorityAction.increase,
          label: '上一层',
          icon: Icons.arrow_upward_rounded,
          enabled: controller.canIncreaseSelectedPriority,
        ),
        _priorityMenuItem(
          action: CanvasPriorityAction.decrease,
          label: '下一层',
          icon: Icons.arrow_downward_rounded,
          enabled: controller.canDecreaseSelectedPriority,
        ),
        const PopupMenuDivider(height: 8),
        _priorityMenuItem(
          action: CanvasPriorityAction.highest,
          label: '置顶',
          icon: Icons.vertical_align_top_rounded,
          enabled: controller.canIncreaseSelectedPriority,
        ),
        _priorityMenuItem(
          action: CanvasPriorityAction.lowest,
          label: '置底',
          icon: Icons.vertical_align_bottom_rounded,
          enabled: controller.canDecreaseSelectedPriority,
        ),
      ],
      child: Container(
        height: 40,
        padding: const EdgeInsets.symmetric(horizontal: 12),
        decoration: BoxDecoration(
          color: const Color(0x99112F49),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(
            color: (selected == null ? mutedText : cyan).withValues(alpha: .42),
          ),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(
              Icons.layers_rounded,
              color: selected == null ? mutedText : cyan,
              size: 17,
            ),
            const SizedBox(width: 8),
            const Text(
              '优先级',
              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w700),
            ),
            const SizedBox(width: 9),
            Text(
              selected?.priorityLabel ?? '---',
              style: TextStyle(
                color: selected == null ? mutedText : cyan,
                fontSize: 12,
                fontWeight: FontWeight.w800,
              ),
            ),
            const SizedBox(width: 3),
            const Icon(Icons.expand_more_rounded, color: mutedText, size: 17),
          ],
        ),
      ),
    );
  }

  PopupMenuItem<CanvasPriorityAction> _priorityMenuItem({
    required CanvasPriorityAction action,
    required String label,
    required IconData icon,
    required bool enabled,
  }) {
    return PopupMenuItem<CanvasPriorityAction>(
      value: action,
      enabled: enabled,
      height: 40,
      child: Row(
        children: <Widget>[
          Icon(icon, size: 17, color: enabled ? cyan : mutedText),
          const SizedBox(width: 10),
          Text(label, style: const TextStyle(fontSize: 12)),
        ],
      ),
    );
  }
}

class _QuickLayoutMenu extends StatelessWidget {
  const _QuickLayoutMenu({required this.selected, required this.onSelected});

  final MatrixLayout selected;
  final ValueChanged<MatrixLayout> onSelected;

  @override
  Widget build(BuildContext context) {
    return PopupMenuButton<MatrixLayout>(
      key: const ValueKey('quick-layout-menu'),
      tooltip: '选择输入开窗数量布局',
      position: PopupMenuPosition.under,
      offset: const Offset(0, 6),
      padding: EdgeInsets.zero,
      color: const Color(0xFF0A2942),
      surfaceTintColor: Colors.transparent,
      shadowColor: Colors.black.withValues(alpha: .42),
      elevation: 14,
      clipBehavior: Clip.antiAlias,
      constraints: const BoxConstraints.tightFor(width: 174),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(8),
        side: BorderSide(color: amber.withValues(alpha: .28)),
      ),
      onSelected: onSelected,
      itemBuilder: (context) => _quickLayouts
          .map((layout) {
            final active =
                layout.rows == selected.rows && layout.cols == selected.cols;
            return PopupMenuItem<MatrixLayout>(
              value: layout,
              height: 44,
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 3),
              child: Container(
                width: double.infinity,
                height: 38,
                alignment: Alignment.centerLeft,
                padding: const EdgeInsets.symmetric(horizontal: 13),
                decoration: BoxDecoration(
                  color: active
                      ? amber.withValues(alpha: .14)
                      : Colors.transparent,
                  borderRadius: BorderRadius.circular(5),
                  border: active
                      ? Border(left: BorderSide(color: amber, width: 3))
                      : null,
                ),
                child: Text(
                  '${layout.rows}×${layout.cols}',
                  style: TextStyle(
                    color: active ? amber : Colors.white,
                    fontSize: 13,
                    fontWeight: active ? FontWeight.w700 : FontWeight.w500,
                  ),
                ),
              ),
            );
          })
          .toList(growable: false),
      child: Container(
        height: 40,
        padding: const EdgeInsets.symmetric(horizontal: 12),
        decoration: BoxDecoration(
          color: const Color(0x99112F49),
          borderRadius: BorderRadius.circular(8),
          border: Border.all(color: amber.withValues(alpha: .48)),
        ),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            const Icon(Icons.grid_view_rounded, color: amber, size: 17),
            const SizedBox(width: 8),
            const Text(
              '开窗布局',
              style: TextStyle(fontSize: 12, fontWeight: FontWeight.w700),
            ),
            const SizedBox(width: 10),
            Container(width: 1, height: 17, color: Colors.white12),
            const SizedBox(width: 10),
            Text(
              '${selected.rows}×${selected.cols}',
              style: const TextStyle(
                color: amber,
                fontSize: 12,
                fontWeight: FontWeight.w700,
              ),
            ),
            const SizedBox(width: 4),
            const Icon(Icons.expand_more_rounded, color: mutedText, size: 17),
          ],
        ),
      ),
    );
  }
}

class _FreeformCanvas extends StatefulWidget {
  const _FreeformCanvas({required this.controller, this.previewClient});
  final SiteController controller;
  final http.Client? previewClient;

  @override
  State<_FreeformCanvas> createState() => _FreeformCanvasState();
}

class _FreeformCanvasState extends State<_FreeformCanvas> {
  final GlobalKey canvasKey = GlobalKey();
  LayoutCell? hoverCell;

  Offset? _normalizedPoint(Offset globalOffset) {
    final renderObject = canvasKey.currentContext?.findRenderObject();
    if (renderObject is! RenderBox ||
        renderObject.size.width <= 0 ||
        renderObject.size.height <= 0) {
      return null;
    }
    final local = renderObject.globalToLocal(globalOffset);
    return Offset(
      (local.dx / renderObject.size.width).clamp(0.0, 1.0),
      (local.dy / renderObject.size.height).clamp(0.0, 1.0),
    );
  }

  void _setHoverCell(LayoutCell? next) {
    final changed = hoverCell?.row != next?.row || hoverCell?.col != next?.col;
    if (changed) setState(() => hoverCell = next);
  }

  @override
  Widget build(BuildContext context) {
    final placements = widget.controller.canvasPlacements;
    final selectedPlacement = widget.controller.selectedCanvasPlacement;

    return DragTarget<int>(
      onWillAcceptWithDetails: (details) =>
          widget.controller.canPlaceNodeOnCanvas(details.data),
      onMove: (details) {
        final normalized = _normalizedPoint(details.offset);
        _setHoverCell(
          normalized == null
              ? null
              : widget.controller.layoutCellAt(normalized),
        );
      },
      onLeave: (_) => _setHoverCell(null),
      onAcceptWithDetails: (details) {
        final normalized = _normalizedPoint(details.offset);
        if (normalized != null) {
          widget.controller.placeNodeOnCanvas(
            details.data,
            normalized,
            snapToLayout: true,
          );
        }
        _setHoverCell(null);
      },
      builder: (context, candidateData, rejectedData) {
        final accepting = candidateData.isNotEmpty;
        return AnimatedContainer(
          duration: const Duration(milliseconds: 220),
          padding: const EdgeInsets.all(2),
          decoration: BoxDecoration(
            color: const Color(0xFF020C16),
            borderRadius: BorderRadius.circular(15),
            border: Border.all(
              color: accepting ? cyan : mint.withValues(alpha: .45),
              width: accepting ? 2 : 1,
            ),
            boxShadow: <BoxShadow>[
              BoxShadow(
                color: (accepting ? cyan : mint).withValues(alpha: .1),
                blurRadius: accepting ? 28 : 22,
              ),
            ],
          ),
          child: ClipRRect(
            borderRadius: BorderRadius.circular(12),
            child: LayoutBuilder(
              builder: (context, constraints) {
                final canvasSize = Size(
                  constraints.maxWidth,
                  constraints.maxHeight,
                );
                return SizedBox.expand(
                  key: const ValueKey('canvas-content'),
                  child: Stack(
                    key: canvasKey,
                    clipBehavior: Clip.hardEdge,
                    children: <Widget>[
                      const Positioned.fill(
                        child: ColoredBox(
                          key: ValueKey('canvas-background-layer'),
                          color: Color(0xFF020C16),
                        ),
                      ),
                      Positioned.fill(
                        child: CustomPaint(painter: _CanvasGridPainter()),
                      ),
                      if (placements.isEmpty)
                        const Positioned.fill(
                          child: IgnorePointer(
                            child: Center(child: _EmptyCanvasHint()),
                          ),
                        ),
                      ...placements.map(
                        (placement) => _CanvasNodeLayer(
                          key: ValueKey('canvas-node-${placement.placementId}'),
                          controller: widget.controller,
                          placement: placement,
                          canvasSize: canvasSize,
                          previewClient: widget.previewClient,
                        ),
                      ),
                      Positioned.fill(
                        child: IgnorePointer(
                          child: CustomPaint(
                            painter: _LayoutGuidePainter(
                              layout: widget.controller.layout,
                              highlightedCell: hoverCell,
                            ),
                          ),
                        ),
                      ),
                      if (selectedPlacement != null)
                        Positioned(
                          left: selectedPlacement.frame.left * canvasSize.width,
                          top: selectedPlacement.frame.top * canvasSize.height,
                          width:
                              selectedPlacement.frame.width * canvasSize.width,
                          height:
                              selectedPlacement.frame.height *
                              canvasSize.height,
                          child: IgnorePointer(
                            child: Padding(
                              padding: const EdgeInsets.all(4),
                              child: DecoratedBox(
                                key: ValueKey(
                                  'canvas-selection-outline-${selectedPlacement.placementId}',
                                ),
                                decoration: BoxDecoration(
                                  border: Border.all(
                                    color: cyan.withValues(alpha: .95),
                                    width: 2,
                                  ),
                                ),
                              ),
                            ),
                          ),
                        ),
                      Positioned(
                        right: 14,
                        bottom: 12,
                        child: _CanvasBadge(
                          icon: accepting
                              ? Icons.add_circle_outline_rounded
                              : Icons.open_with_rounded,
                          label: accepting
                              ? '松开以吸附到 ${hoverCell?.id ?? '输入窗口'}'
                              : '输入开窗辅助线 · 拖动节点自动吸附',
                          color: accepting ? cyan : mutedText,
                        ),
                      ),
                    ],
                  ),
                );
              },
            ),
          ),
        );
      },
    );
  }
}

class _CanvasBadge extends StatelessWidget {
  const _CanvasBadge({
    required this.icon,
    required this.label,
    required this.color,
  });
  final IconData icon;
  final String label;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xCC071B2B),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: color.withValues(alpha: .24)),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 5),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(icon, color: color, size: 13),
            const SizedBox(width: 5),
            Text(label, style: TextStyle(color: color, fontSize: 10)),
          ],
        ),
      ),
    );
  }
}

class _EmptyCanvasHint extends StatelessWidget {
  const _EmptyCanvasHint();

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: <Widget>[
        Container(
          width: 66,
          height: 66,
          decoration: BoxDecoration(
            shape: BoxShape.circle,
            color: cyan.withValues(alpha: .08),
            border: Border.all(color: cyan.withValues(alpha: .24)),
          ),
          child: const Icon(Icons.swipe_rounded, color: cyan, size: 30),
        ),
        const SizedBox(height: 12),
        const Text(
          '从左侧拖入输入节点',
          style: TextStyle(fontSize: 14, fontWeight: FontWeight.w700),
        ),
        const SizedBox(height: 5),
        const Text(
          '节点可自由移动、缩放；双击节点可铺满画布',
          style: TextStyle(color: mutedText, fontSize: 11),
        ),
      ],
    );
  }
}

class _CanvasNodeLayer extends StatefulWidget {
  const _CanvasNodeLayer({
    super.key,
    required this.controller,
    required this.placement,
    required this.canvasSize,
    this.previewClient,
  });
  final SiteController controller;
  final CanvasNodePlacement placement;
  final Size canvasSize;
  final http.Client? previewClient;

  @override
  State<_CanvasNodeLayer> createState() => _CanvasNodeLayerState();
}

class _CanvasNodeLayerState extends State<_CanvasNodeLayer> {
  bool interacting = false;

  void _beginInteraction() {
    if (!interacting) setState(() => interacting = true);
  }

  void _endInteraction() {
    if (interacting) setState(() => interacting = false);
  }

  @override
  Widget build(BuildContext context) {
    final frame = widget.placement.frame;
    final node = widget.controller.nodeById(widget.placement.nodeId);
    if (node == null) return const SizedBox.shrink();
    return AnimatedPositioned(
      key: ValueKey('canvas-node-box-${widget.placement.placementId}'),
      duration: interacting ? Duration.zero : const Duration(milliseconds: 300),
      curve: Curves.easeOutCubic,
      left: frame.left * widget.canvasSize.width,
      top: frame.top * widget.canvasSize.height,
      width: frame.width * widget.canvasSize.width,
      height: frame.height * widget.canvasSize.height,
      child: _CanvasNodeView(
        controller: widget.controller,
        placement: widget.placement,
        node: node,
        canvasSize: widget.canvasSize,
        previewClient: widget.previewClient,
        onInteractionStart: _beginInteraction,
        onInteractionEnd: _endInteraction,
      ),
    );
  }
}

class _CanvasNodeView extends StatelessWidget {
  const _CanvasNodeView({
    required this.controller,
    required this.placement,
    required this.node,
    required this.canvasSize,
    this.previewClient,
    required this.onInteractionStart,
    required this.onInteractionEnd,
  });
  final SiteController controller;
  final CanvasNodePlacement placement;
  final MatrixNode node;
  final Size canvasSize;
  final http.Client? previewClient;
  final VoidCallback onInteractionStart;
  final VoidCallback onInteractionEnd;

  @override
  Widget build(BuildContext context) {
    final selected = controller.selectedPlacementId == placement.placementId;
    final online = node.isOnline;
    final accent = online ? cyan : mutedText;
    return MouseRegion(
      cursor: placement.isMaximized
          ? SystemMouseCursors.basic
          : SystemMouseCursors.move,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: () => controller.selectCanvasPlacement(placement.placementId),
        onDoubleTap: () =>
            controller.toggleCanvasNodeMaximized(placement.placementId),
        onPanStart: placement.isMaximized
            ? null
            : (_) {
                onInteractionStart();
                controller.selectCanvasPlacement(placement.placementId);
              },
        onPanUpdate: placement.isMaximized
            ? null
            : (details) => controller.moveCanvasNode(
                placement.placementId,
                Offset(
                  details.delta.dx / canvasSize.width,
                  details.delta.dy / canvasSize.height,
                ),
              ),
        onPanEnd: placement.isMaximized ? null : (_) => onInteractionEnd(),
        child: AnimatedContainer(
          duration: const Duration(milliseconds: 220),
          curve: Curves.easeOutCubic,
          decoration: BoxDecoration(
            // Keep all placements on one continuous wall surface. The
            // layout painter supplies the only visible screen seams.
            color: online ? const Color(0xFF062F4E) : const Color(0xFF0B293E),
          ),
          child: Stack(
            children: <Widget>[
              Positioned.fill(
                child: _CanvasSignalPreview(
                  key: ValueKey('canvas-live-preview-${placement.placementId}'),
                  node: node,
                  client: previewClient,
                ),
              ),
              Positioned(
                left: 10,
                top: 8,
                child: _PriorityBadge(priorityLabel: placement.priorityLabel),
              ),
              if (!node.signalSourceConfigured)
                Positioned(
                  left: 13,
                  top: 38,
                  child: Icon(
                    online
                        ? Icons.desktop_windows_outlined
                        : Icons.desktop_access_disabled_outlined,
                    color: accent,
                    size: 24,
                  ),
                ),
              Positioned(
                right: 28,
                top: 22,
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    SizedBox(
                      width: 27,
                      height: 27,
                      child: IconButton(
                        padding: EdgeInsets.zero,
                        tooltip: placement.isMaximized ? '双击还原' : '铺满画布',
                        onPressed: () => controller.toggleCanvasNodeMaximized(
                          placement.placementId,
                        ),
                        icon: Icon(
                          placement.isMaximized
                              ? Icons.fullscreen_exit_rounded
                              : Icons.fullscreen_rounded,
                          color: cyan,
                          size: 16,
                        ),
                      ),
                    ),
                    SizedBox(
                      width: 25,
                      height: 27,
                      child: IconButton(
                        padding: EdgeInsets.zero,
                        tooltip: '移除节点',
                        onPressed: () => controller.removeNodeFromCanvas(
                          placement.placementId,
                        ),
                        icon: const Icon(
                          Icons.close_rounded,
                          color: mutedText,
                          size: 15,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
              if (!node.signalSourceConfigured)
                Center(
                  child: Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 44),
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: <Widget>[
                        Text(
                          node.ip,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: TextStyle(
                            color: online ? Colors.white : mutedText,
                            fontSize: placement.isMaximized ? 18 : 15,
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                        const SizedBox(height: 5),
                        const Text(
                          '默认背景 · 未配置信号来源',
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: TextStyle(color: amber, fontSize: 10),
                        ),
                      ],
                    ),
                  ),
                ),
              Positioned(
                left: 10,
                bottom: 8,
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: const Color(0xB2071928),
                    borderRadius: BorderRadius.circular(4),
                  ),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 6,
                      vertical: 3,
                    ),
                    child: Text(
                      '输入窗 ${controller.layoutContainerIdForPlacement(placement)} · '
                      '${(placement.frame.width * 100).round()}% × '
                      '${(placement.frame.height * 100).round()}%',
                      style: const TextStyle(
                        color: Color(0xFFD1DFEA),
                        fontSize: 9,
                      ),
                    ),
                  ),
                ),
              ),
              if (selected && !placement.isMaximized) ..._resizeHandles(),
            ],
          ),
        ),
      ),
    );
  }

  List<Widget> _resizeHandles() {
    return <Widget>[
      _ResizeHandle(
        alignment: Alignment.topCenter,
        cursor: SystemMouseCursors.resizeUp,
        handle: CanvasResizeHandle.top,
        width: double.infinity,
        height: 14,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
      _ResizeHandle(
        alignment: Alignment.centerRight,
        cursor: SystemMouseCursors.resizeRight,
        handle: CanvasResizeHandle.right,
        width: 14,
        height: double.infinity,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
      _ResizeHandle(
        alignment: Alignment.bottomCenter,
        cursor: SystemMouseCursors.resizeDown,
        handle: CanvasResizeHandle.bottom,
        width: double.infinity,
        height: 14,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
      _ResizeHandle(
        alignment: Alignment.centerLeft,
        cursor: SystemMouseCursors.resizeLeft,
        handle: CanvasResizeHandle.left,
        width: 14,
        height: double.infinity,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
      _ResizeHandle(
        alignment: Alignment.topLeft,
        cursor: SystemMouseCursors.resizeUpLeft,
        handle: CanvasResizeHandle.topLeft,
        width: 24,
        height: 24,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
      _ResizeHandle(
        alignment: Alignment.topRight,
        cursor: SystemMouseCursors.resizeUpRight,
        handle: CanvasResizeHandle.topRight,
        width: 24,
        height: 24,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
      _ResizeHandle(
        alignment: Alignment.bottomLeft,
        cursor: SystemMouseCursors.resizeDownLeft,
        handle: CanvasResizeHandle.bottomLeft,
        width: 24,
        height: 24,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
      _ResizeHandle(
        alignment: Alignment.bottomRight,
        cursor: SystemMouseCursors.resizeDownRight,
        handle: CanvasResizeHandle.bottomRight,
        width: 24,
        height: 24,
        controller: controller,
        placementId: placement.placementId,
        canvasSize: canvasSize,
        onStart: onInteractionStart,
        onEnd: onInteractionEnd,
      ),
    ];
  }
}

class _CanvasSignalPreview extends StatefulWidget {
  const _CanvasSignalPreview({super.key, required this.node, this.client});

  final MatrixNode node;
  final http.Client? client;

  @override
  State<_CanvasSignalPreview> createState() => _CanvasSignalPreviewState();
}

class _CanvasSignalPreviewState extends State<_CanvasSignalPreview> {
  bool get _canLoad =>
      widget.node.isOnline &&
      widget.node.signalSourceConfigured &&
      widget.node.signalPreviewUri != null;

  @override
  Widget build(BuildContext context) {
    if (!widget.node.signalSourceConfigured) {
      return ColoredBox(
        color: widget.node.isOnline
            ? const Color(0xFF062F4E)
            : const Color(0xFF0B293E),
      );
    }
    final canLoad = _canLoad;
    return ColoredBox(
      color: widget.node.isOnline
          ? const Color(0xFF062F4E)
          : const Color(0xFF0B293E),
      child: canLoad
          ? Stack(
              key: ValueKey('canvas-preview-frame-${widget.node.nodeId}'),
              fit: StackFit.expand,
              children: <Widget>[
                Positioned.fill(
                  child: LiveJpegPreview(
                    uri: widget.node.signalPreviewUri!,
                    client: widget.client,
                    interval: const Duration(milliseconds: 250),
                    // Every input window shows the complete source frame.
                    // The placement controls only the window's geometry.
                    fit: BoxFit.fill,
                    placeholderBuilder: (context, waiting) =>
                        _previewPlaceholder(
                          waiting
                              ? '等待 ${widget.node.signalSourceLabel} 画面'
                              : '实时预览连接失败',
                          cyan,
                          Icons.videocam_outlined,
                        ),
                  ),
                ),
                const DecoratedBox(
                  decoration: BoxDecoration(
                    gradient: LinearGradient(
                      begin: Alignment.topCenter,
                      end: Alignment.bottomCenter,
                      colors: <Color>[
                        Color(0x66030C13),
                        Colors.transparent,
                        Color(0x77030C13),
                      ],
                      stops: <double>[0, .35, 1],
                    ),
                  ),
                ),
              ],
            )
          : _previewPlaceholder(
              widget.node.isOnline ? '节点未启用实时预览' : '节点离线',
              mutedText,
              Icons.wallpaper_outlined,
            ),
    );
  }

  Widget _previewPlaceholder(String text, Color color, IconData icon) {
    return Center(
      key: const ValueKey('canvas-preview-placeholder'),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 38),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(icon, color: color, size: 28),
            const SizedBox(height: 7),
            Text(
              text,
              maxLines: 2,
              overflow: TextOverflow.ellipsis,
              textAlign: TextAlign.center,
              style: TextStyle(
                color: color,
                fontSize: 11,
                fontWeight: FontWeight.w600,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ResizeHandle extends StatelessWidget {
  const _ResizeHandle({
    required this.alignment,
    required this.cursor,
    required this.handle,
    required this.width,
    required this.height,
    required this.controller,
    required this.placementId,
    required this.canvasSize,
    required this.onStart,
    required this.onEnd,
  });
  final Alignment alignment;
  final MouseCursor cursor;
  final CanvasResizeHandle handle;
  final double width;
  final double height;
  final SiteController controller;
  final int placementId;
  final Size canvasSize;
  final VoidCallback onStart;
  final VoidCallback onEnd;

  @override
  Widget build(BuildContext context) {
    return Align(
      key: ValueKey('canvas-resize-${handle.name}-$placementId'),
      alignment: alignment,
      child: MouseRegion(
        cursor: cursor,
        child: GestureDetector(
          behavior: HitTestBehavior.opaque,
          onPanStart: (_) => onStart(),
          onPanUpdate: (details) => controller.resizeCanvasNode(
            placementId,
            handle,
            Offset(
              details.delta.dx / canvasSize.width,
              details.delta.dy / canvasSize.height,
            ),
          ),
          onPanEnd: (_) => onEnd(),
          onPanCancel: onEnd,
          child: SizedBox(width: width, height: height),
        ),
      ),
    );
  }
}

class _PriorityBadge extends StatelessWidget {
  const _PriorityBadge({required this.priorityLabel});

  final String priorityLabel;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xD9082237),
        borderRadius: BorderRadius.circular(5),
        border: Border.all(color: cyan.withValues(alpha: .5)),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 3),
        child: Text(
          '优先级 $priorityLabel',
          style: const TextStyle(
            color: cyan,
            fontSize: 9,
            fontWeight: FontWeight.w800,
          ),
        ),
      ),
    );
  }
}

class _CanvasGridPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final gridPaint = Paint()
      ..color = Colors.white.withValues(alpha: .035)
      ..strokeWidth = 1;
    for (double x = 0; x <= size.width; x += 28) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), gridPaint);
    }
    for (double y = 0; y <= size.height; y += 28) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

class _LayoutGuidePainter extends CustomPainter {
  const _LayoutGuidePainter({required this.layout, this.highlightedCell});

  final MatrixLayout layout;
  final LayoutCell? highlightedCell;

  @override
  void paint(Canvas canvas, Size size) {
    final rows = layout.rows.clamp(1, 12).toInt();
    final cols = layout.cols.clamp(1, 12).toInt();
    // Keep the guide in the same normalized coordinate space as the node
    // layers so grid seams line up with the actual playback coordinates.
    final wall = Offset.zero & size;
    final cellWidth = wall.width / cols;
    final cellHeight = wall.height / rows;

    final highlighted = highlightedCell;
    if (highlighted != null) {
      final highlightRect = Rect.fromLTWH(
        wall.left + highlighted.col * cellWidth,
        wall.top + highlighted.row * cellHeight,
        cellWidth,
        cellHeight,
      );
      canvas.drawRect(
        highlightRect,
        Paint()..color = cyan.withValues(alpha: .12),
      );
    }

    final outerPaint = Paint()
      ..color = mint.withValues(alpha: .94)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.8;
    canvas.drawRect(wall, outerPaint);

    final seamPaint = Paint()
      ..color = Colors.white.withValues(alpha: .72)
      ..strokeWidth = 1.2;
    for (var col = 1; col < cols; col++) {
      final x = wall.left + col * cellWidth;
      _drawDashedLine(
        canvas,
        Offset(x, wall.top),
        Offset(x, wall.bottom),
        seamPaint,
      );
    }
    for (var row = 1; row < rows; row++) {
      final y = wall.top + row * cellHeight;
      _drawDashedLine(
        canvas,
        Offset(wall.left, y),
        Offset(wall.right, y),
        seamPaint,
      );
    }
  }

  void _drawDashedLine(Canvas canvas, Offset start, Offset end, Paint paint) {
    const dash = 7.0;
    const gap = 5.0;
    final distance = (end - start).distance;
    final direction = (end - start) / distance;
    for (var offset = 0.0; offset < distance; offset += dash + gap) {
      final from = start + direction * offset;
      final to = start + direction * (offset + dash).clamp(0.0, distance);
      canvas.drawLine(from, to, paint);
    }
  }

  @override
  bool shouldRepaint(covariant _LayoutGuidePainter oldDelegate) {
    return oldDelegate.layout.rows != layout.rows ||
        oldDelegate.layout.cols != layout.cols ||
        oldDelegate.layout.screenWidth != layout.screenWidth ||
        oldDelegate.layout.screenHeight != layout.screenHeight ||
        oldDelegate.highlightedCell?.row != highlightedCell?.row ||
        oldDelegate.highlightedCell?.col != highlightedCell?.col;
  }
}
