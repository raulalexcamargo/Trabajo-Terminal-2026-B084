// ============================================================================
// Autor: Raul Alejandro Moreno Camargo
// Proyecto: ECU BMS - Banco de baterías Li-ion 3S
// Entorno: PlatformIO (ESP32 DOIT DevKit V1) + UART2 hacia Raspberry Pi 5
//
// Implementa las Reglas de Negocio (Tabla 10 del documento):
//   RN01  Temperatura > temp_max          -> desactiva Actuador 1 y Actuador 2
//   RN02  Voltaje banco < volt_min        -> desactiva Actuador 2 (carga/motor)
//   RN03  Voltaje banco > volt_max        -> desactiva Actuador 1 (fuente)
//   RN04  Reconexión solo tras 30 s continuos en rango seguro (RN01, RN02, RN03)
//   RN05  Al encender: actuadores desactivados hasta una lectura válida y en rango
//   RN07  Cada corte / reconexión se reporta a la Raspberry con los valores de los sensores
//   RN08  Los set points solo llegan desde la Raspberry (UART), se guardan en flash
//   RN09  Tendencia: ΔT > 5 °C en < 10 s o ΔV > 0.5 V en < 5 s -> alerta SIN corte
//   RN10  Corriente > corr_max            -> desactiva Actuador 2 de inmediato
//         (no está en RN04: se queda cortado hasta REARMAR desde la Raspberry)
//   RN11  Modo de operación (lo elige el usuario en la app):
//           APAGADO -> fuente y motor desactivados
//           CARGA   -> solo la fuente; al llenarse el banco (RN03) pasa solo a APAGADO
//           MOTOR   -> solo el motor;  con RN02 (descargado) o RN10 pasa solo a APAGADO
//         Nunca se carga y se usa el motor a la vez. Las protecciones RN01-RN10 mandan
//         sobre el modo. Al encender o reiniciar, el ESP32 arranca SIEMPRE en APAGADO.
//
// Actuador ACTIVO = deja pasar energía. DESACTIVADO = corta (estado seguro).
//   Actuador 1 = RL1 "Alimentación banco"  (fuente -> banco)  GPIO 26
//   Actuador 2 = RL2 "Salida BMS"          (banco -> motor)   GPIO 27
//
// No actives WiFi ni Bluetooth en este ESP32: GPIO 25, 14 y 12 son ADC2.
// ============================================================================

#include <Arduino.h>
#include <math.h>
#include <Preferences.h>
#include <driver/rtc_io.h>  // Pull-Down en GPIO 32

// ============================================================================
// 1. PINES
// ============================================================================
namespace Pines {
    constexpr int V_ENTRADA  = 36; // Voltímetro entrada (fuente)
    constexpr int V_SALIDA   = 39; // Voltímetro salida (motor)
    constexpr int V_TAP_4V   = 34; // Toma celda 4V
    constexpr int V_TAP_8V   = 35; // Toma celda 8V
    constexpr int V_TAP_12V  = 32; // Toma celda 12V (= voltaje del banco en 3S)
    const int LISTA_VOLT[5] = {V_ENTRADA, V_SALIDA, V_TAP_4V, V_TAP_8V, V_TAP_12V};

    constexpr int I_CARGA    = 33; // Amperímetro carga (fuente -> banco)
    constexpr int I_DESCARGA = 25; // Amperímetro descarga (banco -> motor)

    constexpr int ACTUADOR_1 = 26; // RL1 Alimentación banco (FUENTE)
    constexpr int ACTUADOR_2 = 27; // RL2 Salida BMS (CARGA / MOTOR)

    constexpr int TEMP_1     = 14; // NTC interno
    constexpr int TEMP_2     = 12; // NTC externo (pin de arranque: si no enciende, usa GPIO 13)

    constexpr int UART2_RX   = 16; // -> TX de la RPi 5 (GPIO 14)
    constexpr int UART2_TX   = 17; // -> RX de la RPi 5 (GPIO 15)
}

constexpr int NIVEL_ACTIVO   = HIGH;  // si tu relé se activa con LOW, cámbialo a LOW
constexpr int NIVEL_INACTIVO = (NIVEL_ACTIVO == HIGH) ? LOW : HIGH;

// ============================================================================
// 2. CALIBRACIÓN (la tuya, sin cambios)
// ============================================================================
namespace Calibracion {
    constexpr float V_CC          = 5.0;
    constexpr float R_FIJA        = 10000.0;
    constexpr float GANANCIA_CAS  = 10.0 / 15.0;
    constexpr float BETA          = 3950.0;
    constexpr float R0            = 10000.0;
    constexpr float TEMP_0        = 298.15;

    constexpr int   MUESTRAS_V    = 64;
    constexpr float M1_V          = 45.906325;
    constexpr float B1_V          = -4.326752;
    constexpr float M_V           = 33.167102;
    constexpr float B_V           = -1.366222;

    constexpr float V_REF_ESP32   = 3.3;
    constexpr int   RES_ADC       = 4095;
    constexpr float V_CERO_AMP    = 2.3642;
    constexpr float PENDIENTE_I   = 29.6083;
    constexpr float ZONA_MUERTA_I = 0.10;   // antes 0.45: ocultaba tu carga de 0.2-0.5 A
}

// Voltaje en el PIN por debajo del cual se lee 0 V (Entrada, Salida, 4V, 8V, 12V).
// Antes las tomas 4V y 12V tenían 0.500 V = ~15.2 V reales: el banco 3S (máx 12.6 V)
// siempre se leía 0 V. 0.160 V en el pin = ~3.0 V reales, 0.120 V = ~1.2 V reales.
float umbral_vacio_v[5] = {0.160, 0.160, 0.120, 0.160, 0.160};

// Ajuste fino por canal, DESPUÉS de tu regresión: V = V_regresion * GANANCIA + OFFSET
//                          Entrada  Salida  Toma4V  Toma8V  Toma12V
float ajuste_ganancia[5] = {1.000,   1.000,  1.000,  1.000,  1.000};
float ajuste_offset[5]   = {0.000,   0.000,  0.000,  0.000,  0.000};
Preferences nvs;

// ---- Calibración lineal por canal (se hace con la tecla K en main_consola.cpp) ----
// V = m * mV_en_el_pin + b. Se guarda en la flash (NVS "calv") y la usan las DOS versiones
// del firmware: calibras con main_consola.cpp, subes main.cpp y la calibración se conserva.
// Si un canal no está calibrado se usa tu regresión de 2 tramos (M1_V, M_V).
struct CalVolt { uint32_t ver; float m[5]; float b[5]; uint8_t ok[5]; uint8_t relleno[3]; };
constexpr uint32_t VERSION_CALV = 1;
CalVolt calv = {};
float mv_crudo[5] = {0, 0, 0, 0, 0};       // último mV leído en cada pin
constexpr float V_MIN_LECTURA = 0.5;       // con calibración: menos de esto se muestra 0 V

