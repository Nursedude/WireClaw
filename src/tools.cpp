/**
 * @file tools.cpp
 * @brief ESP32 tool definitions and handlers for LLM tool calling
 */

#include "tools.h"
#include "llm_client.h"
#include "devices.h"
#include "nats_hal.h"
#include "rules.h"
#include "display.h"
#include "lora_ears.h"
#include "ble_scan.h"
#include "anomaly.h"
#include <Arduino.h>
#include <WiFi.h>
#include "soc/soc_caps.h"
#include <LittleFS.h>
#include <nats_esp32.h>
#include <esp_task_wdt.h>
#if !defined(CONFIG_IDF_TARGET_ESP32)
#include "driver/temperature_sensor.h"
#endif

/* Forward declarations (defined in main.cpp) */
extern void led(uint8_t r, uint8_t g, uint8_t b);
extern void ledOff();
extern bool g_led_user;
extern NatsClient natsClient;
extern bool g_nats_connected;
extern bool g_nats_enabled;
#if !defined(CONFIG_IDF_TARGET_ESP32)
extern temperature_sensor_handle_t g_temp_sensor;
#endif

/* NATS virtual sensor helpers (from main.cpp) */
extern void natsSubscribeDeviceSensors();
extern void natsUnsubscribeDevice(const char *name);

/*============================================================================
 * Simple JSON argument parser (for tool arguments)
 *============================================================================*/

static int jsonArgInt(const char *json, const char *key, int default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    return atoi(p);
}

static bool jsonArgString(const char *json, const char *key,
                            char *dst, int dst_len) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    int w = 0;
    while (*p && *p != '"' && w < dst_len - 1) {
        if (*p == '\\' && *(p + 1)) p++;
        dst[w++] = *p++;
    }
    dst[w] = '\0';
    return w > 0;
}

static bool jsonArgBool(const char *json, const char *key, bool default_val) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return default_val;
}

/* Check if a key exists (even with empty string value or null) */
static bool jsonArgExists(const char *json, const char *key) {
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern) != nullptr;
}

/*============================================================================
 * Tool Definitions (OpenAI function calling format) - compacted
 *============================================================================*/

