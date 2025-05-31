# 🛡️ Sistema de Detección de Intrusos WiFi con ESP32 y Telegram

Este proyecto usa un ESP32 para escanear dispositivos WiFi cercanos, identificar posibles intrusos desconocidos, y enviar alertas en tiempo real a través de Telegram. Perfecto para monitorear la seguridad de tu red doméstica o de oficina.

## 🚀 Funcionalidades

- Escaneo periódico de redes WiFi cercanas.
- Filtrado por fuerza de señal RSSI.
- Detección de dispositivos no autorizados (MAC no conocidas).
- Identificación del fabricante mediante la API de `macvendors.com`.
- Clasificación de amenaza según intensidad de señal.
- Envío de alertas detalladas a un chat de Telegram.
- Registro temporal de dispositivos detectados.
- Comando `/status` vía Telegram para consultar el estado del sistema.

---

## 📦 Requisitos

### Hardware
- ESP32 (cualquier versión con WiFi)
- Cable micro USB
- Conexión a Internet vía WiFi

### Software
- Arduino IDE
- Librerías necesarias:
  - `UniversalTelegramBot` (de Brian Lough)
  - `WiFi` (nativa en ESP32)
  - `WiFiClientSecure`
  - `ArduinoJson` (v6.x)
  - `Preferences`

---

## 🔧 Instalación y Configuración

### 1. Instalar Librerías en Arduino IDE

Abre el Arduino IDE y ve a:
- `Programa` > `Incluir biblioteca` > `Gestionar bibliotecas...`

Instala:
- **UniversalTelegramBot** (autor: Brian Lough)
- **ArduinoJson** (versión 6.15.2 o superior)

### 2. Configura tus credenciales en el código

Modifica estas variables en el sketch:

```cpp
const char* WIFI_SSID = "TuSSID";
const char* WIFI_PASSWORD = "TuContraseñaWiFi";
const char* BOT_TOKEN = "Token_de_Tu_Bot";
const char* CHAT_ID = "Tu_Chat_ID";
```

## 🤖 Cómo crear el Bot de Telegram y obtener los IDs

### 1. Obtener el Bot Token
Abre Telegram y busca @BotFather.

Escribe el comando:

```bash
/newbot
```
Sigue las instrucciones (elige un nombre y un nombre de usuario único para tu bot).

El BotFather te dará un Token, algo como:
```cpp
123456789:ABCDefghIjk-LMNopQRstuVWxyZ
```
Guarda este token, lo necesitarás como BOT_TOKEN.

### 2. Obtener tu Chat ID
En Telegram, envía un mensaje al bot userinfobot.

este te dará tu ID de usuario, algo como:
```cpp
Id: 123456789
First: MA
Lang: es
```

## ✅ Cómo usar
Carga el sketch en tu ESP32 desde el Arduino IDE.

Abre el monitor serial a 115200 baudios.

Espera a que el sistema se conecte al WiFi y comience a escanear.

Recibirás alertas por Telegram si se detecta un dispositivo desconocido con señal fuerte.

## 📸 Ejemplo de Alerta

```cpp

🚨 INTRUSO DETECTADO 🚨

🔍 MAC: `A0:B1:C2:D3:E4:F5`
🏭 Fabricante: Samsung Electronics
📱 Tipo: 📱 Samsung
📶 Señal: -42 dBm 🟠 CERCA
📻 Canal: 6
🌐 Red: La cueva
🕒 Hora: 31/05/2025 17:25:00 UTC-6

⚠️ Nivel de amenaza: 🟠 CERCA
📊 Potencia RSSI:
• > -30 dBm: Muy cerca (alta amenaza)
• -30 a -50: Cerca (amenaza media)
• -50 a -70: Distancia media
• < -70 dBm: Lejano (amenaza baja)

```

## 📚 Comandos disponibles vía Telegram

```cpp
/status	Muestra el estado del sistema
```

## 🛠️ Personalización
Puedes añadir tus propios dispositivos seguros modificando el arreglo KNOWN_MACS[].

Puedes ajustar la sensibilidad modificando el umbral de RSSI:
```cpp
const int RSSI_THRESHOLD = -80;
```