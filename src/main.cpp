#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "ESP32FtpServer.h"
#include <kodedot/display_manager.h>
#include <lvgl.h>
#include <TCA9555.h>
#include <kodedot/pin_config.h>
#include <esp_task_wdt.h> // Watchdog

int clk = 6;
int cmd = 5;
int d0  = 7;

extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30;

FtpServer ftpSrv;
DisplayManager display;
static TCA9555 ioexp(IOEXP_I2C_ADDR);

static bool is_screen_on = true;
static uint32_t last_button_press_time = 0;
static uint32_t last_brightness_change = 0;
static const uint32_t DEBOUNCE_DELAY_MS = 200;
static const uint32_t BRIGHTNESS_DELAY_MS = 150;

lv_obj_t* status_label;

void handle_buttons();
void ui_show_connecting();
void ui_show_connected(const char* ip, int port, const char* user, const char* pass);
void ui_update_status(const char* msg);
void loadWifiConfig();
static inline bool isPressed(uint8_t pinIndex);

// === CORE 1 TASK: FTP SERVER ===
void ftpTask(void* pv) {
    while (1) {
        ftpSrv.handleFTP();
        vTaskDelay(pdMS_TO_TICKS(4)); // ~250 Hz, WDT-safe
    }
}

// === UI TASKS (Core 0) ===
void ui_show_connecting() {
    lv_obj_clean(lv_scr_act());
    lv_obj_t* cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, LV_PCT(80), LV_PCT(50));
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    lv_obj_t* spinner = lv_spinner_create(cont);
    lv_obj_set_size(spinner, 64, 64);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0xFF7F1F), LV_PART_INDICATOR);

    status_label = lv_label_create(cont);
    lv_obj_set_style_text_font(status_label, &Inter_20, 0);
    lv_label_set_text(status_label, "Connecting to Wi-Fi...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
}

