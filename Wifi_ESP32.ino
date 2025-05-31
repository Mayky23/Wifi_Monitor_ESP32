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
const long GMT_OFFSET_SEC = -21600;  // UTC-6 (México/Centroamérica)
const int DAYLIGHT_OFFSET_SEC = 3600;

// Configuración del sistema
const unsigned long SCAN_INTERVAL = 60000;     // 60 segundos
const unsigned long RECONNECT_TIMEOUT = 30000; // 30 segundos
const int MAX_RETRIES = 3;
const int RSSI_THRESHOLD = -80;  // Solo alertar si RSSI > -80 dBm

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
bool systemInitialized = false;

// ========== FUNCIONES DE RED ==========
bool connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  
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
    return true;
  } else {
    Serial.println("\n❌ Error de conexión WiFi");
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
  snprintf(buffer, sizeof(buffer), "%02d/%02d/%04d %02d:%02d:%02d UTC-6", 
           timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  return String(buffer);
}

// ========== FUNCIONES DE IDENTIFICACIÓN ==========
String getVendorInfo(const String& mac) {
  if (WiFi.status() != WL_CONNECTED) return "Sin conexión";
  
  HTTPClient http;
  http.setTimeout(5000);  // Timeout de 5 segundos
  
  String url = "https://api.macvendors.com/" + mac;
  http.begin(url);
  http.addHeader("User-Agent", "ESP32-SecurityScanner/1.0");
  
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
  message += "📊 *Potencia RSSI*:\n";
  message += "• > -30 dBm: Muy cerca (alta amenaza)\n";
  message += "• -30 a -50: Cerca (amenaza media)\n";
  message += "• -50 a -70: Distancia media\n";
  message += "• < -70 dBm: Lejano (amenaza baja)";
  
  Serial.println("📤 Enviando alerta Telegram...");
  Serial.println(message);
  
  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    if (bot.sendMessage(CHAT_ID, message, "Markdown")) {
      Serial.println("✅ Alerta enviada exitosamente");
      return true;
    }
    Serial.println("❌ Error enviando alerta, reintento " + String(attempt + 1));
    delay(2000);
  }
  
  Serial.println("❌ Falló el envío de alerta después de " + String(MAX_RETRIES) + " intentos");
  return false;
}

// ========== FUNCIÓN PRINCIPAL DE ESCANEO ==========
void performSecurityScan() {
  Serial.println("🔍 Iniciando escaneo de seguridad...");
  
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
  
  // Enviar estadísticas periódicas
  static unsigned long lastStatsTime = 0;
  if (millis() - lastStatsTime > 3600000) {  // Cada hora
    String stats = "📊 *Estadísticas del sistema*\n\n";
    stats += "🕒 *Último escaneo*: " + getCurrentDateTime() + "\n";
    stats += "📡 *Dispositivos monitoreados*: " + String(detectedDevices.size()) + "\n";
    stats += "⚡ *Memoria libre*: " + String(ESP.getFreeHeap()) + " bytes\n";
    stats += "📶 *Señal WiFi*: " + String(WiFi.RSSI()) + " dBm";
    
    bot.sendMessage(CHAT_ID, stats, "Markdown");
    lastStatsTime = millis();
  }
}

// ========== CONFIGURACIÓN INICIAL ==========
void setup() {
  Serial.begin(115200);
  Serial.println("\n🚀 Iniciando Sistema de Detección de Intrusos WiFi v2.0");
  
  // Inicializar preferencias
  preferences.begin("wifisec", false);
  
  // Configurar cliente Telegram
  telegramClient.setInsecure();  // Para desarrollo - usar certificados en producción
  
  // Conectar WiFi
  if (connectToWiFi()) {
    Serial.println("✅ Sistema inicializado correctamente");
    
    // Enviar mensaje de inicio
    String startMsg = "🛡️ *Sistema de Seguridad WiFi Activado*\n\n";
    startMsg += "🕒 *Iniciado*: " + getCurrentDateTime() + "\n";
    startMsg += "📡 *Red monitoreada*: " + String(WIFI_SSID) + "\n";
    startMsg += "⏱️ *Intervalo de escaneo*: " + String(SCAN_INTERVAL/1000) + " segundos\n";
    startMsg += "🎯 *Umbral RSSI*: " + String(RSSI_THRESHOLD) + " dBm\n\n";
    startMsg += "El sistema está monitoreando intrusos...";
    
    bot.sendMessage(CHAT_ID, startMsg, "Markdown");
    systemInitialized = true;
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
  
  // Realizar escaneo de seguridad
  if (millis() - lastScanTime >= SCAN_INTERVAL) {
    performSecurityScan();
    lastScanTime = millis();
  }
  
  // Procesar mensajes de Telegram (para comandos futuros)
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  while (numNewMessages) {
    for (int i = 0; i < numNewMessages; i++) {
      String chat_id = String(bot.messages[i].chat_id);
      String text = bot.messages[i].text;
      
      if (chat_id == CHAT_ID) {
        if (text == "/status") {
          String status = "📊 *Estado del Sistema*\n\n";
          status += "✅ Sistema operativo\n";
          status += "🕒 " + getCurrentDateTime() + "\n";
          status += "📶 RSSI: " + String(WiFi.RSSI()) + " dBm\n";
          status += "💾 Memoria: " + String(ESP.getFreeHeap()) + " bytes";
          bot.sendMessage(chat_id, status, "Markdown");
        }
      }
    }
    numNewMessages = bot.getUpdates(bot.last_message_received + 1);
  }
  
  delay(1000);  // Pequeña pausa para no saturar el procesador
}