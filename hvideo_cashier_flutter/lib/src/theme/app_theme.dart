import 'package:flutter/material.dart';

const Color ink900 = Color(0xFF031426);
const Color ink850 = Color(0xFF061D34);
const Color ink800 = Color(0xFF082943);
const Color panelColor = Color(0xE60A2943);
const Color cyan = Color(0xFF20D6E8);
const Color blue = Color(0xFF2789FF);
const Color mint = Color(0xFF38E6B2);
const Color amber = Color(0xFFFFB454);
const Color danger = Color(0xFFFF647C);
const Color mutedText = Color(0xFF8FAEC4);

ThemeData buildAppTheme() {
  final scheme = ColorScheme.fromSeed(
    seedColor: cyan,
    brightness: Brightness.dark,
    surface: ink850,
    error: danger,
  );

  return ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    colorScheme: scheme,
    scaffoldBackgroundColor: ink900,
    fontFamilyFallback: const [
      'Microsoft YaHei UI',
      'PingFang SC',
      'sans-serif',
    ],
    textTheme: const TextTheme(
      headlineMedium: TextStyle(
        fontSize: 28,
        fontWeight: FontWeight.w700,
        letterSpacing: .3,
      ),
      titleLarge: TextStyle(fontSize: 18, fontWeight: FontWeight.w700),
      titleMedium: TextStyle(fontSize: 15, fontWeight: FontWeight.w600),
      bodyMedium: TextStyle(fontSize: 13, height: 1.45),
      labelLarge: TextStyle(fontSize: 13, fontWeight: FontWeight.w600),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: const Color(0x99112640),
      hintStyle: const TextStyle(color: mutedText),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: Color(0x334BDFF2)),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: Color(0x334BDFF2)),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(12),
        borderSide: const BorderSide(color: cyan, width: 1.3),
      ),
      contentPadding: const EdgeInsets.symmetric(horizontal: 14, vertical: 13),
    ),
    sliderTheme: const SliderThemeData(
      activeTrackColor: cyan,
      inactiveTrackColor: Color(0x334BDFF2),
      thumbColor: Color(0xFFDDFBFF),
      overlayColor: Color(0x3320D6E8),
      trackHeight: 4,
    ),
    tooltipTheme: TooltipThemeData(
      decoration: BoxDecoration(
        color: const Color(0xFF11364F),
        borderRadius: BorderRadius.circular(8),
      ),
      textStyle: const TextStyle(color: Colors.white, fontSize: 12),
    ),
  );
}
