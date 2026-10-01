import 'package:flutter/material.dart';

import '../theme/app_theme.dart';

class TechGridBackground extends StatelessWidget {
  const TechGridBackground({super.key, required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: const BoxDecoration(
        gradient: RadialGradient(
          center: Alignment(.22, -.7),
          radius: 1.45,
          colors: <Color>[Color(0xFF0B3555), ink900],
          stops: <double>[0, .72],
        ),
      ),
      child: CustomPaint(painter: _GridPainter(), child: child),
    );
  }
}

class _GridPainter extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    final grid = Paint()
      ..color = const Color(0xFF3FD6EB).withValues(alpha: .035)
      ..strokeWidth = 1;
    const spacing = 44.0;
    for (double x = 0; x <= size.width; x += spacing) {
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), grid);
    }
    for (double y = 0; y <= size.height; y += spacing) {
      canvas.drawLine(Offset(0, y), Offset(size.width, y), grid);
    }

    final beam = Paint()
      ..shader = const LinearGradient(
        colors: <Color>[
          Colors.transparent,
          Color(0x3320D6E8),
          Colors.transparent,
        ],
      ).createShader(Rect.fromLTWH(0, 0, size.width, 1));
    canvas.drawRect(Rect.fromLTWH(0, 0, size.width, 1), beam);
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