static const char *TOOLS_JSON = R"JSON([
{"type":"function","function":{"name":"led_set","description":"Set RGB LED 0-255","parameters":{"type":"object","properties":{"r":{"type":"integer"},"g":{"type":"integer"},"b":{"type":"integer"}},"required":["r","g","b"]}}},
{"type":"function","function":{"name":"gpio_write","description":"Set GPIO pin HIGH/LOW","parameters":{"type":"object","properties":{"pin":{"type":"integer"},"value":{"type":"integer"}},"required":["pin","value"]}}},
{"type":"function","function":{"name":"gpio_read","description":"Read GPIO pin state","parameters":{"type":"object","properties":{"pin":{"type":"integer"}},"required":["pin"]}}},
{"type":"function","function":{"name":"device_info","description":"Get heap, uptime, WiFi, chip info","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"file_read","description":"Read file from filesystem","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}}},
{"type":"function","function":{"name":"file_write","description":"Write file to filesystem","parameters":{"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]}}},
{"type":"function","function":{"name":"nats_publish","description":"Publish NATS message","parameters":{"type":"object","properties":{"subject":{"type":"string"},"payload":{"type":"string"}},"required":["subject","payload"]}}},
{"type":"function","function":{"name":"temperature_read","description":"Read chip temperature (C)","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"device_register","description":"Register sensor/actuator","parameters":{"type":"object","properties":{"name":{"type":"string"},"type":{"type":"string","enum":["digital_in","analog_in","ntc_10k","ldr","nats_value","serial_text","digital_out","relay","pwm"],"description":"digital_in: GPIO digital read, analog_in: raw ADC reading, ntc_10k: NTC 10K thermistor (temp in C, set inverted=true if NTC is on the 3.3V side), ldr: light-dependent resistor (light level), nats_value: virtual sensor from NATS subject, serial_text: UART text input, digital_out: GPIO digital write, relay: relay on/off, pwm: PWM output"},"pin":{"type":"integer"},"unit":{"type":"string"},"inverted":{"type":"boolean"},"subject":{"type":"string","description":"NATS subject (for nats_value)"},"baud":{"type":"integer","description":"Baud rate for serial_text (default 9600)"}},"required":["name","type"]}}},
{"type":"function","function":{"name":"device_list","description":"List registered devices","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"device_remove","description":"Remove device by name","parameters":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}}},
{"type":"function","function":{"name":"sensor_read","description":"Read named sensor value","parameters":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}}},
{"type":"function","function":{"name":"actuator_set","description":"Set actuator value","parameters":{"type":"object","properties":{"name":{"type":"string"},"value":{"type":"integer"}},"required":["name","value"]}}},
{"type":"function","function":{"name":"rule_create","description":"Create automation rule. Use chained condition for chain-only targets.","parameters":{"type":"object","properties":{"rule_name":{"type":"string"},"sensor_name":{"type":"string"},"sensor_pin":{"type":"integer"},"condition":{"type":"string","description":"gt|lt|eq|neq|change|always|chained"},"threshold":{"type":"integer"},"interval_seconds":{"type":"integer"},"actuator_name":{"type":"string"},"on_action":{"type":"string","description":"gpio_write|led_set|nats_publish|actuator|telegram|serial_send"},"on_pin":{"type":"integer"},"on_value":{"type":"integer"},"on_r":{"type":"integer"},"on_g":{"type":"integer"},"on_b":{"type":"integer"},"on_nats_subject":{"type":"string"},"on_nats_payload":{"type":"string"},"on_telegram_message":{"type":"string","description":"Use {value} or {device_name}"},"on_serial_text":{"type":"string","description":"Text to send via serial_text UART"},"off_action":{"type":"string","description":"auto|none|gpio_write|led_set|nats_publish|actuator|telegram|serial_send"},"off_pin":{"type":"integer"},"off_value":{"type":"integer"},"off_r":{"type":"integer"},"off_g":{"type":"integer"},"off_b":{"type":"integer"},"off_nats_subject":{"type":"string"},"off_nats_payload":{"type":"string"},"off_telegram_message":{"type":"string"},"off_serial_text":{"type":"string","description":"Text for serial off-action"},"chain_rule":{"type":"string","description":"Rule ID to trigger after ON action (e.g. rule_01)"},"chain_delay_seconds":{"type":"integer","description":"Delay before ON chain fires (0=immediate)"},"chain_off_rule":{"type":"string","description":"Rule ID to trigger after OFF action"},"chain_off_delay_seconds":{"type":"integer","description":"Delay before OFF chain fires (0=immediate)"}},"required":["rule_name"]}}},
{"type":"function","function":{"name":"rule_list","description":"List all rules","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"rule_delete","description":"Delete rule by ID (e.g. rule_01), or pass 'all' to delete every rule at once.","parameters":{"type":"object","properties":{"rule_id":{"type":"string","description":"Rule ID or 'all'"}},"required":["rule_id"]}}},
{"type":"function","function":{"name":"rule_enable","description":"Enable/disable rule","parameters":{"type":"object","properties":{"rule_id":{"type":"string"},"enabled":{"type":"boolean"}},"required":["rule_id","enabled"]}}},
{"type":"function","function":{"name":"serial_send","description":"Send text over serial_text UART","parameters":{"type":"object","properties":{"text":{"type":"string","description":"Text to send (newline appended)"}},"required":["text"]}}},
{"type":"function","function":{"name":"remote_chat","description":"Chat with another WireClaw device via NATS","parameters":{"type":"object","properties":{"device":{"type":"string"},"message":{"type":"string"}},"required":["device","message"]}}},
{"type":"function","function":{"name":"display_print","description":"Write a metric line to the OLED status screen (boards with a display). Empty text clears the row.","parameters":{"type":"object","properties":{"row":{"type":"integer","description":"0-based metric row"},"text":{"type":"string","description":"Up to 23 chars; empty clears"}},"required":["row"]}}},
{"type":"function","function":{"name":"display_tier","description":"Set the brain-tier glyph on the OLED status screen: F=frontier, L=local LLM, R=rules-only. The device decays it on its own clock when pushes stop (15 min adds '?', 30 min becomes SOLO).","parameters":{"type":"object","properties":{"tier":{"type":"string","description":"F, L or R"}},"required":["tier"]}}},
{"type":"function","function":{"name":"display_alert","description":"Show an inverted alert banner (signal class + locally ticking age) on the OLED status page. Empty class clears it; nothing else does.","parameters":{"type":"object","properties":{"class":{"type":"string","description":"Signal class, up to 23 chars; empty clears"},"age_s":{"type":"integer","description":"Alert age in seconds at push time"}}}}},
{"type":"function","function":{"name":"battery_read","description":"Read battery voltage in volts (boards with a VBAT divider)","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"lora_stats","description":"RX-only LoRa listener stats: seconds since last mesh packet heard, packet/CRC counters, last header fields (boards with an SX1262)","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"mesh_send","description":"Broadcast a text message onto the Meshtastic LoRa channel (boards with an SX1262 and TX built). Airtime-limited.","parameters":{"type":"object","properties":{"text":{"type":"string","description":"Message text (<=200 chars)"}},"required":["text"]}}},
{"type":"function","function":{"name":"mesh_set_channel","description":"Set the LoRa TX channel name and PSK at runtime (RAM only; the key never lands in flash). Returns the resulting channel hash.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Channel name"},"psk":{"type":"string","description":"16/32-byte key, hex or base64; empty = public default"}},"required":["name"]}}},
{"type":"function","function":{"name":"ble_stats","description":"Passive BLE advertisement listener stats: seconds since last advert heard, advert/unique-device counters, last RSSI (boards with BLE built)","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"anomaly_stats","description":"Edge anomaly witness: max |z| across self-learned telemetry baselines (heap/temp/RSSI/rx-rate), leading with the score; -1 while the baseline is still learning (anomaly builds)","parameters":{"type":"object","properties":{}}}},
{"type":"function","function":{"name":"chain_create","description":"Create multi-step automation chain (up to 5 steps) in one call. Steps execute in order with delays.","parameters":{"type":"object","properties":{"sensor_name":{"type":"string","description":"Sensor to monitor"},"condition":{"type":"string","description":"gt|lt|eq|neq|change|always"},"threshold":{"type":"integer"},"interval_seconds":{"type":"integer"},"step1_action":{"type":"string","description":"telegram|led_set|gpio_write|nats_publish|actuator|serial_send"},"step1_message":{"type":"string","description":"For telegram/nats/serial_send"},"step1_r":{"type":"integer"},"step1_g":{"type":"integer"},"step1_b":{"type":"integer"},"step1_pin":{"type":"integer"},"step1_value":{"type":"integer"},"step1_actuator":{"type":"string"},"step1_nats_subject":{"type":"string"},"step2_action":{"type":"string","description":"Action after step1"},"step2_delay":{"type":"integer","description":"Seconds before step2"},"step2_message":{"type":"string"},"step2_r":{"type":"integer"},"step2_g":{"type":"integer"},"step2_b":{"type":"integer"},"step2_pin":{"type":"integer"},"step2_value":{"type":"integer"},"step2_actuator":{"type":"string"},"step2_nats_subject":{"type":"string"},"step3_action":{"type":"string","description":"Step3 (optional)"},"step3_delay":{"type":"integer","description":"Seconds before step3"},"step3_message":{"type":"string"},"step3_r":{"type":"integer"},"step3_g":{"type":"integer"},"step3_b":{"type":"integer"},"step3_pin":{"type":"integer"},"step3_value":{"type":"integer"},"step3_actuator":{"type":"string"},"step3_nats_subject":{"type":"string"},"step4_action":{"type":"string","description":"Step4 (optional)"},"step4_delay":{"type":"integer","description":"Seconds before step4"},"step4_message":{"type":"string"},"step4_r":{"type":"integer"},"step4_g":{"type":"integer"},"step4_b":{"type":"integer"},"step4_pin":{"type":"integer"},"step4_value":{"type":"integer"},"step4_actuator":{"type":"string"},"step4_nats_subject":{"type":"string"},"step5_action":{"type":"string","description":"Step5 (optional)"},"step5_delay":{"type":"integer","description":"Seconds before step5"},"step5_message":{"type":"string"},"step5_r":{"type":"integer"},"step5_g":{"type":"integer"},"step5_b":{"type":"integer"},"step5_pin":{"type":"integer"},"step5_value":{"type":"integer"},"step5_actuator":{"type":"string"},"step5_nats_subject":{"type":"string"}},"required":["sensor_name","condition","threshold","step1_action","step2_action"]}}}
])JSON";

/*============================================================================
 * Original Tool Handlers
 *============================================================================*/

static void tool_led_set(const char *args, char *result, int result_len) {
    int r = jsonArgInt(args, "r", 0);
    int g = jsonArgInt(args, "g", 0);
    int b = jsonArgInt(args, "b", 0);

    r = constrain(r, 0, 255);
    g = constrain(g, 0, 255);
    b = constrain(b, 0, 255);

    led((uint8_t)r, (uint8_t)g, (uint8_t)b);
    g_led_user = true;
    Device *rgb = deviceFind("rgb_led");
    if (rgb) rgb->last_value = (r << 16) | (g << 8) | b;
    snprintf(result, result_len, "LED set to RGB(%d, %d, %d)", r, g, b);
}

static void tool_gpio_write(const char *args, char *result, int result_len) {
    int pin = jsonArgInt(args, "pin", -1);
    int value = jsonArgInt(args, "value", 0);

    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
        snprintf(result, result_len, "Error: invalid pin %d (must be 0-%d)", pin, SOC_GPIO_PIN_COUNT - 1);
        return;
    }

    pinMode(pin, OUTPUT);
    digitalWrite(pin, value ? HIGH : LOW);
    snprintf(result, result_len, "GPIO %d set to %s", pin, value ? "HIGH" : "LOW");
}

static void tool_gpio_read(const char *args, char *result, int result_len) {
    int pin = jsonArgInt(args, "pin", -1);

    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) {
        snprintf(result, result_len, "Error: invalid pin %d (must be 0-%d)", pin, SOC_GPIO_PIN_COUNT - 1);
        return;
    }

    int value = digitalRead(pin);
    snprintf(result, result_len, "GPIO %d = %d (%s)", pin, value,
             value ? "HIGH" : "LOW");
}

static void tool_device_info(const char *args, char *result, int result_len) {
    (void)args;
    snprintf(result, result_len,
        "Free heap: %u bytes, Total heap: %u bytes, "
        "Uptime: %lu seconds, "
        "WiFi: %s (rssi %d dBm), IP: %s, "
        "Chip: %s rev %d, %d cores, %lu MHz",
        ESP.getFreeHeap(), ESP.getHeapSize(),
        millis() / 1000,
        WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
        (int)WiFi.RSSI(),
        WiFi.localIP().toString().c_str(),
        ESP.getChipModel(), ESP.getChipRevision(),
        ESP.getChipCores(), ESP.getCpuFreqMHz());
}

static void tool_file_read(const char *args, char *result, int result_len) {
    char path[64];
    if (!jsonArgString(args, "path", path, sizeof(path))) {
        snprintf(result, result_len, "Error: missing 'path' argument");
        return;
    }

    File f = LittleFS.open(path, "r");
    if (!f) {
        snprintf(result, result_len, "Error: file not found: %s", path);
        return;
    }

    int len = f.readBytes(result, result_len - 1);
    result[len] = '\0';
    f.close();
}

static void tool_file_write(const char *args, char *result, int result_len) {
    char path[64];
    char content[512];

    if (!jsonArgString(args, "path", path, sizeof(path))) {
        snprintf(result, result_len, "Error: missing 'path' argument");
        return;
    }

    /* Protect config.json from being overwritten */
    if (strcmp(path, "/config.json") == 0) {
        snprintf(result, result_len, "Error: cannot overwrite config.json via tool");
        return;
    }

    if (!jsonArgString(args, "content", content, sizeof(content))) {
        snprintf(result, result_len, "Error: missing 'content' argument");
        return;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        snprintf(result, result_len, "Error: cannot open %s for writing", path);
        return;
    }

    f.print(content);
    f.close();
    snprintf(result, result_len, "Wrote %d bytes to %s", (int)strlen(content), path);
}

static void tool_nats_publish(const char *args, char *result, int result_len) {
    if (!g_nats_connected) {
        snprintf(result, result_len, "Error: NATS not connected");
        return;
    }

    char subject[128];
    char payload[256];

    if (!jsonArgString(args, "subject", subject, sizeof(subject))) {
        snprintf(result, result_len, "Error: missing 'subject' argument");
        return;
    }

    if (!jsonArgString(args, "payload", payload, sizeof(payload))) {
        snprintf(result, result_len, "Error: missing 'payload' argument");
        return;
    }

    nats_err_t err = natsClient.publish(subject, payload);
    if (err == NATS_OK) {
        snprintf(result, result_len, "Published to %s: %s", subject, payload);
    } else {
        snprintf(result, result_len, "Error: publish failed: %s",
                 nats_err_str(err));
    }
}

static void tool_temperature_read(const char *args, char *result, int result_len) {
    (void)args;
#if !defined(CONFIG_IDF_TARGET_ESP32)
    float temp = 0.0f;
    if (g_temp_sensor != NULL) {
        temperature_sensor_get_celsius(g_temp_sensor, &temp);
    }
    snprintf(result, result_len, "Chip temperature: %.1f C", temp);
#else
    snprintf(result, result_len, "Error: temperature sensor not available on this chip");
#endif
}

/*============================================================================
 * Device Registry Tool Handlers
 *============================================================================*/

static void tool_device_register(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    char type_str[24];

    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }
    if (!jsonArgString(args, "type", type_str, sizeof(type_str))) {
        snprintf(result, result_len, "Error: missing 'type'");
        return;
    }

    int pin = jsonArgInt(args, "pin", PIN_NONE);

    char unit[DEV_UNIT_LEN] = "";
    jsonArgString(args, "unit", unit, sizeof(unit));
    bool inverted = jsonArgBool(args, "inverted", false);

    char subject[32] = "";
    jsonArgString(args, "subject", subject, sizeof(subject));

    /* Map type string to DeviceKind */
    DeviceKind kind;
    if (strcmp(type_str, "digital_in") == 0)         kind = DEV_SENSOR_DIGITAL;
    else if (strcmp(type_str, "analog_in") == 0)     kind = DEV_SENSOR_ANALOG_RAW;
    else if (strcmp(type_str, "ntc_10k") == 0)       kind = DEV_SENSOR_NTC_10K;
    else if (strcmp(type_str, "ldr") == 0)            kind = DEV_SENSOR_LDR;
    else if (strcmp(type_str, "nats_value") == 0)     kind = DEV_SENSOR_NATS_VALUE;
    else if (strcmp(type_str, "serial_text") == 0)   kind = DEV_SENSOR_SERIAL_TEXT;
    else if (strcmp(type_str, "digital_out") == 0)   kind = DEV_ACTUATOR_DIGITAL;
    else if (strcmp(type_str, "relay") == 0)          kind = DEV_ACTUATOR_RELAY;
    else if (strcmp(type_str, "pwm") == 0)            kind = DEV_ACTUATOR_PWM;
    else {
        snprintf(result, result_len, "Error: unknown type '%s'", type_str);
        return;
    }

    /* Validate: nats_value requires subject, serial_text enforces one-device limit */
    if (kind == DEV_SENSOR_NATS_VALUE) {
        if (subject[0] == '\0') {
            snprintf(result, result_len, "Error: nats_value requires 'subject'");
            return;
        }
        if (!g_nats_enabled) {
            snprintf(result, result_len, "Warning: NATS not enabled. Registered but won't receive data");
        }
        pin = PIN_NONE;
    } else if (kind == DEV_SENSOR_SERIAL_TEXT) {
        /* Only one serial_text device allowed */
        Device *devs = deviceGetAll();
        for (int i = 0; i < MAX_DEVICES; i++) {
            if (devs[i].used && devs[i].kind == DEV_SENSOR_SERIAL_TEXT) {
                snprintf(result, result_len, "Error: only one serial_text device allowed (already: '%s')",
                         devs[i].name);
                return;
            }
        }
        pin = PIN_NONE;
    } else if (pin == PIN_NONE && deviceIsActuator(kind)) {
        snprintf(result, result_len, "Error: actuator requires 'pin'");
        return;
    }

    int baud_rate = jsonArgInt(args, "baud", 9600);

    if (halIsReservedName(name)) {
        snprintf(result, result_len, "Error: '%s' is a reserved HAL keyword", name);
        return;
    }

    if (!deviceRegister(name, kind, (uint8_t)pin, unit, inverted,
                        subject[0] ? subject : nullptr,
                        kind == DEV_SENSOR_SERIAL_TEXT ? (uint32_t)baud_rate : 0)) {
        snprintf(result, result_len, "Error: register failed (duplicate name or full)");
        return;
    }

    devicesSave();

    if (kind == DEV_SENSOR_NATS_VALUE) {
        natsSubscribeDeviceSensors();
        snprintf(result, result_len, "Registered nats_value sensor '%s' on subject '%s'",
                 name, subject);
    } else if (kind == DEV_SENSOR_SERIAL_TEXT) {
        snprintf(result, result_len, "Registered serial_text sensor '%s' at %d baud (RX=%d TX=%d)",
                 name, baud_rate, SERIAL_TEXT_RX, SERIAL_TEXT_TX);
    } else {
        snprintf(result, result_len, "Registered %s '%s' on pin %d",
                 deviceIsSensor(kind) ? "sensor" : "output", name, pin);
    }
}

static void tool_device_list(const char *args, char *result, int result_len) {
    (void)args;
    Device *devs = deviceGetAll();
    int w = 0;
    int count = 0;

    for (int i = 0; i < MAX_DEVICES && w < result_len - 40; i++) {
        if (!devs[i].used) continue;
        Device *d = &devs[i];

        if (count > 0) w += snprintf(result + w, result_len - w, "; ");

        if (d->kind == DEV_SENSOR_SERIAL_TEXT) {
            float val = deviceReadSensor(d);
            const char *msg = serialTextGetMsg();
            w += snprintf(result + w, result_len - w, "%s(serial_text %ubaud)=%.1f%s",
                         d->name, (unsigned)d->baud, val, d->unit);
            if (msg[0]) {
                w += snprintf(result + w, result_len - w, " msg='%.20s'", msg);
            }
        } else if (d->kind == DEV_SENSOR_NATS_VALUE) {
            float val = deviceReadSensor(d);
            w += snprintf(result + w, result_len - w, "%s(nats_value %s)=%.1f%s",
                         d->name, d->nats_subject, val, d->unit);
        } else if (deviceIsSensor(d->kind)) {
            float val = deviceReadSensor(d);
            w += snprintf(result + w, result_len - w, "%s(%s pin%d)=%.1f%s",
                         d->name, deviceKindName(d->kind), d->pin, val, d->unit);
        } else {
            w += snprintf(result + w, result_len - w, "%s(%s pin%d%s)",
                         d->name, deviceKindName(d->kind), d->pin,
                         d->inverted ? " inv" : "");
        }
        count++;
    }

    if (count == 0) {
        snprintf(result, result_len, "No devices registered");
    }
}

static void tool_device_remove(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }

    /* Unsubscribe NATS if this is a nats_value device */
    Device *dev = deviceFind(name);
    if (dev && dev->kind == DEV_SENSOR_NATS_VALUE) {
        natsUnsubscribeDevice(name);
    }

    if (!deviceRemove(name)) {
        snprintf(result, result_len, "Error: device '%s' not found", name);
        return;
    }

    devicesSave();
    snprintf(result, result_len, "Removed device '%s'", name);
}

static void tool_sensor_read(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }

    Device *dev = deviceFind(name);
    if (!dev) {
        snprintf(result, result_len, "Error: sensor '%s' not found", name);
        return;
    }
    if (!deviceIsSensor(dev->kind)) {
        snprintf(result, result_len, "Error: '%s' is not a sensor", name);
        return;
    }

    float val = deviceReadSensor(dev);
    if (dev->kind == DEV_SENSOR_SERIAL_TEXT) {
        const char *msg = serialTextGetMsg();
        if (msg[0])
            snprintf(result, result_len, "%s: %.1f %s (last: '%s')", name, val, dev->unit, msg);
        else
            snprintf(result, result_len, "%s: %.1f %s (no data yet)", name, val, dev->unit);
    } else {
        snprintf(result, result_len, "%s: %.1f %s", name, val, dev->unit);
    }
}

