#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "ESP32FtpServer.h"
#include <kodedot/display_manager.h>

/* ───────── KODE DOT SD PINS ───────── */
int clk = 6;    /* Clock pin (CLK) */
int cmd = 5;    /* Command pin (CMD) */
int d0  = 7;    /* Data0 pin (D0) */

FtpServer ftpSrv;

void loadWifiConfig() {
  // FIX: Since we mount at "/sdcard", the file path MUST start with "/sdcard"
  const char* configPath = "/Wi-Fi.json";

  if (!SD_MMC.exists(configPath)) {
    Serial.printf("Config Error: %s not found!\n", configPath);
    return;
  }

  File file = SD_MMC.open(configPath, "r");
  if (!file) {
    Serial.println("Config Error: Could not open Wi-Fi.json");
    return;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.print("JSON Error: ");
    Serial.println(error.c_str());
    return;
  }

  JsonArray networks = doc.as<JsonArray>();
  for (JsonObject net : networks) {
    const char* ssid = net["ssid"];
    const char* pass = net["pass"];

    Serial.printf("Connecting to: %s\n", ssid);
    WiFi.begin(ssid, pass);

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Success!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      return; 
    } else {
      Serial.println("Failed, trying next...");
      WiFi.disconnect();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nBooting Kode Dot FTP...");

  if (!SD_MMC.setPins(clk, cmd, d0)) {
    Serial.println("Pin change failed!");
    return;
  }

  // Mount at /sdcard
  if (!SD_MMC.begin("/sdcard", 1)) {
    Serial.println("Card Mount Failed! (Check hardware/pull-ups)");
    return;
  }

  Serial.println("SD Mounted.");

  loadWifiConfig();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Starting FTP Server...");
    ftpSrv.begin("kode", "kode");
    Serial.println("FTP Ready!");
  }
}

void loop() {
  ftpSrv.handleFTP();
}