/**
 * Renspired ESP32 Gateway
 *
 * UART bridge between TI-Nspire and LLM APIs.
 *
 * Dependencies: ArduinoJson
 */

#include "driver/uart.h"
#include "esp_sleep.h"
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

// ============================================================================
// CONFIGURATION - Edit these values
// ============================================================================

#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASSWORD"
#define GEMINI_API_KEY "API_KEY"

// System prompt - custom instructions for the model
// Comment out for no system prompt
#define SYSTEM_PROMPT                                                          \
  "You are a helpful assistant running on a TI-Nspire calculator."             \
  "You can think as much as you need, but keep responses relatively concise."  \
  "Avoid text formatting like Markdown and LaTeX that won't be able to be "    \
  "remdered - ASCII only."

// Uncomment to use a local LLM (openai compatible) instead of Gemini
// #define USE_LOCAL_LLM
#ifdef USE_LOCAL_LLM
#define LOCAL_LLM_HOST "IP"
#define LOCAL_LLM_PORT 8080
#endif

// ============================================================================
// Constants
// ============================================================================

#define UART_RX_PIN 20
#define UART_TX_PIN 21
#define BAUD_RATE 115200
#define EOT_CHAR 0x04
#define MAX_REQUEST_SIZE 8192
#define IDLE_SLEEP_TIMEOUT_MS 30000
#define UART_WAKEUP_THRESHOLD 3 // Number of RX edges to wake from light-sleep

// ============================================================================
// Globals
// ============================================================================

HardwareSerial NspireUART(1);
#ifdef USE_LOCAL_LLM
WiFiClient client;
#else
WiFiClientSecure client;
#endif

bool handshakeComplete = false;
char requestBuffer[MAX_REQUEST_SIZE];
int reqIdx = 0;
unsigned long lastActivityTime = 0; // for idle sleep

// ============================================================================
// Setup
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== Renspired Gateway ===");

  // Initialize UART to Nspire
  // 230400 baud technically seems to work
  // but isn't worth the extra risk with our throughput requirements
  NspireUART.begin(BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  NspireUART.setRxBufferSize(4096);

  Serial.printf("Connecting to wireless: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWireless connection failed");
  }

#ifndef USE_LOCAL_LLM
  client.setInsecure();
#endif

  // Clear state and signal ready
  handshakeComplete = false;
  reqIdx = 0;
  memset(requestBuffer, 0, sizeof(requestBuffer));
  lastActivityTime = millis();

  WiFi.setSleep(true);
  Serial.println("WiFi modem sleeping");

  Serial.println("Ready. Wait for handshake...");
}

// ============================================================================
// Handshake with Nspire
// ============================================================================

void handleHandshake() {
  static char buf[32];
  static int idx = 0;
  static unsigned long lastReadyTime = 0;

  if (millis() - lastActivityTime > IDLE_SLEEP_TIMEOUT_MS) {
    idx = 0; // Reset buffer before sleeping
    enterLightSleep();
  }

  // Send ESP_READY often in case the nspire misses it
  if (millis() - lastReadyTime >= 1000) {
    NspireUART.print("ESP_READY\n");
    lastReadyTime = millis();
  }

  while (NspireUART.available()) {
    char c = NspireUART.read();
    lastActivityTime = millis(); // Reset idle timer

    if (c == '\n') {
      buf[idx] = '\0';

      if (strcmp(buf, "SYNC") == 0) {
        NspireUART.print("READY\n");
        NspireUART.flush();
        handshakeComplete = true;
        reqIdx = 0;
        lastActivityTime = millis();
        Serial.println("Handshake complete");
      } else if (strcmp(buf, "RST") == 0) {
        ESP.restart();
      }
      idx = 0;
    } else if (c != '\r' && idx < 31) {
      buf[idx++] = c;
    }
  }
}

// ============================================================================
// API response handling
// ============================================================================

// Global buffer to accumulate response text
#define MAX_RESPONSE_BUF 8192
static char g_responseBuf[MAX_RESPONSE_BUF];
static int g_responseLen = 0;

void appendToResponse(const char *text, int len) {
  if (g_responseLen + len < MAX_RESPONSE_BUF) {
    memcpy(g_responseBuf + g_responseLen, text, len);
    g_responseLen += len;
    g_responseBuf[g_responseLen] = '\0';
  }
}