// Respaldo: tu calibración del 29/10/2026 (KC). Se usa SOLO si el ESP32 no tiene una guardada,
// por ejemplo un ESP32 nuevo. Cada chip lee un poco distinto: con uno nuevo, vuelve a calibrar con K.
//                                    Entrada   Salida    Toma4V    Toma8V    Toma12V
const CalVolt CAL_RESPALDO = {VERSION_CALV,
                                    {0.03487f, 0.03493f, 0.03494f, 0.03532f, 0.03630f},   // m
                                    {-0.748f,  -1.519f,  -4.131f,  -3.637f,  -4.103f},    // b
                                    {1, 1, 1, 1, 1}, {0, 0, 0}};
bool cal_desde_flash = false;

void cargarCalibracionVolt() {
    nvs.begin("calv", true);
    CalVolt c = {};
    if (nvs.getBytesLength("cal") == sizeof(CalVolt)) nvs.getBytes("cal", &c, sizeof(CalVolt));
    nvs.end();
    cal_desde_flash = (c.ver == VERSION_CALV);
    calv = cal_desde_flash ? c : CAL_RESPALDO;
}

float offset_amp_carga    = -5.61;
float offset_amp_descarga = -6.00;

// ============================================================================
// 3. CONFIG_SEGURIDAD (RN08: solo se cambia desde la Raspberry; se guarda en NVS)
// ============================================================================
struct ConfigSeguridad {
    int32_t  n_celdas;        // 1..3: qué toma es el voltaje del banco
    float    temp_max;        // RN01 °C
    float    volt_min;        // RN02 V del banco
    float    volt_max;        // RN03 V del banco
    float    corr_max;        // RN10 A (motor)
    uint32_t t_reconexion_s;  // RN04 segundos continuos en rango seguro
    float    hist_temp;       // margen para "rango seguro" de RN01 (°C)
    float    hist_v_min;      // margen para "rango seguro" de RN02 (V)
    float    hist_v_max;      // margen para "rango seguro" de RN03 (V)
    uint32_t persist_ms;      // tiempo que debe durar RN01-RN03 antes de cortar (filtra ruido)
    float    tend_dt;         // RN09 °C
    uint32_t tend_t_ms;       // RN09 ventana temperatura
    float    tend_dv;         // RN09 V
    uint32_t tend_v_ms;       // RN09 ventana voltaje
};

const ConfigSeguridad CONFIG_DEFECTO = {
    3,        // n_celdas (3S)
    45.0,     // temp_max   (diagrama de secuencia: T > 45 °C)
    9.0,      // volt_min   (V < 9.0 V)
    12.6,     // volt_max   (V > 12.6 V)
    15.0,     // corr_max   (AJUSTA a tu motor)
    30,       // t_reconexion_s (RN04)
    5.0,      // hist_temp:  rango seguro RN01 = T < 40 °C
    1.0,      // hist_v_min: rango seguro RN02 = V > 10.0 V
    0.3,      // hist_v_max: rango seguro RN03 = V < 12.3 V
    1000,     // persist_ms
    5.0, 10000,   // RN09 temperatura: 5 °C en 10 s
    0.5, 5000     // RN09 voltaje: 0.5 V en 5 s
};

ConfigSeguridad cfg = CONFIG_DEFECTO;
constexpr uint32_t VERSION_CFG = 2;

void cargarConfig() {
    nvs.begin("bms", true);
    if (nvs.getUInt("ver", 0) == VERSION_CFG && nvs.getBytesLength("cfg") == sizeof(ConfigSeguridad)) {
        nvs.getBytes("cfg", &cfg, sizeof(ConfigSeguridad));
    }
    nvs.end();
}

void guardarConfig() {
    nvs.begin("bms", false);
    nvs.putBytes("cfg", &cfg, sizeof(ConfigSeguridad));
    nvs.putUInt("ver", VERSION_CFG);
    nvs.end();
}

// ============================================================================
// 4. DATOS
// ============================================================================
struct DatosBMS {
    float v_in, v_out, v_4v, v_8v, v_12v;
    float i_carga, i_descarga;
    float temp1, temp2;
    bool  act1, act2;          // true = ACTIVO (deja pasar)
};
DatosBMS bms = {0, 0, 0, 0, 0, 0, 0, 0, 0, false, false};

float v_banco = 0, t_max = 0;
bool  sensores_ok = false;
bool  inicio_seguro = false;   // RN05

// RN11: modo de operación
enum ModoOp { MODO_APAGADO = 0, MODO_CARGA = 1, MODO_MOTOR = 2 };
ModoOp modo_op = MODO_APAGADO;                 // al arrancar: APAGADO (el motor nunca arranca solo)
const char* NOMBRE_MODO[3] = {"APAGADO", "CARGA", "MOTOR"};
bool  modo_prueba = false;     // 'P' en el monitor USB: mantenimiento, 60 s
uint32_t t_prueba = 0;

// ============================================================================
// 5. REGLAS
// ============================================================================
struct Regla {
    const char* id;
    const char* nombre;
    bool corta_a1, corta_a2;
    bool enclavada;            // true: no se reconecta sola (RN10)
    bool activa;
    bool contando_fuera, contando_seguro;
    uint32_t desde_fuera, desde_seguro;

    Regla(const char* i, const char* n, bool a1, bool a2, bool enc)
        : id(i), nombre(n), corta_a1(a1), corta_a2(a2), enclavada(enc), activa(false),
          contando_fuera(false), contando_seguro(false), desde_fuera(0), desde_seguro(0) {}

    uint32_t segundosParaReconectar() const {
        if (!activa || enclavada || !contando_seguro) return 0;
        uint32_t pasado = (millis() - desde_seguro) / 1000;
        return pasado >= cfg.t_reconexion_s ? 0 : cfg.t_reconexion_s - pasado;
    }
};

enum { R_RN01, R_RN02, R_RN03, R_RN10, R_SENSOR, N_REGLAS };
Regla reglas[N_REGLAS] = {
    //     id       nombre                   act1   act2   enclavada
    Regla("RN01", "ALTA_TEMPERATURA",        true,  true,  false),
    Regla("RN02", "SOBREDESCARGA",           false, true,  false),
    Regla("RN03", "SOBRECARGA",              true,  false, false),
    Regla("RN10", "SOBRECORRIENTE",          false, true,  true ),
    Regla("RN05", "FALLA_SENSOR",            true,  true,  false),
};

