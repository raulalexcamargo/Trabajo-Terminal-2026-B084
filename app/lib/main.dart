// Autor: Raul Alejandro Moreno Camargo
// App BMS: monitoreo del banco de baterías conectado a la Raspberry Pi 5.
//
//  - Monitoreo: alertas ARRIBA (con sonido), relevadores, batería, sensores y celdas.
//  - Historial: eventos guardados en la Raspberry (RN07). Solo lectura.
//  - Ajustes: conexión (recuerda la última IP), datos del banco (editable) y
//             límites de seguridad (solo lectura, se cambian en la Raspberry).

import 'dart:async';
import 'package:audioplayers/audioplayers.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'bateria_widget.dart';
import 'bms_api.dart';

void main() => runApp(const BmsApp());

class BmsApp extends StatelessWidget {
  const BmsApp({super.key});

  @override
  Widget build(BuildContext c) => MaterialApp(
    title: 'BMS Banco de Baterias',
    debugShowCheckedModeBanner: false,
    theme: ThemeData(
      colorScheme: ColorScheme.fromSeed(
        seedColor: const Color(0xFF1B6B4A),
        brightness: Brightness.dark,
      ),
      useMaterial3: true,
    ),
    home: const PantallaPrincipal(),
  );
}

String _dos(int n) => n.toString().padLeft(2, '0');
String hora(DateTime t) => '${_dos(t.hour)}:${_dos(t.minute)}:${_dos(t.second)}';
/// 9270 -> "2 h 35 min";  540 -> "9 min";  40 -> "menos de 1 min"
String duracion(int seg) {
  if (seg < 60) return 'menos de 1 min';
  final h = seg ~/ 3600, m = (seg % 3600) ~/ 60;
  if (h == 0) return '$m min';
  return m == 0 ? '$h h' : '$h h $m min';
}

String fechaHora(DateTime t) =>
    '${_dos(t.day)}/${_dos(t.month)}  ${hora(t)}';

// ============================================================
//  Pantalla principal
// ============================================================
class PantallaPrincipal extends StatefulWidget {
  const PantallaPrincipal({super.key});

  @override
  State<PantallaPrincipal> createState() => _PantallaPrincipalState();
}

class _PantallaPrincipalState extends State<PantallaPrincipal> {
  static const _claveHost = 'ultimo_host';

  int _pestana = 0;
  String host = '';
  bool conectado = false;
  bool conectando = false;
  String? errorConexion;

  BmsApi? _api;
  BmsLive? _live;
  EstadoBanco? estadoActual;
  List<Sensor> sensoresActuales = [];

  StreamSubscription? _subEstado, _subSensores, _subAlertas;

  AudioPlayer? _player; // se crea con la primera alerta
  final _alertasSonadas = <String>{};
  DateTime _ultimoSonido = DateTime(2000);
  final GlobalKey<VistaHistorialState> _claveHistorial = GlobalKey();

  @override
  void initState() {
    super.initState();
    estadoActual = _estadoDemo();
    sensoresActuales = _sensoresDemo();
    _cargarUltimoHost();
  }

  @override
  void dispose() {
    _desconectar(actualizar: false);
    _player?.dispose();
    super.dispose();
  }

  // -------- IP guardada en el teléfono --------
  Future<void> _cargarUltimoHost() async {
    final prefs = await SharedPreferences.getInstance();
    final h = prefs.getString(_claveHost);
    if (h == null || h.isEmpty) return;
    setState(() => host = h);
    _conectar(); // se conecta solo a la última Raspberry usada
  }

