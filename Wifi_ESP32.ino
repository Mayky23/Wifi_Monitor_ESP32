#include <WiFi.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ========== CONFIGURACIÓN ==========
const char* WIFI_SSID = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_CONTRASEÑA";
const char* BOT_TOKEN = "TU_TOKEN_DEL_BOT";
const char* CHAT_ID = "TU_CHAT_ID";

// Configuración NTP
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = 3600;      // UTC+1 (España/Madrid)
const int DAYLIGHT_OFFSET_SEC = 3600;  // Horario de verano (CEST)

// Configuración del sistema
const unsigned long SCAN_INTERVAL = 60000;     // 60 segundos
const unsigned long RECONNECT_TIMEOUT = 30000; // 30 segundos
const unsigned long HEARTBEAT_INTERVAL = 300000; // 5 minutos para heartbeat
const unsigned long STATUS_CHECK_INTERVAL = 180000; // 3 minutos para auto-verificación
const int MAX_RETRIES = 3;
const int RSSI_THRESHOLD = -80;  // Solo alertar si RSSI > -80 dBm
const int MAX_WIFI_FAILS = 3;    // Máximo de fallos WiFi antes de alerta

// Lista blanca de MACs conocidas
const String KNOWN_MACS[] = {
  "AA:BB:CC:DD:EE:FF",
  "11:22:33:44:55:66"
};
const int KNOWN_MACS_COUNT = sizeof(KNOWN_MACS) / sizeof(KNOWN_MACS[0]);

// ========== VARIABLES GLOBALES ==========
WiFiClientSecure telegramClient;
UniversalTelegramBot bot(BOT_TOKEN, telegramClient);
Preferences preferences;

struct DeviceInfo {
  String mac;
  String vendor;
  String deviceType;
  int rssi;
  int channel;
  String ssid;
  unsigned long lastSeen;
  bool alerted;
};

std::vector<DeviceInfo> detectedDevices;
unsigned long lastScanTime = 0;
unsigned long lastConnectAttempt = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastStatusCheck = 0;
unsigned long bootTime = 0;
bool systemInitialized = false;
bool startupMessageSent = false;
bool lastServiceStatus = true;
int wifiFailCount = 0;
int totalScansPerformed = 0;
int totalIntrudersDetected = 0;

// ========== FUNCIONES DE RED ==========
bool connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiFailCount = 0;  // Reset contador de fallos
    return true;
  }

  if (millis() - lastConnectAttempt < RECONNECT_TIMEOUT) return false;
  lastConnectAttempt = millis();

  Serial.println("🔄 Intentando conexión WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi conectado! IP: " + WiFi.localIP().toString());
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    wifiFailCount = 0;
    return true;
  } else {
    Serial.println("\n❌ Error de conexión WiFi");
    wifiFailCount++;
    return false;
  }
}

// ========== FUNCIONES DE TIEMPO ==========
String getCurrentDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Error obteniendo hora";
  }

  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%02d/%02d/%04d %02d:%02d:%02d", 
           timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buffer);
}

String getUptime() {
  unsigned long uptime = millis() - bootTime;
  int days = uptime / 86400000;
  uptime %= 86400000;
  int hours = uptime / 3600000;
  uptime %= 3600000;
  int minutes = uptime / 60000;
  
  String uptimeStr = "";
  if (days > 0) uptimeStr += String(days) + "d ";
  if (hours > 0) uptimeStr += String(hours) + "h ";
  uptimeStr += String(minutes) + "m";
  
  return uptimeStr;
}

// ========== FUNCIONES DE IDENTIFICACIÓN ==========
String getVendorInfo(const String& mac) {
  if (WiFi.status() != WL_CONNECTED) return "Sin conexión";

  HTTPClient http;
  http.setTimeout(5000);  // Timeout de 5 segundos

  String url = "https://api.macvendors.com/" + mac;
  http.begin(url);
  http.addHeader("User-Agent", "ESP32-SecurityScanner/2.0");

  int httpCode = http.GET();
  String vendor = "Desconocido";

  if (httpCode == 200) {
    vendor = http.getString();
    vendor.trim();
    if (vendor.length() > 50) {
      vendor = vendor.substring(0, 47) + "...";
    }
  } else if (httpCode == 429) {
    vendor = "Límite API excedido";
  } else {
    vendor = "Error API (" + String(httpCode) + ")";
  }

  http.end();
  delay(100);  // Pequeña pausa para no saturar la API
  return vendor;
}

