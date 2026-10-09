/*
  garen_setup_c3 - substituto caseiro do "Garen Setup" com ESP32-C3.

  Fala TuyaMCU com a central Garen Fit Connect (conector SETUP, 9600 8N1, 3,3V)
  e cria um hotspot Wi-Fi com uma página para acionar o portão, ver o estado,
  ajustar e salvar os parâmetros e gravar controles remotos.

  Ligação (central -> ESP32-C3):
    5V   -> 5V/VIN da placa do C3 (NÃO no 3V3)
    GND  -> GND
    TX   (PA2 da central) -> GAREN_RX_PIN
    RX   (PA3 da central) -> GAREN_TX_PIN
  Ao gravar pelo USB, desconecte o fio de 5V da central (mantenha GND/TX/RX).

  Protocolo: veja docs/protocolo.md no repositório. Resumo:
    1   bool  escrever true = acionar (como o controle); leitura = motor ligado
    3   bool  portão NÃO está fechado
    101 bool  liga junto com o motor
    108 PAUSA s | 110 FORÇA % | 111 RAMPA FC | 117 RAMPA AB | 118 FREIO %
    119 enum  menu TX: escrever 1 = abrir; central: 2 = controle detectado,
              5 = já cadastrado, 0 = fechou; escrever 3 = confirmar gravação,
              6 (com o menu aberto) = apagar todos os controles
    120 enum  curso: escrever 1 = gravação automática; volta a 0 ao terminar
    123 bool  escrever true = SALVAR
    151 value controles cadastrados   113 value desconhecido (não escrever)
  - A central ignora o 0x08; o status completo vem quando o status de rede
    passa de 03 para 04.
  - Escrita de parâmetros só no modo Instalador (o PID muda com o modo).
    Valores > 100 são ignorados. Subindo em parâmetros de passo 5 a central
    passa um passo do alvo, então enviamos alvo-1. Sem SALVAR (DP 123) o valor
    se perde ao desligar a central.

  Core: esp32 by Espressif 3.x. Placa: "ESP32C3 Dev Module",
  USB CDC On Boot: Enabled.
*/

#include <WiFi.h>
#include <WebServer.h>

// ---------- configuração ----------
#define GAREN_RX_PIN 20          // recebe do TX da central (ajuste à sua placa)
#define GAREN_TX_PIN 21          // envia para o RX da central
#define AP_SSID      "Garen-Portao"
#define AP_PASSWORD  "troque-esta-senha"   // mínimo 8 caracteres - TROQUE!
#define AP_TX_POWER  WIFI_POWER_11dBm      // menor = menos consumo/alcance
#define POLL_MS      1000        // intervalo de leitura do status completo
#define HEARTBEAT_MS 15000
#define PRODUCT_MS   10000       // reconsulta o PID (o modo pode mudar pelos jumpers)
#define ONLINE_TIMEOUT_MS 20000  // sem resposta por mais que isso = offline
#define ECHO_TIMEOUT_MS   600    // quanto esperar a central confirmar um comando
#define TX_SESSION_MS     30000  // tempo máximo de uma sessão "gravar controle"

#define PID_USUARIO    "l9era1stzldt1ybu"
#define PID_INSTALADOR "cpk9ndmiftjvwhwz"

// DPs de comando
#define DP_ACIONAR  1
#define DP_ABERTO   3
#define DP_AUX      101
#define DP_TX       119
#define DP_CURSO    120
#define DP_SALVAR   123
#define DP_CONTROLES 151

// Parâmetros editáveis (faixas do manual da Fit Connect). A pausa vai até 120
// pelos botões da placa, mas pela serial o máximo alcançável é 105.
struct Param { uint8_t dp; const char *key, *label, *unit; int16_t min, max, step; };
const Param PARAMS[] = {
  {108, "pausa",    "Pausa",            "s", 0, 105, 5},
  {110, "forca",    "Força",            "%", 5, 100, 5},
  {117, "rampa_ab", "Rampa abertura",   "",  0, 15,  1},
  {111, "rampa_fc", "Rampa fechamento", "",  0, 15,  1},
  {118, "freio",    "Freio",            "%", 0, 100, 5},
};