  Future<void> _guardarHost(String h) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString(_claveHost, h);
  }

  // -------- Conexión --------
  Future<void> _conectar() async {
    final hostLimpio = host.trim();
    if (hostLimpio.isEmpty) {
      setState(() => errorConexion = 'Escribe la IP de la Raspberry, ej. 192.168.1.42:8000');
      return;
    }
    setState(() {
      conectando = true;
      errorConexion = null;
    });

    final apiTemp = BmsApi(hostLimpio);
    try {
      await apiTemp.banco();
      final sensoresRemotos = await apiTemp.sensores();
      final estadoInicial = await apiTemp.estado();

      _desconectar(actualizar: false);
      _api = apiTemp;
      _live = BmsLive(hostLimpio);
      _subSensores = _live!.sensores.listen((s) {
        if (mounted) setState(() => sensoresActuales = s);
      });
      _subEstado = _live!.estado.listen((e) {
        if (mounted) setState(() => estadoActual = e);
      });
      _subAlertas = _live!.alertas.listen(_nuevaAlerta);
      _live!.conectar();

      // las alertas que ya existían no vuelven a sonar
      _alertasSonadas.addAll(estadoInicial.alertas.map((a) => a.clave));
      await _guardarHost(hostLimpio);

      if (!mounted) return;
      setState(() {
        conectado = true;
        conectando = false;
        sensoresActuales = sensoresRemotos;
        estadoActual = estadoInicial;
      });
      _claveHistorial.currentState?.cargar();
      _aviso('Conectado a $hostLimpio');
    } catch (e) {
      if (!mounted) return;
      setState(() {
        conectado = false;
        conectando = false;
        errorConexion =
        'No se pudo conectar con $hostLimpio.\nRevisa la IP, que el teléfono esté en el mismo WiFi '
            'y que el servicio esté corriendo en la Raspberry (systemctl status bms).';
      });
    }
  }

  void _desconectar({bool actualizar = true}) {
    _subEstado?.cancel();
    _subSensores?.cancel();
    _subAlertas?.cancel();
    _live?.cerrar();
    _live = null;
    _api = null;
    if (!actualizar) return;
    setState(() {
      conectado = false;
      conectando = false;
      estadoActual = _estadoDemo();
      sensoresActuales = _sensoresDemo();
    });
  }

  // -------- Alertas nuevas: sonido + vibración + aviso --------
  Future<void> _nuevaAlerta(Alerta a) async {
    if (!mounted || !_alertasSonadas.add(a.clave)) return;
    if (_alertasSonadas.length > 300) _alertasSonadas.remove(_alertasSonadas.first);

    _claveHistorial.currentState?.cargar();

    final ahora = DateTime.now();
    if (ahora.difference(_ultimoSonido).inMilliseconds > 1500) {
      _ultimoSonido = ahora;
      try {
        final p = _player ??= AudioPlayer();
        await p.stop();
        await p.play(AssetSource(
            a.esCritica ? 'sonidos/critica.wav' : 'sonidos/aviso.wav'));
      } catch (_) {}
      if (a.esCritica) {
        HapticFeedback.heavyImpact();
      } else {
        HapticFeedback.mediumImpact();
      }
    }

    if (!mounted) return;
    final color = a.esCritica ? Colors.red.shade700 : Colors.orange.shade800;
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(
        backgroundColor: color,
        duration: const Duration(seconds: 5),
        content: Row(children: [
          Icon(iconoAlerta(a.tipo), color: Colors.white),
          const SizedBox(width: 10),
          Expanded(
            child: Text(a.titulo ?? a.mensaje,
                style: const TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
          ),
        ]),
        action: SnackBarAction(
          label: 'VER',
          textColor: Colors.white,
          onPressed: () => setState(() => _pestana = 0),
        ),
      ));
  }

  void _aviso(String m, {bool esError = false}) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(m),
      backgroundColor: esError ? Theme.of(context).colorScheme.error : null,
    ));
  }

  // -------- Datos de vista previa (sin conexión) --------
  EstadoBanco _estadoDemo() => EstadoBanco.fromJson({
    'banco_id': 'B1 (DEMO)',
    'valores': {
      'V_4V': 4.14, 'V_8V': 8.27, 'V_12V': 12.41,
      'I_CARGA': 0.0, 'I_DESC': 3.20, 'T1': 31.5, 'T2': 30.9,
    },
    'v_celdas': {'C1': 4.14, 'C2': 4.13, 'C3': 4.14},
    'v_pack': 12.41,
    'i_main': -3.20,
    'temp_max': 31.5,
    'temp_min': 30.9,
    'temp_prom': 31.2,
    'potencia_w': -39.7,
    'soc': 82.5,
    'soh': 98.0,
    'modo': 'DESCARGA',
    'tiempo_restante_s': 9270,
    'tiempo_hasta': 'vacio',
    'desbalance_mv': 10.0,
    'alertas': [],
    'corte_activo': false,
    'conectado': false,
    'actuadores': {'fuente': 1, 'motor': 1, 'espera_fuente_s': 0, 'espera_motor_s': 0},
    'inicio_seguro': true,
    'reglas_activas': [],
  });

  List<Sensor> _sensoresDemo() => [
    for (final s in [
      ['V_4V', 'VOLT', 'V', 'Toma 4V (celda 1)'],
      ['V_8V', 'VOLT', 'V', 'Toma 8V (celdas 1-2)'],
      ['V_12V', 'VOLT', 'V', 'Toma 12V (celdas 1-3)'],
      ['I_CARGA', 'CORR', 'A', 'Carga'],
      ['I_DESC', 'CORR', 'A', 'Descarga (motor)'],
      ['T1', 'TEMP', '°C', 'Interno'],
      ['T2', 'TEMP', '°C', 'Externo'],
    ])
      Sensor.fromJson({'id': s[0], 'magnitud': s[1], 'unidad': s[2], 'ubicacion': s[3]})
  ];

  @override
  Widget build(BuildContext c) {
    final e = estadoActual ?? _estadoDemo();
    final paginas = [
      _VistaDashboard(estado: e, sensores: sensoresActuales, conectado: conectado, api: _api),
      VistaHistorial(key: _claveHistorial, api: _api),
      _VistaAjustesConexion(
        hostInicial: host,
        conectado: conectado,
        conectando: conectando,
        errorConexion: errorConexion,
        api: _api,
        onCambiarHost: (h) => host = h,
        onConectar: _conectar,
        onDesconectar: _desconectar,
      ),
    ];

    final nAlertas = conectado ? e.alertas.length : 0;
    return Scaffold(
      body: IndexedStack(index: _pestana, children: paginas),
      bottomNavigationBar: NavigationBar(
        selectedIndex: _pestana,
        onDestinationSelected: (i) {
          setState(() => _pestana = i);
          if (i == 1) _claveHistorial.currentState?.cargar();
        },
        destinations: [
          NavigationDestination(
            icon: Badge(
              isLabelVisible: !conectado || nAlertas > 0,
              label: Text(conectado ? '$nAlertas' : 'DEMO'),
              backgroundColor: !conectado
                  ? Colors.orange
                  : (e.hayCritica ? Colors.red : Colors.orange),
              child: const Icon(Icons.dashboard_outlined),
            ),
            selectedIcon: const Icon(Icons.dashboard),
            label: 'Monitoreo',
          ),
          const NavigationDestination(
            icon: Icon(Icons.history),
            label: 'Historial',
          ),
          NavigationDestination(
            icon: Icon(Icons.settings_outlined,
                color: conectado ? Colors.green : Colors.orange),
            selectedIcon: const Icon(Icons.settings),
            label: 'Ajustes',
          ),
        ],
      ),
    );
  }
}

// ============================================================
//  Iconos y colores por tipo de alerta / evento
// ============================================================
IconData iconoAlerta(String? tipo) => switch (tipo) {
  'RN01' => Icons.local_fire_department,
  'RN02' => Icons.battery_alert,
  'RN03' => Icons.battery_full,
  'RN10' => Icons.bolt,
  'RN05' => Icons.sensors_off,
  'RN09' => Icons.trending_up,
  'ERROR_MODO' => Icons.block,
  'CALENTANDO' => Icons.thermostat,
  'SOC_BAJO' => Icons.battery_2_bar,
  'SIN_DATOS' => Icons.link_off,
  _ => Icons.warning_amber,
};

// ============================================================
//  Pestaña 1: Monitoreo
// ============================================================
class _VistaDashboard extends StatelessWidget {
  final EstadoBanco estado;
  final List<Sensor> sensores;
  final bool conectado;
  final BmsApi? api;

  const _VistaDashboard({
    required this.estado,
    required this.sensores,
    required this.conectado,
    required this.api,
  });

  Sensor? _meta(String id) {
    for (final s in sensores) {
      if (s.id == id) return s;
    }
    return null;
  }

