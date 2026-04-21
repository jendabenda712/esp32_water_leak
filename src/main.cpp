#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <Preferences.h>

namespace Config {
    constexpr gpio_num_t PIN_POWER  = GPIO_NUM_2;
    constexpr gpio_num_t PIN_SENSOR = GPIO_NUM_3;

    constexpr const char* DEFAULT_MQTT_SERVER = "homeserver.local";
    constexpr const char* DEFAULT_MQTT_PORT = "1883";
    constexpr const char* HOME_MQTT_TOPIC_PREFIX = "home/water/";

    constexpr uint64_t HEARTBEAT_US = 24ULL * 60ULL * 60ULL * 1000000ULL; // 24 hours in microseconds

    constexpr uint8_t WIFI_MAX_CONN_RETRIES = 20;

};

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// -------------------------------------------------------------------------
// HELPERS
// -------------------------------------------------------------------------

void sendMQTTMessage(const String& mqttServer, const String& mqttPort, const String& topicSuffix, const String& payload) {

    mqttClient.setServer(mqttServer.c_str(), mqttPort.toInt());

    String fullTopic = Config::HOME_MQTT_TOPIC_PREFIX + topicSuffix;

    Serial.print("\nSend message to MQTT broker...");
    String clientId = "WaterLeakageSensor-" + WiFi.macAddress();
    if (mqttClient.connect(clientId.c_str())) {
        mqttClient.publish(fullTopic.c_str(), payload.c_str());
        Serial.println("\nMsg sent!");
        delay(100);
        mqttClient.disconnect();
    } else {
        Serial.println("MQTT connection failed!");
    }
}

void resetAndRestart() {
    WiFiManager wm;
    Preferences prefs;

    wm.resetSettings();
    prefs.begin("app-data", false);
    prefs.putBool("is_setup", false);
    prefs.end();
    delay(100);
    ESP.restart();
}

void connectAndSendMQTT(const String& mqttServer, const String& mqttPort, const String& topicSuffix, const String& payload) {

    WiFi.mode(WIFI_STA);
    WiFi.begin();

    Serial.print("\n[INFO] Connecting to WiFi...");
    int retryCount = 0;
    while (WiFi.status() != WL_CONNECTED && retryCount < Config::WIFI_MAX_CONN_RETRIES) {
        delay(500);
        Serial.print(".");
        retryCount++;
    }

    if (retryCount >= Config::WIFI_MAX_CONN_RETRIES) {
        Serial.println("\n[ERROR] Failed to connect to WiFi after multiple attempts.");
        // Reset WiFi settings to allow for a fresh start on the next boot (e.g., if credentials changed)
        resetAndRestart();
        return;
    }

    Serial.println("\n[INFO] Connected to WiFi!");
    sendMQTTMessage(mqttServer, mqttPort, topicSuffix, payload);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}


void handleFirstSetup() {

    String mqttServer;
    String mqttPort;
    Preferences prefs;
    String topicSuffix = "unknown";

    // Set up WiFiManager and auto-connect to WiFi (with fallback to config portal)
    WiFiManager wm;
    uint32_t portalTimeout = 3 * 60; // 3 minutes
    wm.setConfigPortalTimeout(portalTimeout);
    WiFiManagerParameter custom_mqtt_server("server", "MQTT Server IP", Config::DEFAULT_MQTT_SERVER, 100);
    WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", Config::DEFAULT_MQTT_PORT, 6);
    WiFiManagerParameter custom_topic_suffix("topic_suffix", "Device Location (e.g., Basement)", topicSuffix.c_str(), 30);
    wm.addParameter(&custom_mqtt_server);
    wm.addParameter(&custom_mqtt_port);
    wm.addParameter(&custom_topic_suffix);
    bool connected = false;
    String configSsid = "jendatech" + WiFi.macAddress();
    configSsid.replace(":", "");
    configSsid.replace(":", "");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    for (int i = 0; i < 3; i++) {
        if (wm.autoConnect(configSsid.c_str())) {
            connected = true;
            break;
        }


        wm.setConfigPortalTimeout(portalTimeout * (i + 2)); // Increase timeout for the next attempt
    }

    if (!connected) {
        Serial.println("\n[ERROR]Too much fails to connect. Resetting WiFi settings and going to sleep.");
        // Failed to connect after 3 tries, reset WiFi settings and go to sleep
        wm.resetSettings();
        ESP.restart();
    }

    Serial.println("\n[INFO]Connected to WiFi!");

    prefs.begin("app-data", false);
    prefs.putBool("is_setup", true);
    prefs.putString("server", custom_mqtt_server.getValue());
    prefs.putString("port", custom_mqtt_port.getValue());
    topicSuffix = custom_topic_suffix.getValue();
    topicSuffix.replace(" ", "_");
    topicSuffix.toLowerCase();
    prefs.putString("topic_suffix", topicSuffix);
    prefs.end();
    return;
}

void setup() {

    Preferences prefs;

    Serial.begin(115200);
    delay(3000);
    Serial.println("\n\n[INFO] Starting up...");

    pinMode(Config::PIN_POWER, OUTPUT);
    digitalWrite(Config::PIN_POWER, HIGH);
    pinMode(Config::PIN_SENSOR, INPUT_PULLDOWN);

    // 1. Why did we wake up?
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    // 2. Do we have any WiFi configuration stored?
    prefs.begin("app-data", true);
    bool isConfigured = prefs.getBool("is_setup", false);
    String mqttServer = prefs.getString("server", Config::DEFAULT_MQTT_SERVER);
    String mqttPort = prefs.getString("port", Config::DEFAULT_MQTT_PORT);
    String topicSuffix = prefs.getString("topic_suffix", "unknown");
    prefs.end();

    if (!isConfigured) {
        Serial.println("\n[ERROR] Device not configured yet. Starting setup...");
        handleFirstSetup();
    }
    else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        Serial.println("\n[INFO] Routine-Check");
        connectAndSendMQTT(mqttServer, mqttPort, topicSuffix, String("HEARTBEAT"));
    }
    else if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        Serial.println("\n[ERROR] Water detected!");
        connectAndSendMQTT(mqttServer, mqttPort, topicSuffix, String("ALARM"));
    }
    else if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
        Serial.println("\n[INFO] Cold boot. Skipping WiFi...");
    } else {
        Serial.println("\n[INFO] Unusual boot. Skipping WiFi...");
    }
    Serial.println("\n[INFO] Going to sleep...");

    digitalWrite(Config::PIN_POWER, LOW);

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    esp_deep_sleep_enable_gpio_wakeup(1ULL << Config::PIN_SENSOR, ESP_GPIO_WAKEUP_GPIO_HIGH);
    esp_sleep_enable_timer_wakeup(Config::HEARTBEAT_US);
    Serial.println("\n[INFO] Going to sleep...");
    Serial.flush();
    delay(100);
    esp_deep_sleep_start();
}

void loop() {
    // Nothing to do here, everything is handled in setup() and then we go to sleep.
    Serial.println("[FATAL] loop() should never execute!");
    while(true) {
        delay(1000); // Prevent watchdog reset
    }
}