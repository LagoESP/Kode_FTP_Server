#include <Arduino.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "ESP32FtpServer.h"
#include <kodedot/display_manager.h>
#include <lvgl.h>

// --- Includes para el IO Expander y botones ---
#include <TCA9555.h>
#include <kodedot/pin_config.h>


/* ───────── KODE DOT SD PINS ───────── */
int clk = 6;    /* Clock pin (CLK) */
int cmd = 5;    /* Command pin (CMD) */
int d0  = 7;    /* Data0 pin (D0) */

// --- External Fonts from src/fonts/ ---
extern const lv_font_t Inter_20;
extern const lv_font_t Inter_30; 

FtpServer ftpSrv;
DisplayManager display;

// --- Instancia del IO Expander (GLOBAL) ---
static TCA9555 ioexp(IOEXP_I2C_ADDR);

// --- Variables de estado de botones y pantalla ---
static bool is_screen_on = true;
static uint32_t last_button_press_time = 0;
static const uint32_t DEBOUNCE_DELAY_MS = 200; // 200ms de antirrebote
static const uint32_t GUI_LOOP_DELAY_MS = 5; // Delay del loop principal

// UI Components
lv_obj_t* status_label;

/* --- UI Helper Functions --- */

void ui_show_connecting() {
    lv_obj_clean(lv_scr_act()); // Clear screen
    
    // Create a container for alignment
    lv_obj_t * cont = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont, LV_PCT(80), LV_PCT(50));
    lv_obj_center(cont);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    
    // Spinner
    lv_obj_t * spinner = lv_spinner_create(cont);
    lv_obj_set_size(spinner, 64, 64);
    // CORREGIDO: Sintaxis de LVGL v9 para eliminar la advertencia
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0xFF7F1F), LV_PART_INDICATOR);

    
    // Text
    status_label = lv_label_create(cont);
    // Use Inter font here too
    lv_obj_set_style_text_font(status_label, &Inter_20, 0);
    lv_label_set_text(status_label, "Connecting to Wi-Fi...");
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
}

void ui_show_connected(const char* ip, int port, const char* user, const char* pass) {
    lv_obj_clean(lv_scr_act()); // Clear screen

    // 1. Create QR Code
    // URI Format: ftp://user:pass@IP:PORT
    char qr_data[128];
    snprintf(qr_data, sizeof(qr_data), "ftp://%s:%s@%s:%d", user, pass, ip, port);
    
    // QR Widget (200px size)
    lv_obj_t * qr = lv_qrcode_create(lv_scr_act());
    lv_qrcode_set_size(qr, 200);
    lv_color_t lv_color_light = lv_color_hex(0xFFFFFF); // White background
    lv_color_t lv_color_dark = lv_color_hex(0xf79c89); // Kode Dot Orange
    lv_qrcode_set_dark_color(qr, lv_color_dark);
    lv_qrcode_set_light_color(qr, lv_color_light);
    lv_qrcode_update(qr, qr_data, strlen(qr_data));
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 40);
    
    // 2. Border/Background for QR to make it pop
    lv_obj_set_style_border_width(qr, 5, 0);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_outline_width(qr, 2, 0);
    lv_obj_set_style_outline_color(qr, lv_color_black(), 0);

    // 3. Info Text
    lv_obj_t * info_label = lv_label_create(lv_scr_act());
    lv_label_set_text_fmt(info_label, 
        "FTP SERVER #5f9c79 READY#\n\n"
        "IP: #304738 %s#\n"
        "Port: #2c6343 %d#\n"
        "User: #769c86 %s#\n"
        "Pass: #8eb19d %s#",
        ip, port, user, pass
    );
    
    // Style the text with Inter Font
    lv_label_set_recolor(info_label, true); // Enable inline color parsing
    lv_obj_set_style_text_align(info_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // CHANGED: Switch from montserrat to Inter
    lv_obj_set_style_text_font(info_label, &Inter_20, 0); 
    
    lv_obj_align(info_label, LV_ALIGN_BOTTOM_MID, 0, -40);
}

void ui_update_status(const char* msg) {
    if (status_label) {
        lv_label_set_text(status_label, msg);
        display.update(); // Force redraw immediately
    }
}

/* --- System Logic --- */

void loadWifiConfig() {
    // CAMBIO: Ruta ajustada a la raíz, según tu petición
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

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

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

        Serial.printf("Connecting to: %s\n", ssid);
        
        // Update UI
        char msg[64];
        snprintf(msg, sizeof(msg), "Connecting to:\n%s", ssid);
        ui_update_status(msg);

        WiFi.begin(ssid, pass);

        unsigned long startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
            display.update(); // Keep UI alive during wait
            delay(10);
        }

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
    ui_update_status("All connections\nfailed!");
}