  @override
  Widget build(BuildContext c) {
    return Scaffold(
      appBar: AppBar(
        title: Row(children: [
          const Icon(Icons.battery_charging_full, size: 24),
          const SizedBox(width: 8),
          const Text('Banco de Baterías'),
          const Spacer(),
          Chip(
            avatar: Icon(conectado ? Icons.wifi : Icons.wifi_off,
                size: 16, color: conectado ? Colors.green : Colors.orange),
            label: Text(conectado ? 'EN VIVO' : 'DEMO',
                style: TextStyle(
                    fontSize: 11,
                    color: conectado ? Colors.green : Colors.orange,
                    fontWeight: FontWeight.bold)),
            backgroundColor:
            (conectado ? Colors.green : Colors.orange).withValues(alpha: 0.15),
            side: BorderSide.none,
          ),
        ]),
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          if (!conectado)
            const _Banner(
              icono: Icons.info_outline,
              texto: 'Vista previa con datos de ejemplo. Conéctate a la Raspberry en la pestaña Ajustes.',
              color: Colors.orange,
            ),
          // ---------- ALERTAS ARRIBA ----------
          if (conectado) _PanelAlertas(alertas: estado.alertas),
          _TarjetaControl(e: estado, api: conectado ? api : null),
          const SizedBox(height: 12),
          _TarjetaReles(e: estado, conectado: conectado),
          const SizedBox(height: 12),
          _TarjetaSoc(e: estado),
          const SizedBox(height: 12),
          ..._porMagnitud(c),
          if (estado.vCeldas.length > 1) _TarjetaCeldas(e: estado),
        ],
      ),
    );
  }

  List<Widget> _porMagnitud(BuildContext context) {
    final grupos = <Magnitud, List<MapEntry<String, double>>>{};
    for (final kv in estado.valores.entries) {
      final m = _meta(kv.key);
      if (m == null) continue;
      grupos.putIfAbsent(m.magnitud, () => []).add(kv);
    }
    const titulos = {
      Magnitud.temp: 'Temperatura',
      Magnitud.volt: 'Voltaje',
      Magnitud.corr: 'Corriente',
    };
    const iconos = {
      Magnitud.temp: Icons.thermostat,
      Magnitud.volt: Icons.bolt,
      Magnitud.corr: Icons.electric_meter,
    };
    return [Magnitud.volt, Magnitud.corr, Magnitud.temp]
        .where((m) => grupos.containsKey(m))
        .map((m) => Padding(
      padding: const EdgeInsets.only(bottom: 12),
      child: Card(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(children: [
                Icon(iconos[m], size: 20),
                const SizedBox(width: 8),
                Text('${titulos[m]}  (${grupos[m]!.length})',
                    style: Theme.of(context).textTheme.titleMedium),
              ]),
              const SizedBox(height: 8),
              ...grupos[m]!.map((kv) {
                final meta = _meta(kv.key)!;
                final esBanco = meta.rol == 'PACK_V';
                return Padding(
                  padding: const EdgeInsets.symmetric(vertical: 5),
                  child: Row(children: [
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(meta.ubicacion ?? kv.key,
                              style: TextStyle(
                                  fontSize: 14,
                                  fontWeight: esBanco ? FontWeight.bold : null)),
                          Text(esBanco ? '${kv.key} · voltaje del banco' : kv.key,
                              style: Theme.of(context).textTheme.labelSmall),
                        ],
                      ),
                    ),
                    Text('${kv.value.toStringAsFixed(2)} ${meta.unidad}',
                        style: const TextStyle(
                            fontSize: 20,
                            fontWeight: FontWeight.w600,
                            fontFeatures: [FontFeature.tabularFigures()])),
                  ]),
                );
              }),
            ],
          ),
        ),
      ),
    ))
        .toList();
  }
}

// ---------- Alertas (arriba de todo) ----------
class _PanelAlertas extends StatelessWidget {
  final List<Alerta> alertas;
  const _PanelAlertas({required this.alertas});

  @override
  Widget build(BuildContext c) {
    if (alertas.isEmpty) {
      return const _Banner(
        icono: Icons.check_circle,
        texto: 'Todo en orden: sin alertas',
        color: Colors.green,
      );
    }
    return Column(children: alertas.map((a) => _TarjetaAlerta(a: a)).toList());
  }
}

class _TarjetaAlerta extends StatelessWidget {
  final Alerta a;
  const _TarjetaAlerta({required this.a});

  @override
  Widget build(BuildContext c) {
    final color = a.esCritica ? Colors.red : Colors.orange;
    return Container(
      margin: const EdgeInsets.only(bottom: 10),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.12),
        borderRadius: BorderRadius.circular(12),
        border: Border(left: BorderSide(color: color, width: 6)),
      ),
      padding: const EdgeInsets.fromLTRB(14, 12, 14, 12),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Icon(iconoAlerta(a.tipo), color: color, size: 30),
        const SizedBox(width: 12),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(a.titulo ?? a.mensaje,
                style: TextStyle(
                    color: color.shade200, fontSize: 16, fontWeight: FontWeight.bold)),
            if (a.titulo != null) ...[
              const SizedBox(height: 4),
              Text(a.mensaje, style: const TextStyle(fontSize: 13.5, height: 1.3)),
            ],
            const SizedBox(height: 6),
            Text(
              '${a.esCritica ? 'CRÍTICA' : 'AVISO'} · '
                  '${a.corta ? 'protección activa' : 'solo informativa'} · desde ${hora(a.ts)}',
              style: Theme.of(c).textTheme.labelSmall,
            ),
          ]),
        ),
      ]),
    );
  }
}

// ---------- Control: modo de operación (RN11) ----------
class _TarjetaControl extends StatefulWidget {
  final EstadoBanco e;
  final BmsApi? api; // null = sin conexión: el control se ve pero no se puede usar
  const _TarjetaControl({required this.e, required this.api});

  @override
  State<_TarjetaControl> createState() => _TarjetaControlState();
}

class _TarjetaControlState extends State<_TarjetaControl> {
  String? _pedido; // modo pedido que el ESP32 todavía no confirma
  DateTime _pedidoEn = DateTime(2000);
  bool _enviando = false;

  static const _ayuda = {
    'APAGADO': 'Fuente y motor desconectados.',
    'CARGA': 'Fuente conectada y motor bloqueado. Se apaga sola cuando el banco se llena.',
    'MOTOR': 'Motor encendido y carga bloqueada. Se apaga solo si el banco se descarga o hay sobrecorriente.',
  };

  String get _seleccionado {
    final e = widget.e;
    if (_pedido != null &&
        _pedido != e.modoOp &&
        DateTime.now().difference(_pedidoEn).inSeconds < 3) {
      return _pedido!; // se muestra lo pedido mientras llega la confirmación
    }
    return e.modoOp;
  }

  Future<void> _cambiar(String modo) async {
    final api = widget.api;
    if (api == null || modo == widget.e.modoOp) return;
    if (modo == 'MOTOR') {
      final ok = await showDialog<bool>(
        context: context,
        builder: (d) => AlertDialog(
          icon: const Icon(Icons.speed),
          title: const Text('¿Encender el motor?'),
          content: const Text('Se desconectará la fuente de carga mientras el motor esté encendido.'),
          actions: [
            TextButton(onPressed: () => Navigator.pop(d, false), child: const Text('Cancelar')),
            FilledButton(onPressed: () => Navigator.pop(d, true), child: const Text('Encender')),
          ],
        ),
      );
      if (ok != true || !mounted) return;
    }
    setState(() {
      _enviando = true;
      _pedido = modo;
      _pedidoEn = DateTime.now();
    });
    try {
      await api.cambiarModo(modo);
    } catch (err) {
      if (!mounted) return;
      _pedido = null;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        backgroundColor: Colors.orange.shade800,
        content: Text('$err'.replaceFirst('Exception: ', '')),
      ));
    } finally {
      if (mounted) setState(() => _enviando = false);
    }
  }

  @override
  Widget build(BuildContext c) {
    final e = widget.e;
    final activo = widget.api != null;
    final lleno = e.reglasActivas.contains('RN03');
    final bloqueoMotor = e.reglasActivas.contains('RN02') || e.reglasActivas.contains('RN10');
    final sel = _seleccionado;
    String? aviso;
    if (!activo) {
      aviso = 'Conéctate a la Raspberry para controlar el banco.';
    } else if (lleno) {
      aviso = 'El banco está cargado: no se puede elegir Cargar.';
    } else if (bloqueoMotor) {
      aviso = e.reglasActivas.contains('RN10')
          ? 'Hubo sobrecorriente: rearma en la Raspberry (bms rearmar) antes de usar el motor.'
          : 'El banco está descargado: cárgalo antes de usar el motor.';
    }
    return Card(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 12, 16, 14),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Text('Control', style: Theme.of(c).textTheme.titleMedium),
            const Spacer(),
            if (_enviando)
              const SizedBox(width: 16, height: 16, child: CircularProgressIndicator(strokeWidth: 2)),
          ]),
          const SizedBox(height: 10),
          SizedBox(
            width: double.infinity,
            child: SegmentedButton<String>(
              showSelectedIcon: false,
              segments: [
                ButtonSegment(
                    value: 'APAGADO',
                    label: const Text('Apagado'),
                    icon: const Icon(Icons.power_settings_new),
                    enabled: activo),
                ButtonSegment(
                    value: 'CARGA',
                    label: const Text('Cargar'),
                    icon: const Icon(Icons.battery_charging_full),
                    enabled: activo && !lleno),
                ButtonSegment(
                    value: 'MOTOR',
                    label: const Text('Motor'),
                    icon: const Icon(Icons.speed),
                    enabled: activo && !bloqueoMotor),
              ],
              selected: {sel},
              onSelectionChanged: _enviando ? null : (s) => _cambiar(s.first),
            ),
          ),
          const SizedBox(height: 10),
          Text(_ayuda[sel] ?? '', style: const TextStyle(fontSize: 13)),
          if (aviso != null) ...[
            const SizedBox(height: 6),
            Text(aviso, style: TextStyle(fontSize: 12.5, color: Colors.orange.shade300)),
          ],
        ]),
      ),
    );
  }
}