// ---------------- Reporte de eventos a la Raspberry (RN07) ----------------
void enviarEvento(const char* evento, const char* regla, const char* nombre, const char* extra = "") {
    // Estado de los actuadores YA con este evento aplicado (RN07: evidencia correcta)
    bool a1 = inicio_seguro && modo_op == MODO_CARGA, a2 = inicio_seguro && modo_op == MODO_MOTOR;
    for (int i = 0; i < N_REGLAS; i++) {
        if (!reglas[i].activa) continue;
        if (reglas[i].corta_a1) a1 = false;
        if (reglas[i].corta_a2) a2 = false;
    }
    char linea[400];
    snprintf(linea, sizeof(linea),
        "{\"evento\":\"%s\",\"regla\":\"%s\",\"nombre\":\"%s\",\"ms\":%lu,"
        "\"act1\":%d,\"act2\":%d,\"v_banco\":%.2f,\"i_carga\":%.2f,\"i_desc\":%.2f,"
        "\"temp1\":%.2f,\"temp2\":%.2f%s}",
        evento, regla, nombre, (unsigned long)millis(),
        a1 ? 1 : 0, a2 ? 1 : 0, v_banco, bms.i_carga, bms.i_descarga,
        bms.temp1, bms.temp2, extra);
    Serial2.println(linea);
    Serial.printf("\n[EVENTO] %s %s (%s)\n", evento, regla, nombre);
}

// fuera:  la condición de la regla se cumple (ej. T > temp_max)
// seguro: el valor está en el rango seguro para reconectar (ej. T < temp_max - hist)
void evaluarRegla(Regla& r, bool fuera, bool seguro, uint32_t persist_ms) {
    uint32_t ahora = millis();
    if (!r.activa) {
        if (fuera) {
            if (!r.contando_fuera) { r.contando_fuera = true; r.desde_fuera = ahora; }
            if (ahora - r.desde_fuera >= persist_ms) {
                r.activa = true;
                r.contando_fuera = r.contando_seguro = false;
                enviarEvento("CORTE", r.id, r.nombre);
            }
        } else {
            r.contando_fuera = false;
        }
        return;
    }
    if (r.enclavada) return;                       // RN10: solo con REARMAR
    // RN04: debe permanecer en rango seguro t_reconexion_s segundos SEGUIDOS
    if (seguro) {
        if (!r.contando_seguro) { r.contando_seguro = true; r.desde_seguro = ahora; }
        if (ahora - r.desde_seguro >= cfg.t_reconexion_s * 1000UL) {
            r.activa = false;
            r.contando_seguro = false;
            enviarEvento("RECONEXION", r.id, r.nombre);
        }
    } else {
        r.contando_seguro = false;                 // salió del rango: el conteo vuelve a 0
    }
}

void rearmar() {
    for (int i = 0; i < N_REGLAS; i++) {
        if (reglas[i].enclavada && reglas[i].activa) {
            reglas[i].activa = false;
            reglas[i].contando_fuera = false;
            enviarEvento("REARME", reglas[i].id, reglas[i].nombre);
        }
    }
}

// ---------------- RN09: tendencia (alerta sin corte) ----------------
constexpr int N_HIST = 64;
struct Muestra { uint32_t ms; float t; float v; };
Muestra hist[N_HIST];
int hist_n = 0, hist_i = 0;
uint32_t ult_alerta_t = 0, ult_alerta_v = 0;
bool hay_alerta_t = false, hay_alerta_v = false;

void evaluarTendencia() {
    uint32_t ahora = millis();
    hist[hist_i] = {ahora, t_max, v_banco};
    hist_i = (hist_i + 1) % N_HIST;
    if (hist_n < N_HIST) hist_n++;

    float t_min_ventana = t_max, v_min_ventana = v_banco, v_max_ventana = v_banco;
    for (int k = 0; k < hist_n; k++) {
        const Muestra& m = hist[k];
        if (ahora - m.ms <= cfg.tend_t_ms) t_min_ventana = min(t_min_ventana, m.t);
        if (ahora - m.ms <= cfg.tend_v_ms) {
            v_min_ventana = min(v_min_ventana, m.v);
            v_max_ventana = max(v_max_ventana, m.v);
        }
    }
    float dT = t_max - t_min_ventana;                // calentamiento rápido
    float dV = v_max_ventana - v_min_ventana;        // cambio rápido de voltaje (sube o baja)

    // Se avisa una vez por ventana para no inundar a la Raspberry
    char extra[64];
    if (dT > cfg.tend_dt && ahora - ult_alerta_t >= cfg.tend_t_ms) {
        ult_alerta_t = ahora;
        snprintf(extra, sizeof(extra), ",\"delta\":%.2f,\"magnitud\":\"TEMP\"", dT);
        enviarEvento("TENDENCIA", "RN09", "TENDENCIA_CRITICA", extra);
    }
    if (dV > cfg.tend_dv && ahora - ult_alerta_v >= cfg.tend_v_ms) {
        ult_alerta_v = ahora;
        snprintf(extra, sizeof(extra), ",\"delta\":%.2f,\"magnitud\":\"VOLT\"", dV);
        enviarEvento("TENDENCIA", "RN09", "TENDENCIA_CRITICA", extra);
    }
    hay_alerta_t = (ahora - ult_alerta_t < cfg.tend_t_ms) && ult_alerta_t != 0;
    hay_alerta_v = (ahora - ult_alerta_v < cfg.tend_v_ms) && ult_alerta_v != 0;
}

// ============================================================================
// 6. LECTURA DE SENSORES (tu código)
// ============================================================================
float leerVoltaje100V(int indice) {
    int pin = Pines::LISTA_VOLT[indice];
    if (pin == 32) {
        rtc_gpio_pullup_dis(GPIO_NUM_32);
        rtc_gpio_pulldown_en(GPIO_NUM_32);
    }
    analogRead(Pines::V_ENTRADA);
    delayMicroseconds(200);
    analogRead(pin);
    delayMicroseconds(300);

    uint32_t suma_mv = 0;
    for (int i = 0; i < Calibracion::MUESTRAS_V; i++) {
        suma_mv += analogReadMilliVolts(pin);
        delayMicroseconds(200);
    }
    float v_salida = ((float)suma_mv / Calibracion::MUESTRAS_V) / 1000.0;
    mv_crudo[indice] = v_salida * 1000.0;

    if (calv.ver == VERSION_CALV && calv.ok[indice]) {      // calibración por puntos (tecla K)
        float v = calv.m[indice] * mv_crudo[indice] + calv.b[indice];
        v = v * ajuste_ganancia[indice] + ajuste_offset[indice];
        return (v < V_MIN_LECTURA) ? 0.0 : v;
    }
    if (v_salida <= umbral_vacio_v[indice]) return 0.0;

    float v_real = (v_salida <= 0.2542)
        ? (Calibracion::M1_V * v_salida) + Calibracion::B1_V
        : (Calibracion::M_V * v_salida) + Calibracion::B_V;
    v_real = v_real * ajuste_ganancia[indice] + ajuste_offset[indice];
    return (v_real < 0.0) ? 0.0 : v_real;
}

