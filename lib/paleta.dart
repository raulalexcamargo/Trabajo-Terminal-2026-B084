// Autor: Raul Alejandro Moreno Camargo
// Paleta de la app: negro petróleo de fondo y lima "voltio" como color de marca.
//
// Los colores de estado solo se usan para comunicar estado (normal, aviso, crítico),
// así una alerta siempre resalta sobre el resto de la pantalla.

import 'package:flutter/material.dart';

abstract final class Paleta {
  // ---------- Superficies (de la más oscura a la más clara) ----------
  static const fondo = Color(0xFF0B1211);
  static const superficie = Color(0xFF141D1B);
  static const superficieAlta = Color(0xFF1C2825);
  static const superficieMaxima = Color(0xFF263430);
  static const borde = Color(0xFF2E3E39);

  // ---------- Texto ----------
  static const texto = Color(0xFFE9F0EA);
  static const textoSuave = Color(0xFFA5B5AE);

  // ---------- Marca ----------
  static const voltio = Color(0xFFB4F24A); // botones, selección y estado normal
  static const sobreVoltio = Color(0xFF172300);
  static const turquesa = Color(0xFF6FD9C4); // iconos de sección y detalles

  // ---------- Estados ----------
  static const ok = voltio;
  static const aviso = Color(0xFFFFA23E);
  static const critico = Color(0xFFFF5F6D);
  static const info = Color(0xFF6CC4FF);
  static const neutro = Color(0xFF8FA69E);

  // Fondos sólidos para avisos con texto blanco encima
  static const avisoFuerte = Color(0xFFA85400);
  static const criticoFuerte = Color(0xFFC62F3B);

  /// Versión clara de un color de estado, para texto sobre un fondo teñido.
  static Color claro(Color color) => Color.lerp(color, Colors.white, 0.45)!;
}

/// Tema oscuro de la app. El esquema se define a mano porque
/// `ColorScheme.fromSeed` apaga la saturación de los colores en modo oscuro.
ThemeData temaBms() {
  const esquema = ColorScheme(
    brightness: Brightness.dark,
    primary: Paleta.voltio,
    onPrimary: Paleta.sobreVoltio,
    primaryContainer: Color(0xFF34480F),
    onPrimaryContainer: Color(0xFFDDF9A8),
    secondary: Paleta.turquesa,
    onSecondary: Color(0xFF00201A),
    secondaryContainer: Color(0xFF34480F),
    onSecondaryContainer: Color(0xFFDDF9A8),
    tertiary: Paleta.info,
    onTertiary: Color(0xFF00223A),
    tertiaryContainer: Color(0xFF0F3D5C),
    onTertiaryContainer: Color(0xFFCDE8FF),
    error: Paleta.critico,
    onError: Color(0xFF3B0009),
    errorContainer: Color(0xFF5C1520),
    onErrorContainer: Color(0xFFFFD9DC),
    surface: Paleta.fondo,
    onSurface: Paleta.texto,
    onSurfaceVariant: Paleta.textoSuave,
    surfaceContainerLowest: Color(0xFF070D0C),
    surfaceContainerLow: Color(0xFF101817),
    surfaceContainer: Paleta.superficie,
    surfaceContainerHigh: Paleta.superficieAlta,
    surfaceContainerHighest: Paleta.superficieMaxima,
    surfaceTint: Colors.transparent,
    outline: Color(0xFF5F726B),
    outlineVariant: Paleta.borde,
    inverseSurface: Paleta.texto,
    onInverseSurface: Paleta.fondo,
    inversePrimary: Color(0xFF4F6B14),
  );

  return ThemeData(
    useMaterial3: true,
    colorScheme: esquema,
    scaffoldBackgroundColor: Paleta.fondo,
    dividerColor: Paleta.borde,
    appBarTheme: const AppBarTheme(
      backgroundColor: Paleta.fondo,
      foregroundColor: Paleta.texto,
      scrolledUnderElevation: 0,
    ),
    cardTheme: CardThemeData(
      color: Paleta.superficie,
      elevation: 0,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(16),
        side: const BorderSide(color: Paleta.borde),
      ),
    ),
    navigationBarTheme: NavigationBarThemeData(
      backgroundColor: Paleta.superficie,
      indicatorColor: Paleta.voltio.withValues(alpha: 0.18),
      iconTheme: WidgetStateProperty.resolveWith((estados) => IconThemeData(
            color: estados.contains(WidgetState.selected)
                ? Paleta.voltio
                : Paleta.textoSuave,
          )),
    ),
    snackBarTheme: const SnackBarThemeData(
      backgroundColor: Paleta.superficieMaxima,
      contentTextStyle: TextStyle(color: Paleta.texto),
      actionTextColor: Paleta.voltio,
    ),
  );
}