// ---------- Relevadores ----------
class _TarjetaReles extends StatelessWidget {
  final EstadoBanco e;
  final bool conectado;
  const _TarjetaReles({required this.e, required this.conectado});

  static const _motivo = {
    'RN01': 'temperatura alta',
    'RN02': 'banco descargado',
    'RN03': 'banco cargado',
    'RN05': 'falla de sensor',
    'RN10': 'sobrecorriente (rearmar en la Raspberry)',
  };

  static const _nombreModo = {'APAGADO': 'Apagado', 'CARGA': 'Cargar batería', 'MOTOR': 'Motor'};

  String _detalle(bool? activo, List<String> reglas, int espera, String modoQueLoUsa) {
    if (!conectado) return 'Sin datos';
    if (activo == null) return 'Esperando datos del ESP32';
    if (activo) return 'Deja pasar la energía';
    if (!e.inicioSeguro) return 'Arranque seguro: esperando lectura válida';
    final motivos = reglas.where(e.reglasActivas.contains).map((r) => _motivo[r]!).toList();
    if (motivos.isEmpty && e.modoOp != modoQueLoUsa) {
      return 'Desconectado por el modo: ${_nombreModo[e.modoOp] ?? e.modoOp}';
    }
    var t = motivos.isEmpty ? 'Cortado' : 'Cortado por ${motivos.join(', ')}';
    if (espera > 0) t += ' · reconecta en $espera s';
    return t;
  }

  Widget _fila(BuildContext c, String nombre, String sub, bool? activo, String detalle) {
    final color = activo == null
        ? Colors.grey
        : (activo ? Colors.green : Colors.red);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Row(children: [
        CircleAvatar(
          radius: 20,
          backgroundColor: color.withValues(alpha: 0.18),
          child: Icon(activo == false ? Icons.power_off : Icons.power, color: color),
        ),
        const SizedBox(width: 12),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(nombre, style: const TextStyle(fontWeight: FontWeight.bold)),
            Text(sub, style: Theme.of(c).textTheme.labelSmall),
            const SizedBox(height: 2),
            Text(detalle, style: TextStyle(fontSize: 12.5, color: color.shade200)),
          ]),
        ),
        Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
          decoration: BoxDecoration(
            color: color.withValues(alpha: 0.2),
            borderRadius: BorderRadius.circular(20),
            border: Border.all(color: color),
          ),
          child: Text(activo == null ? '—' : (activo ? 'ACTIVO' : 'CORTADO'),
              style: TextStyle(color: color, fontWeight: FontWeight.bold, fontSize: 12)),
        ),
      ]),
    );
  }

  @override
  Widget build(BuildContext c) => Card(
    child: Padding(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('Relevadores', style: Theme.of(c).textTheme.titleMedium),
        const SizedBox(height: 4),
        _fila(c, 'Actuador 1 · Fuente', 'Conecta la fuente al banco (carga)', e.actFuente,
            _detalle(e.actFuente, const ['RN01', 'RN03', 'RN05'], e.esperaFuenteS, 'CARGA')),
        const Divider(height: 8),
        _fila(c, 'Actuador 2 · Motor', 'Conecta el banco al motor (descarga)', e.actMotor,
            _detalle(e.actMotor, const ['RN01', 'RN02', 'RN05', 'RN10'], e.esperaMotorS, 'MOTOR')),
      ]),
    ),
  );
}

// ---------- SOC / SOH con la batería dibujada ----------
class _TarjetaSoc extends StatelessWidget {
  final EstadoBanco e;
  const _TarjetaSoc({required this.e});

  @override
  Widget build(BuildContext c) {
    final (etiqueta, icono, color) = switch (e.modo) {
      ModoBanco.carga => ('CARGANDO', Icons.arrow_downward, Colors.green),
      ModoBanco.descarga => ('DESCARGANDO', Icons.arrow_upward, Colors.orange),
      ModoBanco.reposo => ('EN REPOSO', Icons.pause, Colors.blueGrey),
    };
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(children: [
          Row(mainAxisAlignment: MainAxisAlignment.center, children: [
            Icon(icono, color: color, size: 18),
            const SizedBox(width: 6),
            Text(etiqueta,
                style: TextStyle(color: color, fontWeight: FontWeight.bold, letterSpacing: 1)),
          ]),
          const SizedBox(height: 12),
          SizedBox(
            height: 110,
            child: BateriaWidget(
              soc: e.soc,
              modo: e.modo,
              numCeldas: e.vCeldas.isEmpty ? 3 : e.vCeldas.length,
              corteActivo: e.corteActivo,
            ),
          ),
          const SizedBox(height: 8),
          Text('${e.soc.toStringAsFixed(0)} %',
              style: const TextStyle(fontSize: 44, fontWeight: FontWeight.w300)),
          if (e.tiempoRestanteS != null && e.tiempoHasta != null)
            Row(mainAxisAlignment: MainAxisAlignment.center, children: [
              Icon(Icons.schedule,
                  size: 16, color: e.tiempoHasta == 'lleno' ? Colors.green : Colors.orange),
              const SizedBox(width: 6),
              Text(
                  '≈ ${duracion(e.tiempoRestanteS!)} '
                      '${e.tiempoHasta == 'lleno' ? 'para llenarse' : 'para descargarse'}',
                  style: TextStyle(
                      fontSize: 14,
                      color: e.tiempoHasta == 'lleno' ? Colors.green : Colors.orange)),
            ]),
          const SizedBox(height: 12),
          Row(mainAxisAlignment: MainAxisAlignment.spaceEvenly, children: [
            _Metrica('SOH', '${e.soh.toStringAsFixed(0)} %'),
            _MetricaPotencia(e.potenciaW),
            if (e.vPack != null) _Metrica('Banco', '${e.vPack!.toStringAsFixed(2)} V'),
          ]),
        ]),
      ),
    );
  }
}