float leerCorriente(int pin, float offset_reposo) {
    analogRead(Pines::V_ENTRADA);
    delayMicroseconds(200);
    analogRead(pin);
    delayMicroseconds(500);

    long sumaADC = 0;
    const int muestras = 100;
    for (int i = 0; i < muestras; i++) {
        sumaADC += analogRead(pin);
        delayMicroseconds(400);
    }
    float lecturaPromedio = sumaADC / (float)muestras;
    float v_cas  = (lecturaPromedio * Calibracion::V_REF_ESP32) / Calibracion::RES_ADC;
    float v_hall = v_cas * (5.0 / 3.3);

    if (v_cas < 1.00) return (0.0 - Calibracion::V_CERO_AMP) * Calibracion::PENDIENTE_I; // -70 A

    float corriente = ((v_hall - Calibracion::V_CERO_AMP) * Calibracion::PENDIENTE_I) + offset_reposo;
    if (fabs(corriente) < Calibracion::ZONA_MUERTA_I) corriente = 0.0;
    return corriente;
}

float leerTemperatura(int pin) {
    analogRead(pin);
    delayMicroseconds(300);
    long suma_mv = 0;
    const int muestras = 30;
    for (int i = 0; i < muestras; i++) {
        suma_mv += analogReadMilliVolts(pin);
        delay(1);
    }
    float v_cas = (suma_mv / (float)muestras) / 1000.0;
    float v_ntc = v_cas / Calibracion::GANANCIA_CAS;
    if (v_ntc <= 0.05) v_ntc = 0.05;
    if (v_ntc >= (Calibracion::V_CC - 0.05)) v_ntc = Calibracion::V_CC - 0.05;
    float r_ntc = Calibracion::R_FIJA * ((Calibracion::V_CC - v_ntc) / v_ntc);
    float temp_kelvin = 1.0 / ((1.0 / Calibracion::TEMP_0) + (1.0 / Calibracion::BETA) * log(r_ntc / Calibracion::R0));
    return temp_kelvin - 273.15;
}

void leerSensores() {
    bms.v_in  = leerVoltaje100V(0);
    bms.v_out = leerVoltaje100V(1);
    bms.v_4v  = leerVoltaje100V(2);
    bms.v_8v  = leerVoltaje100V(3);
    bms.v_12v = leerVoltaje100V(4);
    bms.i_carga    = leerCorriente(Pines::I_CARGA, offset_amp_carga);
    bms.i_descarga = leerCorriente(Pines::I_DESCARGA, offset_amp_descarga);
    bms.temp1 = leerTemperatura(Pines::TEMP_1);
    bms.temp2 = leerTemperatura(Pines::TEMP_2);
}

// ============================================================================
// 7. LÓGICA DE ACTUADORES
// ============================================================================
void evaluarReglas() {
    const float tap[3] = {bms.v_4v, bms.v_8v, bms.v_12v};
    v_banco = tap[constrain(cfg.n_celdas, 1, 3) - 1];
    t_max   = max(bms.temp1, bms.temp2);
    const float t_min = min(bms.temp1, bms.temp2);

    // Lectura válida: amperímetros alimentados, NTC conectados y banco detectado
    const bool i_ok = bms.i_carga > -60.0 && bms.i_descarga > -60.0;
    const bool t_ok = t_min > -30.0 && t_max < 120.0;
    const bool v_ok = v_banco >= 1.0;
    sensores_ok = i_ok && t_ok && v_ok;

    evaluarRegla(reglas[R_SENSOR], !sensores_ok, sensores_ok, 2000);

    if (t_ok)  // RN01
        evaluarRegla(reglas[R_RN01], t_max > cfg.temp_max, t_max < cfg.temp_max - cfg.hist_temp, cfg.persist_ms);
    if (v_ok) {
        // RN02 sobredescarga
        evaluarRegla(reglas[R_RN02], v_banco < cfg.volt_min, v_banco > cfg.volt_min + cfg.hist_v_min, cfg.persist_ms);
        // RN03 sobrecarga
        evaluarRegla(reglas[R_RN03], v_banco > cfg.volt_max, v_banco < cfg.volt_max - cfg.hist_v_max, cfg.persist_ms);
    }
    if (i_ok)  // RN10 inmediata (sin persistencia)
        evaluarRegla(reglas[R_RN10], fabs(bms.i_descarga) > cfg.corr_max, false, 0);

    // RN11: fin automático de la operación
    if (modo_op == MODO_CARGA && reglas[R_RN03].activa) {
        modo_op = MODO_APAGADO;
        enviarEvento("FIN_MODO", "RN11", "CARGA_COMPLETA", ",\"modo\":\"APAGADO\",\"causa\":\"RN03\"");
    }
    if (modo_op == MODO_MOTOR && (reglas[R_RN02].activa || reglas[R_RN10].activa)) {
        const char* causa = reglas[R_RN10].activa ? "RN10" : "RN02";
        char extra[64];
        snprintf(extra, sizeof(extra), ",\"modo\":\"APAGADO\",\"causa\":\"%s\"", causa);
        modo_op = MODO_APAGADO;
        enviarEvento("FIN_MODO", "RN11", reglas[R_RN10].activa ? "MOTOR_SOBRECORRIENTE" : "MOTOR_BANCO_DESCARGADO", extra);
    }

    // RN05: hasta la primera lectura válida con TODO en rango, nada se activa
    if (!inicio_seguro && sensores_ok &&
        t_max <= cfg.temp_max && v_banco >= cfg.volt_min && v_banco <= cfg.volt_max &&
        fabs(bms.i_descarga) <= cfg.corr_max) {
        inicio_seguro = true;
        enviarEvento("INICIO_SEGURO", "RN05", "LECTURA_VALIDA");
    }

    if (sensores_ok) evaluarTendencia();   // RN09
}

void aplicarActuadores() {
    bool a1 = true, a2 = true;

    if (modo_prueba) {
        if (millis() - t_prueba >= 60000) {
            modo_prueba = false;
            Serial.println("\n[!] Fin del modo PRUEBA");
        } else {
            a1 = bms.act1;  // lo que pidas con 1/2/A/S
            a2 = bms.act2;
        }
    }
    if (!modo_prueba) {
        a1 = (modo_op == MODO_CARGA);             // RN11: el modo decide qué se puede conectar
        a2 = (modo_op == MODO_MOTOR);
        for (int i = 0; i < N_REGLAS; i++) {
            if (!reglas[i].activa) continue;
            if (reglas[i].corta_a1) a1 = false;
            if (reglas[i].corta_a2) a2 = false;
        }
        if (!inicio_seguro) a1 = a2 = false;      // RN05
    }

    bms.act1 = a1;
    bms.act2 = a2;
    digitalWrite(Pines::ACTUADOR_1, a1 ? NIVEL_ACTIVO : NIVEL_INACTIVO);
    digitalWrite(Pines::ACTUADOR_2, a2 ? NIVEL_ACTIVO : NIVEL_INACTIVO);
}