HardwareSerial garen(1);
WebServer web(80);

// ---------- estado ----------
struct Dp { bool seen; int32_t value; uint32_t stamp; };
Dp dps[256];                      // indexado pelo número do DP
uint32_t version = 0;             // muda sempre que algum DP muda de valor
String productInfo, pid;
uint32_t lastRxMs = 0, lastHbMs = 0, lastPollMs = 0, lastProductMs = 0, lastTriggerMs = 0;
uint8_t pollStage = 0;            // 0 = ocioso, 1 = enviou 03/03, aguardando para enviar 03/04
uint32_t pollStageMs = 0;
bool unsaved = false;             // há parâmetro alterado sem SALVAR

// sessão "gravar controle"
enum TxPhase : uint8_t { TX_IDLE, TX_WAITING, TX_CONFIRMING };
TxPhase txPhase = TX_IDLE;
uint32_t txStartMs = 0;
int32_t txCountBefore = 0;
String txResult;                  // "", "gravado", "ja_cadastrado", "tempo_esgotado"

bool installerMode() { return pid == PID_INSTALADOR; }
const char *modeName() {
  if (pid == PID_INSTALADOR) return "instalador";
  if (pid == PID_USUARIO) return "usuario";
  return "desconhecido";
}

// ---------- TuyaMCU: envio ----------
void tuyaSend(uint8_t cmd, const uint8_t *data, uint16_t len) {
  uint8_t hdr[6] = {0x55, 0xAA, 0x00, cmd, (uint8_t)(len >> 8), (uint8_t)len};
  uint8_t sum = 0;
  for (uint8_t b : hdr) sum += b;
  for (uint16_t i = 0; i < len; i++) sum += data[i];
  garen.write(hdr, 6);
  if (len) garen.write(data, len);
  garen.write(sum);
}

void sendWifiStatus(uint8_t st) { tuyaSend(0x03, &st, 1); }

void sendDpBool(uint8_t dp, bool v) {
  const uint8_t p[5] = {dp, 1, 0, 1, (uint8_t)v};
  tuyaSend(0x06, p, 5);
}

void sendDpEnum(uint8_t dp, uint8_t v) {
  const uint8_t p[5] = {dp, 4, 0, 1, v};
  tuyaSend(0x06, p, 5);
}