static void tool_actuator_set(const char *args, char *result, int result_len) {
    char name[DEV_NAME_LEN];
    if (!jsonArgString(args, "name", name, sizeof(name))) {
        snprintf(result, result_len, "Error: missing 'name'");
        return;
    }

    int value = jsonArgInt(args, "value", 0);

    Device *dev = deviceFind(name);
    if (!dev) {
        snprintf(result, result_len, "Error: actuator '%s' not found", name);
        return;
    }
    if (!deviceIsActuator(dev->kind)) {
        snprintf(result, result_len, "Error: '%s' is not an actuator", name);
        return;
    }

    if (!deviceSetActuator(dev, value)) {
        snprintf(result, result_len, "Error: failed to set '%s'", name);
        return;
    }

    snprintf(result, result_len, "Set %s to %d", name, value);
}

/*============================================================================
 * Rule Engine Tool Handlers
 *============================================================================*/

static ActionType parseActionType(const char *s) {
    if (strcmp(s, "gpio_write") == 0)   return ACT_GPIO_WRITE;
    if (strcmp(s, "led_set") == 0)      return ACT_LED_SET;
    if (strcmp(s, "nats_publish") == 0) return ACT_NATS_PUBLISH;
    if (strcmp(s, "actuator") == 0)     return ACT_ACTUATOR;
    if (strcmp(s, "telegram") == 0)     return ACT_TELEGRAM;
    if (strcmp(s, "serial_send") == 0)  return ACT_SERIAL_SEND;
    return ACT_GPIO_WRITE;
}