// ============================================================================
// 8. COMANDOS
// ============================================================================
void tararSensoresEnCero() {
    Serial.println("\n[*] Tarando sensores en 0.00 (sin carga ni corriente)...");
    for (int i = 0; i < 5; i++) {
        uint32_t suma_mv = 0;
        for (int j = 0; j < Calibracion::MUESTRAS_V; j++) {
            suma_mv += analogReadMilliVolts(Pines::LISTA_VOLT[i]);
            delayMicroseconds(200);
        }
        umbral_vacio_v[i] = ((float)suma_mv / Calibracion::MUESTRAS_V) / 1000.0 + 0.015;
    }
    float c = leerCorriente(Pines::I_CARGA, 0.0);
    float d = leerCorriente(Pines::I_DESCARGA, 0.0);
    if (c > -60.0) offset_amp_carga = -c;
    if (d > -60.0) offset_amp_descarga = -d;
    Serial.println("[OK] Sensores ajustados a 0.00 en reposo.");
    Serial.println("\n>>> Copia estas 3 lineas en main.cpp (la version final) para que la tara no se pierda:");
    Serial.printf("float umbral_vacio_v[5] = {%.3f, %.3f, %.3f, %.3f, %.3f};\n",
                  umbral_vacio_v[0], umbral_vacio_v[1], umbral_vacio_v[2], umbral_vacio_v[3], umbral_vacio_v[4]);
    Serial.printf("float offset_amp_carga    = %.2f;\n", offset_amp_carga);
    Serial.printf("float offset_amp_descarga = %.2f;\n\n", offset_amp_descarga);
}

extern const char* NOMBRE_CANAL[5];
void imprimirConfig() {
    Serial.printf("\n[CONFIG_SEGURIDAD] %dS | temp_max %.1f C | volt_min %.2f V | volt_max %.2f V | corr_max %.1f A\n",
                  (int)cfg.n_celdas, cfg.temp_max, cfg.volt_min, cfg.volt_max, cfg.corr_max);
    Serial.printf("  RN04 reconexion %lu s | rango seguro: T < %.1f, V > %.2f, V < %.2f | persistencia %lu ms\n",
                  (unsigned long)cfg.t_reconexion_s, cfg.temp_max - cfg.hist_temp,
                  cfg.volt_min + cfg.hist_v_min, cfg.volt_max - cfg.hist_v_max, (unsigned long)cfg.persist_ms);
    Serial.printf("  RN09 tendencia: %.1f C en %lu ms, %.2f V en %lu ms\n",
                  cfg.tend_dt, (unsigned long)cfg.tend_t_ms, cfg.tend_dv, (unsigned long)cfg.tend_v_ms);
    Serial.print("  Voltimetros: ");
    for (int i = 0; i < 5; i++)
        Serial.printf("%s=%s  ", NOMBRE_CANAL[i], (calv.ver == VERSION_CALV && calv.ok[i]) ? "calibrado" : "regresion");
    Serial.println();
    if (!cal_desde_flash)
        Serial.println("  ** Este ESP32 no tiene calibracion propia: se usa la de RESPALDO del codigo. "
                       "Calibra con K para mayor precision. **");
}

// ---------------- Calibración de voltímetros por puntos (tecla K) ----------------
//   K5      -> captura un punto: todas las entradas conectadas a una fuente de 5.00 V
//   K12.6   -> otro punto (mínimo 2 voltajes distintos, mejor 3 o 4)
//   KC      -> calcula, revisa que sea lineal y GUARDA en la flash
//   KB      -> borra los puntos capturados (para empezar de nuevo)
//   KX      -> borra la calibración guardada en este ESP32 (vuelve a la de RESPALDO del código)
constexpr int MAX_PTS = 8;
float pts_v[MAX_PTS];
float pts_mv[MAX_PTS][5];
int n_pts = 0;
const char* NOMBRE_CANAL[5] = {"Entrada ", "Salida  ", "Toma 4V ", "Toma 8V ", "Toma 12V"};

float promedioPin_mV(int pin, int muestras) {
    analogRead(Pines::V_ENTRADA);
    delayMicroseconds(200);
    analogRead(pin);
    delayMicroseconds(300);
    uint32_t suma = 0;
    for (int i = 0; i < muestras; i++) {
        suma += analogReadMilliVolts(pin);
        delayMicroseconds(200);
    }
    return suma / (float)muestras;
}

void ayudaCalibracion() {
    Serial.println("\n[K] Calibracion de voltimetros:");
    Serial.println("    1) Conecta la fuente a los voltimetros que quieras calibrar (pueden ser los 5 a la vez)");
    Serial.println("    2) Mide la fuente con el MULTIMETRO y escribe K y ese voltaje, ej:  K5.02  y Enter");
    Serial.println("    3) Cambia el voltaje y repite (minimo 2 puntos; recomendado 4: ~5, 8, 11 y 13 V)");
    Serial.println("    4) Escribe KC para calcular y guardar.   KB = borrar puntos   KX = borrar calibracion");
    Serial.printf ("    Puntos capturados: %d\n", n_pts);
}

void capturarPunto(float v_real) {
    if (n_pts >= MAX_PTS) {
        Serial.println("\n[K] Ya hay 8 puntos. Escribe KC para calcular o KB para empezar de nuevo.");
        return;
    }
    pts_v[n_pts] = v_real;
    Serial.printf("\n[K] Punto %d a %.3f V:\n", n_pts + 1, v_real);
    for (int i = 0; i < 5; i++) {
        pts_mv[n_pts][i] = promedioPin_mV(Pines::LISTA_VOLT[i], 256);
        Serial.printf("    %s  %7.1f mV\n", NOMBRE_CANAL[i], pts_mv[n_pts][i]);
    }
    n_pts++;
    Serial.printf("[K] %d punto(s). %s\n", n_pts,
                  n_pts < 2 ? "Cambia el voltaje y captura otro." : "Captura otro o escribe KC.");
}