class _Metrica extends StatelessWidget {
  final String etiqueta, valor;
  const _Metrica(this.etiqueta, this.valor);

  @override
  Widget build(BuildContext c) => Column(children: [
    Text(etiqueta, style: Theme.of(c).textTheme.labelSmall),
    const SizedBox(height: 2),
    Text(valor, style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w600)),
  ]);
}

/// Potencia sin signo, con flecha: ↓ verde = entra al banco, ↑ naranja = sale hacia el motor.
class _MetricaPotencia extends StatelessWidget {
  final double w;
  const _MetricaPotencia(this.w);

  @override
  Widget build(BuildContext c) {
    final reposo = w.abs() < 0.5;
    final entra = w > 0;
    final color = reposo ? null : (entra ? Colors.green : Colors.orange);
    return Column(children: [
      Text('Potencia', style: Theme.of(c).textTheme.labelSmall),
      const SizedBox(height: 2),
      Row(mainAxisSize: MainAxisSize.min, children: [
        if (!reposo)
          Icon(entra ? Icons.arrow_downward : Icons.arrow_upward, size: 18, color: color),
        Text('${w.abs().toStringAsFixed(w.abs() < 10 ? 1 : 0)} W',
            style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600, color: color)),
      ]),
      Text(reposo ? 'sin flujo' : (entra ? 'entrando' : 'saliendo'),
          style: TextStyle(fontSize: 11, color: color)),
    ]);
  }
}

class _TarjetaCeldas extends StatelessWidget {
  final EstadoBanco e;
  const _TarjetaCeldas({required this.e});

  @override
  Widget build(BuildContext c) {
    final maxV = e.vCeldas.values.fold<double>(0.0, (a, b) => a > b ? a : b);
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
            Text('Celdas', style: Theme.of(c).textTheme.titleMedium),
            Text('desbalance ${e.desbalanceMv.toStringAsFixed(0)} mV',
                style: TextStyle(
                    color: e.desbalanceMv > 50 ? Colors.orange : null, fontSize: 12)),
          ]),
          const SizedBox(height: 12),
          ...e.vCeldas.entries.map((kv) => Padding(
            padding: const EdgeInsets.symmetric(vertical: 3),
            child: Row(children: [
              SizedBox(width: 36, child: Text(kv.key, style: const TextStyle(fontSize: 12))),
              Expanded(
                child: LinearProgressIndicator(
                  value: maxV > 0 ? (kv.value / maxV).clamp(0.0, 1.0).toDouble() : 0.0,
                  minHeight: 8,
                ),
              ),
              const SizedBox(width: 10),
              Text('${kv.value.toStringAsFixed(2)} V', style: const TextStyle(fontSize: 13)),
            ]),
          )),
        ]),
      ),
    );
  }
}

// ============================================================
//  Pestaña 2: Historial (RN07) - solo lectura
// ============================================================
class VistaHistorial extends StatefulWidget {
  final BmsApi? api;
  const VistaHistorial({super.key, required this.api});

  @override
  State<VistaHistorial> createState() => VistaHistorialState();
}

class VistaHistorialState extends State<VistaHistorial> {
  List<Evento> eventos = [];
  bool soloFallas = false;
  bool cargando = false;
  String? error;

  @override
  void didUpdateWidget(VistaHistorial old) {
    super.didUpdateWidget(old);
    if (old.api != widget.api) cargar();
  }

  Future<void> cargar() async {
    final api = widget.api;
    if (api == null) {
      if (mounted) setState(() => eventos = []);
      return;
    }
    if (mounted) setState(() => cargando = true);
    try {
      final l = await api.eventos(limite: 300, soloFallas: soloFallas);
      if (!mounted) return;
      setState(() {
        eventos = l;
        error = null;
        cargando = false;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        error = '$e'.replaceFirst('Exception: ', '');
        cargando = false;
      });
    }
  }

  (IconData, Color) _estilo(Evento e) => switch (e.evento) {
    'CORTE' => (iconoAlerta(e.regla),
    e.severidad == 'CRITICA' ? Colors.red : Colors.orange),
    'RECONEXION' => (Icons.power, Colors.green),
    'REARME' || 'SOLICITUD_REARME' => (Icons.restart_alt, Colors.lightBlue),
    'TENDENCIA' => (Icons.trending_up, Colors.amber),
    'AVISO' => (Icons.warning_amber, Colors.amber),
    'MODO' => (Icons.swap_vert, Colors.lightBlue),
    'INICIO_SEGURO' => (Icons.verified_user, Colors.green),
    'SIN_DATOS_ESP32' => (Icons.link_off, Colors.grey),
    'RECONEXION_ESP32' => (Icons.link, Colors.green),
    'CONFIG' => (Icons.tune, Colors.blueGrey),
    _ => (Icons.info_outline, Colors.blueGrey),
  };

  String _valores(Evento e) {
    final p = <String>[];
    if (e.vBanco != null) p.add('${e.vBanco!.toStringAsFixed(2)} V');
    if (e.iCarga != null && e.iDesc != null) {
      p.add('carga ${e.iCarga!.toStringAsFixed(1)} A · motor ${e.iDesc!.toStringAsFixed(1)} A');
    }
    if (e.temp1 != null && e.temp2 != null) {
      p.add('${e.temp1!.toStringAsFixed(1)} / ${e.temp2!.toStringAsFixed(1)} °C');
    }
    if (e.actFuente != null && e.actMotor != null) {
      p.add('fuente ${e.actFuente == 1 ? 'ON' : 'OFF'} · motor ${e.actMotor == 1 ? 'ON' : 'OFF'}');
    }
    return p.join('  |  ');
  }