static ConditionOp parseConditionOp(const char *s) {
    if (strcmp(s, "gt") == 0)     return COND_GT;
    if (strcmp(s, "lt") == 0)     return COND_LT;
    if (strcmp(s, "eq") == 0)     return COND_EQ;
    if (strcmp(s, "neq") == 0)    return COND_NEQ;
    if (strcmp(s, "change") == 0) return COND_CHANGE;
    if (strcmp(s, "always") == 0) return COND_ALWAYS;
    if (strcmp(s, "chained") == 0) return COND_CHAINED;
    return COND_GT;
}

static void tool_rule_create(const char *args, char *result, int result_len) {
    char rule_name[RULE_NAME_LEN];
    if (!jsonArgString(args, "rule_name", rule_name, sizeof(rule_name))) {
        snprintf(result, result_len, "Error: missing 'rule_name'");
        return;
    }

    /* Sensor source: device name or raw pin */
    char sensor_name[DEV_NAME_LEN] = "";
    jsonArgString(args, "sensor_name", sensor_name, sizeof(sensor_name));
    uint8_t sensor_pin = (uint8_t)jsonArgInt(args, "sensor_pin", PIN_NONE);

    char cond_str[16];
    if (!jsonArgString(args, "condition", cond_str, sizeof(cond_str))) {
        /* Default to "chained" if no condition and no sensor source */
        if (!sensor_name[0] && sensor_pin == PIN_NONE) {
            strcpy(cond_str, "chained");
        } else {
            snprintf(result, result_len, "Error: missing 'condition'");
            return;
        }
    }
    ConditionOp condition = parseConditionOp(cond_str);
    int32_t threshold = jsonArgInt(args, "threshold", 0);
    bool sensor_analog = false;

    /* Validate sensor source (not required for COND_CHAINED) */
    if (condition != COND_CHAINED) {
        if (sensor_name[0]) {
            Device *dev = deviceFind(sensor_name);
            if (!dev) {
                snprintf(result, result_len, "Error: sensor '%s' not found in device registry", sensor_name);
                return;
            }
            if (!deviceIsSensor(dev->kind)) {
                snprintf(result, result_len, "Error: '%s' is not a sensor", sensor_name);
                return;
            }
        } else if (sensor_pin == PIN_NONE) {
            snprintf(result, result_len, "Error: provide sensor_name or sensor_pin");
            return;
        }
    }

    int interval_s = jsonArgInt(args, "interval_seconds", 5);
    if (interval_s < 5) interval_s = 5;
    uint32_t interval_ms = (uint32_t)interval_s * 1000;

    /* Determine ON action */
    ActionType on_action = ACT_GPIO_WRITE;
    char on_actuator[DEV_NAME_LEN] = "";
    uint8_t on_pin = 0;
    int32_t on_value = 1;
    char on_nats_subj[RULE_NATS_SUBJ_LEN] = "";
    char on_nats_pay[RULE_NATS_PAY_LEN] = "";

    /* Determine OFF action */
    bool has_off = false;
    ActionType off_action = ACT_GPIO_WRITE;
    char off_actuator[DEV_NAME_LEN] = "";
    uint8_t off_pin = 0;
    int32_t off_value = 0;
    char off_nats_subj[RULE_NATS_SUBJ_LEN] = "";
    char off_nats_pay[RULE_NATS_PAY_LEN] = "";

    /* Check for actuator_name shorthand */
    char actuator_name[DEV_NAME_LEN] = "";
    jsonArgString(args, "actuator_name", actuator_name, sizeof(actuator_name));

    if (actuator_name[0]) {
        /* Validate actuator exists */
        Device *act = deviceFind(actuator_name);
        if (!act) {
            snprintf(result, result_len, "Error: actuator '%s' not found", actuator_name);
            return;
        }
        if (!deviceIsActuator(act->kind)) {
            snprintf(result, result_len, "Error: '%s' is not an actuator", actuator_name);
            return;
        }

        on_action = ACT_ACTUATOR;
        strncpy(on_actuator, actuator_name, DEV_NAME_LEN - 1);

        /* Auto-set off action */
        has_off = true;
        off_action = ACT_ACTUATOR;
        strncpy(off_actuator, actuator_name, DEV_NAME_LEN - 1);
    } else {
        /* Explicit on_action */
        char act_str[24] = "";
        jsonArgString(args, "on_action", act_str, sizeof(act_str));
        if (act_str[0]) {
            on_action = parseActionType(act_str);
        }

        on_pin = (uint8_t)jsonArgInt(args, "on_pin", 0);
        on_value = jsonArgInt(args, "on_value", 1);
        jsonArgString(args, "on_nats_subject", on_nats_subj, sizeof(on_nats_subj));
        jsonArgString(args, "on_nats_payload", on_nats_pay, sizeof(on_nats_pay));

        /* For telegram action, store message in on_nats_pay (reused field) */
        if (on_action == ACT_TELEGRAM) {
            jsonArgString(args, "on_telegram_message", on_nats_pay, sizeof(on_nats_pay));
        }
        /* For serial_send action, store text in on_nats_pay (reused field) */
        if (on_action == ACT_SERIAL_SEND) {
            jsonArgString(args, "on_serial_text", on_nats_pay, sizeof(on_nats_pay));
        }

        /* Pack on_r/on_g/on_b into on_value for led_set */
        if (on_action == ACT_LED_SET && jsonArgExists(args, "on_r")) {
            int r = constrain(jsonArgInt(args, "on_r", 0), 0, 255);
            int g = constrain(jsonArgInt(args, "on_g", 0), 0, 255);
            int b = constrain(jsonArgInt(args, "on_b", 0), 0, 255);
            on_value = (r << 16) | (g << 8) | b;
        }
    }

    /* Check for explicit off_action */
    char off_act_str[24] = "";
    jsonArgString(args, "off_action", off_act_str, sizeof(off_act_str));
    if (off_act_str[0]) {
        if (strcmp(off_act_str, "none") == 0) {
            has_off = false;
        } else if (strcmp(off_act_str, "auto") == 0) {
            has_off = true;
            off_action = on_action;
            strncpy(off_actuator, on_actuator, DEV_NAME_LEN - 1);
            off_pin = on_pin;
            off_value = 0;
            /* For led_set auto-off, check if off_r/off_g/off_b provided */
            if (off_action == ACT_LED_SET && jsonArgExists(args, "off_r")) {
                int r = constrain(jsonArgInt(args, "off_r", 0), 0, 255);
                int g = constrain(jsonArgInt(args, "off_g", 0), 0, 255);
                int b = constrain(jsonArgInt(args, "off_b", 0), 0, 255);
                off_value = (r << 16) | (g << 8) | b;
            }
            /* For telegram auto-off, check if off_telegram_message provided */
            if (off_action == ACT_TELEGRAM) {
                jsonArgString(args, "off_telegram_message", off_nats_pay, sizeof(off_nats_pay));
            }
            if (off_action == ACT_SERIAL_SEND) {
                jsonArgString(args, "off_serial_text", off_nats_pay, sizeof(off_nats_pay));
            }
        } else {
            has_off = true;
            off_action = parseActionType(off_act_str);
            off_pin = (uint8_t)jsonArgInt(args, "off_pin", 0);
            off_value = jsonArgInt(args, "off_value", 0);
            jsonArgString(args, "off_nats_subject", off_nats_subj, sizeof(off_nats_subj));
            jsonArgString(args, "off_nats_payload", off_nats_pay, sizeof(off_nats_pay));
            /* Pack off_r/off_g/off_b into off_value for led_set */
            if (off_action == ACT_LED_SET && jsonArgExists(args, "off_r")) {
                int r = constrain(jsonArgInt(args, "off_r", 0), 0, 255);
                int g = constrain(jsonArgInt(args, "off_g", 0), 0, 255);
                int b = constrain(jsonArgInt(args, "off_b", 0), 0, 255);
                off_value = (r << 16) | (g << 8) | b;
            }
            /* For telegram action, store message in off_nats_pay */
            if (off_action == ACT_TELEGRAM) {
                jsonArgString(args, "off_telegram_message", off_nats_pay, sizeof(off_nats_pay));
            }
            if (off_action == ACT_SERIAL_SEND) {
                jsonArgString(args, "off_serial_text", off_nats_pay, sizeof(off_nats_pay));
            }
        }
    }

    /* Chain parameters */
    char chain_rule[RULE_ID_LEN] = "";
    jsonArgString(args, "chain_rule", chain_rule, sizeof(chain_rule));
    uint32_t chain_delay_ms = (uint32_t)jsonArgInt(args, "chain_delay_seconds", 0) * 1000;
    char chain_off_rule[RULE_ID_LEN] = "";
    jsonArgString(args, "chain_off_rule", chain_off_rule, sizeof(chain_off_rule));
    uint32_t chain_off_delay_ms = (uint32_t)jsonArgInt(args, "chain_off_delay_seconds", 0) * 1000;

    const char *id = ruleCreate(rule_name, sensor_name, sensor_pin, sensor_analog,
                                condition, threshold, interval_ms,
                                on_action, on_actuator, on_pin, on_value,
                                on_nats_subj, on_nats_pay,
                                has_off, off_action, off_actuator,
                                off_pin, off_value, off_nats_subj, off_nats_pay,
                                chain_rule, chain_delay_ms,
                                chain_off_rule, chain_off_delay_ms);

    if (!id) {
        snprintf(result, result_len, "Error: rule creation failed (max %d rules)", MAX_RULES);
        return;
    }

    /* Detect self-reference (ruleCreate silently cleared it) */
    if (chain_rule[0] && strcmp(chain_rule, id) == 0)
        chain_rule[0] = '\0';
    if (chain_off_rule[0] && strcmp(chain_off_rule, id) == 0)
        chain_off_rule[0] = '\0';

    rulesSave();

    /* Build descriptive response */
    const char *cond_sym = "?";
    switch (condition) {
        case COND_GT: cond_sym = ">"; break;
        case COND_LT: cond_sym = "<"; break;
        case COND_EQ: cond_sym = "=="; break;
        case COND_NEQ: cond_sym = "!="; break;
        case COND_CHANGE: cond_sym = "changed"; break;
        case COND_ALWAYS: cond_sym = "always"; break;
        case COND_CHAINED: cond_sym = "chained"; break;
    }

    int w = 0;
    if (condition == COND_CHAINED) {
        w = snprintf(result, result_len, "Rule created: %s '%s' - chained (fires only via chain)",
                     id, rule_name);
    } else {
        const char *src = sensor_name[0] ? sensor_name : "pin";
        w = snprintf(result, result_len, "Rule created: %s '%s' - %s %s %d (every %ds)%s",
                     id, rule_name, src, cond_sym, (int)threshold, interval_s,
                     has_off ? " with auto-off" : "");
    }
    if (chain_rule[0] && w < result_len - 1) {
        w += snprintf(result + w, result_len - w, " ->%s(%ds)",
                      chain_rule, (int)(chain_delay_ms / 1000));
    }
    if (chain_off_rule[0] && w < result_len - 1) {
        w += snprintf(result + w, result_len - w, " off->%s(%ds)",
                 chain_off_rule, (int)(chain_off_delay_ms / 1000));
    }
    /* Warn about orphan delays */
    if (!chain_rule[0] && chain_delay_ms > 0 && w < result_len - 1) {
        w += snprintf(result + w, result_len - w,
                      " (Warning: chain_delay ignored, no chain_rule)");
    }
    if (!chain_off_rule[0] && chain_off_delay_ms > 0 && w < result_len - 1) {
        w += snprintf(result + w, result_len - w,
                      " (Warning: chain_off_delay ignored, no chain_off_rule)");
    }
}

