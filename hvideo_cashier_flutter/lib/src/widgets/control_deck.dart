import 'package:flutter/material.dart';

import '../controllers/site_controller.dart';
import '../models/matrix_models.dart';
import '../theme/app_theme.dart';
import 'tech_components.dart';

class ControlDeck extends StatelessWidget {
  const ControlDeck({super.key, required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    return TechPanel(
      padding: const EdgeInsets.symmetric(horizontal: 15, vertical: 13),
      child: LayoutBuilder(
        builder: (context, constraints) {
          final compact = constraints.maxWidth < 760;
          final controls = _PlaybackRow(controller: controller);
          final actions = _QuickActions(controller: controller);
          if (compact) {
            return Column(
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: <Widget>[
                controls,
                const SizedBox(height: 10),
                Align(alignment: Alignment.centerRight, child: actions),
                _LiveProgress(controller: controller),
              ],
            );
          }
          return Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: <Widget>[
              Row(
                children: <Widget>[
                  Expanded(child: controls),
                  const SizedBox(width: 12),
                  actions,
                ],
              ),
              _LiveProgress(controller: controller),
            ],
          );
        },
      ),
    );
  }
}

class _PlaybackRow extends StatelessWidget {
  const _PlaybackRow({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    return Wrap(
      spacing: 8,
      runSpacing: 6,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: <Widget>[
        _RoundButton(
          icon: controller.isPlaying
              ? Icons.pause_rounded
              : Icons.play_arrow_rounded,
          tooltip: controller.isPlaying ? '暂停' : '播放',
          color: cyan,
          onTap: controller.busy ? null : controller.togglePlayback,
          size: 48,
        ),
        const SizedBox(width: 1),
        _RoundButton(
          icon: Icons.stop_rounded,
          tooltip: '停止',
          color: danger,
          onTap: controller.busy ? null : controller.stop,
        ),
        _VolumeControl(controller: controller),
        SegmentedButton<AudioOutputMode>(
          showSelectedIcon: false,
          segments: const <ButtonSegment<AudioOutputMode>>[
            ButtonSegment<AudioOutputMode>(
              value: AudioOutputMode.hdmi,
              label: Text('HDMI'),
            ),
            ButtonSegment<AudioOutputMode>(
              value: AudioOutputMode.analog,
              label: Text('3.5mm'),
            ),
            ButtonSegment<AudioOutputMode>(
              value: AudioOutputMode.both,
              label: Text('BOTH'),
            ),
          ],
          selected: <AudioOutputMode>{controller.audioOutputMode},
          onSelectionChanged: controller.busy || !controller.isConnected
              ? null
              : (selection) => controller.setAudioOutput(selection.single),
        ),
      ],
    );
  }
}

class _VolumeControl extends StatelessWidget {
  const _VolumeControl({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    final enabled = controller.isConnected && !controller.busy;
    return Container(
      height: 42,
      padding: const EdgeInsets.symmetric(horizontal: 4),
      decoration: BoxDecoration(
        color: const Color(0x66122F48),
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: Colors.white.withValues(alpha: .08)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          IconButton(
            key: const ValueKey('volume-mute-button'),
            tooltip: controller.isMuted ? '取消静音' : '静音',
            onPressed: enabled ? controller.toggleMute : null,
            icon: Icon(
              controller.isMuted
                  ? Icons.volume_off_rounded
                  : Icons.volume_up_rounded,
              color: enabled ? cyan : mutedText,
              size: 21,
            ),
            visualDensity: VisualDensity.compact,
          ),
          IconButton(
            key: const ValueKey('volume-down-button'),
            tooltip: '降低音量',
            onPressed: enabled && controller.audioVolumePercent > 0
                ? () => controller.adjustAudioVolume(-5)
                : null,
            icon: const Icon(Icons.remove_rounded, size: 18),
            visualDensity: VisualDensity.compact,
          ),
          SizedBox(
            width: 132,
            child: Slider(
              key: const ValueKey('volume-slider'),
              value: controller.audioVolumePercent / 100,
              onChanged: enabled ? controller.previewAudioVolume : null,
              onChangeEnd: enabled
                  ? (value) =>
                        controller.setAudioVolumePercent((value * 100).round())
                  : null,
            ),
          ),
          IconButton(
            key: const ValueKey('volume-up-button'),
            tooltip: '提高音量',
            onPressed: enabled && controller.audioVolumePercent < 100
                ? () => controller.adjustAudioVolume(5)
                : null,
            icon: const Icon(Icons.add_rounded, size: 18),
            visualDensity: VisualDensity.compact,
          ),
          SizedBox(
            width: 38,
            child: Text(
              '${controller.audioVolumePercent}%',
              textAlign: TextAlign.center,
              style: const TextStyle(
                color: mutedText,
                fontSize: 11,
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _QuickActions extends StatelessWidget {
  const _QuickActions({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    return Wrap(
      spacing: 8,
      runSpacing: 8,
      alignment: WrapAlignment.end,
      children: <Widget>[
        AnimatedActionButton(
          key: const ValueKey('clear-canvas-button'),
          label: '清屏',
          icon: Icons.layers_clear_rounded,
          compact: true,
          tooltip: '清空输入画布',
          onPressed: controller.placedNodeCount == 0
              ? null
              : controller.clearCanvas,
        ),
        AnimatedActionButton(
          key: const ValueKey('fullscreen-layout-button'),
          label: '铺满屏',
          icon: Icons.crop_landscape_rounded,
          compact: true,
          onPressed: () => controller.applyLayout(
            const MatrixLayout(
              rows: 1,
              cols: 1,
              screenWidth: 1920,
              screenHeight: 1080,
            ),
          ),
        ),
      ],
    );
  }
}

class _LiveProgress extends StatelessWidget {
  const _LiveProgress({required this.controller});

  final SiteController controller;

  @override
  Widget build(BuildContext context) {
    final playing = controller.isPlaying;
    final captureActive = controller.hasActiveSignal;
    final configured = controller.hasConfiguredSignal;
    final color = playing || captureActive
        ? cyan
        : configured
        ? amber
        : mutedText;
    final status = playing
        ? '播放中'
        : captureActive
        ? 'LIVE'
        : configured
        ? '等待信号'
        : '未配置信号';
    return Padding(
      padding: const EdgeInsets.only(top: 11),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: <Widget>[
          Row(
            children: <Widget>[
              const Text(
                '视频进度',
                style: TextStyle(color: mutedText, fontSize: 10.5),
              ),
              const Spacer(),
              AnimatedSwitcher(
                duration: const Duration(milliseconds: 180),
                child: Text(
                  status,
                  key: ValueKey<String>(status),
                  style: TextStyle(
                    color: color,
                    fontSize: 10.5,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ),
            ],
          ),
          const SizedBox(height: 6),
          ClipRRect(
            borderRadius: BorderRadius.circular(2),
            child: LinearProgressIndicator(
              key: const ValueKey('live-progress'),
              value: playing
                  ? null
                  : captureActive
                  ? 1
                  : 0,
              minHeight: 4,
              color: color,
              backgroundColor: Colors.white.withValues(alpha: .08),
            ),
          ),
        ],
      ),
    );
  }
}

class _RoundButton extends StatefulWidget {
  const _RoundButton({
    required this.icon,
    required this.tooltip,
    required this.color,
    required this.onTap,
    this.size = 40,
  });

  final IconData icon;
  final String tooltip;
  final Color color;
  final VoidCallback? onTap;
  final double size;

  @override
  State<_RoundButton> createState() => _RoundButtonState();
}

class _RoundButtonState extends State<_RoundButton> {
  bool pressed = false;

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: widget.tooltip,
      child: GestureDetector(
        onTapDown: widget.onTap == null
            ? null
            : (_) => setState(() => pressed = true),
        onTapCancel: widget.onTap == null
            ? null
            : () => setState(() => pressed = false),
        onTapUp: widget.onTap == null
            ? null
            : (_) {
                setState(() => pressed = false);
                widget.onTap?.call();
              },
        child: AnimatedScale(
          scale: pressed ? .9 : 1,
          duration: const Duration(milliseconds: 100),
          child: Container(
            width: widget.size,
            height: widget.size,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: widget.color.withValues(alpha: .15),
              border: Border.all(color: widget.color.withValues(alpha: .62)),
              boxShadow: <BoxShadow>[
                BoxShadow(
                  color: widget.color.withValues(alpha: .14),
                  blurRadius: 15,
                ),
              ],
            ),
            child: Icon(
              widget.icon,
              color: widget.onTap == null ? mutedText : widget.color,
              size: widget.size * .5,
            ),
          ),
        ),
      ),
    );
  }
}
