// Autor: Raul Alejandro Moreno Camargo
// Capa de datos de la app Flutter. Habla con bms_service.py en la Raspberry Pi 5.
//
// RN06: la app solo LEE lecturas, alertas, límites e historial.
//       Lo único que puede cambiar son los datos del banco (arreglo, tipo de batería
//       y capacidad nominal). Los límites de seguridad y el rearme se hacen en la Raspberry.

import 'dart:async';
import 'dart:convert';
import 'package:http/http.dart' as http;
import 'package:web_socket_channel/web_socket_channel.dart';

// ============================================================
//  Modelos
// ============================================================
enum Magnitud { temp, volt, corr }

Magnitud _mag(String? s) => switch (s) {
      'TEMP' => Magnitud.temp,
      'VOLT' => Magnitud.volt,
      _ => Magnitud.corr,
    };

double? _d(dynamic v) => v == null ? null : (v as num).toDouble();

class Sensor {
  final String id;
  final Magnitud magnitud;
  final String unidad;
  final String? ubicacion;
  final String? rol; // PACK_V | CHG_I | MAIN_I | null
  final String? celdaId;

  Sensor({
    required this.id,
    required this.magnitud,
    required this.unidad,
    this.ubicacion,
    this.rol,
    this.celdaId,
  });

  factory Sensor.fromJson(Map<String, dynamic> j) => Sensor(
        id: j['id'],
        magnitud: _mag(j['magnitud']),
        unidad: j['unidad'] ?? '',
        ubicacion: j['ubicacion'],
        rol: j['rol'],
        celdaId: j['celda_id'],
      );
}

class Alerta {
  final String sensorId;
  final String severidad; // CRITICA | AVISO
  final String accion; // CORTE | NOTIF
  final String? titulo;
  final String mensaje;
  final String? tipo; // RN01, RN02, RN03, RN05, RN09, RN10, CALENTANDO, SOC_BAJO, SIN_DATOS
  final double valor;
  final DateTime ts;

  Alerta.fromJson(Map<String, dynamic> j)
      : sensorId = j['sensor_id'] ?? '',
        severidad = j['severidad'] ?? 'AVISO',
        accion = j['accion'] ?? 'NOTIF',
        titulo = j['titulo'],
        mensaje = j['mensaje'] ?? '',
        tipo = j['tipo'],
        valor = _d(j['valor']) ?? 0,
        ts = DateTime.fromMillisecondsSinceEpoch(
            (((j['ts'] as num?) ?? 0) * 1000).round());

  bool get esCritica => severidad == 'CRITICA';
  bool get corta => accion == 'CORTE';

  /// Identifica la alerta para no repetir el sonido de la misma.
  String get clave => '$tipo|${ts.millisecondsSinceEpoch}';
}

enum ModoBanco { carga, descarga, reposo }

class EstadoBanco {
  final String bancoId;
  final Map<String, double> valores; // sensorId -> valor
  final Map<String, double> vCeldas;
  final double? vPack;
  final double iMain;
  final double? tempMax;
  final double? tempMin;
  final double? tempProm;
  final double potenciaW;
  final double soc;
  final double soh;
  final ModoBanco modo;
  final double desbalanceMv;
  final List<Alerta> alertas;
  final bool corteActivo;
  final bool conectado;

  // Relevadores (actuadores) que reporta el ESP32
  final bool? actFuente; // Actuador 1: fuente -> banco
  final bool? actMotor; // Actuador 2: banco -> motor
  final int esperaFuenteS;
  final int esperaMotorS;
  final bool inicioSeguro;
  final List<String> reglasActivas;