void ui_show_connected(const char* ip, int port, const char* user, const char* pass) {
    lv_obj_clean(lv_scr_act());

    char qr_data[128];
    snprintf(qr_data, sizeof(qr_data), "ftp://%s:%s@%s:%d", user, pass, ip, port);
    
    lv_obj_t* qr = lv_qrcode_create(lv_scr_act());
    lv_qrcode_set_size(qr, 250);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0xf79c89));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_update(qr, qr_data, strlen(qr_data));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 40);
    
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_outline_width(qr, 2, 0);
    lv_obj_set_style_outline_color(qr, lv_color_black(), 0);

    lv_obj_t* info_label = lv_label_create(lv_scr_act());
    lv_label_set_text_fmt(info_label,
        "FTP SERVER #5f9c79 READY#\n\n"
        "IP: #304738 %s#\n"
        "Port: #2c6343 %d#\n"
        "User: #769c86 %s#\n"
        "Pass: #8eb19d %s#",
        ip, port, user, pass
    );
    lv_label_set_recolor(info_label, true);
    lv_obj_set_style_text_align(info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(info_label, &Inter_20, 0);
    lv_obj_align(info_label, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void ui_update_status(const char* msg) {
    if (status_label) {
        lv_label_set_text(status_label, msg);
        display.update();
    }
}

void loadWifiConfig() {
    const char* configPath = "/Wi-Fi.json";
    if (!SD_MMC.exists(configPath)) {
        ui_update_status("Error: Wi-Fi.json\nnot found!");
        Serial.printf("Config Error: %s not found!\n", configPath);
        delay(2000);
        return;
    }

    File file = SD_MMC.open(configPath, "r");
    if (!file) {
        ui_update_status("Error: Cannot open\nWi-Fi.json");
        Serial.println("Config Error: Could not open Wi-Fi.json");
        return;
    }

    // 🔑 Use StaticJsonDocument to avoid heap frag + limit size
    const size_t CAPACITY = JSON_OBJECT_SIZE(2) * 5 + JSON_ARRAY_SIZE(5) + 200;
    StaticJsonDocument<CAPACITY> doc;

    DeserializationError error = deserializeJson(doc, file);
    file.close(); // Safe: doc is static, owns its memory

    if (error) {
        Serial.print("JSON Error: ");
        Serial.println(error.c_str());
        ui_update_status("Error: JSON Parse");
        return;
    }

    JsonArray networks = doc.as<JsonArray>();
    for (JsonObject net : networks) {
        const char* ssid = net["ssid"];
        const char* pass = net["pass"];

        if (!ssid || !pass) continue; // 🔑 Null-check

        Serial.printf("Connecting to: %s\n", ssid);
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to:\n%s", ssid);
        ui_update_status(msg);

        WiFi.begin(ssid, pass);

        unsigned long startAttempt = millis();
        const unsigned long CONNECT_TIMEOUT = 8000;

        while (WiFi.status() != WL_CONNECTED && (millis() - startAttempt) < CONNECT_TIMEOUT) {
            display.update();
            handle_buttons();
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("Success!");
            Serial.print("IP Address: ");
            Serial.println(WiFi.localIP());
            return;
        } else {
            Serial.println("Failed, trying next...");
            WiFi.disconnect();
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    ui_update_status("All connections\nfailed!");
}
static inline bool isPressed(uint8_t pinIndex) {
    int v = ioexp.read1(pinIndex);
    return (v != TCA9555_INVALID_READ) && (v == LOW);
}

void handle_buttons() {
    if (millis() - last_button_press_time < DEBOUNCE_DELAY_MS) return;

    bool left_pressed = isPressed(EXPANDER_PAD_LEFT);
    bool right_pressed = isPressed(EXPANDER_PAD_RIGHT);
    bool down_pressed = isPressed(EXPANDER_BUTTON_BOTTOM);

    if ((left_pressed || right_pressed) && (millis() - last_brightness_change < BRIGHTNESS_DELAY_MS)) return;

    if (left_pressed) {
        uint8_t current_pct = display.getBrightnessPercentage();
        int new_pct = current_pct - 5;
        if (new_pct < 1) new_pct = 1;
        uint8_t new_level_255 = (uint8_t)((new_pct / 100.0f) * 255.0f);
        display.setBrightness(new_level_255);
        last_button_press_time = millis();
        last_brightness_change = millis();
    } else if (right_pressed) {
        uint8_t current_pct = display.getBrightnessPercentage();
        int new_pct = current_pct + 5;
        if (new_pct > 100) new_pct = 100;
        uint8_t new_level_255 = (uint8_t)((new_pct / 100.0f) * 255.0f);
        display.setBrightness(new_level_255);
        last_button_press_time = millis();
        last_brightness_change = millis();
    } else if (down_pressed) {
        is_screen_on = !is_screen_on;
        if (is_screen_on) display.getGfx()->displayOn();
        else display.getGfx()->displayOff();
        last_button_press_time = millis();
    }
}

void setup() {
    Serial.begin(115200);
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = 5000,
        .idle_core_mask = 0,  // Watch Core 0 only
        .trigger_panic = true
    };
    esp_task_wdt_reconfigure(&wdt_config);  // Reconfigure if already running
    esp_task_wdt_add(NULL);  // Add current task (loop) to WDT
    // In loop() (Core 0 only):
    esp_task_wdt_reset();  // ✅ This *is* available in Arduino-ESP32
    display.init();
    ui_show_connecting();
    display.update();

    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander not found!");
        ui_update_status("Error:\nIO Expander");
    } else {
        Serial.println("IO Expander initialized.");
    }

    Serial.println("\nBooting Kode Dot FTP...");
    if (!SD_MMC.setPins(clk, cmd, d0)) {
        Serial.println("SD Pin setup failed!");
        ui_update_status("SD Pin Error");
        return;
    }

    if (!SD_MMC.begin("/sdcard", 1)) {
        Serial.println("SD Mount Failed!");
        ui_update_status("SD Mount Failed");
        return;
    }
    Serial.println("SD Mounted.");

    loadWifiConfig();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi Connected. Starting FTP Server on Core 1.");

        const char* user = "kode";
        const char* pass = "kode";
        ftpSrv.begin(user, pass);

        // 🔑 LAUNCH FTP ON CORE 1
        xTaskCreatePinnedToCore(
            ftpTask,          // Task function
            "FTP_Server",     // Name
            8192,             // Stack size (8KB)
            nullptr,          // Parameters
            2,                // Priority (lower than UI)
            nullptr,          // Task handle
            1                 // Core 1
        );

        ui_show_connected(WiFi.localIP().toString().c_str(), 21, user, pass);
    } else {
        Serial.println("WiFi connection failed. FTP disabled.");
        ui_update_status("WiFi Failed\nFTP Disabled");
    }
}

void loop() {
    esp_task_wdt_reset();
    // 🔑 CORE 0: UI ONLY — no FTP here!
    display.update();
    handle_buttons();
    vTaskDelay(pdMS_TO_TICKS(2)); // ~500 Hz, smooth UI
}