void calcularCalibracion() {
    if (n_pts < 2) {
        Serial.println("\n[K] Necesitas al menos 2 puntos con voltajes distintos.");
        return;
    }
    CalVolt c = {};
    c.ver = VERSION_CALV;
    Serial.println("\n[K] Resultado (V = m * mV + b):");
    for (int ch = 0; ch < 5; ch++) {
        float mv_min = 1e9, mv_max = -1e9;
        for (int k = 0; k < n_pts; k++) {
            mv_min = min(mv_min, pts_mv[k][ch]);
            mv_max = max(mv_max, pts_mv[k][ch]);
        }
        if (mv_max - mv_min < 15.0) {
            Serial.printf("  %s  NO CALIBRADO: su salida casi no cambio (%.0f-%.0f mV). "
                          "No estaba conectado o el AD620 no responde.\n", NOMBRE_CANAL[ch], mv_min, mv_max);
            continue;
        }
        // Detectar el "piso" del AD620: con la pendiente de los 2 voltajes más altos se predice
        // cuántos mV deberían tener los puntos bajos; si marcan más, la salida está pegada.
        int i1 = 0, i2 = -1;
        for (int k = 1; k < n_pts; k++) if (pts_v[k] > pts_v[i1]) i1 = k;
        for (int k = 0; k < n_pts; k++) if (k != i1 && (i2 < 0 || pts_v[k] > pts_v[i2])) i2 = k;
        bool usar[MAX_PTS];
        int n_usar = 0;
        float v_piso = -1;
        float pend = (pts_v[i1] - pts_v[i2]) != 0
                     ? (pts_mv[i1][ch] - pts_mv[i2][ch]) / (pts_v[i1] - pts_v[i2]) : 0;   // mV por V
        for (int k = 0; k < n_pts; k++) {
            float esperado = pts_mv[i2][ch] + pend * (pts_v[k] - pts_v[i2]);
            usar[k] = !(pend > 0 && pts_mv[k][ch] > esperado + 10.0);
            if (usar[k]) n_usar++; else v_piso = max(v_piso, pts_v[k]);
        }
        if (n_usar < 2) {
            Serial.printf("  %s  NO CALIBRADO: menos de 2 puntos utiles.\n", NOMBRE_CANAL[ch]);
            continue;
        }
        float mx = 0, my = 0;
        for (int k = 0; k < n_pts; k++) if (usar[k]) { mx += pts_mv[k][ch]; my += pts_v[k]; }
        mx /= n_usar; my /= n_usar;
        float sxx = 0, sxy = 0;
        for (int k = 0; k < n_pts; k++) if (usar[k]) {
            sxx += (pts_mv[k][ch] - mx) * (pts_mv[k][ch] - mx);
            sxy += (pts_mv[k][ch] - mx) * (pts_v[k] - my);
        }
        float m = sxx > 0 ? sxy / sxx : 0, b = my - m * mx;
        if (m <= 0) {
            Serial.printf("  %s  NO CALIBRADO: la salida baja cuando el voltaje sube. Revisa el cableado.\n",
                          NOMBRE_CANAL[ch]);
            continue;
        }
        float err_max = 0, v_err = 0;
        for (int k = 0; k < n_pts; k++) if (usar[k]) {
            float e = fabs(m * pts_mv[k][ch] + b - pts_v[k]);
            if (e > err_max) { err_max = e; v_err = pts_v[k]; }
        }
        c.m[ch] = m; c.b[ch] = b; c.ok[ch] = 1;
        Serial.printf("  %s  m=%.5f  b=%+.3f   error maximo %.2f V", NOMBRE_CANAL[ch], m, b, err_max);
        if (err_max > 0.30) Serial.printf("  <- poco lineal en %.2f V", v_err);
        Serial.println();
        if (v_piso >= 0) {
            float mv_suelo = 1e9;
            for (int k = 0; k < n_pts; k++) if (!usar[k]) mv_suelo = min(mv_suelo, pts_mv[k][ch]);
            Serial.printf("            ATENCION: la salida del AD620 no baja de ~%.0f mV. Mide bien solo arriba de "
                          "~%.1f V\n            (el punto de %.2f V no se uso). Sube el offset/REF del modulo AD620.\n",
                          mv_suelo, m * mv_suelo + b, v_piso);
        }
    }
    calv = c;
    cal_desde_flash = true;
    nvs.begin("calv", false);
    nvs.putBytes("cal", &calv, sizeof(CalVolt));
    nvs.end();
    Serial.println("[K] Guardado en la flash. Lo usaran main_consola.cpp y main.cpp aunque reinicies o vuelvas a subir el codigo.");
}

void procesarK(const char* txt) {
    while (*txt == ' ') txt++;
    if (*txt == '\0') { ayudaCalibracion(); return; }
    char c0 = toupper(*txt);
    if (c0 == 'C') { calcularCalibracion(); return; }
    if (c0 == 'B') { n_pts = 0; Serial.println("\n[K] Puntos borrados."); return; }
    if (c0 == 'X') {
        nvs.begin("calv", false);
        nvs.remove("cal");
        nvs.end();
        calv = CAL_RESPALDO;
        cal_desde_flash = false;
        Serial.println("\n[K] Calibracion de este ESP32 borrada: se usa la de RESPALDO del codigo.");
        return;
    }
    if (!isdigit((unsigned char)*txt)) { ayudaCalibracion(); return; }
    float v = atof(txt);
    if (v < 0 || v > 100) { Serial.println("\n[K] Voltaje fuera de rango (0-100 V)."); return; }
    capturarPunto(v);
}

// Monitor USB de VS Code (mantenimiento en la mesa)
void verificarComandosUSB() {
    static bool leyendo_k = false;
    static char kbuf[16];
    static size_t klen = 0;
    while (Serial.available() > 0) {
        char c = Serial.read();
        if (leyendo_k) {                       // después de K se lee hasta Enter (ej. K12.6)
            if (c == '\n' || c == '\r') {
                kbuf[klen] = '\0';
                leyendo_k = false;
                procesarK(kbuf);
            } else if (klen < sizeof(kbuf) - 1) {
                kbuf[klen++] = c;
            }
            continue;
        }
        switch (c) {
            case 'k': case 'K': leyendo_k = true; klen = 0; break;
            case 'p': case 'P':
                modo_prueba = !modo_prueba;
                t_prueba = millis();
                bms.act1 = bms.act2 = false;
                Serial.printf("\n[!] Modo PRUEBA %s\n", modo_prueba
                    ? "ACTIVO 60 s: reglas IGNORADAS. Usa 1, 2, A, S (sin banco ni motor)" : "terminado");
                break;
            case '1': if (modo_prueba) bms.act1 = !bms.act1; break;
            case '2': if (modo_prueba) bms.act2 = !bms.act2; break;
            case 'a': case 'A': if (modo_prueba) bms.act1 = bms.act2 = true;  break;
            case 's': case 'S': if (modo_prueba) bms.act1 = bms.act2 = false; break;
            case 'r': case 'R': rearmar(); break;
            case 'z': case 'Z': tararSensoresEnCero(); break;
            case 'c': case 'C': imprimirConfig(); break;
            case 'o': case 'O': modo_op = MODO_APAGADO; Serial.println("\n[MODO] APAGADO"); break;
            case 'g': case 'G':
                if (reglas[R_RN03].activa) Serial.println("\n[MODO] No: el banco ya esta cargado (RN03)");
                else { modo_op = MODO_CARGA; Serial.println("\n[MODO] CARGA"); }
                break;
            case 'm': case 'M':
                if (reglas[R_RN02].activa || reglas[R_RN10].activa) Serial.println("\n[MODO] No: banco descargado o sobrecorriente");
                else { modo_op = MODO_MOTOR; Serial.println("\n[MODO] MOTOR"); }
                break;
        }
    }
}