String inferDeviceType(const String& mac, const String& vendor, const String& ssid) {
  String macUpper = mac;
  macUpper.toUpperCase();
  String vendorLower = vendor;
  vendorLower.toLowerCase();

  // Identificación por prefijo MAC (OUI)
  if (macUpper.startsWith("A4:77:33") || macUpper.startsWith("00:23:12")) return "📱 iPhone";
  if (macUpper.startsWith("DC:A6:32") || macUpper.startsWith("B8:27:EB")) return "🥧 Raspberry Pi";
  if (macUpper.startsWith("18:74:2E") || macUpper.startsWith("FC:A6:67")) return "🔊 Amazon Echo";
  if (macUpper.startsWith("50:C7:BF") || macUpper.startsWith("18:B4:30")) return "📺 Smart TV";
  if (macUpper.startsWith("00:16:B6")) return "💻 Laptop";

  // Identificación por fabricante
  if (vendorLower.indexOf("apple") != -1) return "🍎 Dispositivo Apple";
  if (vendorLower.indexOf("samsung") != -1) return "📱 Samsung";
  if (vendorLower.indexOf("xiaomi") != -1) return "📱 Xiaomi";
  if (vendorLower.indexOf("intel") != -1) return "💻 PC/Laptop Intel";
  if (vendorLower.indexOf("broadcom") != -1) return "📡 Dispositivo Broadcom";
  if (vendorLower.indexOf("qualcomm") != -1) return "📱 Dispositivo Móvil";
  if (vendorLower.indexOf("amazon") != -1) return "🔊 Dispositivo Amazon";
  if (vendorLower.indexOf("google") != -1) return "🏠 Google Home/Nest";
  if (vendorLower.indexOf("tp-link") != -1) return "🌐 Router TP-Link";

  return "❓ Dispositivo desconocido";
}

String getThreatLevel(int rssi) {
  if (rssi > -30) return "🔴 MUY CERCA";
  if (rssi > -50) return "🟠 CERCA";
  if (rssi > -70) return "🟡 MEDIO";
  return "🟢 LEJANO";
}

// ========== FUNCIONES DE SEGURIDAD ==========
bool isKnownDevice(const String& mac) {
  for (int i = 0; i < KNOWN_MACS_COUNT; i++) {
    if (mac.equalsIgnoreCase(KNOWN_MACS[i])) {
      return true;
    }
  }
  return false;
}

bool wasRecentlyAlerted(const String& mac) {
  for (auto& device : detectedDevices) {
    if (device.mac.equalsIgnoreCase(mac)) {
      // Si fue detectado hace menos de 5 minutos, no alertar de nuevo
      if (millis() - device.lastSeen < 300000) {
        device.lastSeen = millis();  // Actualizar timestamp
        return device.alerted;
      }
      device.alerted = false;  // Reset si pasó mucho tiempo
      break;
    }
  }
  return false;
}

void updateDeviceRecord(const DeviceInfo& newDevice) {
  bool found = false;
  for (auto& device : detectedDevices) {
    if (device.mac.equalsIgnoreCase(newDevice.mac)) {
      device = newDevice;
      found = true;
      break;
    }
  }

  if (!found) {
    detectedDevices.push_back(newDevice);
  }

  // Limpiar registros antiguos (más de 1 hora)
  detectedDevices.erase(
    std::remove_if(detectedDevices.begin(), detectedDevices.end(),
                   [](const DeviceInfo& d) { return millis() - d.lastSeen > 3600000; }),
    detectedDevices.end()
  );
}

// ========== FUNCIONES DE MENSAJERÍA ==========
bool sendTelegramMessage(const String& message, const String& parseMode = "") {
  Serial.println("📤 Enviando mensaje Telegram...");
  
  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    bool result;
    if (parseMode.length() > 0) {
      result = bot.sendMessage(CHAT_ID, message, parseMode);
    } else {
      result = bot.sendMessage(CHAT_ID, message, "");
    }
    
    if (result) {
      Serial.println("✅ Mensaje enviado exitosamente");
      return true;
    }
    Serial.println("❌ Error enviando mensaje, reintento " + String(attempt + 1));
    delay(2000);
  }

  Serial.println("❌ Falló el envío después de " + String(MAX_RETRIES) + " intentos");
  return false;
}