static void tool_rule_list(const char *args, char *result, int result_len) {
    (void)args;
    const Rule *rules = ruleGetAll();
    int w = 0;
    int count = 0;

    for (int i = 0; i < MAX_RULES && w < result_len - 60; i++) {
        if (!rules[i].used) continue;
        const Rule *r = &rules[i];

        if (count > 0) w += snprintf(result + w, result_len - w, "; ");

        uint32_t ago = r->last_triggered ? (millis() - r->last_triggered) / 1000 : 0;
        if (r->condition == COND_CHAINED) {
            w += snprintf(result + w, result_len - w,
                "%s '%s' %s chained %s last=%us",
                r->id, r->name,
                r->enabled ? "ON" : "OFF",
                r->fired ? "FIRED" : "idle",
                (unsigned)ago);
        } else {
            uint32_t eval_ago = r->last_eval ? (millis() - r->last_eval) / 1000 : 0;
            w += snprintf(result + w, result_len - w,
                "%s '%s' %s %s%s %d val=%.1f %s last=%us eval=%us every=%us",
                r->id, r->name,
                r->enabled ? "ON" : "OFF",
                r->sensor_name[0] ? r->sensor_name : "pin",
                r->sensor_name[0] ? "" : "",
                (int)r->threshold,
                r->last_reading,
                r->fired ? "FIRED" : "idle",
                (unsigned)ago,
                (unsigned)eval_ago, (unsigned)(r->interval_ms / 1000));
        }
        if (r->chain_id[0]) {
            w += snprintf(result + w, result_len - w, " ->%s(%us)",
                          r->chain_id, (unsigned)(r->chain_delay_ms / 1000));
        }
        if (r->chain_off_id[0]) {
            w += snprintf(result + w, result_len - w, " off->%s(%us)",
                          r->chain_off_id, (unsigned)(r->chain_off_delay_ms / 1000));
        }
        count++;
    }

    if (count == 0) {
        snprintf(result, result_len, "No rules defined");
    }
}

static void tool_rule_delete(const char *args, char *result, int result_len) {
    char rule_id[RULE_ID_LEN];
    if (!jsonArgString(args, "rule_id", rule_id, sizeof(rule_id))) {
        snprintf(result, result_len, "Error: missing 'rule_id'");
        return;
    }

    if (!ruleDelete(rule_id)) {
        snprintf(result, result_len, "Error: rule '%s' not found", rule_id);
        return;
    }

    rulesSave();

    if (strcmp(rule_id, "all") == 0)
        snprintf(result, result_len, "All rules deleted");
    else
        snprintf(result, result_len, "Deleted rule %s", rule_id);
}

static void tool_rule_enable(const char *args, char *result, int result_len) {
    char rule_id[RULE_ID_LEN];
    if (!jsonArgString(args, "rule_id", rule_id, sizeof(rule_id))) {
        snprintf(result, result_len, "Error: missing 'rule_id'");
        return;
    }

    bool enabled = jsonArgBool(args, "enabled", true);

    if (!ruleEnable(rule_id, enabled)) {
        snprintf(result, result_len, "Error: rule '%s' not found", rule_id);
        return;
    }

    rulesSave();
    snprintf(result, result_len, "Rule %s %s", rule_id, enabled ? "enabled" : "disabled");
}

/*============================================================================
 * Serial Text Tool Handler
 *============================================================================*/

