import 'package:flutter/material.dart';

import '../controllers/site_controller.dart';
import '../models/matrix_models.dart';
import '../theme/app_theme.dart';
import 'tech_components.dart';

class ScenePanel extends StatelessWidget {
  const ScenePanel({
    super.key,
    required this.controller,
    this.horizontal = false,
  });

  final SiteController controller;
  final bool horizontal;

  @override
  Widget build(BuildContext context) {
    final cards = controller.presets
        .map(
          (preset) => _PresetCard(
            controller: controller,
            preset: preset,
            width: horizontal ? 190 : null,
          ),
        )
        .toList(growable: false);

    return TechPanel(
      padding: const EdgeInsets.all(14),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        mainAxisSize: horizontal ? MainAxisSize.min : MainAxisSize.max,
        children: <Widget>[
          SectionTitle(
            title: '输入开窗预设',
            subtitle: '一键切换客户端输入窗口',
            icon: Icons.auto_awesome_mosaic_rounded,
            trailing: Icon(
              Icons.bolt_rounded,
              color: amber.withValues(alpha: .85),
              size: 20,
            ),
          ),
          const SizedBox(height: 13),
          if (horizontal)
            SizedBox(
              height: 142,
              child: ListView.separated(
                scrollDirection: Axis.horizontal,
                itemCount: cards.length,
                separatorBuilder: (_, _) => const SizedBox(width: 10),
                itemBuilder: (context, index) => cards[index],
              ),
            )
          else
            Expanded(
              child: cards.isEmpty
                  ? const _EmptyPresetState()
                  : ListView.separated(
                      itemCount: cards.length,
                      separatorBuilder: (_, _) => const SizedBox(height: 11),
                      itemBuilder: (context, index) => cards[index],
                    ),
            ),
        ],
      ),
    );
  }
}

class _PresetCard extends StatefulWidget {
  const _PresetCard({
    required this.controller,
    required this.preset,
    this.width,
  });

  final SiteController controller;
  final LayoutPreset preset;
  final double? width;

  @override
  State<_PresetCard> createState() => _PresetCardState();
}

class _PresetCardState extends State<_PresetCard> {
  bool hovered = false;