void sendStartupMessages() {
  if (startupMessageSent || !systemInitialized) return;

  // Primer mensaje: Estado del servicio
  String statusMsg = "🛡️ *Servicio Activo* ✅\n\n";
  statusMsg += "🕒 *Iniciado*: " + getCurrentDateTime() + "\n";
  statusMsg += "📡 *Red*: " + String(WIFI_SSID) + "\n";
  statusMsg += "📶 *Señal WiFi*: " + String(WiFi.RSSI()) + " dBm\n";
  statusMsg += "🆔 *IP*: " + WiFi.localIP().toString();

  if (sendTelegramMessage(statusMsg, "Markdown")) {
    delay(2000); // Pausa entre mensajes

    // Segundo mensaje: Explicación del funcionamiento
    String infoMsg = "🤖 *Sistema de Detección WiFi Activo*\n\n";
    infoMsg += "Este bot monitorea dispositivos WiFi cercanos y alerta sobre posibles intrusos cada ";
    infoMsg += String(SCAN_INTERVAL/1000) + " segundos.\n\n";
    infoMsg += "💡 *Comandos disponibles*:\n";
    infoMsg += "• `/estatus` - Ver estado del sistema\n";
    infoMsg += "• `/stats` - Estadísticas detalladas\n";
    infoMsg += "• `/help` - Ayuda completa";

    sendTelegramMessage(infoMsg, "Markdown");
    startupMessageSent = true;
    lastServiceStatus = true;
  }
}

void sendServiceDownAlert() {
  String alertMsg = "🚨 *Servicio Desconectado* ❌\n\n";
  alertMsg += "⚠️ El sistema de seguridad WiFi ha perdido conectividad\n";
  alertMsg += "🕒 *Detectado*: " + getCurrentDateTime() + "\n";
  alertMsg += "🔧 *Causa posible*: Pérdida de alimentación o conectividad WiFi\n\n";
  alertMsg += "🔄 El sistema intentará reconectarse automáticamente...";

  // Intentar enviar mensaje con reintentos limitados
  for (int i = 0; i < 2; i++) {
    if (sendTelegramMessage(alertMsg, "Markdown")) {
      lastServiceStatus = false;
      break;
    }
    delay(5000);
  }
}

void sendHeartbeat() {
  if (millis() - lastHeartbeat < HEARTBEAT_INTERVAL) return;

  String heartbeatMsg = "💓 *Sistema Operativo*\n";
  heartbeatMsg += "🕒 " + getCurrentDateTime() + "\n";
  heartbeatMsg += "⏱️ Activo: " + getUptime() + "\n";
  heartbeatMsg += "🔍 Escaneos: " + String(totalScansPerformed) + "\n";
  heartbeatMsg += "🚨 Intrusos: " + String(totalIntrudersDetected);

  if (sendTelegramMessage(heartbeatMsg, "Markdown")) {
    lastHeartbeat = millis();
  }
}

// ========== FUNCIONES DE ALERTAS ==========
bool sendTelegramAlert(const DeviceInfo& device) {
  String message = "🚨 *INTRUSO DETECTADO* 🚨\n\n";
  message += "🔍 *MAC*: `" + device.mac + "`\n";
  message += "🏭 *Fabricante*: " + device.vendor + "\n";
  message += "📱 *Tipo*: " + device.deviceType + "\n";
  message += "📶 *Señal*: " + String(device.rssi) + " dBm " + getThreatLevel(device.rssi) + "\n";
  message += "📻 *Canal*: " + String(device.channel) + "\n";
  message += "🌐 *Red*: " + device.ssid + "\n";
  message += "🕒 *Hora*: " + getCurrentDateTime() + "\n\n";

  message += "⚠️ *Nivel de amenaza*: " + getThreatLevel(device.rssi) + "\n";
  message += "📊 *Referencia RSSI*:\n";
  message += "• > -30 dBm: Muy cerca (alta amenaza)\n";
  message += "• -30 a -50: Cerca (amenaza media)\n";
  message += "• -50 a -70: Distancia media\n";
  message += "• < -70 dBm: Lejano (amenaza baja)";

  totalIntrudersDetected++;
  return sendTelegramMessage(message, "Markdown");
}