void processJsonLine(const String &line) {
  String trimmed = line;
  trimmed.trim();

  // Remove random bullshit
  if (trimmed.length() == 0 || trimmed == "[" || trimmed == "]" ||
      trimmed == "{" || trimmed == "}" || trimmed == "," || trimmed == "[{" ||
      trimmed == "}]" || trimmed == "},") {
    return;
  }

  // SSE format (openai) - single line JSON
  if (trimmed.startsWith("data: ")) {
    String json = trimmed.substring(6);
    if (json == "[DONE]" || json.length() == 0)
      return;

    JsonDocument doc;
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
      // openai format: choices[0].delta.content
      JsonVariant choices = doc["choices"];
      if (!choices.isNull() && choices.is<JsonArray>() && choices.size() > 0) {
        JsonVariant delta = choices[0]["delta"];
        if (!delta.isNull() && delta["reasoning_content"].isNull()) {
          const char *text = delta["content"];
          if (text && strlen(text) > 0) {
            appendToResponse(text, strlen(text));
            Serial.print(text);
          }
        }
      }
    }
    return;
  }

  // Gemini format, look for "text": "..." on its own line
  int textIdx = trimmed.indexOf("\"text\":");
  if (textIdx != -1) {
    // Find opening quote
    int valueStart = trimmed.indexOf('"', textIdx + 7);
    if (valueStart == -1)
      return;
    valueStart++; // Skip the quote

    // Find the closing quote, escape handling
    int valueEnd = -1;
    bool escaped = false;
    for (int i = valueStart; i < trimmed.length(); i++) {
      if (escaped) {
        escaped = false;
      } else if (trimmed[i] == '\\') {
        escaped = true;
      } else if (trimmed[i] == '"') {
        valueEnd = i;
        break;
      }
    }

    if (valueEnd > valueStart) {
      String text = trimmed.substring(valueStart, valueEnd);
      // Unescape
      text.replace("\\n", "\n");
      text.replace("\\r", "\r");
      text.replace("\\t", "\t");
      text.replace("\\\"", "\"");
      text.replace("\\\\", "\\");

      // Skip thought signatures
      if (text.length() > 0) {
        appendToResponse(text.c_str(), text.length());
        Serial.print(text);
      }
    }
  }
}

// ============================================================================
// API request handling
// ============================================================================