  @override
  Widget build(BuildContext c) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Historial'),
        actions: [
          IconButton(
            tooltip: 'Actualizar',
            onPressed: widget.api == null ? null : cargar,
            icon: const Icon(Icons.refresh),
          ),
        ],
      ),
      body: widget.api == null
          ? const Center(
        child: Padding(
          padding: EdgeInsets.all(24),
          child: Text('Conéctate a la Raspberry en Ajustes para ver el historial.',
              textAlign: TextAlign.center),
        ),
      )
          : Column(children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
          child: SegmentedButton<bool>(
            segments: const [
              ButtonSegment(value: false, label: Text('Todo'), icon: Icon(Icons.list)),
              ButtonSegment(
                  value: true, label: Text('Solo fallas'), icon: Icon(Icons.warning_amber)),
            ],
            selected: {soloFallas},
            onSelectionChanged: (s) {
              setState(() => soloFallas = s.first);
              cargar();
            },
          ),
        ),
        if (cargando) const LinearProgressIndicator(),
        if (error != null)
          Padding(
            padding: const EdgeInsets.all(12),
            child: Text(error!, style: const TextStyle(color: Colors.red)),
          ),
        Expanded(
          child: RefreshIndicator(
            onRefresh: cargar,
            child: eventos.isEmpty && !cargando
                ? ListView(children: const [
              SizedBox(height: 120),
              Center(child: Text('Sin eventos registrados')),
            ])
                : ListView.separated(
              padding: const EdgeInsets.fromLTRB(12, 4, 12, 24),
              itemCount: eventos.length,
              separatorBuilder: (_, __) => const Divider(height: 1),
              itemBuilder: (_, i) {
                final e = eventos[i];
                final (icono, color) = _estilo(e);
                final vals = _valores(e);
                return ListTile(
                  contentPadding: const EdgeInsets.symmetric(horizontal: 4),
                  leading: CircleAvatar(
                    backgroundColor: color.withValues(alpha: 0.18),
                    child: Icon(icono, color: color),
                  ),
                  title: Text(e.mensaje, style: const TextStyle(fontSize: 13.5)),
                  subtitle: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      const SizedBox(height: 3),
                      Text(
                          '${fechaHora(e.fecha)}  ·  ${e.evento}'
                              '${e.regla != null ? ' ${e.regla}' : ''}',
                          style: TextStyle(
                              fontSize: 11.5,
                              color: color,
                              fontWeight: FontWeight.w600)),
                      if (vals.isNotEmpty)
                        Text(vals, style: const TextStyle(fontSize: 11)),
                    ],
                  ),
                );
              },
            ),
          ),
        ),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: Text(
            'Para borrar el historial, en la Raspberry: bms borrar eventos',
            style: Theme.of(c).textTheme.labelSmall,
          ),
        ),
      ]),
    );
  }
}

// ============================================================
//  Pestaña 3: Ajustes y conexión
// ============================================================
class _VistaAjustesConexion extends StatefulWidget {
  final String hostInicial;
  final bool conectado;
  final bool conectando;
  final String? errorConexion;
  final BmsApi? api;
  final ValueChanged<String> onCambiarHost;
  final VoidCallback onConectar;
  final VoidCallback onDesconectar;

  const _VistaAjustesConexion({
    required this.hostInicial,
    required this.conectado,
    required this.conectando,
    required this.errorConexion,
    required this.api,
    required this.onCambiarHost,
    required this.onConectar,
    required this.onDesconectar,
  });

  @override
  State<_VistaAjustesConexion> createState() => _VistaAjustesConexionState();
}

class _VistaAjustesConexionState extends State<_VistaAjustesConexion> {
  late final TextEditingController _ctrlHost =
  TextEditingController(text: widget.hostInicial);

  @override
  void didUpdateWidget(_VistaAjustesConexion old) {
    super.didUpdateWidget(old);
    // la IP guardada se carga después de abrir la app
    if (widget.hostInicial != old.hostInicial && _ctrlHost.text != widget.hostInicial) {
      _ctrlHost.text = widget.hostInicial;
    }
  }

  @override
  void dispose() {
    _ctrlHost.dispose();
    super.dispose();
  }

  void _abrir(BuildContext c, Widget Function(BmsApi) pantalla) {
    final api = widget.api;
    if (api == null) {
      ScaffoldMessenger.of(c).showSnackBar(const SnackBar(
          content: Text('Primero conéctate a la Raspberry.')));
      return;
    }
    Navigator.push(c, MaterialPageRoute(builder: (_) => pantalla(api)));
  }

  @override
  Widget build(BuildContext c) {
    final colorEstado = widget.conectado
        ? Colors.green
        : (widget.conectando ? Colors.blue : Colors.orange);
    return Scaffold(
      appBar: AppBar(title: const Text('Ajustes y conexión')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          Card(
            child: Padding(
              padding: const EdgeInsets.all(20),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Row(children: [
                  Icon(
                    widget.conectado
                        ? Icons.check_circle
                        : (widget.conectando ? Icons.sync : Icons.highlight_off),
                    color: colorEstado,
                    size: 28,
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Text(
                      widget.conectado
                          ? 'Conectado a la Raspberry Pi'
                          : (widget.conectando ? 'Conectando...' : 'Desconectado'),
                      style: Theme.of(c)
                          .textTheme
                          .titleMedium
                          ?.copyWith(fontWeight: FontWeight.bold),
                    ),
                  ),
                ]),
                const Divider(height: 32),
                TextField(
                  controller: _ctrlHost,
                  enabled: !widget.conectando && !widget.conectado,
                  keyboardType: TextInputType.url,
                  decoration: const InputDecoration(
                    labelText: 'IP de la Raspberry Pi',
                    hintText: '192.168.1.42:8000',
                    border: OutlineInputBorder(),
                    prefixIcon: Icon(Icons.dns),
                    helperText: 'Se guarda la última IP con la que te conectaste',
                  ),
                  onChanged: widget.onCambiarHost,
                ),
                const SizedBox(height: 16),
                SizedBox(
                  width: double.infinity,
                  child: widget.conectado
                      ? OutlinedButton.icon(
                    onPressed: widget.onDesconectar,
                    icon: const Icon(Icons.link_off, color: Colors.red),
                    label: const Text('Desconectar', style: TextStyle(color: Colors.red)),
                  )
                      : FilledButton.icon(
                    onPressed: widget.conectando ? null : widget.onConectar,
                    icon: widget.conectando
                        ? const SizedBox(
                        width: 18,
                        height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2))
                        : const Icon(Icons.link),
                    label: Text(widget.conectando ? 'Conectando...' : 'Conectar'),
                  ),
                ),
                if (widget.errorConexion != null) ...[
                  const SizedBox(height: 12),
                  Text(widget.errorConexion!,
                      style: const TextStyle(color: Colors.red, fontSize: 12)),
                ],
              ]),
            ),
          ),
          const SizedBox(height: 20),
          Text('Banco de baterías',
              style: Theme.of(c).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.bold)),
          const SizedBox(height: 8),
          Card(
            child: Column(children: [
              ListTile(
                leading: const Icon(Icons.battery_saver, color: Colors.green),
                title: const Text('Datos del banco'),
                subtitle: const Text('Arreglo (1S-3S), tipo de batería y capacidad nominal'),
                trailing: const Icon(Icons.chevron_right),
                onTap: () => _abrir(c, (api) => PantallaBanco(api: api)),
              ),
              const Divider(height: 1),
              ListTile(
                leading: const Icon(Icons.tune, color: Colors.orange),
                title: const Text('Límites de seguridad'),
                subtitle: const Text('Temperatura, voltajes, corriente y tiempos'),
                trailing: const Icon(Icons.chevron_right),
                onTap: () => _abrir(c, (api) => PantallaUmbrales(api: api)),
              ),
            ]),
          ),
        ],
      ),
    );
  }
}