static void tool_serial_send(const char *args, char *result, int result_len) {
    if (!serialTextActive()) {
        snprintf(result, result_len,
            "Error: no serial_text device registered. "
            "Use device_register with type='serial_text' first.");
        return;
    }

    char text[256];
    if (!jsonArgString(args, "text", text, sizeof(text))) {
        snprintf(result, result_len, "Error: missing 'text' argument");
        return;
    }

    if (serialTextSend(text)) {
        snprintf(result, result_len, "Sent to serial: %s", text);
    } else {
        snprintf(result, result_len, "Error: serial send failed");
    }
}

/*============================================================================
 * Multi-Device Tool Handler
 *============================================================================*/

static void tool_remote_chat(const char *args, char *result, int result_len) {
    if (!g_nats_connected) {
        snprintf(result, result_len, "Error: NATS not connected");
        return;
    }

    char device[32];
    char message[256];

    if (!jsonArgString(args, "device", device, sizeof(device))) {
        snprintf(result, result_len, "Error: missing 'device'");
        return;
    }
    if (!jsonArgString(args, "message", message, sizeof(message))) {
        snprintf(result, result_len, "Error: missing 'message'");
        return;
    }

    /* Build subject: "{device}.chat" */
    char subject[64];
    snprintf(subject, sizeof(subject), "%s.chat", device);

    /* NATS request/reply with 30s timeout */
    static nats_request_t req;

    nats_err_t err = natsClient.requestStart(&req, subject, message, 30000);
    if (err != NATS_OK) {
        snprintf(result, result_len, "Error: request failed: %s", nats_err_str(err));
        return;
    }

    /* Poll until complete or timeout */
    while (true) {
        esp_task_wdt_reset();
        natsClient.process();
        nats_err_t status = natsClient.requestCheck(&req);
        if (status == NATS_OK) {
            /* Got response - response_data is uint8_t[], response_len is size_t */
            int copy_len = (int)req.response_len < result_len - 1
                           ? (int)req.response_len : result_len - 1;
            memcpy(result, req.response_data, copy_len);
            result[copy_len] = '\0';
            return;
        }
        if (status != NATS_ERR_WOULD_BLOCK) {
            snprintf(result, result_len, "Error: %s (device '%s' may be offline)",
                     nats_err_str(status), device);
            return;
        }
        delay(50);
    }
}

/*============================================================================
 * Display Tool Handler (boards with an OLED)
 *============================================================================*/

static void tool_display_print(const char *args, char *result, int result_len) {
    if (!displayAvailable()) {
        /* honest absent: a build without a panel (or a failed init) reports
         * the truth instead of a silent ok */
        snprintf(result, result_len, "Error: no display on this device");
        return;
    }
    int row = jsonArgInt(args, "row", -1);
    char text[DISPLAY_METRIC_COLS];
    text[0] = '\0';
    jsonArgString(args, "text", text, sizeof(text)); /* absent/empty = clear */
    if (!displayPrintRow(row, text)) {
        snprintf(result, result_len, "Error: row %d out of range (0-%d)",
                 row, DISPLAY_METRIC_ROWS - 1);
        return;
    }
    if (text[0])
        snprintf(result, result_len, "Display row %d set: %s", row, text);
    else
        snprintf(result, result_len, "Display row %d cleared", row);
}

static void tool_display_tier(const char *args, char *result, int result_len) {
    if (!displayAvailable()) {
        snprintf(result, result_len, "Error: no display on this device");
        return;
    }
    char t[8];
    t[0] = '\0';
    jsonArgString(args, "tier", t, sizeof(t));
    if (!t[0] || t[1] != '\0' ||
        (t[0] != 'F' && t[0] != 'L' && t[0] != 'R')) {
        snprintf(result, result_len, "Error: tier must be F, L or R");
        return;
    }
    if (!displaySetTier(t[0])) {
        snprintf(result, result_len,
                 "Error: display pages not built on this device");
        return;
    }
    snprintf(result, result_len,
             "Brain tier set: %c (decays to SOLO if pushes stop)", t[0]);
}

static void tool_display_alert(const char *args, char *result, int result_len) {
    if (!displayAvailable()) {
        snprintf(result, result_len, "Error: no display on this device");
        return;
    }
    char cls[DISPLAY_METRIC_COLS];
    cls[0] = '\0';
    jsonArgString(args, "class", cls, sizeof(cls)); /* absent/empty = clear */
    int age_s = jsonArgInt(args, "age_s", 0);
    if (!displaySetAlert(cls, age_s)) {
        snprintf(result, result_len,
                 "Error: display pages not built on this device");
        return;
    }
    if (cls[0])
        snprintf(result, result_len,
                 "Alert banner up: %s (age %ds, ticking locally; only an "
                 "empty class clears it)", cls, age_s);
    else
        snprintf(result, result_len, "Alert banner cleared");
}

/*============================================================================
 * Battery Read Tool Handler (boards with a switched VBAT divider)
 *============================================================================*/

#ifdef WIRECLAW_VBAT_ADC
#ifndef WIRECLAW_VBAT_SCALE
#define WIRECLAW_VBAT_SCALE 4.9f /* 390k/100k divider: VBAT = VADC * 4.9 */
#endif
#endif

float batteryReadVolts(unsigned int *adc_mv_out) {
#ifdef WIRECLAW_VBAT_ADC
    /* Heltec-style battery sense: a GPIO-switched divider feeds VBAT/SCALE
     * into the ADC. The divider is switched in only for the read — it draws
     * battery current (uA) while enabled. Reported volts are the measured
     * value verbatim; with no battery attached the node may read near 0 V or
     * the charger float — interpretation belongs to the consumer. */
#ifdef WIRECLAW_VBAT_CTRL
    pinMode(WIRECLAW_VBAT_CTRL, OUTPUT);
    digitalWrite(WIRECLAW_VBAT_CTRL, HIGH); /* active HIGH per datasheet */
    delay(10); /* divider settle */
#endif
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) {
        mv += analogReadMilliVolts(WIRECLAW_VBAT_ADC);
        delay(2);
    }
    mv /= 8;
#ifdef WIRECLAW_VBAT_CTRL
    digitalWrite(WIRECLAW_VBAT_CTRL, LOW);
#endif
    if (adc_mv_out) *adc_mv_out = (unsigned int)mv;
    return ((float)mv / 1000.0f) * WIRECLAW_VBAT_SCALE;
#else
    (void)adc_mv_out;
    return NAN;
#endif
}

static void tool_battery_read(const char *args, char *result, int result_len) {
    (void)args;
    unsigned int mv = 0;
    float vbat = batteryReadVolts(&mv);
    if (isnan(vbat)) {
        snprintf(result, result_len, "Error: no battery sense on this device");
        return;
    }
    snprintf(result, result_len, "Battery: %.2f V (adc %u mV)", vbat, mv);
}

/*============================================================================
 * LoRa Ears Tool Handler (boards with an SX1262; RX-only)
 *============================================================================*/

static void tool_lora_stats(const char *args, char *result, int result_len) {
    (void)args;
    if (!loraEarsAvailable()) {
        snprintf(result, result_len, "Error: no LoRa listener on this device");
        return;
    }
    loraEarsStats(result, result_len);
}

static void tool_mesh_send(const char *args, char *result, int result_len) {
    char text[256];
    text[0] = '\0';
    jsonArgString(args, "text", text, sizeof(text));
    loraMeshSend(text, result, result_len); /* honest error if TX not built */
}

static void tool_mesh_set_channel(const char *args, char *result,
                                  int result_len) {
    char name[40], psk[96];
    name[0] = '\0';
    psk[0] = '\0';
    jsonArgString(args, "name", name, sizeof(name));
    jsonArgString(args, "psk", psk, sizeof(psk)); /* absent/empty = public */
    if (!name[0]) {
        snprintf(result, result_len, "Error: name required");
        return;
    }
    if (!loraMeshSetChannel(name, psk)) {
        int kl = loraLastKeyLen();
        if (kl >= 0 && kl != 16 && kl != 32)
            /* length only — never the key — tells hex/base64 OK but wrong size
             * vs an unparseable format (kl==0) */
            snprintf(result, result_len,
                     "Error: key decoded to %d bytes, need 16 or 32 "
                     "(check the PSK is complete)", kl);
        else
            snprintf(result, result_len,
                     "Error: PSK not recognized as hex or base64 (or TX not "
                     "built) — check format");
        return;
    }
    /* Optional durability: persist to the device's own flash so a reboot
     * keeps the channel (operator-opt-in; default RAM-only). */
    bool persist = jsonArgInt(args, "persist", 0) != 0;
    bool persisted = persist && loraPersistChannel(name, psk);
    /* Echo name + hash only (the hash is the cleartext header byte — lets a
     * receiver-side check confirm the channel — never the key). */
    snprintf(result, result_len, "Channel set: '%s' (hash 0x%02x)%s",
             name, (unsigned)loraMeshChannelHash(),
             persist ? (persisted ? " [persisted]" : " [persist FAILED]") : "");
}