  EstadoBanco.fromJson(Map<String, dynamic> j)
      : bancoId = j['banco_id'] ?? 'B1',
        valores = (j['valores'] as Map? ?? {})
            .map((k, v) => MapEntry(k as String, (v as num).toDouble())),
        vCeldas = (j['v_celdas'] as Map? ?? {})
            .map((k, v) => MapEntry(k as String, (v as num).toDouble())),
        vPack = _d(j['v_pack']),
        iMain = _d(j['i_main']) ?? 0,
        tempMax = _d(j['temp_max']),
        tempMin = _d(j['temp_min']),
        tempProm = _d(j['temp_prom']),
        potenciaW = _d(j['potencia_w']) ?? 0,
        soc = _d(j['soc']) ?? 0,
        soh = _d(j['soh']) ?? 0,
        modo = switch (j['modo']) {
          'CARGA' => ModoBanco.carga,
          'DESCARGA' => ModoBanco.descarga,
          _ => ModoBanco.reposo,
        },
        desbalanceMv = _d(j['desbalance_mv']) ?? 0,
        alertas = ((j['alertas'] as List?) ?? [])
            .map((a) => Alerta.fromJson(a as Map<String, dynamic>))
            .toList(),
        corteActivo = j['corte_activo'] ?? false,
        conectado = j['conectado'] ?? false,
        actFuente = (j['actuadores'] as Map?)?['fuente'] == null
            ? null
            : (j['actuadores']['fuente'] as num) == 1,
        actMotor = (j['actuadores'] as Map?)?['motor'] == null
            ? null
            : (j['actuadores']['motor'] as num) == 1,
        esperaFuenteS =
            ((j['actuadores'] as Map?)?['espera_fuente_s'] as num?)?.toInt() ?? 0,
        esperaMotorS =
            ((j['actuadores'] as Map?)?['espera_motor_s'] as num?)?.toInt() ?? 0,
        inicioSeguro = j['inicio_seguro'] ?? false,
        reglasActivas =
            ((j['reglas_activas'] as List?) ?? []).map((e) => '$e').toList();

  bool get hayCritica => alertas.any((a) => a.esCritica);
}

class Banco {
  final String id, nombre, quimica;
  final int numCeldas;
  final double capacidadAh;
  final double? voltMin, voltMax; // cortes RN02 / RN03 recalculados por la Raspberry

  Banco.fromJson(Map<String, dynamic> j)
      : id = j['id'] ?? 'B1',
        nombre = j['nombre'] ?? '',
        quimica = j['quimica'] ?? 'LIION',
        numCeldas = (j['num_celdas'] as num? ?? 3).toInt(),
        capacidadAh = _d(j['capacidad_ah']) ?? 0,
        voltMin = _d(j['volt_min']),
        voltMax = _d(j['volt_max']);
}

class Umbral {
  final int id;
  final String magnitud, severidad, accion;
  final String? rol, sensorId, regla;
  final double? valorMin, valorMax;

  Umbral.fromJson(Map<String, dynamic> j)
      : id = (j['id'] as num).toInt(),
        magnitud = j['magnitud'] ?? '',
        severidad = j['severidad'] ?? '',
        accion = j['accion'] ?? '',
        rol = j['rol'],
        sensorId = j['sensor_id'],
        regla = j['regla'],
        valorMin = _d(j['valor_min']),
        valorMax = _d(j['valor_max']);
}

/// Un registro del historial (RN07).
class Evento {
  final DateTime fecha;
  final String evento; // CORTE, RECONEXION, TENDENCIA, AVISO, MODO, CONFIG, ...
  final String? regla, nombre, severidad;
  final String mensaje;
  final int? actFuente, actMotor;
  final double? vBanco, iCarga, iDesc, temp1, temp2, soc;

  Evento.fromJson(Map<String, dynamic> j)
      : fecha = DateTime.tryParse(j['fecha'] ?? '')?.toLocal() ?? DateTime.now(),
        evento = j['evento'] ?? '',
        regla = j['regla'],
        nombre = j['nombre'],
        severidad = j['severidad'],
        mensaje = j['mensaje'] ?? '',
        actFuente = (j['act_fuente'] as num?)?.toInt(),
        actMotor = (j['act_motor'] as num?)?.toInt(),
        vBanco = _d(j['v_banco']),
        iCarga = _d(j['i_carga']),
        iDesc = _d(j['i_desc']),
        temp1 = _d(j['temp1']),
        temp2 = _d(j['temp2']),
        soc = _d(j['soc']);
}

class PuntoSerie {
  final DateTime t;
  final double v;
  PuntoSerie(this.t, this.v);
}

// ============================================================
//  Cliente REST
// ============================================================
class BmsApi {
  final String host; // ej. "192.168.1.42:8000"
  BmsApi(this.host);

  static const _timeout = Duration(seconds: 6);

  Uri _u(String p, [Map<String, String>? q]) => Uri.http(host, p, q);

  /// Lanza una excepción con el mensaje que manda la Raspberry si algo falla.
  dynamic _json(http.Response r) {
    final cuerpo = utf8.decode(r.bodyBytes);
    if (r.statusCode >= 200 && r.statusCode < 300) return jsonDecode(cuerpo);
    String detalle = 'Error ${r.statusCode}';
    try {
      detalle = (jsonDecode(cuerpo) as Map)['detail']?.toString() ?? detalle;
    } catch (_) {}
    throw Exception(detalle);
  }