void sendDpValue(uint8_t dp, int32_t v) {
  const uint8_t p[8] = {dp, 2, 0, 4, (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  tuyaSend(0x06, p, 8);
}

// ---------- TuyaMCU: recepção ----------
void onTxReport(int32_t v) {
  if (txPhase == TX_IDLE) return;
  if (v == 2 && txPhase == TX_WAITING) {        // controle detectado: confirma
    txCountBefore = dps[DP_CONTROLES].value;
    sendDpEnum(DP_TX, 3);
    txPhase = TX_CONFIRMING;
  } else if (v == 5) {
    txResult = "ja_cadastrado";
  } else if (v == 0) {                           // menu fechou
    if (txResult.isEmpty()) txResult = "tempo_esgotado";
    txPhase = TX_IDLE;
  }
}

void handleDpReport(const uint8_t *d, uint16_t len) {
  uint16_t i = 0;
  while (i + 4 <= len) {
    uint8_t id = d[i], type = d[i + 1];
    uint16_t l = (d[i + 2] << 8) | d[i + 3];
    if (i + 4 + l > len) break;
    int32_t v = 0;
    if (type == 2 && l == 4) {
      v = (int32_t)((uint32_t)d[i + 4] << 24 | (uint32_t)d[i + 5] << 16 |
                    (uint32_t)d[i + 6] << 8 | d[i + 7]);
    } else {
      for (uint16_t k = 0; k < l && k < 4; k++) v = (v << 8) | d[i + 4 + k];
    }
    if (!dps[id].seen || dps[id].value != v) version++;
    dps[id].seen = true;
    dps[id].value = v;
    dps[id].stamp = millis() | 1;   // nunca 0: marca que chegou um relatório
    if (id == DP_TX) onTxReport(v);
    if (id == DP_CONTROLES && txPhase == TX_CONFIRMING && v > txCountBefore) {
      txResult = "gravado";
    }
    i += 4 + l;
  }
}

void handleFrame(uint8_t cmd, const uint8_t *d, uint16_t len) {
  lastRxMs = millis();
  switch (cmd) {
    case 0x01: {
      productInfo = "";
      for (uint16_t k = 0; k < len; k++) productInfo += (char)d[k];
      int a = productInfo.indexOf("\"p\":\"");
      if (a >= 0) {
        int b = productInfo.indexOf('"', a + 5);
        String p = productInfo.substring(a + 5, b);
        if (p != pid) { pid = p; version++; }
      }
      break;
    }
    case 0x07: handleDpReport(d, len); break;
    case 0x2B: { uint8_t st = 0x04; tuyaSend(0x2B, &st, 1); break; }   // MCU pede status de rede
    default: break;
  }
}

void pollSerial() {
  static uint8_t buf[300];
  static uint16_t n = 0;
  while (garen.available()) {
    uint8_t b = garen.read();
    if (n == 0 && b != 0x55) continue;
    if (n == 1 && b != 0xAA) { n = (b == 0x55) ? 1 : 0; continue; }
    buf[n++] = b;
    if (n >= 6) {
      uint16_t len = (buf[4] << 8) | buf[5];
      if (len > sizeof(buf) - 7) { n = 0; continue; }
      if (n == 7u + len) {
        uint8_t sum = 0;
        for (uint16_t k = 0; k < n - 1; k++) sum += buf[k];
        if (sum == buf[n - 1]) handleFrame(buf[3], buf + 6, len);
        n = 0;
      }
    }
  }
}

// Espera a central reportar o DP (eco do comando) sem travar a serial.
bool waitDpReport(uint8_t dp, uint32_t sentAt) {
  while (millis() - sentAt < ECHO_TIMEOUT_MS) {
    pollSerial();
    if (dps[dp].stamp && (int32_t)(dps[dp].stamp - sentAt) >= 0) return true;
    delay(2);
  }
  return false;
}

// Leva um parâmetro até o alvo. A central anda em passos na direção do valor
// recebido e, subindo, passa um passo além de um múltiplo exato: por isso
// envia alvo-1 ao subir. Confere o eco e corrige (até 3 tentativas).
bool setParam(const Param &p, int32_t target) {
  for (int attempt = 0; attempt < 3; attempt++) {
    int32_t cur = dps[p.dp].value;
    if (cur == target) return true;
    int32_t out = (target > cur && p.step > 1) ? target - 1 : target;
    uint32_t sentAt = millis();
    sendDpValue(p.dp, out);
    waitDpReport(p.dp, sentAt);
  }
  return dps[p.dp].value == target;
}

bool saveParams() {
  uint32_t sentAt = millis();
  sendDpBool(DP_SALVAR, true);
  bool ok = waitDpReport(DP_CONTROLES, sentAt);   // a central responde reenviando o DP 151
  if (ok) unsaved = false;
  return ok;
}

// ---------- ciclo do "módulo" ----------
void moduleLoop() {
  uint32_t now = millis();
  if (now - lastHbMs >= HEARTBEAT_MS || lastHbMs == 0) {
    tuyaSend(0x00, nullptr, 0);
    lastHbMs = now;
  }
  if (now - lastProductMs >= PRODUCT_MS) {
    tuyaSend(0x01, nullptr, 0);
    lastProductMs = now;
  }
  if (pollStage == 0 && now - lastPollMs >= POLL_MS) {
    sendWifiStatus(0x03);         // "só roteador"...
    pollStage = 1;
    pollStageMs = now;
  } else if (pollStage == 1 && now - pollStageMs >= 100) {
    sendWifiStatus(0x04);         // ...e "nuvem": a central despeja todos os DPs
    pollStage = 0;
    lastPollMs = now;
  }
  if (txPhase != TX_IDLE && now - txStartMs > TX_SESSION_MS) {
    if (txResult.isEmpty()) txResult = "tempo_esgotado";
    txPhase = TX_IDLE;
  }
}

bool online() { return lastRxMs && millis() - lastRxMs < ONLINE_TIMEOUT_MS; }

// ---------- web: JSON ----------
String jsonVal(uint8_t id) { return dps[id].seen ? String(dps[id].value) : String("null"); }
String jsonBool(bool b) { return b ? "true" : "false"; }

String statusJson() {
  String j;
  j.reserve(900);
  j = "{\"v\":" + String(version);
  j += ",\"online\":" + jsonBool(online());
  j += String(",\"modo\":\"") + modeName() + "\"";
  j += ",\"motor\":" + jsonBool(dps[DP_ACIONAR].seen && dps[DP_ACIONAR].value);
  j += ",\"fechado\":" + jsonBool(dps[DP_ABERTO].seen && !dps[DP_ABERTO].value);
  j += ",\"aux\":" + jsonBool(dps[DP_AUX].seen && dps[DP_AUX].value);
  j += ",\"curso\":" + jsonBool(dps[DP_CURSO].seen && dps[DP_CURSO].value);
  j += ",\"controles\":" + jsonVal(DP_CONTROLES);
  j += ",\"pendente\":" + jsonBool(unsaved);
  j += String(",\"tx\":{\"ativo\":") + jsonBool(txPhase != TX_IDLE) +
       ",\"fase\":" + String((int)txPhase) + ",\"resultado\":\"" + txResult + "\"}";
  j += ",\"params\":[";
  for (size_t i = 0; i < sizeof(PARAMS) / sizeof(PARAMS[0]); i++) {
    const Param &p = PARAMS[i];
    if (i) j += ',';
    j += "{\"dp\":" + String(p.dp) + ",\"key\":\"" + p.key + "\",\"label\":\"" + p.label +
         "\",\"unit\":\"" + p.unit + "\",\"min\":" + String(p.min) + ",\"max\":" + String(p.max) +
         ",\"step\":" + String(p.step) + ",\"value\":" + jsonVal(p.dp) + "}";
  }
  j += "]}";
  return j;
}

void sendJson(int code, const String &body) {
  web.sendHeader("Cache-Control", "no-store");
  web.send(code, "application/json", body);
}

void sendError(int code, const char *msg) {
  sendJson(code, String("{\"error\":\"") + msg + "\"}");
}

bool requirePost() {
  if (web.method() == HTTP_POST) return true;
  sendError(405, "use POST");
  return false;
}

// ---------- web: rotas ----------
void handleStatus() { sendJson(200, statusJson()); }

void handleTrigger() {
  if (!requirePost()) return;
  if (millis() - lastTriggerMs < 700) { sendError(429, "aguarde"); return; }
  lastTriggerMs = millis();
  uint32_t sentAt = millis();
  sendDpBool(DP_ACIONAR, true);
  waitDpReport(DP_ACIONAR, sentAt);   // a central ecoa o DP 1 em ~50 ms
  sendJson(200, statusJson());
}

void handleParam() {
  if (!requirePost()) return;
  if (!installerMode()) { sendError(403, "coloque a central no modo Instalador"); return; }
  int dp = web.arg("dp").toInt();
  long v = web.arg("v").toInt();
  const Param *p = nullptr;
  for (const Param &q : PARAMS) if (q.dp == dp) p = &q;
  if (!p) { sendError(400, "parametro invalido"); return; }
  if (!dps[dp].seen) { sendError(503, "valor atual ainda desconhecido"); return; }
  if (v < p->min || v > p->max || (v - p->min) % p->step) { sendError(400, "fora da faixa"); return; }
  bool ok = setParam(*p, v);
  if (ok) unsaved = true;
  String j = statusJson();
  j.remove(j.length() - 1);
  j += ",\"confirmado\":" + jsonBool(ok) + "}";
  sendJson(ok ? 200 : 502, j);
}

void handleSave() {
  if (!requirePost()) return;
  if (!installerMode()) { sendError(403, "coloque a central no modo Instalador"); return; }
  if (!saveParams()) { sendError(502, "a central nao confirmou"); return; }
  sendJson(200, statusJson());
}

// Apaga TODOS os controles: abre o menu TX (1) e manda 6. A central zera o
// DP 151 ~2,3 s depois (equivale a segurar o TX da placa por 5 s).
void handleTxErase() {
  if (!requirePost()) return;
  if (!installerMode()) { sendError(403, "coloque a central no modo Instalador"); return; }
  if (txPhase != TX_IDLE) { sendError(409, "gravacao de controle em andamento"); return; }
  sendDpEnum(DP_TX, 1);
  uint32_t t0 = millis();
  while (millis() - t0 < 800) { pollSerial(); delay(2); }
  uint32_t sentAt = millis();
  sendDpEnum(DP_TX, 6);
  bool ok = false;
  while (millis() - sentAt < 5000) {
    pollSerial();
    if (dps[DP_CONTROLES].stamp && (int32_t)(dps[DP_CONTROLES].stamp - sentAt) >= 0 &&
        dps[DP_CONTROLES].value == 0) { ok = true; break; }
    delay(2);
  }
  if (!ok) { sendError(502, "a central nao confirmou"); return; }
  sendJson(200, statusJson());
}

// Gravação de curso automática: a central percorre os dois fins de curso
// sozinha e reporta DP 120 = 0 ao terminar. O portão SE MOVE sem comando.
void handleCurso() {
  if (!requirePost()) return;
  if (dps[DP_CURSO].seen && dps[DP_CURSO].value) { sendError(409, "gravacao de curso em andamento"); return; }
  uint32_t sentAt = millis();
  sendDpEnum(DP_CURSO, 1);
  waitDpReport(DP_CURSO, sentAt);
  sendJson(200, statusJson());
}

void handleTxStart() {
  if (!requirePost()) return;
  if (!installerMode()) { sendError(403, "coloque a central no modo Instalador"); return; }
  txResult = "";
  txPhase = TX_WAITING;
  txStartMs = millis();
  sendDpEnum(DP_TX, 1);
  waitDpReport(DP_TX, txStartMs);
  sendJson(200, statusJson());
}

// ---------- web: página ----------
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="pt-BR"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0f172a"><title>Portão</title><style>
:root{--bg:#f1f5f9;--card:#fff;--fg:#0f172a;--mut:#64748b;--line:#e2e8f0;
--ok:#16a34a;--warn:#d97706;--info:#2563eb;--bad:#dc2626}
@media(prefers-color-scheme:dark){:root{--bg:#0b1120;--card:#172033;--fg:#e2e8f0;--mut:#94a3b8;--line:#243049}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
[hidden]{display:none!important}
body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--fg);
padding:max(16px,env(safe-area-inset-top)) 16px 32px}
main{max-width:440px;margin:0 auto;display:grid;gap:14px}
header{display:flex;justify-content:space-between;align-items:center;gap:8px}
h1{font-size:1.15rem;margin:0}
.pills{display:flex;gap:6px}
.pill{font-size:.75rem;padding:4px 10px;border-radius:99px;background:var(--card);color:var(--mut);display:flex;gap:6px;align-items:center;white-space:nowrap}
.dot{width:8px;height:8px;border-radius:50%;background:var(--bad)}.pill.on .dot{background:var(--ok)}
.card{background:var(--card);border-radius:20px;padding:18px}
.gate{display:grid;justify-items:center;gap:14px;padding:26px 18px}
.ring{width:190px;height:190px;border-radius:50%;border:0;cursor:pointer;display:grid;place-items:center;
color:#fff;background:var(--c,var(--mut));box-shadow:0 0 0 10px color-mix(in srgb,var(--c,var(--mut)) 18%,transparent);
transition:transform .12s,background .25s,box-shadow .25s;font:inherit}
.ring:active{transform:scale(.95)}
.ring svg{width:64px;height:64px}
.ring span{display:block;font-weight:700;font-size:1.05rem;margin-top:6px}
.ring.busy{animation:pulse 1.1s ease-in-out infinite}
@keyframes pulse{50%{box-shadow:0 0 0 22px color-mix(in srgb,var(--c) 6%,transparent)}}
.state{font-size:1.5rem;font-weight:700;margin:0}
.sub{color:var(--mut);font-size:.9rem;margin:-8px 0 0}
.row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-top:1px solid var(--line)}
.row:first-of-type{border-top:0}
.lbl{font-size:.95rem}.lbl small{display:block;color:var(--mut);font-size:.75rem}
.step{display:flex;align-items:center;gap:6px}
.step button{width:38px;height:38px;border-radius:12px;border:1px solid var(--line);background:transparent;color:var(--fg);font-size:1.25rem;cursor:pointer}
.step button:disabled{opacity:.35}.ro .step button{display:none}
.val{min-width:64px;text-align:center;font-weight:700;font-variant-numeric:tabular-nums}
.val.pend{color:var(--warn)}.val.err{color:var(--bad)}
.head{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
.head h2{font-size:1rem;margin:0}
.sw{display:flex;gap:8px;align-items:center;font-size:.85rem;color:var(--mut)}
.sw input{width:42px;height:24px;appearance:none;border-radius:99px;background:var(--line);position:relative;cursor:pointer;transition:.2s}
.sw input::after{content:"";position:absolute;width:18px;height:18px;border-radius:50%;background:#fff;top:3px;left:3px;transition:.2s}
.sw input:checked{background:var(--info)}.sw input:checked::after{left:21px}
.note{color:var(--mut);font-size:.78rem;margin:8px 0 0;line-height:1.4}
.btn{width:100%;padding:13px;border:0;border-radius:14px;font:inherit;font-weight:600;cursor:pointer;margin-top:10px;
background:var(--info);color:#fff}.btn:disabled{opacity:.45}
.btn.warn{background:var(--warn)}
.btn.danger{background:transparent;color:var(--bad);border:1px solid var(--bad)}
.msg{font-size:.9rem;margin:10px 0 0;text-align:center;font-weight:600}
.banner{background:var(--card);border-left:4px solid var(--info);border-radius:14px;padding:12px 14px;font-size:.88rem;line-height:1.4}
.banner small{display:block;color:var(--mut);margin-top:4px;font-size:.78rem}
.toast{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);background:var(--fg);color:var(--bg);
padding:10px 16px;border-radius:12px;font-size:.9rem;opacity:0;transition:opacity .2s;pointer-events:none}
.toast.show{opacity:1}
</style></head><body><main>
<header><h1>Portão</h1><div class="pills">
 <div class="pill" id="modo">—</div>
 <div class="pill" id="pill"><i class="dot"></i><span id="con">conectando…</span></div></div></header>
<div class="banner" id="banner" hidden><b id="bannerTit">Modo cliente</b>: só acionamento e consulta.
 <small>O modo instalador libera ajuste de parâmetros, gravação de controles e de curso. Para ativar: feche os jumpers FORÇA e RAMPA AB, segure TX por 5 s até o LED piscar 1 vez e abra os jumpers.</small></div>
<section class="card gate">
 <button class="ring" id="ring" aria-label="Acionar portão">
  <div><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
  <path d="M3 21V8l9-5 9 5v13"/><path d="M7 21v-9h10v9"/><path d="M7 15h10M7 18h10"/></svg>
  <span>Acionar</span></div></button>
 <p class="state" id="state">—</p><p class="sub" id="sub"></p>
</section>
<section class="card"><div class="head"><h2>Parâmetros</h2>
 <label class="sw" id="swEdit" hidden>editar <input type="checkbox" id="edit"></label></div>
 <div id="params"></div>
 <button class="btn warn" id="save" hidden>Salvar na central</button>
 <p class="note" id="note"></p>
</section>
<section class="card"><div class="head"><h2>Controles remotos</h2><b id="ctl">—</b></div>
 <button class="btn" id="txBtn">Gravar novo controle</button>
 <button class="btn danger" id="txDel" hidden>Apagar todos os controles</button>
 <p class="msg" id="txMsg"></p>
 <p class="note" id="txNote"></p>
</section>
<section class="card" id="cursoCard"><div class="head"><h2>Curso do portão</h2></div>
 <button class="btn" id="cursoBtn">Gravar curso</button>
 <p class="msg" id="cursoMsg"></p>
 <p class="note">A central move o portão sozinha até um fim de curso, depois até o outro e volta. Mantenha a área livre. As rampas dependem do curso gravado.</p>
</section>
</main><div class="toast" id="toast"></div>
<script>
const $=id=>document.getElementById(id);let S=null,busy=false,pend={},cursoAnt=false;
const TX={gravado:'✓ Controle gravado',ja_cadastrado:'Este controle já está cadastrado',tempo_esgotado:'Tempo esgotado — nenhum controle gravado'};
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('show');clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove('show'),2200)}
async function post(url){const r=await fetch(url,{method:'POST'});let j={};try{j=await r.json()}catch(e){}return{ok:r.ok,j}}
function render(s){
 S=s;const r=$('ring'),inst=s.modo==='instalador';let st,sub,c;
 if(!s.online){st='Central offline';sub='verifique o cabo do conector SETUP';c='var(--bad)'}
 else if(s.curso){st='Gravando curso';sub='';c='var(--warn)'}
 else if(s.motor){st='Em movimento';sub='toque para parar';c='var(--warn)'}
 else if(s.fechado){st='Fechado';sub='toque para abrir';c='var(--ok)'}
 else{st='Aberto ou parado';sub='toque para acionar';c='var(--info)'}
 r.style.setProperty('--c',c);r.classList.toggle('busy',s.motor);
 $('state').textContent=st;$('sub').textContent=sub;
 $('pill').classList.toggle('on',s.online);$('con').textContent=s.online?'online':'offline';
 $('modo').textContent=inst?'instalador':s.modo==='usuario'?'usuário':'modo ?';
 $('ctl').textContent=s.controles??'—';
 // parâmetros
 $('swEdit').hidden=!inst;const ed=inst&&$('edit').checked,box=$('params');
 box.classList.toggle('ro',!inst);
 if(box.children.length!==s.params.length)box.innerHTML=s.params.map(p=>
  `<div class="row"><span class="lbl">${p.label}<small>${p.min}–${p.max}${p.unit?' '+p.unit:''}</small></span>
  <div class="step"><button data-dp="${p.dp}" data-d="-1">−</button><span class="val" id="v${p.dp}"></span>
  <button data-dp="${p.dp}" data-d="1">+</button></div></div>`).join('');
 for(const p of s.params){const v=$('v'+p.dp);
  if(!pend[p.dp]){v.textContent=p.value==null?'—':p.value+(p.unit?' '+p.unit:'');v.className='val'}
  box.querySelectorAll(`[data-dp="${p.dp}"]`).forEach(b=>{const d=+b.dataset.d;
   b.disabled=!ed||!s.online||p.value==null||pend[p.dp]||(d<0?p.value<=p.min:p.value>=p.max)})}
 $('banner').hidden=!s.online||inst;
 $('bannerTit').textContent=s.modo==='usuario'?'Modo cliente':'Modo não identificado';
 $('save').hidden=!(inst&&s.pendente);
 $('note').hidden=!inst;
 $('note').textContent=s.pendente?'Há alterações não salvas: elas se perdem se a central desligar.':'Alterações valem na hora; toque em "Salvar na central" para gravá-las.';
 // controles
 const tx=s.tx;$('txBtn').disabled=!inst||!s.online||tx.ativo;
 $('txBtn').hidden=!inst;$('txDel').hidden=!inst;$('txDel').disabled=!s.online||tx.ativo||!s.controles;
 $('txMsg').textContent=TX[tx.resultado]||(tx.ativo?(tx.fase===2?'Controle detectado, gravando…':'Aperte agora o botão do controle novo…'):'');
 if(cursoAnt&&!s.curso)$('cursoMsg').textContent='✓ Curso gravado';
 if(s.curso)$('cursoMsg').textContent='Gravando curso…';
 cursoAnt=s.curso;$('cursoBtn').disabled=!s.online||s.curso;
 $('txNote').hidden=!inst;$('txMsg').hidden=!inst;$('cursoCard').hidden=!inst;
 $('txNote').textContent='Grava só o controle apertado durante a espera (a central precisa detectá-lo).';
}
async function get(){try{render(await (await fetch('/api/status',{cache:'no-store'})).json())}
 catch(e){$('pill').classList.remove('on');$('con').textContent='sem conexão'}}
$('ring').onclick=async()=>{if(busy)return;busy=true;navigator.vibrate&&navigator.vibrate(30);
 $('ring').style.setProperty('--c','var(--warn)');
 try{const{ok,j}=await post('/api/acionar');if(ok)render(j);else toast(j.error||'aguarde um instante')}
 catch(e){toast('falha ao enviar')}finally{setTimeout(()=>busy=false,400)}};
$('params').onclick=async e=>{const b=e.target.closest('button');if(!b||b.disabled||!S)return;
 const dp=+b.dataset.dp,p=S.params.find(x=>x.dp==dp),nv=p.value+(+b.dataset.d)*p.step;
 pend[dp]=1;const v=$('v'+dp);v.textContent=nv+(p.unit?' '+p.unit:'');v.className='val pend';render(S);
 try{const{ok,j}=await post(`/api/param?dp=${dp}&v=${nv}`);delete pend[dp];if(j.params)render(j);
  if(!ok){toast(j.error||'a central não confirmou');$('v'+dp).className='val err'}}
 catch(x){delete pend[dp];toast('falha ao enviar')}};
$('save').onclick=async()=>{const b=$('save');b.disabled=true;
 try{const{ok,j}=await post('/api/salvar');if(ok){render(j);toast('Salvo na central')}else toast(j.error||'falha ao salvar')}
 catch(e){toast('falha ao salvar')}finally{b.disabled=false}};
$('txBtn').onclick=async()=>{try{const{ok,j}=await post('/api/tx');if(ok)render(j);else toast(j.error||'falha')}catch(e){toast('falha ao enviar')}};
$('txDel').onclick=async()=>{
 if(!confirm('Apagar TODOS os controles cadastrados? O portão deixará de abrir pelos controles até você gravá-los de novo.'))return;
 if(!confirm('Tem certeza? Esta ação não pode ser desfeita.'))return;
 const b=$('txDel');b.disabled=true;
 try{const{ok,j}=await post('/api/tx/apagar');if(ok){render(j);toast('Controles apagados')}else toast(j.error||'falha ao apagar')}
 catch(e){toast('falha ao enviar')}};
$('cursoBtn').onclick=async()=>{
 if(!confirm('O portão vai se mover sozinho até os dois fins de curso. A área está livre?'))return;
 try{const{ok,j}=await post('/api/curso');if(ok)render(j);else toast(j.error||'falha')}catch(e){toast('falha ao enviar')}};
$('edit').onchange=()=>S&&render(S);
get();setInterval(get,400);
</script></body></html>)HTML";

void setup() {
  Serial.begin(115200);
  garen.begin(9600, SERIAL_8N1, GAREN_RX_PIN, GAREN_TX_PIN);

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);           // sem economia de energia no rádio: menos latência
  bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD);
  WiFi.setTxPower(AP_TX_POWER);

  web.on("/", [] { web.send_P(200, "text/html", PAGE); });
  web.on("/api/status", handleStatus);
  web.on("/api/acionar", handleTrigger);
  web.on("/api/param", handleParam);
  web.on("/api/salvar", handleSave);
  web.on("/api/tx", handleTxStart);
  web.on("/api/curso", handleCurso);
  web.on("/api/tx/apagar", handleTxErase);
  web.begin();

  // apresentação como um módulo Tuya: heartbeat, info do produto, "conectado à nuvem"
  tuyaSend(0x00, nullptr, 0);
  delay(200);
  tuyaSend(0x01, nullptr, 0);
  lastProductMs = millis();
  delay(200);
  sendWifiStatus(0x04);
  lastHbMs = millis();

  if (apOk) Serial.printf("Hotspot \"%s\" ativo em http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  else Serial.println("ERRO: hotspot não iniciou (a senha precisa ter 8+ caracteres)");
}

void loop() {
  pollSerial();
  moduleLoop();
  web.handleClient();
}