  @override
  Widget build(BuildContext context) {
    final selected = widget.controller.activePresetName == widget.preset.name;
    final accent = Color(widget.preset.accent);
    return SizedBox(
      width: widget.width,
      child: MouseRegion(
        onEnter: (_) => setState(() => hovered = true),
        onExit: (_) => setState(() => hovered = false),
        cursor: SystemMouseCursors.click,
        child: GestureDetector(
          onTap: () => widget.controller.applyPreset(widget.preset),
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 210),
            curve: Curves.easeOutCubic,
            padding: const EdgeInsets.all(11),
            decoration: BoxDecoration(
              color: selected
                  ? accent.withValues(alpha: .13)
                  : const Color(0x88132F47),
              borderRadius: BorderRadius.circular(14),
              border: Border.all(
                color: selected || hovered
                    ? accent.withValues(alpha: .75)
                    : Colors.white.withValues(alpha: .06),
              ),
              boxShadow: selected
                  ? <BoxShadow>[
                      BoxShadow(
                        color: accent.withValues(alpha: .12),
                        blurRadius: 18,
                      ),
                    ]
                  : null,
            ),
            child: Row(
              children: <Widget>[
                SizedBox(
                  width: 70,
                  height: 54,
                  child: _MiniGrid(
                    layout: widget.preset.layout,
                    accent: accent,
                  ),
                ),
                const SizedBox(width: 11),
                Expanded(
                  child: Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: <Widget>[
                      Text(
                        widget.preset.name,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(
                          fontSize: 12.5,
                          fontWeight: FontWeight.w700,
                        ),
                      ),
                      const SizedBox(height: 5),
                      Text(
                        '${widget.preset.layout.rows} × ${widget.preset.layout.cols} · ${widget.preset.layout.outputWidth}×${widget.preset.layout.outputHeight}',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(color: mutedText, fontSize: 9.5),
                      ),
                      const SizedBox(height: 7),
                      Text(
                        selected ? '当前启用' : '点击应用',
                        style: TextStyle(
                          color: selected ? accent : mutedText,
                          fontSize: 10,
                        ),
                      ),
                    ],
                  ),
                ),
                Column(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    Icon(
                      selected
                          ? Icons.check_circle_rounded
                          : Icons.chevron_right_rounded,
                      color: selected ? accent : mutedText,
                      size: 18,
                    ),
                    IconButton(
                      padding: EdgeInsets.zero,
                      constraints: const BoxConstraints.tightFor(
                        width: 30,
                        height: 30,
                      ),
                      splashRadius: 15,
                      tooltip: '删除预设布局',
                      onPressed: () =>
                          widget.controller.removePreset(widget.preset),
                      icon: Icon(
                        Icons.delete_outline_rounded,
                        color: danger.withValues(alpha: .82),
                        size: 16,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _EmptyPresetState extends StatelessWidget {
  const _EmptyPresetState();

  @override
  Widget build(BuildContext context) {
    return const Center(
      child: Text(
        '暂无预设布局',
        textAlign: TextAlign.center,
        style: TextStyle(color: mutedText, fontSize: 12, height: 1.6),
      ),
    );
  }
}

class _MiniGrid extends StatelessWidget {
  const _MiniGrid({required this.layout, required this.accent});

  final MatrixLayout layout;
  final Color accent;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(3),
      decoration: BoxDecoration(
        color: const Color(0xFF071522),
        borderRadius: BorderRadius.circular(7),
        border: Border.all(color: accent.withValues(alpha: .35)),
      ),
      child: GridView.builder(
        padding: EdgeInsets.zero,
        physics: const NeverScrollableScrollPhysics(),
        gridDelegate: SliverGridDelegateWithFixedCrossAxisCount(
          crossAxisCount: layout.cols,
          crossAxisSpacing: 2,
          mainAxisSpacing: 2,
        ),
        itemCount: layout.rows * layout.cols,
        itemBuilder: (_, index) => DecoratedBox(
          decoration: BoxDecoration(
            color: accent.withValues(alpha: index == 0 ? .48 : .24),
            borderRadius: BorderRadius.circular(2),
          ),
        ),
      ),
    );
  }
}

Future<void> showSavePresetDialog(
  BuildContext context,
  SiteController controller,
) async {
  final name = TextEditingController(text: '预设1');
  String? validationMessage;
  final result = await showDialog<String>(
    context: context,
    builder: (context) => _LayoutDialogControllerOwner(
      controllers: <TextEditingController>[name],
      child: StatefulBuilder(
        builder: (context, setDialogState) => AlertDialog(
          backgroundColor: const Color(0xFF09243A),
          title: const Text('保存布局预设'),
          content: SizedBox(
            width: 360,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: <Widget>[
                TextField(
                  key: const ValueKey('current-layout-preset-name'),
                  controller: name,
                  autofocus: true,
                  maxLength: 24,
                  onSubmitted: (_) {
                    final value = name.text.trim();
                    if (value.isEmpty) {
                      setDialogState(() => validationMessage = '请输入预设名称');
                    } else {
                      Navigator.pop(context, value);
                    }
                  },
                  decoration: const InputDecoration(
                    labelText: '预设名称',
                    hintText: '例如：主舞台布局',
                    counterText: '',
                  ),
                ),
                if (validationMessage != null) ...<Widget>[
                  const SizedBox(height: 8),
                  Align(
                    alignment: Alignment.centerLeft,
                    child: Text(
                      validationMessage!,
                      style: const TextStyle(color: danger, fontSize: 11),
                    ),
                  ),
                ],
              ],
            ),
          ),
          actions: <Widget>[
            TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text('取消'),
            ),
            FilledButton.icon(
              key: const ValueKey('confirm-save-current-layout'),
              onPressed: () {
                final value = name.text.trim();
                if (value.isEmpty) {
                  setDialogState(() => validationMessage = '请输入预设名称');
                  return;
                }
                Navigator.pop(context, value);
              },
              icon: const Icon(Icons.bookmark_add_outlined, size: 17),
              label: const Text('保存'),
            ),
          ],
        ),
      ),
    ),
  );
  if (result != null) {
    await controller.saveLayoutPreset(result, controller.layout);
  }
}

Future<void> showLayoutDialog(
  BuildContext context,
  SiteController controller,
) async {
  final presetName = TextEditingController(
    text: '自定义 ${controller.layout.rows}×${controller.layout.cols}',
  );
  final rows = TextEditingController(text: controller.layout.rows.toString());
  final cols = TextEditingController(text: controller.layout.cols.toString());
  final width = TextEditingController(
    text: controller.layout.screenWidth.toString(),
  );
  final height = TextEditingController(
    text: controller.layout.screenHeight.toString(),
  );
  String? validationMessage;
  final result = await showDialog<_LayoutDialogResult>(
    context: context,
    builder: (context) => _LayoutDialogControllerOwner(
      controllers: <TextEditingController>[
        presetName,
        rows,
        cols,
        width,
        height,
      ],
      child: StatefulBuilder(
        builder: (context, setDialogState) {
          void submit({required bool save}) {
            final value = MatrixLayout(
              rows: int.tryParse(rows.text) ?? 0,
              cols: int.tryParse(cols.text) ?? 0,
              screenWidth: int.tryParse(width.text) ?? 0,
              screenHeight: int.tryParse(height.text) ?? 0,
            );
            String? error;
            if (save && presetName.text.trim().isEmpty) {
              error = '请输入预设名称';
            } else if (value.rows < 1 ||
                value.rows > 12 ||
                value.cols < 1 ||
                value.cols > 12) {
              error = '行数和列数必须为 1 至 12';
            } else if (value.screenWidth < 1 || value.screenHeight < 1) {
              error = '单屏宽度和高度必须大于 0';
            }
            if (error != null) {
              setDialogState(() => validationMessage = error);
              return;
            }
            Navigator.pop(
              context,
              _LayoutDialogResult(
                layout: value,
                presetName: presetName.text.trim(),
                save: save,
              ),
            );
          }

          return AlertDialog(
            backgroundColor: const Color(0xFF09243A),
            title: const Text('自定义输入开窗布局'),
            content: SizedBox(
              width: 420,
              child: SingleChildScrollView(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    TextField(
                      key: const ValueKey('layout-preset-name'),
                      controller: presetName,
                      maxLength: 24,
                      decoration: const InputDecoration(
                        labelText: '预设名称',
                        hintText: '例如：主舞台布局',
                        counterText: '',
                      ),
                    ),
                    const SizedBox(height: 12),
                    Row(
                      children: <Widget>[
                        Expanded(
                          child: TextField(
                            key: const ValueKey('layout-rows'),
                            controller: rows,
                            keyboardType: TextInputType.number,
                            decoration: const InputDecoration(labelText: '行数'),
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: TextField(
                            key: const ValueKey('layout-cols'),
                            controller: cols,
                            keyboardType: TextInputType.number,
                            decoration: const InputDecoration(labelText: '列数'),
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    Row(
                      children: <Widget>[
                        Expanded(
                          child: TextField(
                            key: const ValueKey('layout-screen-width'),
                            controller: width,
                            keyboardType: TextInputType.number,
                            decoration: const InputDecoration(
                              labelText: '画布参考宽度',
                            ),
                          ),
                        ),
                        const SizedBox(width: 12),
                        Expanded(
                          child: TextField(
                            key: const ValueKey('layout-screen-height'),
                            controller: height,
                            keyboardType: TextInputType.number,
                            decoration: const InputDecoration(
                              labelText: '画布参考高度',
                            ),
                          ),
                        ),
                      ],
                    ),
                    if (validationMessage != null) ...<Widget>[
                      const SizedBox(height: 10),
                      Align(
                        alignment: Alignment.centerLeft,
                        child: Text(
                          validationMessage!,
                          style: const TextStyle(color: danger, fontSize: 11),
                        ),
                      ),
                    ],
                  ],
                ),
              ),
            ),
            actions: <Widget>[
              TextButton(
                onPressed: () => Navigator.pop(context),
                child: const Text('取消'),
              ),
              TextButton(
                key: const ValueKey('apply-layout-only'),
                onPressed: () => submit(save: false),
                child: const Text('仅应用'),
              ),
              FilledButton.icon(
                key: const ValueKey('save-layout-preset'),
                onPressed: () => submit(save: true),
                icon: const Icon(Icons.bookmark_add_outlined, size: 17),
                label: const Text('保存并应用'),
              ),
            ],
          );
        },
      ),
    ),
  );
  if (result != null) {
    await controller.applyLayout(result.layout);
    if (result.save) {
      await controller.saveLayoutPreset(result.presetName, result.layout);
    }
  }
}

class _LayoutDialogControllerOwner extends StatefulWidget {
  const _LayoutDialogControllerOwner({
    required this.controllers,
    required this.child,
  });

  final List<TextEditingController> controllers;
  final Widget child;

  @override
  State<_LayoutDialogControllerOwner> createState() =>
      _LayoutDialogControllerOwnerState();
}

class _LayoutDialogControllerOwnerState
    extends State<_LayoutDialogControllerOwner> {
  @override
  void dispose() {
    for (final controller in widget.controllers) {
      controller.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => widget.child;
}

class _LayoutDialogResult {
  const _LayoutDialogResult({
    required this.layout,
    required this.presetName,
    required this.save,
  });

  final MatrixLayout layout;
  final String presetName;
  final bool save;
}