bool leerNumero(const char* json, const char* clave, float& salida) {
    char patron[40];
    snprintf(patron, sizeof(patron), "\"%s\":", clave);
    const char* p = strstr(json, patron);
    if (!p) return false;
    salida = atof(p + strlen(patron));
    return true;
}

void responderRPi(const char* ack, bool ok, const char* error = "") {
    Serial2.printf("{\"ack\":\"%s\",\"ok\":%d,\"error\":\"%s\"}\n", ack, ok ? 1 : 0, error);
}

// RN08: comandos desde la Raspberry (líneas JSON)
//   {"cmd":"rearmar"}
//   {"cmd":"get_cfg"}
//   {"cmd":"cfg","temp_max":45,"volt_min":9.0,"volt_max":12.6,"corr_max":15, ...}
void enviarConfigRPi() {
    Serial2.printf("{\"cfg\":{\"n_celdas\":%d,\"temp_max\":%.2f,\"volt_min\":%.2f,\"volt_max\":%.2f,"
                   "\"corr_max\":%.2f,\"t_reconexion_s\":%lu,\"hist_temp\":%.2f,\"hist_v_min\":%.2f,"
                   "\"hist_v_max\":%.2f,\"persist_ms\":%lu,\"tend_dt\":%.2f,\"tend_t_ms\":%lu,"
                   "\"tend_dv\":%.2f,\"tend_v_ms\":%lu}}\n",
                   (int)cfg.n_celdas, cfg.temp_max, cfg.volt_min, cfg.volt_max, cfg.corr_max,
                   (unsigned long)cfg.t_reconexion_s, cfg.hist_temp, cfg.hist_v_min, cfg.hist_v_max,
                   (unsigned long)cfg.persist_ms, cfg.tend_dt, (unsigned long)cfg.tend_t_ms,
                   cfg.tend_dv, (unsigned long)cfg.tend_v_ms);
}

void procesarLineaRPi(const char* linea) {
    if (strstr(linea, "\"cmd\"") == nullptr) return;

    if (strstr(linea, "\"rearmar\"")) {
        rearmar();
        responderRPi("rearmar", true);
    } else if (strstr(linea, "\"modo\"")) {
        // {"cmd":"modo","valor":"APAGADO"|"CARGA"|"MOTOR"}
        ModoOp nuevo;
        if (strstr(linea, "\"CARGA\""))      nuevo = MODO_CARGA;
        else if (strstr(linea, "\"MOTOR\""))  nuevo = MODO_MOTOR;
        else if (strstr(linea, "\"APAGADO\"")) nuevo = MODO_APAGADO;
        else { responderRPi("modo", false, "modo desconocido"); return; }
        if (nuevo == MODO_CARGA && reglas[R_RN03].activa) {
            responderRPi("modo", false, "el banco ya esta cargado (RN03)"); return;
        }
        if (nuevo == MODO_MOTOR && reglas[R_RN02].activa) {
            responderRPi("modo", false, "el banco esta descargado (RN02): primero cargalo"); return;
        }
        if (nuevo == MODO_MOTOR && reglas[R_RN10].activa) {
            responderRPi("modo", false, "sobrecorriente (RN10): primero rearma con bms rearmar"); return;
        }
        if (nuevo != modo_op) {
            modo_op = nuevo;
            char extra[40];
            snprintf(extra, sizeof(extra), ",\"modo\":\"%s\"", NOMBRE_MODO[modo_op]);
            enviarEvento("CAMBIO_MODO", "RN11", NOMBRE_MODO[modo_op], extra);
        }
        responderRPi("modo", true);
    } else if (strstr(linea, "\"get_cfg\"")) {
        enviarConfigRPi();
    } else if (strstr(linea, "\"cfg\"")) {
        ConfigSeguridad n = cfg;
        float x;
        if (leerNumero(linea, "n_celdas", x))       n.n_celdas = (int32_t)lroundf(x);
        if (leerNumero(linea, "temp_max", x))       n.temp_max = x;
        if (leerNumero(linea, "volt_min", x))       n.volt_min = x;
        if (leerNumero(linea, "volt_max", x))       n.volt_max = x;
        if (leerNumero(linea, "corr_max", x))       n.corr_max = x;
        if (leerNumero(linea, "t_reconexion_s", x)) n.t_reconexion_s = (uint32_t)x;
        if (leerNumero(linea, "hist_temp", x))      n.hist_temp = x;
        if (leerNumero(linea, "hist_v_min", x))     n.hist_v_min = x;
        if (leerNumero(linea, "hist_v_max", x))     n.hist_v_max = x;
        if (leerNumero(linea, "persist_ms", x))     n.persist_ms = (uint32_t)x;
        if (leerNumero(linea, "tend_dt", x))        n.tend_dt = x;
        if (leerNumero(linea, "tend_t_ms", x))      n.tend_t_ms = (uint32_t)x;
        if (leerNumero(linea, "tend_dv", x))        n.tend_dv = x;
        if (leerNumero(linea, "tend_v_ms", x))      n.tend_v_ms = (uint32_t)x;

        bool ok = n.n_celdas >= 1 && n.n_celdas <= 3 &&
                  n.volt_min > 0 && n.volt_min + n.hist_v_min < n.volt_max - n.hist_v_max &&
                  n.temp_max > 0 && n.temp_max < 100 && n.corr_max > 0 &&
                  n.hist_temp >= 0 && n.hist_v_min >= 0 && n.hist_v_max >= 0 &&
                  n.t_reconexion_s >= 1 && n.tend_t_ms > 0 && n.tend_v_ms > 0;
        if (ok) {
            if (memcmp(&n, &cfg, sizeof(ConfigSeguridad)) != 0) {
                cfg = n;
                guardarConfig();      // RNF-05: no se pierde al reiniciar
            }
            responderRPi("cfg", true);
            enviarConfigRPi();
        } else {
            responderRPi("cfg", false, "valores fuera de rango o volt_min >= volt_max");
        }
    }
}

void leerUART_RPi() {
    static char buf[512];
    static size_t len = 0;
    while (Serial2.available() > 0) {
        char c = Serial2.read();
        if (c == '\n' || c == '\r') {
            if (len > 0) { buf[len] = '\0'; procesarLineaRPi(buf); len = 0; }
        } else if (len < sizeof(buf) - 1) {
            buf[len++] = c;
        } else {
            len = 0;
        }
    }
}