// ========== FUNCIÓN PRINCIPAL DE ESCANEO ==========
void performSecurityScan() {
  Serial.println("🔍 Iniciando escaneo de seguridad...");
  totalScansPerformed++;

  WiFi.scanDelete();  // Limpiar escaneos anteriores
  int networkCount = WiFi.scanNetworks(false, true, false, 300);  // Escaneo activo

  if (networkCount == 0) {
    Serial.println("⚠️ No se encontraron redes");
    return;
  }

  Serial.println("📡 Encontradas " + String(networkCount) + " redes");
  int intrudersFound = 0;

  for (int i = 0; i < networkCount; i++) {
    String mac = WiFi.BSSIDstr(i);
    int rssi = WiFi.RSSI(i);

    // Filtrar por potencia de señal (solo dispositivos cercanos)
    if (rssi < RSSI_THRESHOLD) continue;

    // Verificar si es un dispositivo conocido
    if (isKnownDevice(mac)) {
      Serial.println("✅ Dispositivo conocido: " + mac);
      continue;
    }

    // Verificar si ya fue alertado recientemente
    if (wasRecentlyAlerted(mac)) {
      Serial.println("🔄 Dispositivo ya alertado: " + mac);
      continue;
    }

    // Nuevo intruso detectado
    Serial.println("🚨 INTRUSO: " + mac + " (RSSI: " + String(rssi) + ")");

    DeviceInfo device;
    device.mac = mac;
    device.rssi = rssi;
    device.channel = WiFi.channel(i);
    device.ssid = WiFi.SSID(i);
    device.lastSeen = millis();
    device.alerted = false;

    // Obtener información del fabricante
    device.vendor = getVendorInfo(mac);
    device.deviceType = inferDeviceType(mac, device.vendor, device.ssid);

    // Enviar alerta
    if (sendTelegramAlert(device)) {
      device.alerted = true;
      intrudersFound++;
    }

    updateDeviceRecord(device);
    delay(1000);  // Pausa entre alertas
  }

  WiFi.scanDelete();
  Serial.println("✅ Escaneo completado. Intrusos nuevos: " + String(intrudersFound));
}

// ========== FUNCIONES DE COMANDOS ==========
void handleTelegramCommands() {
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  
  while (numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
      String chat_id = String(bot.messages[i].chat_id);
      String text = bot.messages[i].text;
      String from_name = bot.messages[i].from_name;

      if (chat_id == CHAT_ID) {
        Serial.println("📨 Comando recibido: " + text + " de " + from_name);

        if (text == "/estatus" || text == "/status") {
          String status = "📊 *Estado del Sistema*\n\n";
          status += "✅ *Estado*: Operativo\n";
          status += "🕒 *Hora actual*: " + getCurrentDateTime() + "\n";
          status += "⏱️ *Tiempo activo*: " + getUptime() + "\n";
          status += "📶 *Señal WiFi*: " + String(WiFi.RSSI()) + " dBm\n";
          status += "🆔 *IP*: " + WiFi.localIP().toString() + "\n";
          status += "💾 *Memoria libre*: " + String(ESP.getFreeHeap()) + " bytes\n";
          status += "🔍 *Escaneos realizados*: " + String(totalScansPerformed) + "\n";
          status += "🚨 *Intrusos detectados*: " + String(totalIntrudersDetected);
          
          bot.sendMessage(chat_id, status, "Markdown");
        }
        else if (text == "/stats") {
          String stats = "📈 *Estadísticas Detalladas*\n\n";
          stats += "🚀 *Iniciado*: " + getCurrentDateTime() + "\n";
          stats += "⏱️ *Uptime*: " + getUptime() + "\n";
          stats += "🔍 *Total escaneos*: " + String(totalScansPerformed) + "\n";
          stats += "🚨 *Total intrusos*: " + String(totalIntrudersDetected) + "\n";
          stats += "📡 *Dispositivos en memoria*: " + String(detectedDevices.size()) + "\n";
          stats += "🌐 *Red monitoreada*: " + String(WIFI_SSID) + "\n";
          stats += "⏱️ *Intervalo escaneo*: " + String(SCAN_INTERVAL/1000) + "s\n";
          stats += "🎯 *Umbral RSSI*: " + String(RSSI_THRESHOLD) + " dBm\n";
          stats += "💾 *Memoria libre*: " + String(ESP.getFreeHeap()) + " bytes\n";
          stats += "📶 *Calidad WiFi*: " + String(WiFi.RSSI()) + " dBm";
          
          bot.sendMessage(chat_id, stats, "Markdown");
        }
        else if (text == "/help") {
          String help = "🤖 *Ayuda - Sistema de Seguridad WiFi*\n\n";
          help += "Este bot monitorea dispositivos WiFi cercanos y alerta sobre posibles intrusos.\n\n";
          help += "📋 *Comandos disponibles*:\n\n";
          help += "• `/estatus` - Estado actual del sistema\n";
          help += "• `/stats` - Estadísticas detalladas\n";
          help += "• `/help` - Esta ayuda\n\n";
          help += "🔧 *Configuración actual*:\n";
          help += "• Escaneo cada " + String(SCAN_INTERVAL/1000) + " segundos\n";
          help += "• Umbral de detección: " + String(RSSI_THRESHOLD) + " dBm\n";
          help += "• Dispositivos conocidos: " + String(KNOWN_MACS_COUNT) + "\n\n";
          help += "⚠️ *Niveles de amenaza*:\n";
          help += "🔴 MUY CERCA: > -30 dBm\n";
          help += "🟠 CERCA: -30 a -50 dBm\n";
          help += "🟡 MEDIO: -50 a -70 dBm\n";
          help += "🟢 LEJANO: < -70 dBm";
          
          bot.sendMessage(chat_id, help, "Markdown");
        }
        else {
          String unknownCmd = "❓ Comando no reconocido: " + text + "\n\n";
          unknownCmd += "Usa /help para ver comandos disponibles.";
          bot.sendMessage(chat_id, unknownCmd, "");
        }
      }
    }
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
}

