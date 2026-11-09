// Autor: Raul Alejandro Moreno Camargo
// Bateria dibujada a mano con CustomPainter (en lugar del LinearProgressIndicator
// o de Icons.battery_*). Se anima sola cuando cambia el SOC.
//
// Uso, dentro de _TarjetaSoc en main.dart, en lugar del ClipRRect + LinearProgressIndicator:
//
//   SizedBox(
//     height: 110,
//     child: BateriaWidget(
//       soc: e.soc,
//       modo: e.modo,
//       numCeldas: e.vCeldas.isEmpty ? 3 : e.vCeldas.length,
//       corteActivo: e.corteActivo,
//     ),
//   ),

import 'package:flutter/material.dart';

import 'bms_api.dart'; // ModoBanco
import 'paleta.dart';

class BateriaWidget extends StatelessWidget {
  final double soc; // 0..100
  final ModoBanco modo;
  final int numCeldas;
  final bool corteActivo;
  final bool sinDatos; // sin conexión: batería vacía y apagada

  const BateriaWidget({
    super.key,
    required this.soc,
    this.modo = ModoBanco.reposo,
    this.numCeldas = 3,
    this.corteActivo = false,
    this.sinDatos = false,
  });

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;
    return Semantics(
      label: sinDatos ? 'Bateria sin datos' : 'Bateria al ${soc.toStringAsFixed(0)} por ciento',
      child: AspectRatio(
        aspectRatio: 2.3,
        child: TweenAnimationBuilder<double>(
          tween: Tween<double>(end: (soc / 100).clamp(0.0, 1.0).toDouble()),
          duration: const Duration(milliseconds: 700),
          curve: Curves.easeOutCubic,
          builder: (context, nivel, _) => CustomPaint(
            painter: _BateriaPainter(
              nivel: nivel,
              cargando: modo == ModoBanco.carga,
              numCeldas: numCeldas < 1 ? 1 : numCeldas,
              corte: corteActivo,
              contorno: corteActivo ? Paleta.critico : cs.onSurface.withValues(alpha: sinDatos ? 0.3 : 0.75),
              fondo: cs.surfaceContainerHighest,
            ),
          ),
        ),
      ),
    );
  }
}

class _BateriaPainter extends CustomPainter {
  final double nivel; // 0..1
  final bool cargando, corte;
  final int numCeldas;
  final Color contorno, fondo;

  _BateriaPainter({
    required this.nivel,
    required this.cargando,
    required this.numCeldas,
    required this.corte,
    required this.contorno,
    required this.fondo,
  });

  Color get _colorNivel {
    if (nivel < 0.20) return Paleta.critico;
    if (nivel < 0.50) return Paleta.aviso;
    return Paleta.ok;
  }

  @override
  void paint(Canvas canvas, Size s) {
    final grosor = s.height * 0.07;
    final anchoCuerpo = s.width * 0.92;
    final radio = Radius.circular(s.height * 0.16);

    // --- cuerpo y borne positivo
    final cuerpo = RRect.fromRectAndRadius(
      Rect.fromLTWH(grosor / 2, grosor / 2, anchoCuerpo - grosor, s.height - grosor),
      radio,
    );
    final borne = RRect.fromRectAndCorners(
      Rect.fromLTWH(anchoCuerpo + grosor * 0.3, s.height * 0.30,
          s.width - anchoCuerpo - grosor * 0.3, s.height * 0.40),
      topRight: Radius.circular(s.height * 0.06),
      bottomRight: Radius.circular(s.height * 0.06),
    );

    canvas.drawRRect(cuerpo, Paint()..color = fondo);
    canvas.drawRRect(borne, Paint()..color = contorno);

    // --- relleno segun SOC (con un margen interno)
    final hueco = grosor * 1.4;
    final interior = cuerpo.deflate(hueco);
    final anchoRelleno = interior.width * nivel;
    if (anchoRelleno > 0) {
      canvas.save();
      canvas.clipRRect(interior);
      final rect = Rect.fromLTWH(interior.left, interior.top, anchoRelleno, interior.height);
      canvas.drawRect(
        rect,
        Paint()
          ..shader = LinearGradient(
            begin: Alignment.topCenter,
            end: Alignment.bottomCenter,
            colors: [
              Color.lerp(_colorNivel, Colors.white, 0.25)!,
              _colorNivel,
              Color.lerp(_colorNivel, Colors.black, 0.15)!,
            ],
          ).createShader(rect),
      );
      // brillo superior
      canvas.drawRect(
        Rect.fromLTWH(interior.left, interior.top + interior.height * 0.12, anchoRelleno,
            interior.height * 0.14),
        Paint()..color = Colors.white.withValues(alpha: 0.25),
      );
      canvas.restore();
    }

    // --- divisiones de celdas (3S = 3 compartimentos)
    final linea = Paint()
      ..color = contorno.withValues(alpha: 0.35)
      ..strokeWidth = grosor * 0.45;
    for (var i = 1; i < numCeldas; i++) {
      final x = interior.left + interior.width * i / numCeldas;
      canvas.drawLine(Offset(x, interior.top), Offset(x, interior.bottom), linea);
    }

    // --- contorno
    canvas.drawRRect(
      cuerpo,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = grosor
        ..color = contorno,
    );

    // --- iconos dibujados: rayo al cargar, X si hay corte
    final c = Offset(cuerpo.center.dx, cuerpo.center.dy);
    final h = interior.height * 0.8;
    if (corte) {
      final p = Paint()
        ..color = Paleta.criticoFuerte
        ..strokeWidth = h * 0.14
        ..strokeCap = StrokeCap.round;
      final d = h * 0.32;
      canvas.drawLine(c + Offset(-d, -d), c + Offset(d, d), p);
      canvas.drawLine(c + Offset(d, -d), c + Offset(-d, d), p);
    } else if (cargando) {
      final w = h * 0.55;
      final rayo = Path()
        ..moveTo(c.dx + w * 0.10, c.dy - h / 2)
        ..lineTo(c.dx - w * 0.45, c.dy + h * 0.06)
        ..lineTo(c.dx - w * 0.02, c.dy + h * 0.06)
        ..lineTo(c.dx - w * 0.12, c.dy + h / 2)
        ..lineTo(c.dx + w * 0.45, c.dy - h * 0.08)
        ..lineTo(c.dx + w * 0.02, c.dy - h * 0.08)
        ..close();
      canvas.drawPath(rayo, Paint()..color = Colors.white);
      canvas.drawPath(
        rayo,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = h * 0.04
          ..strokeJoin = StrokeJoin.round
          ..color = Colors.black.withValues(alpha: 0.55),
      );
    }
  }

  @override
  bool shouldRepaint(_BateriaPainter o) =>
      o.nivel != nivel ||
      o.cargando != cargando ||
      o.corte != corte ||
      o.numCeldas != numCeldas ||
      o.contorno != contorno ||
      o.fondo != fondo;
}