/*============================================================================
 * BLE Ears Tool Handler (boards with BLE built; passive observer)
 *============================================================================*/

static void tool_ble_stats(const char *args, char *result, int result_len) {
    (void)args;
    /* No availability gate: while the scan is down, bleStats reports WHICH
     * stage failed and the retry state (the stub covers non-BLE builds). */
    bleStats(result, result_len);
}

/*============================================================================
 * Anomaly Witness Tool Handler (WIRECLAW_ANOMALY builds)
 *============================================================================*/

static void tool_anomaly_stats(const char *args, char *result, int result_len) {
    (void)args;
    /* The stub answers honestly on non-anomaly builds; during warm-up the
     * leading score is -1, never a fake "0.0 = all normal". */
    anomalyStats(result, result_len);
}

/*============================================================================
 * Chain Create — multi-step chain in one call
 *============================================================================*/

/* Helper: parse step action params from prefixed keys */
static void parseStepAction(const char *args, const char *prefix,
                            ActionType &action, uint8_t &pin, int32_t &value,
                            char *nats_subj, int subj_len,
                            char *nats_pay, int pay_len,
                            char *actuator, int act_len) {
    char key[32];

    /* action */
    snprintf(key, sizeof(key), "%s_action", prefix);
    char act_str[24] = "";
    jsonArgString(args, key, act_str, sizeof(act_str));
    if (act_str[0]) action = parseActionType(act_str);

    /* message — used for telegram, nats_publish, serial_send */
    snprintf(key, sizeof(key), "%s_message", prefix);
    jsonArgString(args, key, nats_pay, pay_len);

    /* nats_subject */
    snprintf(key, sizeof(key), "%s_nats_subject", prefix);
    jsonArgString(args, key, nats_subj, subj_len);

    /* actuator name */
    snprintf(key, sizeof(key), "%s_actuator", prefix);
    jsonArgString(args, key, actuator, act_len);

    /* pin / value */
    snprintf(key, sizeof(key), "%s_pin", prefix);
    pin = (uint8_t)jsonArgInt(args, key, 0);
    snprintf(key, sizeof(key), "%s_value", prefix);
    value = jsonArgInt(args, key, 1);

    /* RGB packing for led_set */
    if (action == ACT_LED_SET) {
        snprintf(key, sizeof(key), "%s_r", prefix);
        if (jsonArgExists(args, key)) {
            int r = constrain(jsonArgInt(args, key, 0), 0, 255);
            snprintf(key, sizeof(key), "%s_g", prefix);
            int g = constrain(jsonArgInt(args, key, 0), 0, 255);
            snprintf(key, sizeof(key), "%s_b", prefix);
            int b = constrain(jsonArgInt(args, key, 0), 0, 255);
            value = (r << 16) | (g << 8) | b;
        }
    }
}

/* Helper: format action name for result string */
static int fmtStepAction(char *buf, int len, ActionType action, int32_t value,
                         const char *message) {
    switch (action) {
        case ACT_LED_SET: {
            int r = (value >> 16) & 0xFF;
            int g = (value >> 8) & 0xFF;
            int b = value & 0xFF;
            return snprintf(buf, len, "LED(%d,%d,%d)", r, g, b);
        }
        case ACT_TELEGRAM:
            return snprintf(buf, len, "telegram");
        case ACT_GPIO_WRITE:
            return snprintf(buf, len, "gpio(%d)", (int)value);
        case ACT_NATS_PUBLISH:
            return snprintf(buf, len, "nats");
        case ACT_ACTUATOR:
            return snprintf(buf, len, "actuator");
        case ACT_SERIAL_SEND:
            return snprintf(buf, len, "serial");
        default:
            return snprintf(buf, len, "?");
    }
}

static void tool_chain_create(const char *args, char *result, int result_len) {
    /* --- Parse trigger (source) params --- */
    char sensor_name[DEV_NAME_LEN] = "";
    if (!jsonArgString(args, "sensor_name", sensor_name, sizeof(sensor_name))) {
        snprintf(result, result_len, "Error: missing 'sensor_name'");
        return;
    }
    char cond_str[16] = "";
    if (!jsonArgString(args, "condition", cond_str, sizeof(cond_str))) {
        snprintf(result, result_len, "Error: missing 'condition'");
        return;
    }
    ConditionOp condition = parseConditionOp(cond_str);
    int32_t threshold = jsonArgInt(args, "threshold", 0);
    int interval_s = jsonArgInt(args, "interval_seconds", 5);
    if (interval_s < 5) interval_s = 5;
    uint32_t interval_ms = (uint32_t)interval_s * 1000;

    /* Validate sensor */
    Device *dev = deviceFind(sensor_name);
    if (!dev) {
        snprintf(result, result_len, "Error: sensor '%s' not found", sensor_name);
        return;
    }

    /* --- Parse step actions --- */
    struct StepInfo {
        ActionType action;
        uint8_t pin;
        int32_t value;
        char nats_subj[RULE_NATS_SUBJ_LEN];
        char nats_pay[RULE_NATS_PAY_LEN];
        char actuator[DEV_NAME_LEN];
        uint32_t delay_ms;
        bool used;
    };
    static const char *prefixes[5] = {"step1","step2","step3","step4","step5"};
    StepInfo steps[5];
    memset(steps, 0, sizeof(steps));

    /* Parse all steps — step1 and step2 required, step3-5 optional */
    for (int s = 0; s < 5; s++) {
        char key[32], act_str[24] = "";
        snprintf(key, sizeof(key), "%s_action", prefixes[s]);
        jsonArgString(args, key, act_str, sizeof(act_str));

        if (!act_str[0]) {
            if (s < 2) {
                snprintf(result, result_len, "Error: missing '%s'", key);
                return;
            }
            break;  /* no more steps */
        }
        steps[s].used = true;
        if (s > 0) {
            snprintf(key, sizeof(key), "%s_delay", prefixes[s]);
            steps[s].delay_ms = (uint32_t)jsonArgInt(args, key, 0) * 1000;
        }
        parseStepAction(args, prefixes[s], steps[s].action, steps[s].pin, steps[s].value,
                        steps[s].nats_subj, RULE_NATS_SUBJ_LEN,
                        steps[s].nats_pay, RULE_NATS_PAY_LEN,
                        steps[s].actuator, DEV_NAME_LEN);
    }

    int num_steps = 0;
    for (int s = 0; s < 5; s++) { if (steps[s].used) num_steps = s + 1; }

    /* --- Create rules end-first --- */
    const char *ids[5] = {nullptr};

    /* Create from last step backwards */
    for (int i = num_steps - 1; i >= 0; i--) {
        StepInfo &s = steps[i];
        const char *chain_id = nullptr;
        uint32_t chain_delay = 0;

        /* If not the last step, chain to the next step */
        if (i < num_steps - 1) {
            chain_id = ids[i + 1];
            chain_delay = steps[i + 1].delay_ms;  /* delay BEFORE next step fires */
        }

        bool is_source = (i == 0);
        char name[RULE_NAME_LEN];
        snprintf(name, sizeof(name), "%s step%d", sensor_name, i + 1);

        const char *id = ruleCreate(
            name,
            is_source ? sensor_name : "",   /* sensor only on source */
            PIN_NONE, false,
            is_source ? condition : COND_CHAINED,
            is_source ? threshold : 0,
            interval_ms,
            s.action, s.actuator, s.pin, s.value,
            s.nats_subj, s.nats_pay,
            false, ACT_GPIO_WRITE, "", 0, 0, "", "",  /* no off action */
            chain_id, chain_delay,
            nullptr, 0  /* no off-chain */
        );

        if (!id) {
            snprintf(result, result_len, "Error: max rules reached at step %d", i + 1);
            return;
        }
        ids[i] = id;
    }

    rulesSave();

    /* --- Build result: "Chain: rule_03 test>50 -> telegram -> 10s -> LED(255,0,0) -> 10s -> LED(0,0,0)" --- */
    int w = snprintf(result, result_len, "Chain created: %s %s>%d",
                     ids[0], sensor_name, (int)threshold);

    for (int i = 0; i < num_steps && w < result_len - 40; i++) {
        char act_buf[32];
        fmtStepAction(act_buf, sizeof(act_buf), steps[i].action, steps[i].value,
                      steps[i].nats_pay);
        if (i > 0 && steps[i].delay_ms > 0) {
            w += snprintf(result + w, result_len - w, " -> %us -> %s",
                          (unsigned)(steps[i].delay_ms / 1000), act_buf);
        } else {
            w += snprintf(result + w, result_len - w, " -> %s", act_buf);
        }
    }
}