// ========== MONITOREO DE ESTADO ==========
void checkSystemHealth() {
  if (millis() - lastStatusCheck < STATUS_CHECK_INTERVAL) return;
  lastStatusCheck = millis();

  // Verificar si el servicio está funcionando correctamente
  bool currentStatus = (WiFi.status() == WL_CONNECTED && wifiFailCount < MAX_WIFI_FAILS);

  // Si cambió el estado del servicio
  if (currentStatus != lastServiceStatus) {
    if (!currentStatus && lastServiceStatus) {
      // El servicio se desconectó
      sendServiceDownAlert();
    } else if (currentStatus && !lastServiceStatus) {
      // El servicio se reconectó
      String reconnectMsg = "✅ *Servicio Reconectado*\n\n";
      reconnectMsg += "🔄 El sistema ha restablecido la conectividad\n";
      reconnectMsg += "🕒 *Reconectado*: " + getCurrentDateTime() + "\n";
      reconnectMsg += "📶 *Señal WiFi*: " + String(WiFi.RSSI()) + " dBm";
      
      sendTelegramMessage(reconnectMsg, "Markdown");
      lastServiceStatus = true;
    }
  }

  // Enviar heartbeat periódico
  if (currentStatus) {
    sendHeartbeat();
  }
}

// ========== CONFIGURACIÓN INICIAL ==========
void setup() {
  Serial.begin(115200);
  Serial.println("\n🚀 Iniciando Sistema de Detección de Intrusos WiFi v2.1");
  
  bootTime = millis();

  // Inicializar preferencias
  preferences.begin("wifisec", false);

  // Configurar cliente Telegram
  telegramClient.setInsecure();  // Para desarrollo - usar certificados en producción

  // Conectar WiFi
  if (connectToWiFi()) {
    Serial.println("✅ Sistema inicializado correctamente");
    systemInitialized = true;
    
    // Pequeña pausa para estabilizar la conexión
    delay(3000);
    
    // Enviar mensajes de inicio
    sendStartupMessages();
  } else {
    Serial.println("❌ Error inicializando sistema");
  }
}

// ========== BUCLE PRINCIPAL ==========
void loop() {
  // Verificar conexión WiFi
  if (!connectToWiFi()) {
    Serial.println("⚠️ Sin conexión WiFi, reintentando...");
    delay(5000);
    return;
  }

  // Enviar mensajes de inicio si no se han enviado
  if (systemInitialized && !startupMessageSent) {
    sendStartupMessages();
  }

  // Verificar salud del sistema
  checkSystemHealth();

  // Realizar escaneo de seguridad
  if (millis() - lastScanTime >= SCAN_INTERVAL) {
    performSecurityScan();
    lastScanTime = millis();
  }

  // Procesar comandos de Telegram
  handleTelegramCommands();

  delay(1000);  // Pequeña pausa para no saturar el procesador
}