// ============================================================
//  Widgets reutilizables
// ============================================================
class _Banner extends StatelessWidget {
  final IconData icono;
  final String texto;
  final Color color;

  const _Banner({required this.icono, required this.texto, required this.color});

  @override
  Widget build(BuildContext c) => Container(
    margin: const EdgeInsets.only(bottom: 12),
    padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
    decoration: BoxDecoration(
      color: color.withValues(alpha: 0.15),
      border: Border.all(color: color),
      borderRadius: BorderRadius.circular(12),
    ),
    child: Row(children: [
      Icon(icono, color: color),
      const SizedBox(width: 12),
      Expanded(
          child: Text(texto,
              style: TextStyle(color: color, fontWeight: FontWeight.w600))),
    ]),
  );
}

// ============================================================
//  Límites de seguridad (editables para la presentación)
// ============================================================
class PantallaUmbrales extends StatefulWidget {
  final BmsApi api;
  const PantallaUmbrales({super.key, required this.api});

  @override
  State<PantallaUmbrales> createState() => _PantallaUmbralesState();
}

class _Campo {
  final String clave, titulo, unidad, regla;
  final bool entero;
  const _Campo(this.clave, this.titulo, this.unidad, this.regla, {this.entero = false});
}

class _PantallaUmbralesState extends State<PantallaUmbrales> {
  // (sección, campos)
  static const _secciones = [
    ('Temperatura', [
      _Campo('temp_max', 'Temperatura máxima', '°C', 'RN01'),
      _Campo('hist_temp', 'Margen para reconectar', '°C', 'RN04'),
      _Campo('aviso_temp_c', 'Aviso de calentamiento', '°C', 'aviso'),
    ]),
    ('Voltaje del banco', [
      _Campo('volt_min', 'Voltaje mínimo (corta el motor)', 'V', 'RN02'),
      _Campo('hist_v_min', 'Margen para reconectar el motor', 'V', 'RN04'),
      _Campo('volt_max', 'Voltaje máximo (corta la fuente)', 'V', 'RN03'),
      _Campo('hist_v_max', 'Margen para reconectar la fuente', 'V', 'RN04'),
    ]),
    ('Corriente y tiempos', [
      _Campo('corr_max', 'Corriente máxima del motor', 'A', 'RN10'),
      _Campo('t_reconexion_s', 'Tiempo para reconectar', 's', 'RN04', entero: true),
      _Campo('aviso_soc_bajo', 'Aviso de poca carga', '%', 'aviso'),
    ]),
  ];

  final _ctrl = <String, TextEditingController>{};
  Map<String, dynamic>? _original;
  bool _guardando = false;
  String? _error;

  @override
  void initState() {
    super.initState();
    _cargar();
  }

  @override
  void dispose() {
    for (final c in _ctrl.values) {
      c.dispose();
    }
    super.dispose();
  }

  String _fmt(dynamic v, bool entero) =>
      v == null ? '' : (entero ? (v as num).round().toString() : (v as num).toDouble().toString());

  Future<void> _cargar() async {
    try {
      final cfg = await widget.api.config();
      if (!mounted) return;
      setState(() {
        _original = cfg;
        _error = null;
        for (final (_, campos) in _secciones) {
          for (final f in campos) {
            (_ctrl[f.clave] ??= TextEditingController()).text = _fmt(cfg[f.clave], f.entero);
          }
        }
      });
    } catch (e) {
      if (mounted) setState(() => _error = '$e'.replaceFirst('Exception: ', ''));
    }
  }

  double? _num(String clave) => double.tryParse((_ctrl[clave]?.text ?? '').replaceAll(',', '.'));

  /// Texto de ayuda que se actualiza mientras escribes.
  String? _ayuda(String clave) {
    String f(double? x) => x == null ? '?' : x.toStringAsFixed(x % 1 == 0 ? 0 : 2);
    switch (clave) {
      case 'temp_max':
        return 'Arriba de esto se cortan fuente y motor';
      case 'hist_temp':
        final t = _num('temp_max'), h = _num('hist_temp');
        return 'Se reconecta abajo de ${f(t != null && h != null ? t - h : null)} °C';
      case 'volt_min':
        return 'Abajo de esto se corta el motor (deja reserva)';
      case 'hist_v_min':
        final v = _num('volt_min'), h = _num('hist_v_min');
        return 'El motor vuelve arriba de ${f(v != null && h != null ? v + h : null)} V';
      case 'volt_max':
        return 'Arriba de esto se corta la fuente (banco lleno)';
      case 'hist_v_max':
        final v = _num('volt_max'), h = _num('hist_v_max');
        return 'La fuente vuelve abajo de ${f(v != null && h != null ? v - h : null)} V';
      case 'corr_max':
        return 'Arriba de esto se corta el motor y hay que rearmar';
      case 't_reconexion_s':
        return 'Tiempo seguido en rango seguro antes de reconectar';
      case 'aviso_temp_c':
        return 'Solo avisa en la app, no corta';
      case 'aviso_soc_bajo':
        return 'Solo avisa en la app, no corta';
    }
    return null;
  }