// ============================================================================
// 9. TELEMETRÍA Y CONSOLA
// ============================================================================
void enviarJSON_RPi() {
    char reglas_txt[200];
    size_t k = snprintf(reglas_txt, sizeof(reglas_txt), "[");
    bool primero = true;
    for (int i = 0; i < N_REGLAS && k < sizeof(reglas_txt) - 12; i++) {
        if (!reglas[i].activa) continue;
        k += snprintf(reglas_txt + k, sizeof(reglas_txt) - k, "%s\"%s\"", primero ? "" : ",", reglas[i].id);
        primero = false;
    }
    snprintf(reglas_txt + k, sizeof(reglas_txt) - k, "]");

    uint32_t espera1 = 0, espera2 = 0;   // segundos para reconexión (RN04)
    for (int i = 0; i < N_REGLAS; i++) {
        uint32_t s = reglas[i].segundosParaReconectar();
        if (reglas[i].corta_a1) espera1 = max(espera1, s);
        if (reglas[i].corta_a2) espera2 = max(espera2, s);
    }

    char json[700];
    snprintf(json, sizeof(json),
        "{\"v_in\":%.2f,\"v_out\":%.2f,\"v_4v\":%.2f,\"v_8v\":%.2f,\"v_12v\":%.2f,\"v_banco\":%.2f,"
        "\"i_carga\":%.2f,\"i_desc\":%.2f,\"temp1\":%.2f,\"temp2\":%.2f,"
        "\"act1\":%d,\"act2\":%d,\"inicio_seguro\":%d,\"prueba\":%d,\"n_celdas\":%d,\"modo_op\":\"%s\","
        "\"reglas\":%s,\"espera_act1_s\":%lu,\"espera_act2_s\":%lu,\"tendencia_t\":%d,\"tendencia_v\":%d}",
        bms.v_in, bms.v_out, bms.v_4v, bms.v_8v, bms.v_12v, v_banco,
        bms.i_carga, bms.i_descarga, bms.temp1, bms.temp2,
        bms.act1 ? 1 : 0, bms.act2 ? 1 : 0, inicio_seguro ? 1 : 0, modo_prueba ? 1 : 0, (int)cfg.n_celdas, NOMBRE_MODO[modo_op],
        reglas_txt, (unsigned long)espera1, (unsigned long)espera2, hay_alerta_t ? 1 : 0, hay_alerta_v ? 1 : 0);
    Serial2.println(json);
}

void mostrarDashboardConsola() {
    Serial.println("\n╔══════════════════════════════════════════════════════════════╗");
    Serial.printf ("║  ECU BMS %-10s  banco %6.2f V   Tmax %6.2f °C          ║\n",
                   modo_prueba ? "[PRUEBA]" : (inicio_seguro ? NOMBRE_MODO[modo_op] : "[RN05]"), v_banco, t_max);
    Serial.println("╠══════════════════════════════╦═══════════════════════════════╣");
    Serial.printf ("║ Entrada  :%6.2f V %5.0f mV ║ I carga    : %8.2f A       ║\n", bms.v_in, mv_crudo[0], bms.i_carga);
    Serial.printf ("║ Salida   :%6.2f V %5.0f mV ║ I descarga : %8.2f A       ║\n", bms.v_out, mv_crudo[1], bms.i_descarga);
    Serial.printf ("║ Toma 4V  :%6.2f V %5.0f mV ║ Temp 1     : %8.2f °C      ║\n", bms.v_4v, mv_crudo[2], bms.temp1);
    Serial.printf ("║ Toma 8V  :%6.2f V %5.0f mV ║ Temp 2     : %8.2f °C      ║\n", bms.v_8v, mv_crudo[3], bms.temp2);
    Serial.printf ("║ Toma 12V :%6.2f V %5.0f mV ║                               ║\n", bms.v_12v, mv_crudo[4]);
    Serial.println("╠══════════════════════════════╩═══════════════════════════════╣");
    Serial.printf ("║ Actuador 1 (fuente) : %-8s  Actuador 2 (motor) : %-8s ║\n",
                   bms.act1 ? "ACTIVO" : "CORTADO", bms.act2 ? "ACTIVO" : "CORTADO");
    Serial.println("║ Modo: [O] apagado  [G] cargar  [M] motor                     ║");
    Serial.println("║ [P] prueba [R] rearmar [Z] tarar [C] config [K] calibrar V   ║");
    Serial.println("╚══════════════════════════════════════════════════════════════╝");
    Serial.print("  Reglas activas: ");
    bool alguna = false;
    for (int i = 0; i < N_REGLAS; i++) {
        if (!reglas[i].activa) continue;
        alguna = true;
        uint32_t s = reglas[i].segundosParaReconectar();
        if (reglas[i].enclavada)  Serial.printf("%s %s (rearmar)  ", reglas[i].id, reglas[i].nombre);
        else if (s > 0)           Serial.printf("%s %s (reconecta en %lus)  ", reglas[i].id, reglas[i].nombre, (unsigned long)s);
        else                      Serial.printf("%s %s  ", reglas[i].id, reglas[i].nombre);
    }
    Serial.println(alguna ? "" : "ninguna");
}

// ============================================================================
// 10. SETUP Y LOOP
// ============================================================================
void setup() {
    Serial.begin(115200);
    Serial2.setRxBufferSize(1024);
    Serial2.begin(115200, SERIAL_8N1, Pines::UART2_RX, Pines::UART2_TX);

    // RN05: actuadores desactivados desde el primer instante
    pinMode(Pines::ACTUADOR_1, OUTPUT);
    pinMode(Pines::ACTUADOR_2, OUTPUT);
    digitalWrite(Pines::ACTUADOR_1, NIVEL_INACTIVO);
    digitalWrite(Pines::ACTUADOR_2, NIVEL_INACTIVO);

    pinMode(Pines::I_CARGA, INPUT_PULLDOWN);
    pinMode(Pines::I_DESCARGA, INPUT_PULLDOWN);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    cargarConfig();
    cargarCalibracionVolt();
    delay(1000);
    imprimirConfig();
    Serial.println("ECU BMS iniciada: actuadores desactivados hasta lectura valida (RN05)");
}

void loop() {
    static uint32_t t_json = 0, t_consola = 0;

    leerSensores();          // ~250 ms
    evaluarReglas();         // RN01-RN05, RN09, RN10
    aplicarActuadores();

    uint32_t ahora = millis();
    if (ahora - t_json >= 500) { enviarJSON_RPi(); t_json = ahora; }          // Tabla 4: 200-500 ms
    if (ahora - t_consola >= 1000) { mostrarDashboardConsola(); t_consola = ahora; }

    verificarComandosUSB();
    leerUART_RPi();
}