void sendApiRequest(const char *requestJson) {
  Serial.println("Starting API request...");
  g_responseLen = 0; // Reset buffer
  g_responseBuf[0] = '\0';

  // Alert user if wireless isn't connected
  if (WiFi.status() != WL_CONNECTED) {
    NspireUART.print("ERR:NET\n");
    NspireUART.write(EOT_CHAR);
    return;
  }

  // Parse incoming request from nspire
  JsonDocument reqDoc;
  DeserializationError error = deserializeJson(reqDoc, requestJson);
  if (error) {
    Serial.printf("Request parse error: %s\n", error.c_str());
    NspireUART.print("ERR:API\n");
    NspireUART.write(EOT_CHAR);
    return;
  }

  const char *currentPrompt = reqDoc["current_prompt"];
  JsonArray history = reqDoc["history"];

  // Connect to API
#ifdef USE_LOCAL_LLM
  if (!client.connect(LOCAL_LLM_HOST, LOCAL_LLM_PORT)) {
#else
  if (!client.connect("generativelanguage.googleapis.com", 443)) {
#endif
    Serial.println("Connection failed");
    NspireUART.print("ERR:NET\n");
    NspireUART.write(EOT_CHAR);
    return;
  }

  // Build and send HTTP request
  String body;

#ifdef USE_LOCAL_LLM
  // OpenAI format for local LLM
  JsonDocument bodyDoc;
  JsonArray messages = bodyDoc["messages"].to<JsonArray>();

// Add system prompt if configured
#ifdef SYSTEM_PROMPT
  JsonObject sysMsg = messages.add<JsonObject>();
  sysMsg["role"] = "system";
  sysMsg["content"] = SYSTEM_PROMPT;
#endif

  // Add history
  for (JsonVariant turn : history) {
    JsonObject msg = messages.add<JsonObject>();
    msg["role"] = turn["role"];
    // Convert Gemini parts format to openai content format
    if (turn["parts"].is<JsonArray>() && turn["parts"].size() > 0) {
      msg["content"] = turn["parts"][0]["text"];
    }
  }

  // Add current prompt
  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = currentPrompt;

  bodyDoc["stream"] = true;
  serializeJson(bodyDoc, body);

  client.print("POST /v1/chat/completions HTTP/1.1\r\n");
  client.printf("Host: %s:%d\r\n", LOCAL_LLM_HOST, LOCAL_LLM_PORT);
#else
  // Gemini format
  JsonDocument bodyDoc;

// Add system instruction if configured
#ifdef SYSTEM_PROMPT
  JsonObject sysInstr = bodyDoc["systemInstruction"].to<JsonObject>();
  JsonArray sysParts = sysInstr["parts"].to<JsonArray>();
  JsonObject sysText = sysParts.add<JsonObject>();
  sysText["text"] = SYSTEM_PROMPT;
#endif

  JsonArray contents = bodyDoc["contents"].to<JsonArray>();

  // Add history, should already be in Gemini format
  for (JsonVariant turn : history) {
    contents.add(turn);
  }

  // Add current prompt
  JsonObject userTurn = contents.add<JsonObject>();
  userTurn["role"] = "user";
  JsonArray parts = userTurn["parts"].to<JsonArray>();
  JsonObject textPart = parts.add<JsonObject>();
  textPart["text"] = currentPrompt;

  serializeJson(bodyDoc, body);

  String url =
      "/v1beta/models/gemini-3-flash-preview:streamGenerateContent?key=";
  url += GEMINI_API_KEY;

  client.print("POST ");
  client.print(url);
  client.print(" HTTP/1.1\r\n");
  client.print("Host: generativelanguage.googleapis.com\r\n");
#endif

  client.print("Content-Type: application/json\r\n");
  client.printf("Content-Length: %d\r\n", body.length());
  client.print("Connection: close\r\n\r\n");
  client.print(body);

  Serial.println("Request sent, reading response...");

  // Read response
  bool headersComplete = false;
  bool sentStatus = false;
  bool isError = false;
  int lineCount = 0;
  String firstLines = ""; // Buffer first few lines to check for errors

  unsigned long timeout = millis() + 60000;

  while ((client.connected() || client.available()) && millis() < timeout) {
    if (client.available()) {
      String line = client.readStringUntil('\n');

      // Skip HTTP headers
      if (!headersComplete) {
        // Check for error status codes in headers
        if (line.startsWith("HTTP/") && line.indexOf(" 4") != -1) {
          isError = true;
        }
        if (line == "\r" || line.length() == 0) {
          headersComplete = true;
        }
        continue;
      }

      lineCount++;

      // Buffer first 5 lines to check for API errors
      if (!sentStatus && lineCount <= 5) {
        firstLines += line + "\n";

        // Check for error patterns
        if (firstLines.indexOf("RESOURCE_EXHAUSTED") != -1 ||
            firstLines.indexOf("\"code\": 429") != -1 ||
            firstLines.indexOf("rateLimitExceeded") != -1) {
          isError = true;
        }

        // After 5 lines, decide and send status
        if (lineCount == 5 || !client.available()) {
          if (isError) {
            Serial.println("Found API error");
            NspireUART.print("ERR:QUOTA\n"); // it might not be always quota but
                                             // let's be real it always is
          } else {
            Serial.println("Response OK");
            NspireUART.print("OK\n");
          }
          NspireUART.flush();
          sentStatus = true;

          // Process buffered lines
          if (!isError) {
            int start = 0;
            int end;
            while ((end = firstLines.indexOf('\n', start)) != -1) {
              processJsonLine(firstLines.substring(start, end));
              start = end + 1;
            }
          }
        }
        continue;
      }

      // Process remaining lines
      if (sentStatus && !isError) {
        processJsonLine(line);
      }
    }
    yield();
  }

  // Handle short responses
  if (!sentStatus) {
    if (firstLines.indexOf("RESOURCE_EXHAUSTED") != -1 ||
        firstLines.indexOf("\"code\": 429") != -1) {
      NspireUART.print("ERR:QUOTA\n");
      NspireUART.write(EOT_CHAR);
      client.stop();
      return;
    } else {
      int start = 0;
      int end;
      while ((end = firstLines.indexOf('\n', start)) != -1) {
        processJsonLine(firstLines.substring(start, end));
        start = end + 1;
      }
    }
  }

  // Send buffered response with packet protocol
  Serial.printf("\n--- Response buffered: %d bytes ---\n", g_responseLen);

  // Send length header
  NspireUART.printf("LEN:%d\n", g_responseLen);
  NspireUART.flush();

  Serial.println("Wait for ACK of LEN");
  unsigned long ackTimeout = millis() + 5000;
  bool gotAck = false;
  while (millis() < ackTimeout && !gotAck) {
    if (NspireUART.available()) {
      char c = NspireUART.read();
      if (c == 'A')
        gotAck = true;
    }
    yield();
  }

  if (!gotAck) {
    Serial.println("No ACK received, abort");
    NspireUART.write(EOT_CHAR);
    client.stop();
    return;
  }
  Serial.println("ACK received, send data");

  // Crazy motherfucker named packets
  const int CHUNK_SIZE = 64;
  int sent = 0;
  while (sent < g_responseLen) {
    int chunkLen = min(CHUNK_SIZE, g_responseLen - sent);

    NspireUART.write((uint8_t *)(g_responseBuf + sent), chunkLen);
    NspireUART.flush();
    sent += chunkLen;

    // Wait for ACK
    ackTimeout = millis() + 2000;
    gotAck = false;
    while (millis() < ackTimeout && !gotAck) {
      if (NspireUART.available()) {
        char c = NspireUART.read();
        if (c == 'A')
          gotAck = true;
      }
      yield();
    }

    if (!gotAck) {
      Serial.printf("No ACK at offset %d, abort\n", sent);
      break;
    }
    Serial.printf("Packet sent: %d/%d\n", sent, g_responseLen);
  }

  // Send EOT
  NspireUART.write(EOT_CHAR);
  NspireUART.flush();

  Serial.println("Sent response");
  client.stop();
}

// ============================================================================
// Power Management
// ============================================================================

void enterLightSleep() {
  Serial.println("Entering light sleep...");
  Serial.flush();

  // Configure UART wakeup, use the ESP-IDF UART number
  // HardwareSerial(1) uses UART_NUM_1
  uart_set_wakeup_threshold(UART_NUM_1, UART_WAKEUP_THRESHOLD);
  esp_sleep_enable_uart_wakeup(UART_NUM_1);

  // Enter light sleep, wake on UART RX activity
  esp_light_sleep_start();

  Serial.println("Woke from light sleep");

  // May not be necessary but I don't feel like finding out
  delay(10);

  // Flush any corrupted wakeup data
  while (NspireUART.available()) {
    NspireUART.read();
  }

  // Wait for WiFi to wake from modem sleep and reconnect if needed
  Serial.print("Waiting for WiFi...");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 5000) {
    delay(100);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK");
  } else {
    Serial.println(" Reconnecting...");
    WiFi.reconnect();
    wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
      delay(100);
    }
    Serial.printf("WiFi %s\n",
                  WiFi.status() == WL_CONNECTED ? "reconnected" : "failed");
  }

  NspireUART.print("AWAKE\n");
  NspireUART.flush();
  Serial.println("Sent AWAKE");

  lastActivityTime = millis();
}

// ============================================================================
// Main loop
// ============================================================================

void loop() {
  if (!handshakeComplete) {
    handleHandshake();
    return;
  }

  if (millis() - lastActivityTime > IDLE_SLEEP_TIMEOUT_MS) {
    enterLightSleep();
  }

  while (NspireUART.available()) {
    char c = NspireUART.read();
    lastActivityTime = millis(); // Reset idle timer on any UART activity

    if (c == '\n') {
      requestBuffer[reqIdx] = '\0';

      if (strcmp(requestBuffer, "SYNC") == 0) {
        NspireUART.print("READY\n");
        NspireUART.flush();
      } else if (strcmp(requestBuffer, "RST") == 0) {
        ESP.restart();
      } else if (strncmp(requestBuffer, "DBG:", 4) == 0) {
        // Debug message from Nspire - print to Serial monitor
        Serial.println(requestBuffer);
      } else if (reqIdx > 0 && requestBuffer[0] == '{') {
        sendApiRequest(requestBuffer);
      }

      reqIdx = 0;
    } else if (c != '\r' && reqIdx < MAX_REQUEST_SIZE - 1) {
      requestBuffer[reqIdx++] = c;
    }
  }
}