  Future<void> _guardar() async {
    final cambios = <String, dynamic>{};
    for (final (_, campos) in _secciones) {
      for (final f in campos) {
        final v = _num(f.clave);
        if (v == null) {
          setState(() => _error = 'Revisa "${f.titulo}": escribe un número');
          return;
        }
        final antes = (_original?[f.clave] as num?)?.toDouble();
        if (antes == null || (antes - v).abs() > 1e-6) cambios[f.clave] = f.entero ? v.round() : v;
      }
    }
    if (cambios.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('No hay cambios.')));
      return;
    }
    await _enviar(() => widget.api.guardarConfig(cambios), 'Guardado y enviado al ESP32');
  }

  Future<void> _restaurar() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (d) => AlertDialog(
        title: const Text('¿Restaurar valores por defecto?'),
        content: Text('Se usarán los valores normales para tu banco '
            '(${_original?['num_celdas'] ?? 3}S ${_original?['quimica'] == 'LIFEPO4' ? 'LiFePO4' : 'Li-ion'}).'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(d, false), child: const Text('Cancelar')),
          FilledButton(onPressed: () => Navigator.pop(d, true), child: const Text('Restaurar')),
        ],
      ),
    );
    if (ok == true) await _enviar(widget.api.restaurarConfig, 'Valores por defecto restaurados');
  }

  Future<void> _enviar(Future<Map<String, dynamic>> Function() accion, String exito) async {
    setState(() {
      _guardando = true;
      _error = null;
    });
    try {
      await accion();
      if (!mounted) return;
      await _cargar();
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(exito)));
    } catch (e) {
      if (mounted) setState(() => _error = '$e'.replaceFirst('Exception: ', ''));
    } finally {
      if (mounted) setState(() => _guardando = false);
    }
  }

  @override
  Widget build(BuildContext c) => Scaffold(
    appBar: AppBar(title: const Text('Límites de seguridad')),
    body: _original == null && _error == null
        ? const Center(child: CircularProgressIndicator())
        : ListView(padding: const EdgeInsets.all(16), children: [
      for (final (titulo, campos) in _secciones) ...[
        Padding(
          padding: const EdgeInsets.only(top: 4, bottom: 8),
          child: Text(titulo,
              style: Theme.of(c).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.bold)),
        ),
        for (final f in campos)
          Padding(
            padding: const EdgeInsets.only(bottom: 14),
            child: TextField(
              controller: _ctrl[f.clave] ??= TextEditingController(),
              enabled: !_guardando,
              keyboardType: TextInputType.numberWithOptions(decimal: !f.entero),
              onChanged: (_) => setState(() {}),
              decoration: InputDecoration(
                labelText: f.titulo,
                suffixText: f.unidad,
                helperText: _ayuda(f.clave),
                border: const OutlineInputBorder(),
                prefixIcon: Padding(
                  padding: const EdgeInsets.all(12),
                  child: Text(f.regla == 'aviso' ? '🔔' : f.regla,
                      style: const TextStyle(fontSize: 11, fontWeight: FontWeight.bold)),
                ),
              ),
            ),
          ),
      ],
      if (_error != null)
        Padding(
          padding: const EdgeInsets.only(bottom: 12),
          child: Text(_error!, style: const TextStyle(color: Colors.red)),
        ),
      FilledButton.icon(
        onPressed: _guardando || _original == null ? null : _guardar,
        icon: _guardando
            ? const SizedBox(
            width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
            : const Icon(Icons.save),
        label: const Text('Guardar y enviar al ESP32'),
      ),
      const SizedBox(height: 8),
      OutlinedButton.icon(
        onPressed: _guardando || _original == null ? null : _restaurar,
        icon: const Icon(Icons.restart_alt),
        label: const Text('Restaurar valores por defecto'),
      ),
      const SizedBox(height: 16),
      Text('El rearme después de una sobrecorriente (RN10) se hace en la Raspberry: bms rearmar',
          style: Theme.of(c).textTheme.labelSmall),
    ]),
  );
}

// ============================================================
//  Datos del banco (editable desde la app)
// ============================================================
class PantallaBanco extends StatefulWidget {
  final BmsApi api;
  const PantallaBanco({super.key, required this.api});

  @override
  State<PantallaBanco> createState() => _PantallaBancoState();
}

class _PantallaBancoState extends State<PantallaBanco> {
  Banco? b;
  int celdas = 3;
  String quimica = 'LIION';
  final cap = TextEditingController();
  bool guardando = false;
  String? error;

  static const _limitesCelda = {'LIION': (3.00, 4.20), 'LIFEPO4': (2.50, 3.65)};

  @override
  void initState() {
    super.initState();
    widget.api.banco().then((x) {
      if (!mounted) return;
      setState(() {
        b = x;
        celdas = x.numCeldas < 1 ? 1 : (x.numCeldas > 3 ? 3 : x.numCeldas);
        quimica = x.quimica;
        cap.text = x.capacidadAh.toStringAsFixed(1);
      });
    }).catchError((e) {
      if (mounted) setState(() => error = '$e'.replaceFirst('Exception: ', ''));
    });
  }

  @override
  void dispose() {
    cap.dispose();
    super.dispose();
  }

  Future<void> _guardar() async {
    final ah = double.tryParse(cap.text.replaceAll(',', '.'));
    if (ah == null || ah <= 0) {
      setState(() => error = 'Escribe la capacidad en Ah, por ejemplo 10');
      return;
    }
    setState(() {
      guardando = true;
      error = null;
    });
    try {
      final r = await widget.api.configurarBanco(
          numCeldas: celdas, quimica: quimica, capacidadAh: ah);
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Guardado: ${r.numCeldas}S ${r.quimica == 'LIION' ? 'Li-ion' : 'LiFePO4'} '
            '${r.capacidadAh.toStringAsFixed(1)} Ah'
            '${r.voltMin != null ? '. Cortes del banco: ${r.voltMin!.toStringAsFixed(1)} V – ${r.voltMax!.toStringAsFixed(1)} V' : ''}'),
      ));
      Navigator.pop(context);
    } catch (e) {
      if (!mounted) return;
      setState(() {
        guardando = false;
        error = '$e'.replaceFirst('Exception: ', '');
      });
    }
  }

  @override
  Widget build(BuildContext c) {
    final (vmin, vmax) = _limitesCelda[quimica] ?? (3.0, 4.2);
    return Scaffold(
      appBar: AppBar(title: const Text('Datos del banco')),
      body: b == null && error == null
          ? const Center(child: CircularProgressIndicator())
          : ListView(padding: const EdgeInsets.all(16), children: [
        Text('Arreglo (celdas en serie)', style: Theme.of(c).textTheme.labelLarge),
        const SizedBox(height: 8),
        SegmentedButton<int>(
          segments: const [
            ButtonSegment(value: 1, label: Text('1S')),
            ButtonSegment(value: 2, label: Text('2S')),
            ButtonSegment(value: 3, label: Text('3S')),
          ],
          selected: {celdas},
          onSelectionChanged: (s) => setState(() => celdas = s.first),
        ),
        const SizedBox(height: 6),
        Text('La placa mide 3 tomas (4V, 8V, 12V), por eso el máximo es 3S.',
            style: Theme.of(c).textTheme.labelSmall),
        const SizedBox(height: 24),
        Text('Tipo de batería', style: Theme.of(c).textTheme.labelLarge),
        const SizedBox(height: 8),
        SegmentedButton<String>(
          segments: const [
            ButtonSegment(value: 'LIION', label: Text('Li-ion')),
            ButtonSegment(value: 'LIFEPO4', label: Text('LiFePO4')),
          ],
          selected: {quimica},
          onSelectionChanged: (s) => setState(() => quimica = s.first),
        ),
        const SizedBox(height: 24),
        TextField(
          controller: cap,
          keyboardType: const TextInputType.numberWithOptions(decimal: true),
          decoration: const InputDecoration(
            labelText: 'Capacidad nominal (Ah)',
            border: OutlineInputBorder(),
            helperText: 'El SOC y el SOH se calculan con este valor',
          ),
        ),
        const SizedBox(height: 16),
        _Banner(
          icono: Icons.info_outline,
          color: Colors.blueGrey,
          texto: 'Con ${celdas}S ${quimica == 'LIION' ? 'Li-ion' : 'LiFePO4'} la Raspberry '
              'ajusta sola los cortes del banco: motor abajo de '
              '${(celdas * vmin).toStringAsFixed(1)} V y fuente arriba de '
              '${(celdas * vmax).toStringAsFixed(1)} V.',
        ),
        if (error != null)
          Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: Text(error!, style: const TextStyle(color: Colors.red)),
          ),
        FilledButton.icon(
          onPressed: guardando || b == null ? null : _guardar,
          icon: guardando
              ? const SizedBox(
              width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2))
              : const Icon(Icons.save),
          label: const Text('Guardar'),
        ),
      ]),
    );
  }
}