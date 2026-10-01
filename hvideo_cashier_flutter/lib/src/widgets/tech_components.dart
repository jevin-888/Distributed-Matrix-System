import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

class TechPanel extends StatelessWidget {
  const TechPanel({
    super.key,
    required this.child,
    this.padding = const EdgeInsets.all(16),
    this.borderRadius = 20,
    this.accent,
  });

  final Widget child;
  final EdgeInsetsGeometry padding;
  final double borderRadius;
  final Color? accent;

  @override
  Widget build(BuildContext context) {
    final edge = accent ?? cyan;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: panelColor,
        borderRadius: BorderRadius.circular(borderRadius),
        border: Border.all(color: edge.withValues(alpha: .22)),
        boxShadow: <BoxShadow>[
          BoxShadow(
            color: Colors.black.withValues(alpha: .24),
            blurRadius: 26,
            offset: const Offset(0, 12),
          ),
          BoxShadow(color: edge.withValues(alpha: .045), blurRadius: 24),
        ],
      ),
      child: ClipRRect(
        borderRadius: BorderRadius.circular(borderRadius),
        child: Stack(
          children: <Widget>[
            Positioned(
              left: 22,
              right: 22,
              top: 0,
              child: Container(
                height: 1,
                decoration: BoxDecoration(
                  gradient: LinearGradient(
                    colors: <Color>[
                      Colors.transparent,
                      edge.withValues(alpha: .8),
                      Colors.transparent,
                    ],
                  ),
                ),
              ),
            ),
            Padding(padding: padding, child: child),
          ],
        ),
      ),
    );
  }
}

class SectionTitle extends StatelessWidget {
  const SectionTitle({
    super.key,
    required this.title,
    this.subtitle,
    this.trailing,
    this.icon,
  });

  final String title;
  final String? subtitle;
  final Widget? trailing;
  final IconData? icon;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: <Widget>[
        if (icon != null) ...<Widget>[
          Container(
            width: 34,
            height: 34,
            decoration: BoxDecoration(
              color: cyan.withValues(alpha: .1),
              borderRadius: BorderRadius.circular(10),
              border: Border.all(color: cyan.withValues(alpha: .2)),
            ),
            child: Icon(icon, color: cyan, size: 18),
          ),
          const SizedBox(width: 10),
        ],
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: <Widget>[
              Text(title, style: Theme.of(context).textTheme.titleMedium),
              if (subtitle != null)
                Padding(
                  padding: const EdgeInsets.only(top: 2),
                  child: Text(
                    subtitle!,
                    style: const TextStyle(color: mutedText, fontSize: 11),
                  ),
                ),
            ],
          ),
        ),
        if (trailing != null) ...<Widget>[trailing!],
      ],
    );
  }
}

class AnimatedActionButton extends StatefulWidget {
  const AnimatedActionButton({
    super.key,
    required this.label,
    required this.icon,
    required this.onPressed,
    this.accent = cyan,
    this.filled = false,
    this.compact = false,
    this.tooltip,
  });

  final String label;
  final IconData icon;
  final VoidCallback? onPressed;
  final Color accent;
  final bool filled;
  final bool compact;
  final String? tooltip;

  @override
  State<AnimatedActionButton> createState() => _AnimatedActionButtonState();
}

class _AnimatedActionButtonState extends State<AnimatedActionButton> {
  bool _hovered = false;
  bool _pressed = false;

  @override
  Widget build(BuildContext context) {
    final enabled = widget.onPressed != null;
    final highlighted = enabled && (_hovered || _pressed || widget.filled);
    final content = MouseRegion(
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      cursor: enabled ? SystemMouseCursors.click : SystemMouseCursors.basic,
      child: GestureDetector(
        onTapDown: enabled ? (_) => setState(() => _pressed = true) : null,
        onTapCancel: enabled ? () => setState(() => _pressed = false) : null,
        onTapUp: enabled
            ? (_) {
                setState(() => _pressed = false);
                widget.onPressed?.call();
              }
            : null,
        child: AnimatedScale(
          scale: _pressed ? .96 : 1,
          duration: const Duration(milliseconds: 100),
          curve: Curves.easeOut,
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 180),
            curve: Curves.easeOutCubic,
            height: widget.compact ? 38 : 44,
            padding: EdgeInsets.symmetric(horizontal: widget.compact ? 11 : 15),
            decoration: BoxDecoration(
              color: highlighted
                  ? widget.accent.withValues(alpha: widget.filled ? .2 : .12)
                  : const Color(0x99112F49),
              borderRadius: BorderRadius.circular(widget.compact ? 10 : 12),
              border: Border.all(
                color: enabled
                    ? widget.accent.withValues(alpha: highlighted ? .75 : .28)
                    : Colors.white12,
              ),
              boxShadow: highlighted
                  ? <BoxShadow>[
                      BoxShadow(
                        color: widget.accent.withValues(alpha: .16),
                        blurRadius: 16,
                      ),
                    ]
                  : null,
            ),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              mainAxisAlignment: MainAxisAlignment.center,
              children: <Widget>[
                Icon(
                  widget.icon,
                  size: widget.compact ? 17 : 19,
                  color: enabled ? widget.accent : mutedText,
                ),
                if (widget.label.isNotEmpty) ...<Widget>[
                  const SizedBox(width: 7),
                  Text(
                    widget.label,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      color: enabled ? Colors.white : mutedText,
                      fontSize: widget.compact ? 12 : 13,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
    return widget.tooltip == null
        ? content
        : Tooltip(message: widget.tooltip!, child: content);
  }
}

class StatusPill extends StatelessWidget {
  const StatusPill({
    super.key,
    required this.label,
    required this.color,
    this.pulsing = false,
  });

  final String label;
  final Color color;
  final bool pulsing;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
      decoration: BoxDecoration(
        color: color.withValues(alpha: .1),
        borderRadius: BorderRadius.circular(99),
        border: Border.all(color: color.withValues(alpha: .28)),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          _StatusDot(color: color, pulsing: pulsing),
          const SizedBox(width: 7),
          Text(
            label,
            style: TextStyle(
              color: color,
              fontSize: 11,
              fontWeight: FontWeight.w700,
            ),
          ),
        ],
      ),
    );
  }
}

class _StatusDot extends StatefulWidget {
  const _StatusDot({required this.color, required this.pulsing});

  final Color color;
  final bool pulsing;

  @override
  State<_StatusDot> createState() => _StatusDotState();
}

class _StatusDotState extends State<_StatusDot>
    with SingleTickerProviderStateMixin {
  late final AnimationController _controller;

  @override
  void initState() {
    super.initState();
    _controller = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1300),
    );
    if (widget.pulsing) _controller.repeat();
  }

  @override
  void didUpdateWidget(covariant _StatusDot oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.pulsing && !_controller.isAnimating) _controller.repeat();
    if (!widget.pulsing && _controller.isAnimating) _controller.stop();
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: _controller,
      builder: (context, child) {
        final t = widget.pulsing ? _controller.value : 0.0;
        return Container(
          width: 8,
          height: 8,
          decoration: BoxDecoration(
            color: widget.color,
            shape: BoxShape.circle,
            boxShadow: <BoxShadow>[
              BoxShadow(
                color: widget.color.withValues(alpha: .55 * (1 - t)),
                blurRadius: 4 + 10 * t,
                spreadRadius: 1 + 3 * t,
              ),
            ],
          ),
        );
      },
    );
  }
}