static inline bool isPressed(uint8_t pinIndex) {
    // Entradas con pull-up externa: activo en LOW
    int v = ioexp.read1(pinIndex);
    return (v != TCA9555_INVALID_READ) && (v == LOW);
}

/**
 * @brief Lee los botones del IO expander y actúa.
 */
void handle_buttons() {
    // Antirrebote simple: solo permite una acción cada 200ms
    if (millis() - last_button_press_time < DEBOUNCE_DELAY_MS) {
        return;
    }

    // Lee los botones (Activo en BAJO)
    bool left_pressed = isPressed(EXPANDER_PAD_LEFT);
    bool right_pressed = isPressed(EXPANDER_PAD_RIGHT);
    bool down_pressed = isPressed(EXPANDER_BUTTON_BOTTOM);

    if (left_pressed) {
        // --- Brillo Abajo ---
        uint8_t current_pct = display.getBrightnessPercentage();
        // CAMBIO: Ajuste de 5%
        int new_pct = current_pct - 5; 
        if (new_pct < 1) new_pct = 1; // Mínimo 1%

        // Convertir porcentaje (0-100) a nivel (0-255)
        uint8_t new_level_255 = (uint8_t)((new_pct / 100.0f) * 255.0f);
        display.setBrightness(new_level_255);
        
        Serial.printf("Brightness set to %d%% (%d/255)\n", new_pct, new_level_255);
        last_button_press_time = millis();

    } else if (right_pressed) {
        // --- Brillo Arriba ---
        uint8_t current_pct = display.getBrightnessPercentage();
        // CAMBIO: Ajuste de 5%
        int new_pct = current_pct + 5;
        if (new_pct > 100) new_pct = 100; // Máximo 100%

        // Convertir porcentaje (0-100) a nivel (0-255)
        uint8_t new_level_255 = (uint8_t)((new_pct / 100.0f) * 255.0f);
        display.setBrightness(new_level_255);
        
        Serial.printf("Brightness set to %d%% (%d/255)\n", new_pct, new_level_255);
        last_button_press_time = millis();

    } else if (down_pressed) {
        // --- Apagar/Encender Pantalla ---
        is_screen_on = !is_screen_on;
        
        // CORREGIDO: Usar displayOn() y displayOff()
        if (is_screen_on) {
            display.getGfx()->displayOn(); // Encender pantalla
        } else {
            display.getGfx()->displayOff(); // Apagar pantalla
        }
        
        Serial.printf("Screen Toggled: %s\n", is_screen_on ? "ON" : "OFF");
        last_button_press_time = millis();
    }
}

void setup() {
    Serial.begin(115200);
    
    // 1. Init Display & LVGL
    Serial.println("Initializing Display...");
    display.init();
    
    // 2. Show Initial UI
    ui_show_connecting();
    display.update();

    // --- AÑADIDO: Inicializar IO Expander ---
    // Usar el objeto 'ioexp' global
    
    // SOLUCIÓN: Evitar colisión de macros. Arduino define INPUT como 0x01,
    // lo que interfiere con TCA9555::INPUT.
    if (!ioexp.begin(INPUT)) {
        Serial.println("Warning: IO Expander (TCA9555) not found!");
        ui_update_status("Error:\nIO Expander");
    } else {
        Serial.println("IO Expander initialized.");
    }
    // Restaurar la macro de Arduino por si se necesita después


    // 3. Init SD Card
    Serial.println("\nBooting Kode Dot FTP...");
    if (!SD_MMC.setPins(clk, cmd, d0)) {
        Serial.println("Pin change failed!");
        ui_update_status("SD Pin Error");
        return;
    }
    // NOTA: Montas en /sdcard, pero accedes a Wi-Fi.json en /
    // Asegúrate de que esto sea correcto. El FTP servirá desde /
    if (!SD_MMC.begin("/sdcard", 1)) {
        Serial.println("Card Mount Failed!");
        ui_update_status("SD Mount Failed\nCheck Card");
        return;
    }
    Serial.println("SD Mounted.");
    
    // 4. Connect Wi-Fi
    loadWifiConfig(); // Esta función ahora usa la ruta /Wi-Fi.json

    // 5. Start FTP & Update UI
    if (WiFi.status() == WL_CONNECTED) {
        const char* user = "kode";
        const char* pass = "kode";
        
        Serial.println("Starting FTP Server...");
        ftpSrv.begin(user, pass);
        Serial.println("FTP Ready!");
        
        // Show final screen with QR
        ui_show_connected(WiFi.localIP().toString().c_str(), 21, user, pass);
    }
}

void loop() {
    ftpSrv.handleFTP();
    display.update();   // Handle LVGL tasks
    handle_buttons(); // Handle button inputs
    delay(GUI_LOOP_DELAY_MS); // Añadir un pequeño delay
}