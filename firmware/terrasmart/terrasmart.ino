/*
  TerraSmart - Terrario inteligente (ESP32 + DHT11 + rele/cooler)
  Firmware + interface web (pixel art) em um unico arquivo.
  Interface criada por um colega da equipe, integrada ao firmware
  e a montagem do grupo.

  LIGACOES (todas na lateral do ESP32 que tem o VIN):
    DHT11  VCC  -> D27 (o codigo coloca esse pino em 3,3 V)
    DHT11  DATA -> D26
    DHT11  GND  -> GND
    Rele   S    -> D25   (pino de sinal)
    Rele   +    -> VIN   (5 V)
    Rele   -    -> GND
    Cooler (12 V): positivo da fonte no COM do rele, NO no positivo do cooler,
    negativo do cooler no negativo da fonte.

  Rotas servidas pelo ESP32:
    GET       /             -> painel TerraSmart (HTML no final deste arquivo)
    GET       /temperatura  -> leituras e estado em JSON (usado pelo painel)
    GET       /status       -> JSON simples: temperatura, umidade, atuador, sensorOk
    POST|GET  /atuador      -> mode=auto | off | cool | heat
                               (compatibilidade: estado=on | off)
    POST      /config       -> min=..&max=..  (faixa de controle, salva na memoria)

  Bibliotecas: "DHT sensor library" (Adafruit) + "Adafruit Unified Sensor".
  Se usar a sonda de solo: "OneWire" + "DallasTemperature".
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DHT.h>

// ===== CONFIGURACAO DO WIFI =====
const char* ssid  = "NOME_DA_SUA_REDE";
const char* senha = "SENHA_DA_SUA_REDE";

// ===== CONFIGURACAO DO HARDWARE (ajuste a sua montagem) =====
#define DHTPOWER 27   // este pino alimenta o sensor (3,3 V)
#define DHTPIN   26   // pino de dados do DHT11
#define DHTTYPE  DHT11

#define PINO_COOLER     25   // pino de sinal (S) do modulo rele
#define PINO_AQUECEDOR  33   // so e usado se AQUECEDOR_INSTALADO = 1
#define PINO_DS18B20    5    // sonda de solo (opcional)

// true: sinal HIGH liga o rele | false: sinal LOW liga o rele.
// O modulo HW-482 tem transistor, entao o normal e HIGH ligar.
// Se o rele ligar quando deveria desligar, troque para false.
#define ATUADOR_ATIVO_ALTO true
#define SAIDA_LIGADA     (ATUADOR_ATIVO_ALTO ? HIGH : LOW)
#define SAIDA_DESLIGADA  (ATUADOR_ATIVO_ALTO ? LOW : HIGH)

#define AQUECEDOR_INSTALADO   0   // 1 = existe aquecedor ligado ao PINO_AQUECEDOR
#define SONDA_SOLO_INSTALADA  0   // 1 = existe DS18B20 ligado ao PINO_DS18B20

// ===== PARAMETROS DE CONTROLE =====
const float HISTERESE = 1.0;                      // graus C
const float TEMP_CORTE_AQUECEDOR = 40.0;          // seguranca no aquecimento manual
const unsigned long TEMPO_MANUAL_MS = 60000;      // "por 60 s"
const unsigned long INTERVALO_LEITURA_MS = 2500;  // DHT11 nao aceita leituras rapidas
const int FALHAS_PARA_INVALIDAR = 3;

#if SONDA_SOLO_INSTALADA
#include <OneWire.h>
#include <DallasTemperature.h>
OneWire oneWire(PINO_DS18B20);
DallasTemperature sonda(&oneWire);
#endif

DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);
Preferences memoria;

// Pagina HTML (definida no final do arquivo)
extern const char PAGINA[];

// ===== ESTADO =====
enum Modo { MODO_AUTO, MODO_DESLIGADO, MODO_VENTILAR, MODO_AQUECER };

Modo modo = MODO_AUTO;
Modo modoAnterior = MODO_AUTO;
unsigned long inicioManual = 0;

float tempMin = 22.0;
float tempMax = 28.0;

float tempAr = NAN;
float umidadeAr = NAN;
float tempSolo = NAN;
float tempControle = NAN;
bool  controlePeloSolo = false;
bool  leituraValida = false;
int   falhasSeguidas = 0;

bool coolerLigado = false;
bool aquecedorLigado = false;

// ===== SAIDAS =====
void aplicarSaidas() {
  if (!AQUECEDOR_INSTALADO) aquecedorLigado = false;
  digitalWrite(PINO_COOLER, coolerLigado ? SAIDA_LIGADA : SAIDA_DESLIGADA);
  if (AQUECEDOR_INSTALADO)
    digitalWrite(PINO_AQUECEDOR, aquecedorLigado ? SAIDA_LIGADA : SAIDA_DESLIGADA);
}

// ===== CONTROLE TERMICO =====
void atualizarControle() {
  // Sem leitura confiavel: tudo desligado
  if (!leituraValida) {
    coolerLigado = false;
    aquecedorLigado = false;
    aplicarSaidas();
    return;
  }

  // Fim do acionamento manual temporario: volta ao modo anterior
  if ((modo == MODO_VENTILAR || modo == MODO_AQUECER) &&
      millis() - inicioManual >= TEMPO_MANUAL_MS) {
    modo = modoAnterior;
    coolerLigado = false;
    aquecedorLigado = false;
  }

  float t = tempControle;

  switch (modo) {
    case MODO_AUTO:
      if (coolerLigado && t <= tempMax - HISTERESE) coolerLigado = false;
      if (aquecedorLigado && t >= tempMin + HISTERESE) aquecedorLigado = false;
      if (t >= tempMax) {
        coolerLigado = true;
        aquecedorLigado = false;
      } else if (t <= tempMin) {
        aquecedorLigado = true;
        coolerLigado = false;
      }
      break;

    case MODO_DESLIGADO:
      coolerLigado = false;
      aquecedorLigado = false;
      break;

    case MODO_VENTILAR:
      coolerLigado = true;
      aquecedorLigado = false;
      break;

    case MODO_AQUECER:
      coolerLigado = false;
      aquecedorLigado = (t < TEMP_CORTE_AQUECEDOR);
      break;
  }

  aplicarSaidas();
}

// ===== LEITURA DOS SENSORES =====
void lerSensores() {
  float t = dht.readTemperature();
  float u = dht.readHumidity();
  bool arOk = !isnan(t) && !isnan(u);

  if (arOk) {
    tempAr = t;
    umidadeAr = u;
  }

  bool soloOk = false;
#if SONDA_SOLO_INSTALADA
  float s = sonda.getTempCByIndex(0);   // resultado do pedido anterior
  sonda.requestTemperatures();          // pede a proxima leitura (sem bloquear)
  soloOk = (s > -50.0 && s < 85.0);
  tempSolo = soloOk ? s : NAN;
#endif

  if (soloOk) {
    tempControle = tempSolo;
    controlePeloSolo = true;
    falhasSeguidas = 0;
    leituraValida = true;
  } else if (arOk) {
    tempControle = tempAr;
    controlePeloSolo = false;
    falhasSeguidas = 0;
    leituraValida = true;
  } else {
    falhasSeguidas++;
    if (falhasSeguidas >= FALHAS_PARA_INVALIDAR) {
      leituraValida = false;
      tempAr = NAN;
      umidadeAr = NAN;
      tempControle = NAN;
    }
  }
}

// ===== AUXILIARES =====
String numeroJson(float v) {
  return isnan(v) ? String("null") : String(v, 1);
}

const char* nomeDoModo() {
  switch (modo) {
    case MODO_AUTO:      return "auto";
    case MODO_DESLIGADO: return "off";
    case MODO_VENTILAR:  return "cool";
    default:             return "heat";
  }
}

void responderErro(int codigo, const char* mensagem) {
  server.send(codigo, "text/plain; charset=utf-8", mensagem);
}

// Resposta JSON (com CORS, para permitir acesso de paginas de outra origem)
void enviarJson(int codigo, const String& json) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(codigo, "application/json", json);
}

// ===== ROTAS =====
void paginaPrincipal() {
  server.send_P(200, "text/html; charset=utf-8", PAGINA);
}

// JSON completo usado pelo painel
void consultarTemperatura() {
  String json = "{";
  json += "\"air\":" + numeroJson(tempAr) + ",";
  json += "\"soil\":" + numeroJson(tempSolo) + ",";
  json += "\"humidity\":" + numeroJson(umidadeAr) + ",";
  json += "\"temperature\":" + numeroJson(tempControle) + ",";
  json += "\"valid\":" + String(leituraValida ? "true" : "false") + ",";
  json += "\"soilEnabled\":" + String(SONDA_SOLO_INSTALADA ? "true" : "false") + ",";
  json += "\"heatEnabled\":" + String(AQUECEDOR_INSTALADO ? "true" : "false") + ",";
  json += "\"source\":\"" + String(controlePeloSolo ? "solo" : "ar") + "\",";
  json += "\"min\":" + String(tempMin, 1) + ",";
  json += "\"max\":" + String(tempMax, 1) + ",";
  json += "\"mode\":\"" + String(nomeDoModo()) + "\",";
  json += "\"cool\":" + String(coolerLigado ? "true" : "false") + ",";
  json += "\"heat\":" + String(aquecedorLigado ? "true" : "false");
  json += "}";

  server.sendHeader("Cache-Control", "no-store");
  enviarJson(200, json);
}

// JSON simples: temperatura, umidade, estado do atuador e saude do sensor
void consultarStatus() {
  String json = "{";
  json += "\"temperatura\":" + numeroJson(tempAr) + ",";
  json += "\"umidade\":" + numeroJson(umidadeAr) + ",";
  json += "\"atuador\":\"" + String(coolerLigado ? "on" : "off") + "\",";
  json += "\"sensorOk\":" + String(leituraValida ? "true" : "false");
  json += "}";
  enviarJson(200, json);
}

// Botoes: Automatico / Desligar / Ventilar 60 s / Aquecer 60 s
void comandoAtuador() {
  if (!server.hasArg("mode") && !server.hasArg("estado")) {
    responderErro(400, "use mode=auto|off|cool|heat (ou estado=on|off)");
    return;
  }

  // "estado=on" (forma antiga) equivale a ventilar; "estado=off" a desligar
  String m = server.hasArg("mode") ? server.arg("mode") : server.arg("estado");
  if (m == "on") m = "cool";
  bool emManual = (modo == MODO_VENTILAR || modo == MODO_AQUECER);

  if (m == "off") {
    modo = MODO_DESLIGADO;
  } else if (!leituraValida) {
    responderErro(409, "sensor sem leitura valida");
    return;
  } else if (m == "auto") {
    modo = MODO_AUTO;
  } else if (m == "cool") {
    if (!emManual) modoAnterior = modo;
    modo = MODO_VENTILAR;
    inicioManual = millis();
  } else if (m == "heat") {
    if (!AQUECEDOR_INSTALADO) {
      responderErro(400, "aquecedor nao instalado");
      return;
    }
    if (!emManual) modoAnterior = modo;
    modo = MODO_AQUECER;
    inicioManual = millis();
  } else {
    responderErro(400, "modo desconhecido");
    return;
  }

  coolerLigado = false;
  aquecedorLigado = false;
  atualizarControle();

  Serial.print("Modo alterado para: ");
  Serial.println(nomeDoModo());

  enviarJson(200, "{\"ok\":true}");
}

// Formulario "Faixa de controle"
void salvarConfig() {
  if (!server.hasArg("min") || !server.hasArg("max")) {
    responderErro(400, "informe 'min' e 'max'");
    return;
  }

  float novoMin = server.arg("min").toFloat();
  float novoMax = server.arg("max").toFloat();

  if (novoMin < 5.0 || novoMin > 38.0 || novoMax < 7.0 || novoMax > 40.0) {
    responderErro(400, "valores fora do intervalo permitido");
    return;
  }
  if (novoMax - novoMin < 2.0) {
    responderErro(400, "deixe pelo menos 2 graus entre os limites");
    return;
  }

  tempMin = novoMin;
  tempMax = novoMax;
  memoria.putFloat("min", tempMin);
  memoria.putFloat("max", tempMax);

  atualizarControle();

  Serial.print("Nova faixa: ");
  Serial.print(tempMin, 1);
  Serial.print(" a ");
  Serial.println(tempMax, 1);

  enviarJson(200, "{\"ok\":true}");
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(1000);

  // Saidas comecam desligadas
  pinMode(PINO_COOLER, OUTPUT);
  if (AQUECEDOR_INSTALADO) pinMode(PINO_AQUECEDOR, OUTPUT);
  aplicarSaidas();

  // Liga a alimentacao do sensor pelo pino D27 e espera ele iniciar
  pinMode(DHTPOWER, OUTPUT);
  digitalWrite(DHTPOWER, HIGH);
  delay(1500);

  dht.begin();

#if SONDA_SOLO_INSTALADA
  sonda.begin();
  sonda.setWaitForConversion(false);
  sonda.requestTemperatures();
#endif

  // Recupera a faixa salva
  memoria.begin("terrasmart", false);
  tempMin = memoria.getFloat("min", 22.0);
  tempMax = memoria.getFloat("max", 28.0);

  Serial.println();
  Serial.println("Iniciando TerraSmart...");
  Serial.println("Conectando ao Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, senha);

  // Aguarda ate 20 segundos pela conexao
  unsigned long inicio = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < 20000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("Wi-Fi conectado!");
    Serial.print("Abra no navegador: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("Nao foi possivel conectar ao Wi-Fi.");
    Serial.println("Confira nome e senha da rede.");
  }

  server.on("/", HTTP_GET, paginaPrincipal);
  server.on("/temperatura", HTTP_GET, consultarTemperatura);
  server.on("/status", HTTP_GET, consultarStatus);
  server.on("/atuador", HTTP_ANY, comandoAtuador);  // POST do painel ou GET (testes)
  server.on("/config", HTTP_POST, salvarConfig);
  server.onNotFound([]() {
    responderErro(404, "rota nao encontrada");
  });

  server.begin();
  Serial.println("Servidor web iniciado.");
}

// ===== LOOP =====
void loop() {
  server.handleClient();

  // Leitura periodica dos sensores + controle termico
  static unsigned long ultimaLeitura = 0;

  if (millis() - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = millis();
    lerSensores();
    atualizarControle();
  }

  // Permite tentar novamente se o Wi-Fi cair
  static unsigned long ultimaTentativa = 0;

  if (WiFi.status() != WL_CONNECTED &&
      millis() - ultimaTentativa >= 10000) {
    ultimaTentativa = millis();
    Serial.println("Wi-Fi desconectado. Reconectando...");
    WiFi.disconnect();
    WiFi.begin(ssid, senha);
  }
}

// =====================================================================
// INTERFACE WEB (TerraSmart) - fica no final para nao atrapalhar a IDE
// =====================================================================
const char PAGINA[] PROGMEM = R"=====(<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="utf-8"/>
<meta content="width=device-width,initial-scale=1" name="viewport"/>
<title>TerraSmart • Seu pequeno ecossistema</title>
<style>
:root {
    color-scheme: dark;
    --bg: #101915;
    --panel: #1a2620;
    --line: #344339;
    --text: #edf2e7;
    --muted: #a1b2a5;
    --green: #cdf88b
}

* {
    box-sizing: border-box
}

body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font: 15px system-ui, sans-serif
}

header,
main,
footer {
    max-width: 1400px;
    margin: auto;
    padding: 24px 4%
}

header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    border-bottom: 1px solid var(--line)
}

.logo {
    font-size: 25px;
    font-weight: 750;
    letter-spacing: -1px
}

.logo span {
    color: var(--green)
}

.tag {
    font-size: 12px;
    border: 1px solid var(--line);
    border-radius: 30px;
    padding: 9px 14px;
    color: var(--green)
}

h1 {
    font-size: clamp(26px, 4vw, 42px);
    letter-spacing: -1.5px;
    margin: 12px 0
}

h2 {
    font-size: 17px;
    margin: 0 0 16px
}

p,
small {
    color: var(--muted);
    line-height: 1.6
}

.intro {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin: 8px 0 26px
}

.eyebrow {
    font-size: 11px;
    letter-spacing: 2px;
    color: var(--green)
}

.grid {
    display: grid;
    grid-template-columns: 1.5fr 1fr;
    gap: 20px
}

.card {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 20px;
    padding: 23px
}

.model {
    position: relative;
    min-height: 490px;
    overflow: hidden;
    background: radial-gradient(ellipse at 50% 60%, #314333, #18241d 70%)
}

#scene {
    width: 100%;
    height: 370px;
    display: block;
    touch-action: none;
    cursor: grab
}

.model small {
    position: absolute;
    bottom: 20px;
    left: 23px
}

.model .tag {
    position: absolute;
    right: 20px;
    top: 20px
}

.metrics {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 14px
}

.value {
    font-size: 43px;
    letter-spacing: -2px;
    margin: 16px 0 5px
}

.unit {
    font-size: 18px;
    color: var(--muted)
}

.wide {
    grid-column: 1/-1
}

.status {
    display: flex;
    justify-content: space-between;
    margin: 14px 0
}

.pill {
    color: var(--green)
}

button,
select,
input {
    font: inherit;
    color: inherit;
    background: #22332a;
    border: 1px solid #4a6050;
    border-radius: 9px;
    padding: 10px
}

button {
    cursor: pointer
}

button:hover {
    border-color: var(--green)
}

button:disabled {
    opacity: .4;
    cursor: not-allowed
}

.primary {
    background: var(--green);
    color: #182218;
    font-weight: 650
}

.controls {
    display: flex;
    gap: 9px;
    flex-wrap: wrap
}

label {
    display: grid;
    gap: 7px;
    font-size: 12px;
    color: var(--muted);
    flex: 1
}

input[type=number] {
    width: 100%
}

#alert {
    border-left: 3px solid var(--green);
    padding: 10px 15px;
    margin: 20px 0;
    color: #d4dfd2;
    background: #202e24
}

#chart {
    width: 100%;
    height: 130px
}

.bottom {
    display: grid;
    grid-template-columns: 1.5fr 1fr;
    gap: 20px;
    margin-top: 20px
}

#demoBox {
    margin-top: 18px
}

input[type=range] {
    width: 100%;
    accent-color: var(--green)
}

footer {
    font-size: 12px;
    color: var(--muted)
}

#feedback {
    min-height: 24px;
    color: var(--green);
    font-size: 13px
}

@media(max-width:850px) {

    .grid,
    .bottom {
        grid-template-columns: 1fr
    }

    .intro {
        display: block
    }

    .model {
        min-height: 430px
    }

    #scene {
        height: 320px
    }

    .value {
        font-size: 34px
    }

    header {
        gap: 12px
    }

    .card {
        padding: 18px
    }
}

.model {
    background: radial-gradient(ellipse at 48% 42%, #f9f7df 0%, #e7eddd 48%, #d4e3d8 100%);
    color: #29463a;
    min-height: 560px;
    padding: 24px;
    display: flex;
    flex-direction: column
}

.model h2 {
    font-size: 20px;
    letter-spacing: -.5px;
    padding-right: 130px
}

.model .tag {
    color: #416652;
    border-color: #b4cbb8;
    background: #ffffff66;
    font-size: 10px
}

.model #scene {
    height: 365px;
    flex: 1;
    min-height: 300px
}

.scene-tools {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 8px;
    font-size: 12px;
    color: #55705d;
    margin: 0 0 16px
}

.scene-tools button {
    background: #ffffff60;
    border-color: #b8c9b6;
    color: #365441;
    font-size: 12px
}

.pet-info {
    background: #ffffffb5;
    border: 1px solid #ffffffd0;
    border-radius: 14px;
    display: flex;
    gap: 12px;
    padding: 16px;
    box-shadow: 0 6px 18px #5470550a
}

.pet-info p {
    color: #617261;
    font-size: 12px;
    margin: 5px 0 0;
    line-height: 1.5
}

.pet-info strong {
    font-size: 16px
}

.pet-info #petDot {
    width: 10px;
    height: 10px;
    border-radius: 100%;
    margin-top: 5px;
    flex-shrink: 0;
    background: #639e52
}

.scene-caption {
    font-size: 10px;
    color: #657969;
    margin-top: 12px;
    line-height: 1.5
}

@media(max-width:600px) {
    .model {
        padding: 16px;
        min-height: 530px
    }

    .model h2 {
        font-size: 17px;
        padding-right: 95px
    }

    .model .tag {
        right: 14px;
        top: 16px;
        padding: 7px
    }

    .model #scene {
        height: 300px;
        min-height: 280px
    }
}

.model {
    background: #ebe6ce
}

.model #scene {
    width: 100%;
    height: auto;
    min-height: 0;
    flex: none;
    aspect-ratio: 3/2;
    image-rendering: pixelated;
    image-rendering: crisp-edges;
    cursor: default;
    touch-action: auto;
    border-radius: 8px;
    margin: 8px 0 16px;
    background: #e9e4c8
}

.model {
    justify-content: space-between
}

.scene-tools {
    flex-wrap: wrap
}

.scene-caption {
    font-size: 10px
}
</style>
</head>
<body>
<header>
  <div class="logo">◈ Terra<span>Smart</span></div>
  <span class="tag" id="connection">Conectando…</span>
</header>
<main>
  <div class="intro">
    <div>
      <div class="eyebrow">UM PEQUENO MUNDO, EM EQUILÍBRIO</div>
      <h1>Seu terrário, em tempo real.</h1>
      <p>Observe o ambiente. Acompanhe cada mudança.</p>
    </div>
    <button id="demoToggle">Iniciar demonstração</button>
  </div>
  <div class="grid">
    <section class="card model">
      <h2>Seu jardim em pixels</h2>
      <span class="tag">PIXEL ART VIVA</span>
      <canvas aria-label="Jardim animado em pixel art: tartaruga, plantas, toca, lago e sensores reagem às leituras" id="scene"></canvas>
      <div class="scene-tools">
        <span>Um jardim que sente a temperatura</span>
        <button aria-pressed="false" id="pauseAnimation">Ⅱ Pausar animação</button>
      </div>
      <div class="pet-info">
        <span id="petDot"></span>
        <div>
          <strong id="petTitle">Aguardando temperatura</strong>
          <p id="petDescription">O mascote reage às leituras do sensor.</p>
        </div>
      </div>
      <div class="scene-caption">Mascote ilustrativo · recipiente 25 × 12 × 17 cm · comportamento simbólico</div>
    </section>
    <div class="metrics">
      <section class="card">
        <small>Temperatura do solo</small>
        <div class="value"><span id="soil">—</span> <span class="unit">°C</span></div>
        <small id="soilNote">Sonda opcional DS18B20</small>
      </section>
      <section class="card">
        <small>Temperatura do ar</small>
        <div class="value"><span id="air">—</span> <span class="unit">°C</span></div>
        <small>DHT11 · interior do terrário</small>
      </section>
      <section class="card">
        <small>Umidade do ar</small>
        <div class="value"><span id="humidity">—</span> <span class="unit">%</span></div>
        <small>Não representa a umidade do solo</small>
      </section>
      <section class="card">
        <small>Controle térmico</small>
        <div class="status">Cooler <b class="pill" id="cool">—</b></div>
        <div class="status">Aquecedor <b class="pill" id="heat">—</b></div>
        <small id="source">Aguardando ESP32</small>
      </section>
      <section class="card wide">
        <h2>Operação</h2>
        <div class="controls">
          <button class="primary command" data-mode="auto">Automático</button>
          <button class="command" data-mode="off">Desligar</button>
          <button class="command" data-mode="cool">Ventilar por 60 s</button>
          <button class="command" data-mode="heat" id="heatButton">Aquecer por 60 s</button>
        </div>
        <p id="mode">Modo: —</p>
        <div aria-live="polite" id="feedback" role="status"></div>
      </section>
    </div>
  </div>
  <div id="alert" role="status">Aguardando leitura. A demonstração usa dados fictícios.</div>
  <div class="bottom">
    <section class="card">
      <h2>Histórico desta sessão <small>· temperatura de controle</small></h2>
      <canvas aria-label="Gráfico das últimas 60 leituras" id="chart"></canvas>
      <small id="historyNote">As leituras aparecem conforme chegam. Histórico mantido apenas nesta aba.</small>
    </section>
    <section class="card">
      <h2>Faixa de controle</h2>
      <form id="settings">
        <div class="controls">
          <label>Mínima (°C)
            <input id="min" max="38" min="5" required="" step="0.5" type="number" value="22"/>
          </label>
          <label>Máxima (°C)
            <input id="max" max="40" min="7" required="" step="0.5" type="number" value="28"/>
          </label>
          <button class="primary" type="submit">Aplicar</button>
        </div>
      </form>
      <small>Histerese: 1 °C. Valores iniciais demonstrativos; ajuste à espécie.</small>
      <div hidden="" id="demoBox">
        <label>Simular temperatura: <span id="simVal">25 °C</span>
          <input id="sim" max="38" min="10" step="0.5" type="range" value="25"/>
        </label>
      </div>
    </section>
  </div>
</main>
<footer>TerraSmart / Projeto semestral · ESP32 + sensores · Interface local, sem dependências externas.</footer>
<script>
// 1. INICIALIZAÇÃO DO PAINEL
'use strict';
const $ = id => document.getElementById(id);
let demo = location.protocol === 'file:',
    online = false,
    latest = null,
    history = [],
    angle = -.28,
    tilt = .48,
    drag = null,
    busy = false,
    epoch = 0,
    limitsLoaded = false;
let sim = {
    min: 22,
    max: 28,
    mode: 'auto',
    cool: false,
    heat: false,
    until: 0
};

// 2. SIMULAÇÃO: usada apenas no modo demonstração.
function demoData() {
    let t = +$('sim').value;
    if (sim.until && Date.now() > sim.until) {
        sim.mode = 'off';
        sim.until = 0
    }
    if (sim.mode === 'auto') {
        if (sim.cool && t <= sim.max - 1) sim.cool = false;
        if (sim.heat && t >= sim.min + 1) sim.heat = false;
        if (t >= sim.max) {
            sim.cool = true;
            sim.heat = false
        } else if (t <= sim.min) {
            sim.heat = true;
            sim.cool = false
        }
    } else {
        sim.cool = sim.mode === 'cool';
        sim.heat = sim.mode === 'heat'
    }
    return {
        air: t + .8,
        soil: t,
        humidity: 68,
        temperature: t,
        valid: true,
        soilEnabled: true,
        heatEnabled: true,
        source: 'solo',
        ...sim
    }
}
const fmt = v => Number.isFinite(v) ? v.toFixed(1).replace('.', ',') : '—';

// 3. ATUALIZAÇÃO: preenche os indicadores com o JSON recebido.
function render(d) {
    latest = d;
    // Na primeira leitura real, mostra no formulário a faixa salva no ESP32.
    if (!demo && !limitsLoaded && Number.isFinite(d.min) && Number.isFinite(d.max)) {
        $('min').value = d.min;
        $('max').value = d.max;
        limitsLoaded = true
    }
    $('connection').textContent = demo ? '● DEMONSTRAÇÃO · dados fictícios' : '● ESP32 conectado';
    $('soil').textContent = fmt(d.soil);
    $('air').textContent = fmt(d.air);
    $('humidity').textContent = fmt(d.humidity);
    $('soilNote').textContent = d.soilEnabled ? 'DS18B20 · sonda no solo' : 'Sonda não instalada';
    $('cool').textContent = d.cool ? 'Ligado' : 'Desligado';
    $('heat').textContent = d.heatEnabled ? (d.heat ? 'Ligado' : 'Desligado') : 'Não instalado';
    $('source').textContent = 'Controle pelo ' + d.source;
    $('mode').textContent = 'Modo: ' + ({
        auto: 'automático',
        off: 'desligado',
        cool: 'ventilação temporária',
        heat: 'aquecimento temporário'
    } [d.mode] || d.mode);
    $('alert').textContent = !d.valid ? 'Falha no sensor de controle. Saídas desligadas.' : d
        .temperature > d.max ? 'Temperatura acima da faixa.' : d.temperature < d.min ?
        'Temperatura abaixo da faixa.' : 'Temperatura dentro da faixa configurada.';
    document.querySelectorAll('.command').forEach(b => b.disabled = !d.valid && b.dataset.mode !==
        'off');
    $('heatButton').disabled = !d.heatEnabled || !d.valid;
    history.push(d.valid ? d.temperature : null);
    history = history.slice(-60);
    drawChart()
}

// 4. COMUNICAÇÃO COM O ESP32: rotas relativas ao endereço da página.
async function api(path, body) {
    let r = await fetch(path, {
        method: body ? 'POST' : 'GET',
        headers: body ? {
            'Content-Type': 'application/x-www-form-urlencoded'
        } : {},
        body: body ? new URLSearchParams(body) : undefined,
        signal: AbortSignal.timeout(4500),
        cache: 'no-store'
    });
    if (!r.ok) throw Error(await r.text());
    return r.json()
}

// Busca leituras atuais; em falha, limpa valores e indica desconexão.
async function poll() {
    const e = epoch;
    try {
        const d = demo ? demoData() : await api('/temperatura');
        if (e !== epoch) return;
        online = true;
        render(d)
    } catch (err) {
        if (e !== epoch) return;
        online = false;
        latest = null;
        $('connection').textContent = '● Sem conexão com ESP32';
        ['soil', 'air', 'humidity'].forEach(id => $(id).textContent = '—');
        $('cool').textContent = $('heat').textContent = 'Desconhecido';
        $('alert').textContent =
            'Sem dados atuais. Abra este painel no endereço do ESP32 ou inicie a demonstração.';
        document.querySelectorAll('.command').forEach(b => b.disabled = true)
    }
}

// Envia comandos e aguarda confirmação do ESP32.
async function command(body, path = '/atuador') {
    if (busy) return;
    busy = true;
    const e = epoch;
    try {
        if (demo) {
            if (body.mode) {
                sim.mode = body.mode;
                sim.until = ['cool', 'heat'].includes(body.mode) ? Date.now() + 60000 : 0
            } else {
                sim.min = +body.min;
                sim.max = +body.max
            }
            render(demoData())
        } else {
            await api(path, body);
            if (e !== epoch) return;
            await poll()
        }
        $('feedback').textContent = demo ? 'Aplicado na simulação.' :
            'Comando confirmado pelo ESP32.'
    } catch (err) {
        $('feedback').textContent = 'Não foi possível aplicar: ' + err.message
    } finally {
        busy = false
    }
}

// 5. BOTÕES E CONFIGURAÇÕES DO PAINEL
document.querySelectorAll('.command').forEach(b => b.onclick = () => command({
    mode: b.dataset.mode
}));
$('settings').onsubmit = e => {
    e.preventDefault();
    let min = +$('min').value,
        max = +$('max').value;
    if (max - min < 2) {
        $('feedback').textContent = 'Deixe pelo menos 2 °C entre os limites.';
        return
    }
    command({
        min,
        max
    }, '/config')
};

function demoUI() {
    $('demoBox').hidden = !demo;
    $('demoToggle').textContent = demo ? 'Conectar ao ESP32' : 'Iniciar demonstração'
}
$('demoToggle').onclick = () => {
    demo = !demo;
    epoch++;
    history = [];
    limitsLoaded = false;
    demoUI();
    poll()
};
$('sim').oninput = () => {
    $('simVal').textContent = $('sim').value + ' °C';
    if (demo) render(demoData())
};

// 6. GRÁFICO DO HISTÓRICO DA ABA
function setupCanvas(c) {
    let r = c.getBoundingClientRect(),
        d = devicePixelRatio || 1;
    c.width = r.width * d;
    c.height = r.height * d;
    let x = c.getContext('2d');
    x.scale(d, d);
    return [x, r.width, r.height]
}

function drawChart() {
    let [c, w, h] = setupCanvas($('chart'));
    let values = history.filter(Number.isFinite),
        lo = Math.min(18, ...values) - 1,
        hi = Math.max(32, ...values) + 1;
    let y = v => h - 20 - (v - lo) / (hi - lo) * (h - 35);
    c.font = '11px system-ui';
    for (let t = Math.ceil(lo / 5) * 5; t < hi; t += 5) {
        c.strokeStyle = '#344339';
        c.beginPath();
        c.moveTo(35, y(t));
        c.lineTo(w, y(t));
        c.stroke();
        c.fillStyle = '#a1b2a5';
        c.fillText(t + '°', 0, y(t) + 4)
    }
    c.strokeStyle = '#cdf88b';
    c.lineWidth = 2;
    c.beginPath();
    let gap = true;
    history.forEach((v, i) => {
        if (v === null) {
            gap = true;
            return
        }
        let x = 35 + i * (w - 45) / 59;
        gap ? c.moveTo(x, y(v)) : c.lineTo(x, y(v));
        gap = false
    });
    c.stroke();
    if (values.length === 1) {
        c.beginPath();
        c.arc(35, y(values[0]), 3, 0, 7);
        c.fillStyle = '#cdf88b';
        c.fill()
    }
}
// 7. CENA PIXEL ART
// A arte é desenhada pelo navegador; o ESP32 apenas entrega o arquivo.
// Original procedural pixel art. Fixed 384 × 256 raster, integer coordinates, no assets/CDN.
const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
const pixelCanvas = $('scene');
pixelCanvas.width = 384;
pixelCanvas.height = 256;
const px = pixelCanvas.getContext('2d');
px.imageSmoothingEnabled = false;
let petX = 222,
    lastFrame = 0,
    petState = 'waiting',
    animationPaused = false;

// Classifica a reação usando os mesmos limites do controle térmico.
function thermalState(d) {
    if (!d || !d.valid || !Number.isFinite(d.temperature)) return 'waiting';
    return d.temperature > d.max ? 'hot' : d.temperature < d.min ? 'cold' : 'happy'
}

function rect(x, y, w, h, color) {
    px.fillStyle = color;
    px.fillRect(Math.round(x), Math.round(y), w, h)
}

function oval(x, y, rx, ry, color) {
    for (let j = -ry; j <= ry; j++) {
        let width = Math.round(rx * Math.sqrt(Math.max(0, 1 - j * j / (ry * ry))));
        rect(x - width, y + j, width * 2 + 1, 1, color)
    }
}

function sprite(rows, x, y, palette, scale = 1, flip = false) {
    rows.forEach((row, j) => [...row].forEach((k, i) => {
        if (palette[k]) rect(x + (flip ? row.length - i - 1 : i) * scale, y + j * scale,
            scale, scale, palette[k])
    }))
}

function cloud(x, y) {
    rect(x + 8, y, 27, 3, '#fbf7dc');
    rect(x + 3, y + 3, 43, 6, '#fbf7dc');
    rect(x, y + 9, 52, 4, '#e9edd5');
    rect(x + 6, y + 13, 35, 2, '#d1e1ce')
}

function fern(x, y, height, t) {
    rect(x, y - height, 2, height, '#375f43');
    for (let i = 0; i < 6; i++) {
        let yy = y - i * height / 7 - 5,
            len = 15 - i * 2,
            sway = (animationPaused || reducedMotion) ? 0 : Math.round(Math.sin(t + i * .4));
        rect(x - len + sway, yy - 3, len, 3, i % 2 ? '#72a84d' : '#568749');
        rect(x + 2, yy - 6, len, 3, '#7dab51');
        rect(x - len + sway, yy - 5, 4, 2, '#a0bf65');
        rect(x + len, yy - 8, 4, 2, '#b3ce74')
    }
}

function mushroom(x, y, size) {
    rect(x - 1, y - 7 * size, 3 * size, 7 * size, '#efdfad');
    oval(x, y - 8 * size, 6 * size, 3 * size, '#b65b4c');
    rect(x - 3 * size, y - 10 * size, 3 * size, size, '#ffcb98');
    rect(x + 2 * size, y - 8 * size, 2 * size, size, '#f6d6a6')
}
const shellPalette = {
    o: '#304b3b',
    g: '#5f8441',
    l: '#9cbb58',
    h: '#d1d580',
    d: '#45653b',
    s: '#d8c889',
    b: '#8e914c',
    e: '#233c35',
    w: '#fff4d4',
    p: '#d59276'
};
const turtle = [
    '       oooooooo                ',
    '     oollhlllhloo              ',
    '    ollggdllgggllo             ',
    '   olggggdllggggglo            ',
    '  olgdddggdddggdddlo    ooooo   ',
    '  olggllgdggllgdgglo  oossssoo  ',
    ' olgglllgdgglllgdgglo osssswsso ',
    ' olgdddggdddggdddgglo osssseeso ',
    ' olggllgdggllgdgggglo osssssssso',
    '  odddddddddddddddoooosssssssso',
    '   obbbbbbbbbbbbo ssssssppssso ',
    '    osssssssssssssssssssssso   ',
    '     oossooooooooossoooooo     ',
    '     ossso      ossso          ',
    '     oooo       oooo           '
];

// Desenha o mascote, sua expressão e seus movimentos.
function turtleDraw(x, y, state, t, moving) {
    let hot = state === 'hot',
        cold = state === 'cold',
        bob = !reducedMotion && !animationPaused && state === 'happy' ? Math.round(Math.sin(t *
        2)) : 0;
    let pal = {
        ...shellPalette
    };
    if (hot) {
        pal.l = '#baa057';
        pal.h = '#e1c678';
        pal.g = '#91874b'
    }
    sprite(turtle, x, y + bob, pal, 2, cold);
    if (hot) {
        rect(x + 50, y + 13, 6, 2, '#344a36');
        rect(x + 50, y + 15, 6, 2, '#d8c889');
        rect(x + 54, y + 23, 3, 3, '#914f49');
        let fall = reducedMotion || animationPaused ? 2 : Math.floor(t * 6) % 9;
        sprite([' b ', 'bwb', 'bbb', ' b '], x + 61, y + 8 + fall, {
            b: '#6fbfd5',
            w: '#d5f0e1'
        }, 1)
    }
    if (moving && !reducedMotion && !animationPaused) {
        let step = Math.floor(t * 7) % 2;
        rect(x + 10, y + 28, 8, 2, step ? '#d8c889' : '#304b3b');
        rect(x + 33, y + 28, 8, 2, step ? '#304b3b' : '#d8c889')
    }
    if (cold && Math.abs(x - 70) < 3) {
        sprite(['gggg', '   g', '  g ', ' g  ', 'gggg'], x + 31, y - 10, {
            g: '#d9e5d0'
        });
        sprite(['ggg', '  g', ' g ', 'ggg'], x + 44, y - 18, {
            g: '#aac8c8'
        })
    }
}

// Desenha o cenário e move o mascote para a toca quando esfria.
function pixelScene(ms = 0) {
    requestAnimationFrame(pixelScene);
    if (ms - lastFrame < 65 && ms !== 0) return;
    let dt = Math.min(.15, (ms - lastFrame) / 1000 || .065);
    lastFrame = ms;
    let t = (reducedMotion || animationPaused) ? 0 : ms / 1000;
    petState = thermalState(latest);
    let hot = petState === 'hot',
        cold = petState === 'cold',
        target = cold ? 70 : 216;
    let moving = Math.abs(target - petX) > 1;
    if (reducedMotion) petX = target;
    else if (!animationPaused) petX += (target - petX) * Math.min(1, dt * 1.15);
    // Sky and softly banded distant landscape.
    rect(0, 0, 384, 256, '#e9e4c8');
    rect(0, 0, 384, 174, cold ? '#b3ced3' : hot ? '#f0d6a5' : '#ccddbe');
    rect(0, 50, 384, 70, cold ? '#c6dbd8' : hot ? '#f1dfb7' : '#dae5c9');
    rect(0, 120, 384, 53, '#bdcbaa');
    oval(306, 45, 18, 18, hot ? '#efa567' : '#eee8aa');
    oval(306, 45, 13, 13, hot ? '#f8cb7e' : '#f9f0c2');
    cloud(48 + Math.sin(t * .06) * 4, 38);
    cloud(182 + Math.sin(t * .05) * 5, 62);
    for (let i = 0; i < 16; i++) {
        let x = i * 27 - 12,
            h = 18 + (i * 13) % 30;
        rect(x, 156 - h, 30, h, '#a9bea0');
        rect(x + 5, 150 - h, 20, 7, '#a9bea0')
    }
    // Aquarium structure, back panes and reflections.
    rect(17, 23, 350, 164, '#71958a');
    rect(21, 27, 342, 155, cold ? '#c5d8ce55' : '#d1dfba55');
    for (let i = 0; i < 12; i++) {
        let x = 22 + i * 30,
            h = 14 + i * 7 % 28;
        rect(x, 145 - h, 31, h + 23, '#afc594');
        rect(x + 7, 139 - h, 17, 6, '#afc594')
    }
    rect(24, 30, 3, 139, '#edf1d2');
    rect(28, 31, 1, 120, '#f5f1d4');
    rect(353, 31, 3, 141, '#edf1d2');
    rect(31, 34, 150, 2, '#edf1d280');
    // Hanging vines and distant foliage.
    for (let i = 0; i < 5; i++) {
        let x = 38 + i * 64,
            len = 14 + (i * 11) % 32;
        rect(x, 29, 1, len, '#5d8855');
        for (let j = 0; j < 4; j++) {
            rect(x + (j % 2 ? 1 : -4), 34 + j * len / 5, 4, 5, j % 2 ? '#7fa15c' : '#91b568')
        }
    }
    fern(43, 163, 57, t);
    fern(155, 163, 37, t);
    fern(321, 162, 65, t);
    fern(343, 168, 34, t);
    fern(187, 164, 25, t);
    // Deep substrate with roots, pebbles and small fossil-like flecks.
    rect(21, 170, 342, 39, '#644e3c');
    rect(21, 176, 342, 8, '#8a6748');
    rect(21, 184, 342, 17, '#76553e');
    rect(21, 201, 342, 8, '#aa9270');
    for (let i = 0; i < 85; i++) {
        let x = 24 + (i * 73) % 335,
            y = 181 + (i * 17) % 25;
        rect(x, y, i % 3 + 1, 2, ['#c4ab7b', '#4d4537', '#997958'][i % 3])
    }
    for (let i = 0; i < 9; i++) {
        let x = 39 + i * 37;
        rect(x, 173, 1, 12, '#c3a776');
        rect(x + 1, 184, 4, 1, '#c3a776');
        rect(x - 3, 180, 3, 1, '#c3a776')
    }
    rect(21, 165, 342, 6, '#739552');
    rect(21, 171, 342, 4, '#4c713f');
    for (let i = 0; i < 64; i++) {
        let x = 23 + i * 5;
        rect(x, 164 - i % 3, 3, 4, i % 2 ? '#abc46d' : '#8bac58')
    }
    // Stone footpath and pond.
    for (let i = 0; i < 6; i++) oval(121 + i * 18, 170 + (i % 2) * 2, 8, 2, '#b8b18b');
    oval(289, 173, 26, 7, '#526e52');
    oval(289, 171, 24, 6, '#b9b88c');
    oval(289, 169, 19, 4, '#67aeb0');
    rect(277, 167, 12, 1, '#c4e7ce');
    rect(290 + (Math.floor(t) % 2), 171, 9, 1, '#96d2c6');
    // Cork house: layered pixel arch, open dark interior, moss roof.
    rect(60, 123, 66, 49, '#715840');
    rect(65, 113, 55, 12, '#715840');
    rect(73, 107, 40, 7, '#715840');
    rect(80, 104, 25, 4, '#715840');
    rect(64, 125, 57, 44, '#c09261');
    rect(69, 116, 47, 12, '#bf8c58');
    rect(78, 110, 29, 8, '#ddb27a');
    rect(73, 134, 38, 38, '#3f4633');
    rect(78, 125, 28, 10, '#3f4633');
    rect(85, 121, 14, 5, '#3f4633');
    rect(77, 151, 30, 20, '#535338');
    for (let i = 0; i < 13; i++) rect(63 + i * 5, 119 - ((i > 2 && i < 10) ? 10 : 0), 4, 3, i % 2 ?
        '#709448' : '#a2b76a');
    rect(66, 134, 3, 18, '#e2b87c');
    rect(116, 133, 3, 27, '#90633e');
    rect(64, 160, 6, 2, '#90633e');
    rect(71, 118, 6, 2, '#e7c493');
    // Sensor, cable, wall fan with responsive blades.
    rect(344, 71, 2, 88, '#6c8170');
    rect(337, 77, 15, 23, '#355d5b');
    rect(339, 79, 11, 18, '#75b6b0');
    for (let j = 0; j < 4; j++) rect(341, 82 + j * 3, 7, 1, '#436e68');
    rect(343, 102, 2, 48, '#bbcdb1');
    rect(342, 150, 4, 12, '#cbd1ad');
    rect(269, 44, 31, 31, '#537065');
    rect(272, 47, 25, 25, '#c5d1b0');
    rect(275, 50, 19, 19, '#354f48');
    let turn = latest?.cool && Math.floor(t * 8) % 2;
    if (turn) {
        rect(278, 52, 4, 15, '#98b9a3');
        rect(276, 57, 16, 4, '#98b9a3')
    } else {
        for (let i = 0; i < 5; i++) {
            rect(277 + i * 2, 52 + i * 2, 3, 3, '#98b9a3');
            rect(289 - i * 2, 52 + i * 2, 3, 3, '#98b9a3')
        }
    }
    rect(282, 57, 5, 5, '#e6dec0');
    rect(273, 45, 2, 2, latest?.cool ? '#c5ef8e' : '#93a68b');
    if (latest?.cool)
        for (let i = 0; i < 3; i++) {
            let yy = 82 + (t * 14 + i * 11) % 40;
            rect(277 + i * 5, yy, 1, 5, '#eff7cf')
        }
    if (latest?.heat) {
        rect(168, 169, 33, 4, '#d89054');
        for (let i = 0; i < 3; i++) {
            let yy = 165 - (t * 8 + i * 5) % 18;
            rect(173 + i * 9, yy, 2, 4, '#e8bc7899')
        }
    }
    // Ground shadow and main animal; cold path ends in doorway.
    oval(petX + 29, 174, 30, 3, '#48633f');
    turtleDraw(Math.round(petX), 143 + (hot ? 3 : 0), petState, t, moving);
    // Front door jambs occlude the animal once it has entered.
    if (cold && petX < 111) {
        rect(60, 137, 13, 35, '#ad7f50');
        rect(112, 132, 14, 40, '#ac7c4f');
        rect(63, 139, 3, 24, '#d6ad72');
        rect(117, 140, 3, 22, '#805b3e')
    }
    mushroom(50, 171, 1);
    mushroom(139, 172, 1);
    mushroom(332, 173, 2);
    for (let i = 0; i < 12; i++) {
        let x = 29 + i * 29;
        rect(x, 173, 2, 5, '#517e43');
        rect(x - 2, 170, 2, 4, '#749b50');
        rect(x + 2, 171, 2, 3, '#a2bc60')
    }
    // Ambient fireflies; symbolic heat/cold indicators.
    if (petState === 'happy')
        for (let i = 0; i < 5; i++) {
            let x = 149 + (i * 37) % 156 + Math.round(Math.sin(t * .6 + i) * 3),
                y = 95 + (i * 17) % 46 + Math.round(Math.cos(t + i) * 2);
            rect(x, y, 2, 2, '#f7efba')
        }
    if (hot)
        for (let i = 0; i < 3; i++) {
            let y = 130 - Math.floor(t * 5 + i * 7) % 22;
            rect(229 + i * 12, y, 1, 4, '#dca271');
            rect(230 + i * 12, y - 3, 1, 3, '#dca271')
        }
    // Top and lower brass-green frame.
    rect(14, 20, 356, 5, '#496b5b');
    rect(14, 25, 356, 2, '#9caf86');
    rect(14, 209, 356, 7, '#496b5b');
    rect(18, 216, 348, 3, '#314f44');
    rect(29, 219, 27, 5, '#688473');
    rect(330, 219, 27, 5, '#688473');
    px.font = '8px monospace';
    px.fillStyle = '#48624f';
    px.fillText('T E R R A S M A R T', 139, 239);
    const messages = {
        waiting: ['Aguardando temperatura', 'Sem leitura válida: o mascote está em espera.'],
        happy: ['Em equilíbrio', 'Explorando o jardim, respirando com calma.'],
        hot: ['Ufa… está quente!', 'Descanso, olhos cansados e gotinhas de calor.'],
        cold: ['Melhor ficar na toca', 'O abrigo virou o cantinho favorito do mascote.']
    };
    $('petTitle').textContent = messages[petState][0];
    $('petDescription').textContent = messages[petState][1];
    $('petDot').style.background = {
        waiting: '#98a39b',
        happy: '#639e52',
        hot: '#d98348',
        cold: '#679ac7'
    } [petState];
}
$('pauseAnimation').onclick = () => {
    animationPaused = !animationPaused;
    $('pauseAnimation').textContent = animationPaused ? '▶ Animar cena' : 'Ⅱ Pausar animação';
    $('pauseAnimation').setAttribute('aria-pressed', String(animationPaused))
};
demoUI();
poll();
setInterval(poll, 5000);
pixelScene();
window.addEventListener('resize', drawChart);
</script>
</body>
</html>
)=====";