/*============================================================================
 * Public API
 *============================================================================*/

const char *toolsGetDefinitions() {
    return TOOLS_JSON;
}

#ifdef WIRECLAW_AGENT_TOOLS_RESTRICTED

/* Tools the ON-DEVICE LLM agent may call: display/LED/read-only sensors
 * ONLY (dude-claw W5 charter). The NATS tool_exec path stays FULL — the
 * brain's ratified rules remain the sanctioned actuation route.
 * Deliberately excluded, and why:
 *   gpio_write/gpio_read/actuator_set/serial_send  physical pins
 *   mesh_send/mesh_set_channel   RF transmission — Part-97/airtime judgment
 *                                is never delegated to a 4B model
 *   nats_publish/remote_chat     bus reach / agent recursion
 *   file_read/file_write         /config.json carries the WiFi + channel
 *                                secrets; an LLM loop must not read them
 *   rule_create, rule_delete, rule_enable, chain_create,
 *   device_register, device_remove               self-rewiring
 *   host_probe                   LAN probing under LLM control
 *   display_tier                 evidence-based claim owned by the brain's
 *                                pusher (same exclusion as mini's allowlist)
 */
static const char *AGENT_TOOL_ALLOWLIST[] = {
    "led_set", "display_print", "display_alert", "temperature_read",
    "battery_read", "lora_stats", "ble_stats", "device_info",
    "sensor_read", "device_list", "rule_list", "anomaly_stats", NULL,
};

bool toolAllowedForAgent(const char *name) {
    if (!name || !name[0]) return false;
    for (int i = 0; AGENT_TOOL_ALLOWLIST[i]; i++) {
        if (strcmp(name, AGENT_TOOL_ALLOWLIST[i]) == 0) return true;
    }
    return false;
}

const char *toolsGetAgentDefinitions() {
    /* Filter TOOLS_JSON down to the allowlist. Entries are one-per-line in
     * the source string, so this is a line filter; built once. On overflow
     * the agent gets an EMPTY tools array plus a serial error — a silently
     * truncated tool list would be a lie the model acts on. */
    static char buf[4096];
    static bool built = false;
    if (built) return buf;
    built = true;
    int w = 0;
    buf[w++] = '[';
    const char *p = TOOLS_JSON;
    while (*p && *p != '\n') p++; /* skip the opening '[' line */
    if (*p) p++;
    bool first = true;
    while (*p) {
        const char *eol = strchr(p, '\n');
        int len = eol ? (int)(eol - p) : (int)strlen(p);
        const char *nm = strstr(p, "\"name\":\"");
        bool keep = false;
        if (nm && (!eol || nm < eol)) {
            nm += 8;
            const char *nend = strchr(nm, '"');
            char name[48];
            int nlen = nend ? (int)(nend - nm) : 0;
            if (nlen > 0 && nlen < (int)sizeof(name)) {
                memcpy(name, nm, nlen);
                name[nlen] = '\0';
                keep = toolAllowedForAgent(name);
            }
        }
        if (keep) {
            int copy = len;
            while (copy > 0 && (p[copy - 1] == ',' || p[copy - 1] == '\r'))
                copy--;
            if (w + copy + 3 >= (int)sizeof(buf)) {
                Serial.printf("[Agent] restricted tool list overflow — the "
                              "agent gets NO tools rather than a truncated "
                              "list\n");
                strcpy(buf, "[]");
                return buf;
            }
            if (!first) buf[w++] = ',';
            buf[w++] = '\n';
            memcpy(buf + w, p, copy);
            w += copy;
            first = false;
        }
        if (!eol) break;
        p = eol + 1;
    }
    buf[w++] = ']';
    buf[w] = '\0';
    return buf;
}

#endif /* WIRECLAW_AGENT_TOOLS_RESTRICTED */

bool toolExecute(const char *name, const char *args_json,
                  char *result, int result_len) {
    if (strcmp(name, "led_set") == 0) {
        tool_led_set(args_json, result, result_len);
    } else if (strcmp(name, "gpio_write") == 0) {
        tool_gpio_write(args_json, result, result_len);
    } else if (strcmp(name, "gpio_read") == 0) {
        tool_gpio_read(args_json, result, result_len);
    } else if (strcmp(name, "device_info") == 0) {
        tool_device_info(args_json, result, result_len);
    } else if (strcmp(name, "file_read") == 0) {
        tool_file_read(args_json, result, result_len);
    } else if (strcmp(name, "file_write") == 0) {
        tool_file_write(args_json, result, result_len);
    } else if (strcmp(name, "nats_publish") == 0) {
        tool_nats_publish(args_json, result, result_len);
    } else if (strcmp(name, "temperature_read") == 0) {
        tool_temperature_read(args_json, result, result_len);
    /* Device registry tools */
    } else if (strcmp(name, "device_register") == 0) {
        tool_device_register(args_json, result, result_len);
    } else if (strcmp(name, "device_list") == 0) {
        tool_device_list(args_json, result, result_len);
    } else if (strcmp(name, "device_remove") == 0) {
        tool_device_remove(args_json, result, result_len);
    } else if (strcmp(name, "sensor_read") == 0) {
        tool_sensor_read(args_json, result, result_len);
    } else if (strcmp(name, "actuator_set") == 0) {
        tool_actuator_set(args_json, result, result_len);
    /* Rule engine tools */
    } else if (strcmp(name, "rule_create") == 0) {
        tool_rule_create(args_json, result, result_len);
    } else if (strcmp(name, "rule_list") == 0) {
        tool_rule_list(args_json, result, result_len);
    } else if (strcmp(name, "rule_delete") == 0) {
        tool_rule_delete(args_json, result, result_len);
    } else if (strcmp(name, "rule_enable") == 0) {
        tool_rule_enable(args_json, result, result_len);
    } else if (strcmp(name, "serial_send") == 0) {
        tool_serial_send(args_json, result, result_len);
    } else if (strcmp(name, "remote_chat") == 0) {
        tool_remote_chat(args_json, result, result_len);
    } else if (strcmp(name, "display_print") == 0) {
        tool_display_print(args_json, result, result_len);
    } else if (strcmp(name, "display_tier") == 0) {
        tool_display_tier(args_json, result, result_len);
    } else if (strcmp(name, "display_alert") == 0) {
        tool_display_alert(args_json, result, result_len);
    } else if (strcmp(name, "battery_read") == 0) {
        tool_battery_read(args_json, result, result_len);
    } else if (strcmp(name, "lora_stats") == 0) {
        tool_lora_stats(args_json, result, result_len);
    } else if (strcmp(name, "mesh_send") == 0) {
        tool_mesh_send(args_json, result, result_len);
    } else if (strcmp(name, "mesh_set_channel") == 0) {
        tool_mesh_set_channel(args_json, result, result_len);
    } else if (strcmp(name, "ble_stats") == 0) {
        tool_ble_stats(args_json, result, result_len);
    } else if (strcmp(name, "anomaly_stats") == 0) {
        tool_anomaly_stats(args_json, result, result_len);
    } else if (strcmp(name, "chain_create") == 0) {
        tool_chain_create(args_json, result, result_len);
    } else {
        snprintf(result, result_len, "Error: unknown tool '%s'", name);
        return false;
    }
    return true;
}
