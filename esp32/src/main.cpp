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
//         Arranque del motor: durante el primer segundo después de conectarlo solo se
//         corta si la corriente pasa de 3 x corr_max, para no confundir el pico de
//         arranque (corriente de irrupción) con una sobrecorriente.
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
// VERSIÓN DE PRODUCCIÓN: sin menús ni consola. Solo se comunica con la Raspberry
// por UART2. Un watchdog reinicia el ESP32 si el programa se traba; al reiniciar
// vuelve a aplicar RN05 (actuadores desactivados hasta lectura válida).
//
// No actives WiFi ni Bluetooth en este ESP32: GPIO 25, 14 y 12 son ADC2.
// ============================================================================

#include <Arduino.h>
#include <math.h>
#include <Preferences.h>
#include <driver/rtc_io.h>  // Pull-Down en GPIO 32
#include <esp_task_wdt.h>    // watchdog
#include <esp_system.h>      // esp_reset_reason(): por qué se reinició

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

// RN10: ventana de arranque del motor (corriente de irrupción)
const uint32_t ARRANQUE_MOTOR_MS = 1000;   // tiempo después de conectar el motor
const float    FACTOR_ARRANQUE   = 3.0;    // límite durante el arranque = 3 x corr_max
uint32_t t_motor_on = 0;                   // millis() en que se conectó el motor

// RN11: modo de operación
enum ModoOp { MODO_APAGADO = 0, MODO_CARGA = 1, MODO_MOTOR = 2 };
ModoOp modo_op = MODO_APAGADO;                 // al arrancar: APAGADO (el motor nunca arranca solo)
const char* NOMBRE_MODO[3] = {"APAGADO", "CARGA", "MOTOR"};

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
    if (i_ok) {  // RN10 inmediata (sin persistencia)
        // Al arrancar, un motor de CD jala varias veces su corriente normal durante unos
        // cientos de ms. En ese primer segundo el límite sube a 3 x corr_max (un corto
        // circuito real lo rebasa igual y se corta en el acto).
        const bool arrancando = bms.act2 && (millis() - t_motor_on < ARRANQUE_MOTOR_MS);
        const float lim_i = arrancando ? cfg.corr_max * FACTOR_ARRANQUE : cfg.corr_max;
        evaluarRegla(reglas[R_RN10], fabs(bms.i_descarga) > lim_i, false, 0);
    }

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
    bool a1 = (modo_op == MODO_CARGA);            // RN11: el modo decide qué se puede conectar
    bool a2 = (modo_op == MODO_MOTOR);
    for (int i = 0; i < N_REGLAS; i++) {
        if (!reglas[i].activa) continue;
        if (reglas[i].corta_a1) a1 = false;
        if (reglas[i].corta_a2) a2 = false;
    }
    if (!inicio_seguro) a1 = a2 = false;          // RN05

    if (a2 && !bms.act2) t_motor_on = millis();   // RN10: empieza la ventana de arranque
    bms.act1 = a1;
    bms.act2 = a2;
    digitalWrite(Pines::ACTUADOR_1, a1 ? NIVEL_ACTIVO : NIVEL_INACTIVO);
    digitalWrite(Pines::ACTUADOR_2, a2 ? NIVEL_ACTIVO : NIVEL_INACTIVO);
}

// ============================================================================
// 8. COMANDOS DESDE LA RASPBERRY (RN08)
// ============================================================================
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
// 9. TELEMETRÍA
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
        "\"act1\":%d,\"act2\":%d,\"inicio_seguro\":%d,\"n_celdas\":%d,\"modo_op\":\"%s\","
        "\"reglas\":%s,\"espera_act1_s\":%lu,\"espera_act2_s\":%lu,\"tendencia_t\":%d,\"tendencia_v\":%d}",
        bms.v_in, bms.v_out, bms.v_4v, bms.v_8v, bms.v_12v, v_banco,
        bms.i_carga, bms.i_descarga, bms.temp1, bms.temp2,
        bms.act1 ? 1 : 0, bms.act2 ? 1 : 0, inicio_seguro ? 1 : 0, (int)cfg.n_celdas, NOMBRE_MODO[modo_op],
        reglas_txt, (unsigned long)espera1, (unsigned long)espera2, hay_alerta_t ? 1 : 0, hay_alerta_v ? 1 : 0);
    Serial2.println(json);
}

// ============================================================================
// 10. SETUP Y LOOP
// ============================================================================
void setup() {
    // RN05: actuadores desactivados desde el primer instante
    pinMode(Pines::ACTUADOR_1, OUTPUT);
    pinMode(Pines::ACTUADOR_2, OUTPUT);
    digitalWrite(Pines::ACTUADOR_1, NIVEL_INACTIVO);
    digitalWrite(Pines::ACTUADOR_2, NIVEL_INACTIVO);

    Serial2.setRxBufferSize(1024);
    Serial2.begin(115200, SERIAL_8N1, Pines::UART2_RX, Pines::UART2_TX);

    pinMode(Pines::I_CARGA, INPUT_PULLDOWN);
    pinMode(Pines::I_DESCARGA, INPUT_PULLDOWN);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    cargarConfig();
    cargarCalibracionVolt();

    // Watchdog: si loop() deja de ejecutarse 8 s, el ESP32 se reinicia solo
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdt = {8000, 0, true};   // timeout_ms, idle_core_mask, trigger_panic
    esp_task_wdt_reconfigure(&wdt);
#else
    esp_task_wdt_init(8, true);
#endif
    esp_task_wdt_add(NULL);

    enviarConfigRPi();   // la Raspberry sabe con qué umbrales arrancó

    // Avisar a la Raspberry por qué arrancó el ESP32. Si fue una caída de voltaje (BROWNOUT)
    // o el watchdog, queda registrado: explica por qué el modo volvió solo a APAGADO.
    const char* causa;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  causa = "ENCENDIDO"; break;
        case ESP_RST_BROWNOUT: causa = "BROWNOUT"; break;
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_WDT:      causa = "WATCHDOG"; break;
        case ESP_RST_PANIC:    causa = "PANIC"; break;
        case ESP_RST_EXT:      causa = "BOTON_EN"; break;
        case ESP_RST_SW:       causa = "SOFTWARE"; break;
        default:               causa = "OTRO"; break;
    }
    enviarEvento("REINICIO_ESP32", "RN05", causa);
}

void loop() {
    static uint32_t t_json = 0;

    leerSensores();          // ~250 ms
    evaluarReglas();         // RN01-RN05, RN09, RN10
    aplicarActuadores();

    uint32_t ahora = millis();
    if (ahora - t_json >= 500) { enviarJSON_RPi(); t_json = ahora; }   // Tabla 4: 200-500 ms

    leerUART_RPi();
    esp_task_wdt_reset();
}
