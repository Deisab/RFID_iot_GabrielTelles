/*
 * Leitor RFID com MQTT - IoT (AMF)
 * ESP32 + MFRC522 + LED no GPIO 2 (simulador Wokwi)
 *
 * Tópicos (prefixo sis1a/SEU-NOME/rfid/):
 *   mac      -> ESP32 publica (retain) ao conectar
 *   cadastro -> ESP32 publica ao cadastrar/remover
 *   acesso   -> ESP32 publica a cada cartão no modo normal
 *   lista    -> ESP32 publica ao receber LISTAR
 *   cmd      -> central envia: CADASTRO | REMOVER <uid> | REMOVER | LISTAR
 *               (REMOVER sem UID = remove o próximo cartão passado no leitor)
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

// ======== TROQUE PELO SEU NOME (sem espaços/acentos) ========
#define SEU_NOME "GabrielTelles"
// ============================================================

// ---------- Configurações ----------
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_PASS   = "";
const char* MQTT_BROKER = "test.mosquitto.org";
const int   MQTT_PORTA  = 1883;

const String PREFIXO     = String("sis1a/") + SEU_NOME + "/rfid/";
const String TOPICO_MAC  = PREFIXO + "mac";
const String TOPICO_CAD  = PREFIXO + "cadastro";
const String TOPICO_ACE  = PREFIXO + "acesso";
const String TOPICO_LIS  = PREFIXO + "lista";
const String TOPICO_CMD  = PREFIXO + "cmd";

// ---------- Pinos ----------
#define PINO_SS   5
#define PINO_RST  22
#define PINO_LED  2

// ---------- Tempos ----------
const unsigned long TEMPO_MODO       = 15000; // modo cadastro/remoção: 15 s
const unsigned long TEMPO_REPETICAO  = 2000;  // ignora mesmo cartão < 2 s
const unsigned long TEMPO_LIBERADO   = 2000;  // LED aceso 2 s
const unsigned long PISCA_MODO       = 250;   // pisca lento no cadastro/remoção
const unsigned long PISCA_NEGADO     = 100;   // pisca rápido no negado
const unsigned long INTERVALO_RECON  = 3000;  // tentativa de reconexão

#define MAX_CARTOES 10

// ---------- Objetos ----------
MFRC522      rfid(PINO_SS, PINO_RST);
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- Estado ----------
String meuMac;
String cartoes[MAX_CARTOES];
int    qtdCartoes = 0;

// NORMAL: libera/nega | CADASTRO: próximo cartão entra na lista | REMOCAO: próximo cartão sai
enum Modo { MODO_NORMAL, MODO_CADASTRO, MODO_REMOCAO };
Modo          modo = MODO_NORMAL;
unsigned long inicioModo = 0;

String        ultimoUid = "";
unsigned long ultimaLeitura = 0;

// LED (controle sem delay, para não travar o MQTT)
unsigned long ledFixoAte = 0;
int           trocasRestantes = 0;   // cada piscada = 2 trocas
unsigned long proximaTroca = 0;
bool          ledLigado = false;

unsigned long ultimaTentativaWifi = 0;
unsigned long ultimaTentativaMqtt = 0;
bool          mqttEstavaConectado = false;

// =================================================================
// Utilidades
// =================================================================
String horaAtual() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return "--:--:--";
  char buf[9];
  strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  return String(buf);
}

// Formata o UID em hexadecimal separado por espaços (4 ou 7 bytes)
String uidParaTexto(const MFRC522::Uid& uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    if (i > 0) s += " ";
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Deixa o UID recebido por comando no mesmo formato (maiúsculo, 1 espaço)
String normalizaUid(String uid) {
  uid.trim();
  uid.toUpperCase();
  while (uid.indexOf("  ") >= 0) uid.replace("  ", " ");
  return uid;
}

void publicar(const String& topico, const String& json, bool retido = false) {
  bool ok = mqtt.publish(topico.c_str(), json.c_str(), retido);
  Serial.printf("[MQTT] %s %s%s -> %s\n",
                ok ? "Publicado em" : "FALHA ao publicar em",
                topico.c_str(), retido ? " (retain)" : "", json.c_str());
}

// =================================================================
// Lista de cartões
// =================================================================
int buscaCartao(const String& uid) {
  for (int i = 0; i < qtdCartoes; i++) {
    if (cartoes[i] == uid) return i;
  }
  return -1;
}

bool removeCartao(const String& uid) {
  int pos = buscaCartao(uid);
  if (pos < 0) return false;
  for (int i = pos; i < qtdCartoes - 1; i++) cartoes[i] = cartoes[i + 1];
  qtdCartoes--;
  cartoes[qtdCartoes] = "";
  return true;
}

// =================================================================
// LED
// =================================================================
void setLed(bool ligado) {
  ledLigado = ligado;
  digitalWrite(PINO_LED, ligado ? HIGH : LOW);
}

void ledLiberado() {
  trocasRestantes = 0;
  ledFixoAte = millis() + TEMPO_LIBERADO;
  setLed(true);
}

void ledNegado() {
  ledFixoAte = 0;
  setLed(false);
  trocasRestantes = 6;          // 3 piscadas
  proximaTroca = millis();
}

void atualizaLed() {
  unsigned long agora = millis();

  if (modo != MODO_NORMAL) {
    if (agora >= proximaTroca) {
      setLed(!ledLigado);
      proximaTroca = agora + PISCA_MODO;
    }
    return;
  }

  if (trocasRestantes > 0) {
    if (agora >= proximaTroca) {
      setLed(!ledLigado);
      trocasRestantes--;
      proximaTroca = agora + PISCA_NEGADO;
    }
    return;
  }

  setLed(agora < ledFixoAte);
}

// =================================================================
// Mensagens
// =================================================================
void publicaMac() {
  String json = "{\"mac\":\"" + meuMac + "\",\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"hora\":\"" + horaAtual() + "\"}";
  publicar(TOPICO_MAC, json, true);
}

void publicaCadastro(const String& uid, const String& status) {
  String json = "{\"mac\":\"" + meuMac + "\",\"uid\":\"" + uid + "\",\"status\":\"" +
                status + "\",\"hora\":\"" + horaAtual() + "\"}";
  publicar(TOPICO_CAD, json);
}

void publicaAcesso(const String& uid, const String& resultado) {
  String json = "{\"mac\":\"" + meuMac + "\",\"uid\":\"" + uid + "\",\"resultado\":\"" +
                resultado + "\",\"hora\":\"" + horaAtual() + "\"}";
  publicar(TOPICO_ACE, json);
}

void publicaLista() {
  String json = "{\"mac\":\"" + meuMac + "\",\"tags\":[";
  for (int i = 0; i < qtdCartoes; i++) {
    if (i > 0) json += ",";
    json += "\"" + cartoes[i] + "\"";
  }
  json += "]}";
  publicar(TOPICO_LIS, json);
}

// =================================================================
// Modos cadastro / remoção
// =================================================================
void entraModo(Modo novo) {
  modo = novo;
  inicioModo = millis();
  proximaTroca = millis();
  trocasRestantes = 0;
  if (novo == MODO_CADASTRO) Serial.println("[MODO] CADASTRO - passe o cartao em ate 15 s");
  else                       Serial.println("[MODO] REMOCAO - passe o cartao a remover em ate 15 s");
}

void voltaModoNormal() {
  modo = MODO_NORMAL;
  setLed(false);
  Serial.println("[MODO] NORMAL (acesso)");
}

void verificaTempoModo() {
  if (modo != MODO_NORMAL && millis() - inicioModo >= TEMPO_MODO) {
    Serial.println("[CADASTRO] Tempo esgotado, nenhum cartao lido");
    publicaCadastro("", "tempo_esgotado");
    voltaModoNormal();
  }
}

void removeEPublica(const String& uid) {
  bool ok = removeCartao(uid);
  Serial.printf("[CADASTRO] %s: %s\n", ok ? "Removida" : "Nao encontrada", uid.c_str());
  publicaCadastro(uid, ok ? "removida" : "nao_encontrada");
}

// =================================================================
// Comandos MQTT (callback)
// =================================================================
void aoReceberMensagem(char* topico, byte* payload, unsigned int tamanho) {
  String msg;
  for (unsigned int i = 0; i < tamanho; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("[CMD] Recebido em %s: \"%s\"\n", topico, msg.c_str());

  String cmd = msg;
  String arg = "";
  int espaco = msg.indexOf(' ');
  if (espaco > 0) {
    cmd = msg.substring(0, espaco);
    arg = msg.substring(espaco + 1);
  }
  cmd.toUpperCase();

  if (cmd == "CADASTRO") {
    entraModo(MODO_CADASTRO);
  } else if (cmd == "REMOVER") {
    String uid = normalizaUid(arg);
    if (uid.length() == 0) {
      entraModo(MODO_REMOCAO);   // sem UID: remove o próximo cartão passado
    } else {
      removeEPublica(uid);
    }
  } else if (cmd == "LISTAR") {
    Serial.printf("[CADASTRO] %d cartao(oes) na lista\n", qtdCartoes);
    publicaLista();
  } else {
    Serial.println("[CMD] Comando desconhecido");
  }
}

// =================================================================
// Leitura de cartão
// =================================================================
void trataCartao(const String& uid) {
  if (modo == MODO_REMOCAO) {
    removeEPublica(uid);
    voltaModoNormal();
    return;
  }

  if (modo == MODO_CADASTRO) {
    if (buscaCartao(uid) >= 0) {
      Serial.printf("[CADASTRO] Ja cadastrada: %s\n", uid.c_str());
      publicaCadastro(uid, "ja_cadastrada");
    } else if (qtdCartoes >= MAX_CARTOES) {
      Serial.println("[CADASTRO] Lista cheia (10 cartoes)");
      publicaCadastro(uid, "lista_cheia");
    } else {
      cartoes[qtdCartoes++] = uid;
      Serial.printf("[CADASTRO] Cadastrada: %s (%d/%d)\n", uid.c_str(), qtdCartoes, MAX_CARTOES);
      publicaCadastro(uid, "cadastrada");
    }
    voltaModoNormal();
    return;
  }

  if (buscaCartao(uid) >= 0) {
    Serial.printf("[ACESSO] LIBERADO: %s\n", uid.c_str());
    publicaAcesso(uid, "liberado");
    ledLiberado();
  } else {
    Serial.printf("[ACESSO] NEGADO: %s\n", uid.c_str());
    publicaAcesso(uid, "negado");
    ledNegado();
  }
}

void lerCartao() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = uidParaTexto(rfid.uid);
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  unsigned long agora = millis();
  if (uid == ultimoUid && agora - ultimaLeitura < TEMPO_REPETICAO) {
    return; // mesmo cartão lido de novo em menos de 2 s
  }
  ultimoUid = uid;
  ultimaLeitura = agora;

  Serial.printf("[RFID] UID lido: %s (%d bytes)\n", uid.c_str(), rfid.uid.size);
  trataCartao(uid);
}

// =================================================================
// Conexões (com reconexão automática, sem travar o loop)
// =================================================================
void conectaWifi() {
  Serial.print("[WIFI] Conectando a ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("[WIFI] Conectado. IP: ");
  Serial.println(WiFi.localIP());
}

void garanteWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - ultimaTentativaWifi < INTERVALO_RECON) return;
  ultimaTentativaWifi = millis();
  Serial.println("[WIFI] Conexao perdida, reconectando...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
}

void garanteMqtt() {
  if (mqtt.connected()) {
    mqttEstavaConectado = true;
    return;
  }
  if (mqttEstavaConectado) {
    mqttEstavaConectado = false;
    Serial.printf("[MQTT] Conexao perdida (estado %d), reconectando...\n", mqtt.state());
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - ultimaTentativaMqtt < INTERVALO_RECON) return;
  ultimaTentativaMqtt = millis();

  // ID único: se dois clientes usam o mesmo ID, o broker derruba um deles.
  // No Wokwi o random() se repete a cada execução, por isso entra a hora do NTP.
  uint32_t sufixo = esp_random() ^ (uint32_t)time(nullptr) ^ micros();
  String clientId = String("esp32-rfid-") + SEU_NOME + "-" + String(sufixo, HEX);
  Serial.printf("[MQTT] Conectando a %s como %s...\n", MQTT_BROKER, clientId.c_str());

  // O Wokwi roda mais devagar que o tempo real, então os PINGs saem atrasados
  // e o broker derruba a conexão (estado -3). Por isso o broker recebe um
  // keepalive folgado (60 s) e, depois de conectado, o ESP32 pinga a cada 10 s.
  mqtt.setKeepAlive(60);
  if (mqtt.connect(clientId.c_str())) {
    mqtt.setKeepAlive(10);
    Serial.println("[MQTT] Conectado!");
    mqtt.subscribe(TOPICO_CMD.c_str());
    Serial.printf("[MQTT] Inscrito em %s\n", TOPICO_CMD.c_str());
    publicaMac();
  } else {
    Serial.printf("[MQTT] Falha (estado %d), nova tentativa em 3 s\n", mqtt.state());
  }
}

// =================================================================
// setup / loop
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n===== Leitor RFID - Laboratorio =====");

  pinMode(PINO_LED, OUTPUT);
  setLed(false);

  SPI.begin();          // SCK 18, MISO 19, MOSI 23
  rfid.PCD_Init();
  Serial.println("[RFID] MFRC522 iniciado");

  conectaWifi();
  meuMac = WiFi.macAddress();
  Serial.print("[WIFI] MAC: ");
  Serial.println(meuMac);

  configTime(-3 * 3600, 0, "pool.ntp.org");
  Serial.print("[NTP] Sincronizando hora");
  struct tm t;
  for (int i = 0; i < 20 && !getLocalTime(&t, 500); i++) Serial.print(".");
  Serial.println();
  Serial.printf("[NTP] Hora local: %s\n", horaAtual().c_str());

  mqtt.setServer(MQTT_BROKER, MQTT_PORTA);
  mqtt.setSocketTimeout(20);
  mqtt.setCallback(aoReceberMensagem);
  garanteMqtt();

  Serial.printf("[INFO] Topicos: %s#\n", PREFIXO.c_str());
  Serial.println("[MODO] NORMAL (acesso)");
}

void loop() {
  garanteWifi();
  garanteMqtt();
  mqtt.loop();

  lerCartao();
  verificaTempoModo();
  atualizaLed();
}