  Future<dynamic> _get(String p, [Map<String, String>? q]) async =>
      _json(await http.get(_u(p, q)).timeout(_timeout));

  Future<Banco> banco() async => Banco.fromJson(await _get('/api/banco'));

  /// Arreglo (1S, 2S, 3S), tipo de batería (LIION / LIFEPO4) y capacidad nominal.
  /// La Raspberry recalcula sola los cortes de voltaje y se los manda al ESP32.
  Future<Banco> configurarBanco({
    int? numCeldas,
    String? quimica,
    double? capacidadAh,
  }) async {
    final r = await http
        .put(
          _u('/api/banco'),
          headers: {'Content-Type': 'application/json'},
          body: jsonEncode({
            if (numCeldas != null) 'num_celdas': numCeldas,
            if (quimica != null) 'quimica': quimica,
            if (capacidadAh != null) 'capacidad_ah': capacidadAh,
          }),
        )
        .timeout(_timeout);
    return Banco.fromJson(_json(r));
  }

  Future<List<Sensor>> sensores() async =>
      (await _get('/api/sensores') as List).map((e) => Sensor.fromJson(e)).toList();

  Future<EstadoBanco> estado() async => EstadoBanco.fromJson(await _get('/api/estado'));

  /// Límites de seguridad (solo lectura).
  Future<List<Umbral>> umbrales() async =>
      (await _get('/api/umbrales') as List).map((e) => Umbral.fromJson(e)).toList();

  /// Historial de eventos (RN07), del más reciente al más viejo.
  Future<List<Evento>> eventos({int limite = 200, bool soloFallas = false}) async =>
      (await _get('/api/eventos', {
        'limite': '$limite',
        'solo_fallas': soloFallas ? 'true' : 'false',
      }) as List)
          .map((e) => Evento.fromJson(e))
          .toList();

  Future<Map<String, List<PuntoSerie>>> historico(
    Magnitud m, {
    String rango = '-1h',
    String cada = '1m',
  }) async {
    final mag = switch (m) {
      Magnitud.temp => 'TEMP',
      Magnitud.volt => 'VOLT',
      Magnitud.corr => 'CORR',
    };
    final j = await _get('/api/historico', {'magnitud': mag, 'rango': rango, 'cada': cada})
        as Map<String, dynamic>;
    return j.map((k, v) => MapEntry(
        k,
        (v as List)
            .map((p) => PuntoSerie(DateTime.parse(p['t']), (p['v'] as num).toDouble()))
            .toList()));
  }
}

// ============================================================
//  Telemetría en vivo (WebSocket, 2 Hz)
// ============================================================
class BmsLive {
  final String host;
  WebSocketChannel? _ch;
  StreamSubscription? _sub;
  Timer? _retry;
  bool _cerrado = false;

  final _estado = StreamController<EstadoBanco>.broadcast();
  final _alertas = StreamController<Alerta>.broadcast();
  final _sensores = StreamController<List<Sensor>>.broadcast();

  Stream<EstadoBanco> get estado => _estado.stream;

  /// Llega una vez por cada alerta NUEVA (aquí suena la app).
  Stream<Alerta> get alertas => _alertas.stream;
  Stream<List<Sensor>> get sensores => _sensores.stream;

  BmsLive(this.host);

  void conectar() {
    if (_cerrado) return;
    _retry?.cancel();
    try {
      _ch = WebSocketChannel.connect(Uri.parse('ws://$host/ws'));
      _sub = _ch!.stream.listen(
        (raw) {
          final m = jsonDecode(raw as String) as Map<String, dynamic>;
          switch (m['tipo']) {
            case 'estado':
              _estado.add(EstadoBanco.fromJson(m['datos']));
            case 'alerta':
              _alertas.add(Alerta.fromJson(m['datos']));
            case 'sensores':
              _sensores.add(
                  (m['datos'] as List).map((e) => Sensor.fromJson(e)).toList());
          }
        },
        onDone: _reintentar,
        onError: (_) => _reintentar(),
        cancelOnError: true,
      );
    } catch (_) {
      _reintentar();
    }
  }

  // Con el equipo en el campo el WiFi se cae; reconectar solo es obligatorio.
  void _reintentar() {
    if (_cerrado) return;
    _sub?.cancel();
    _retry?.cancel();
    _retry = Timer(const Duration(seconds: 3), conectar);
  }

  void cerrar() {
    _cerrado = true;
    _retry?.cancel();
    _sub?.cancel();
    _ch?.sink.close();
    _estado.close();
    _alertas.close();
    _sensores.close();
  }
}
