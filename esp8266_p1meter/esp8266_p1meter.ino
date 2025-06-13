#include <FS.h>
#include <EEPROM.h>
#include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// * Include settings
#include "settings.h"

// * Initiate led blinker library
Ticker ticker;

// * Initiate WIFI client
//WiFiClient espClient;
WiFiClientSecure espClient;



// * Initiate MQTT client
// Use WiFiClientSecure for TLS/SSL connection
PubSubClient mqtt_client(espClient);

// **********************************
// * WIFI                           *
// **********************************

// * Gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager)
{
    Serial.println(F("Entered config mode"));
    Serial.println(WiFi.softAPIP());

    // * If you used auto generated SSID, print it
    Serial.println(myWiFiManager->getConfigPortalSSID());

    // * Entered config mode, make led toggle faster
    ticker.attach(0.2, tick);
}

// **********************************
// * Ticker (System LED Blinker)    *
// **********************************

// * Blink on-board Led
void tick()
{
    // * Toggle state
    int state = digitalRead(LED_BUILTIN);    // * Get the current state of GPIO1 pin
    digitalWrite(LED_BUILTIN, !state);       // * Set pin to the opposite state
}

// **********************************
// * MQTT                           *
// **********************************

// * Send a message to a broker topic
void send_mqtt_message(const char *topic, char *payload)
{
    Serial.printf("MQTT Outgoing on %s: ", topic);
    Serial.println(payload);

    bool result = mqtt_client.publish(topic, payload, false);

    if (!result)
    {
        Serial.printf("MQTT publish to topic %s failed\n", topic);
    }
}

// * Reconnect to MQTT server and subscribe to in and out topics
bool mqtt_reconnect()
{
    // * Loop until we're reconnected
    int MQTT_RECONNECT_RETRIES = 0;
    mqtt_client.setBufferSize(512); // Set the buffer size for MQTT messages
    while (!mqtt_client.connected() && MQTT_RECONNECT_RETRIES < MQTT_MAX_RECONNECT_TRIES)
    {
        MQTT_RECONNECT_RETRIES++;
        Serial.printf("MQTT connection attempt %d / %d ...\n", MQTT_RECONNECT_RETRIES, MQTT_MAX_RECONNECT_TRIES);

        // * Attempt to connect
        if (mqtt_client.connect(HOSTNAME, MQTT_USER, MQTT_PASS))
        {
            Serial.println(F("MQTT connected!"));

            // * Once connected, publish an announcement...
            char *message = new char[16 + strlen(HOSTNAME) + 1];
            strcpy(message, "p1 meter alive: ");
            strcat(message, HOSTNAME);
            mqtt_client.publish("hass/status", message);

            Serial.printf("MQTT root topic: %s\n", MQTT_ROOT_TOPIC);
        }
        else
        {
            Serial.print(F("MQTT Connection failed: rc="));
            Serial.println(mqtt_client.state());
            Serial.println(F(" Retrying in 5 seconds"));
            Serial.println("");

            // * Wait 5 seconds before retrying
            delay(5000);
        }
    }

    if (MQTT_RECONNECT_RETRIES >= MQTT_MAX_RECONNECT_TRIES)
    {
        Serial.printf("*** MQTT connection failed, giving up after %d tries ...\n", MQTT_RECONNECT_RETRIES);
        return false;
    }

    return true;
}

void send_metric(String name, long metric)
{
    Serial.print(F("Sending metric to broker: "));
    Serial.print(name);
    Serial.print(F("="));
    Serial.println(metric);

    char output[10];
    ltoa(metric, output, sizeof(output));

    String topic = String(MQTT_ROOT_TOPIC) + "/" + name;
    send_mqtt_message(topic.c_str(), output);
}

void send_data_to_broker()
{
    // Creëer een JSON-document
    StaticJsonDocument<512> doc;

    // Voeg gridinformatie toe
    if (ACTUAL_CONSUMPTION != 0) {
        doc["grid"]["power"] = ACTUAL_CONSUMPTION;
    } else if (ACTUAL_RETURNDELIVERY != 0) {
        doc["grid"]["power"] = -ACTUAL_RETURNDELIVERY;
    }
     
    doc["grid"]["voltage"] = round(((L1_VOLTAGE + L2_VOLTAGE + L3_VOLTAGE) / 3.0 / 1000) * 10) / 10.0;
    
    // Voeg L1 toe
    if (L1_INSTANT_POWER_USAGE != 0) {
        doc["grid"]["L1"]["power"] = L1_INSTANT_POWER_USAGE;
    }
    else if (L1_INSTANT_POWER_RETURNDELIVERY != 0) {
        doc["grid"]["L1"]["power"] = -L1_INSTANT_POWER_RETURNDELIVERY;
    }

    doc["grid"]["L1"]["voltage"] = L1_VOLTAGE / 1000.0;
    
    // Voeg L2 toe
    if (L2_INSTANT_POWER_USAGE != 0) {
        doc["grid"]["L2"]["power"] = L2_INSTANT_POWER_USAGE;
    }
    else if (L2_INSTANT_POWER_RETURNDELIVERY != 0) {
        doc["grid"]["L2"]["power"] = -L2_INSTANT_POWER_RETURNDELIVERY;
    }
    doc["grid"]["L2"]["voltage"] = L2_VOLTAGE / 1000.0;
    
    // Voeg L3 toe
    if (L3_INSTANT_POWER_USAGE != 0) {
        doc["grid"]["L3"]["power"] = L3_INSTANT_POWER_USAGE;
    }
    else if (L3_INSTANT_POWER_RETURNDELIVERY != 0) {
        doc["grid"]["L3"]["power"] = -L3_INSTANT_POWER_RETURNDELIVERY;
    }
    doc["grid"]["L3"]["voltage"] = L3_VOLTAGE / 1000.0;
    
    // Serialiseer het JSON-document naar een string
    char payload[512];
    serializeJson(doc, payload);

    // Verzend het JSON-bericht naar de MQTT-broker
    mqtt_client.publish(MQTT_JSON_TOPIC, payload);
}

// **********************************
// * P1                             *
// **********************************

unsigned int CRC16(unsigned int crc, unsigned char *buf, int len)
{
	for (int pos = 0; pos < len; pos++)
    {
		crc ^= (unsigned int)buf[pos];    // * XOR byte into least sig. byte of crc
                                          // * Loop over each bit
        for (int i = 8; i != 0; i--)
        {
            // * If the LSB is set
            if ((crc & 0x0001) != 0)
            {
                // * Shift right and XOR 0xA001
                crc >>= 1;
				crc ^= 0xA001;
			}
            // * Else LSB is not set
            else
                // * Just shift right
                crc >>= 1;
		}
	}
	return crc;
}

bool isNumber(char *res, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (((res[i] < '0') || (res[i] > '9')) && (res[i] != '.' && res[i] != 0))
            return false;
    }
    return true;
}

int FindCharInArrayRev(char array[], char c, int len)
{
    for (int i = len - 1; i >= 0; i--)
    {
        if (array[i] == c)
            return i;
    }
    return -1;
}

long getValue(char *buffer, int maxlen, char startchar, char endchar)
{
    int s = FindCharInArrayRev(buffer, startchar, maxlen - 2);
    int l = FindCharInArrayRev(buffer, endchar, maxlen - 2) - s - 1;

    char res[16];
    memset(res, 0, sizeof(res));

    if (strncpy(res, buffer + s + 1, l))
    {
        if (endchar == '*')
        {
            if (isNumber(res, l))
                // * Lazy convert float to long
                return (1000 * atof(res));
        }
        else if (endchar == ')')
        {
            if (isNumber(res, l))
                return atof(res);
        }
    }
    return 0;
}

bool decode_telegram(int len)
{
    int startChar = FindCharInArrayRev(telegram, '/', len);
    int endChar = FindCharInArrayRev(telegram, '!', len);
    bool validCRCFound = false;

    for (int cnt = 0; cnt < len; cnt++) {
        Serial.print(telegram[cnt]);
    }
    Serial.print("\n");

    if (startChar >= 0)
    {
        // * Start found. Reset CRC calculation
        currentCRC = CRC16(0x0000,(unsigned char *) telegram+startChar, len-startChar);
    }
    else if (endChar >= 0)
    {
        // * Add to crc calc
        currentCRC = CRC16(currentCRC,(unsigned char*)telegram+endChar, 1);

        char messageCRC[5];
        strncpy(messageCRC, telegram + endChar + 1, 4);

        messageCRC[4] = 0;   // * Thanks to HarmOtten (issue 5)
        validCRCFound = (strtol(messageCRC, NULL, 16) == currentCRC);

        if (validCRCFound)
            Serial.println(F("CRC Valid!"));
        else
            Serial.println(F("CRC Invalid!"));

        currentCRC = 0;
    }
    else
    {
        currentCRC = CRC16(currentCRC, (unsigned char*) telegram, len);
    }

    // 1-0:1.8.1(000992.992*kWh)
    // 1-0:1.8.1 = Elektra verbruik laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.1", strlen("1-0:1.8.1")) == 0)
    {
        CONSUMPTION_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.8.2(000560.157*kWh)
    // 1-0:1.8.2 = Elektra verbruik hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.2", strlen("1-0:1.8.2")) == 0)
    {
        CONSUMPTION_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }
	
    // 1-0:2.8.1(000560.157*kWh)
    // 1-0:2.8.1 = Elektra teruglevering laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.1", strlen("1-0:2.8.1")) == 0)
    {
        RETURNDELIVERY_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.8.2(000560.157*kWh)
    // 1-0:2.8.2 = Elektra teruglevering hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.2", strlen("1-0:2.8.2")) == 0)
    {
        RETURNDELIVERY_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.7.0(00.424*kW) Actueel verbruik
    // 1-0:1.7.x = Electricity consumption actual usage (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.7.0", strlen("1-0:1.7.0")) == 0)
    {
        ACTUAL_CONSUMPTION = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.7.0(00.000*kW) Actuele teruglevering (-P) in 1 Watt resolution
    if (strncmp(telegram, "1-0:2.7.0", strlen("1-0:2.7.0")) == 0)
    {
        ACTUAL_RETURNDELIVERY = getValue(telegram, len, '(', '*');
    }

    // 1-0:21.7.0(00.378*kW)
    // 1-0:21.7.0 = Instantaan vermogen Elektriciteit levering L1
    if (strncmp(telegram, "1-0:21.7.0", strlen("1-0:21.7.0")) == 0)
    {
        L1_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:41.7.0(00.378*kW)
    // 1-0:41.7.0 = Instantaan vermogen Elektriciteit levering L2
    if (strncmp(telegram, "1-0:41.7.0", strlen("1-0:41.7.0")) == 0)
    {
        L2_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:61.7.0(00.378*kW)
    // 1-0:61.7.0 = Instantaan vermogen Elektriciteit levering L3
    if (strncmp(telegram, "1-0:61.7.0", strlen("1-0:61.7.0")) == 0)
    {
        L3_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:22.7.0(00.378*kW)
    // 1-0:22.7.0 = Instantaan vermogen Elektriciteit terug levering L1
    if (strncmp(telegram, "1-0:22.7.0", strlen("1-0:22.7.0")) == 0)
    {
        L1_INSTANT_POWER_RETURNDELIVERY = getValue(telegram, len, '(', '*');
    }

    // 1-0:42.7.0(00.378*kW)
    // 1-0:42.7.0 = Instantaan vermogen Elektriciteit terug levering L2
    if (strncmp(telegram, "1-0:42.7.0", strlen("1-0:42.7.0")) == 0)
    {
        L2_INSTANT_POWER_RETURNDELIVERY = getValue(telegram, len, '(', '*');
    }

    // 1-0:62.7.0(00.378*kW)
    // 1-0:62.7.0 = Instantaan vermogen Elektriciteit terug levering L3
    if (strncmp(telegram, "1-0:62.7.0", strlen("1-0:62.7.0")) == 0)
    {
        L3_INSTANT_POWER_RETURNDELIVERY = getValue(telegram, len, '(', '*');
    }

    // 1-0:32.7.0(232.0*V)
    // 1-0:32.7.0 = Voltage L1
    if (strncmp(telegram, "1-0:32.7.0", strlen("1-0:32.7.0")) == 0)
    {
        L1_VOLTAGE = getValue(telegram, len, '(', '*');
    }
    // 1-0:52.7.0(232.0*V)
    // 1-0:52.7.0 = Voltage L2
    if (strncmp(telegram, "1-0:52.7.0", strlen("1-0:52.7.0")) == 0)
    {
        L2_VOLTAGE = getValue(telegram, len, '(', '*');
    }   
    // 1-0:72.7.0(232.0*V)
    // 1-0:72.7.0 = Voltage L3
    if (strncmp(telegram, "1-0:72.7.0", strlen("1-0:72.7.0")) == 0)
    {
        L3_VOLTAGE = getValue(telegram, len, '(', '*');
    }

    return validCRCFound;
}

void read_p1_hardwareserial() 
{
    if (Serial.available())
    {
        memset(telegram, 0, sizeof(telegram));

        while (Serial.available())
        {
            ESP.wdtDisable();
            int len = Serial.readBytesUntil('\n', telegram, P1_MAXLINELENGTH);
            ESP.wdtEnable(1);

            processLine(len);
        }
    }
}

void processLine(int len) {
    telegram[len] = '\n';
    telegram[len + 1] = 0;
    yield();

    bool result = decode_telegram(len + 1);
    if (result) {
        send_data_to_broker();
        LAST_UPDATE_SENT = millis();
    }
}

// **********************************
// * EEPROM helpers                 *
// **********************************

String read_eeprom(int offset, int len)
{
    Serial.print(F("read_eeprom()"));

    String res = "";
    for (int i = 0; i < len; ++i)
    {
        res += char(EEPROM.read(i + offset));
    }
    return res;
}

void write_eeprom(int offset, int len, String value)
{
    Serial.println(F("write_eeprom()"));
    for (int i = 0; i < len; ++i)
    {
        if ((unsigned)i < value.length())
        {
            EEPROM.write(i + offset, value[i]);
        }
        else
        {
            EEPROM.write(i + offset, 0);
        }
    }
}

// ******************************************
// * Callback for saving WIFI config        *
// ******************************************

bool shouldSaveConfig = false;

// * Callback notifying us of the need to save config
void save_wifi_config_callback ()
{
    Serial.println(F("Should save config"));
    shouldSaveConfig = true;
}

// **********************************
// * Setup OTA                      *
// **********************************

void setup_ota()
{
    Serial.println(F("Arduino OTA activated."));

    // * Port defaults to 8266
    ArduinoOTA.setPort(8266);

    // * Set hostname for OTA
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]()
    {
        Serial.println(F("Arduino OTA: Start"));
    });

    ArduinoOTA.onEnd([]()
    {
        Serial.println(F("Arduino OTA: End (Running reboot)"));
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
    {
        Serial.printf("Arduino OTA Progress: %u%%\r", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error)
    {
        Serial.printf("Arduino OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
            Serial.println(F("Arduino OTA: Auth Failed"));
        else if (error == OTA_BEGIN_ERROR)
            Serial.println(F("Arduino OTA: Begin Failed"));
        else if (error == OTA_CONNECT_ERROR)
            Serial.println(F("Arduino OTA: Connect Failed"));
        else if (error == OTA_RECEIVE_ERROR)
            Serial.println(F("Arduino OTA: Receive Failed"));
        else if (error == OTA_END_ERROR)
            Serial.println(F("Arduino OTA: End Failed"));
    });

    ArduinoOTA.begin();
    Serial.println(F("Arduino OTA finished"));
}

// **********************************
// * Setup MDNS discovery service   *
// **********************************

void setup_mdns()
{
    Serial.println(F("Starting MDNS responder service"));

    bool mdns_result = MDNS.begin(HOSTNAME);
    if (mdns_result)
    {
        MDNS.addService("http", "tcp", 80);
    }
}

// **********************************
// * Setup Main                     *
// **********************************

void setup()
{
    // * Configure EEPROM
    EEPROM.begin(512);

    // Setup a hw serial connection for communication with the P1 meter and logging (not using inversion)
    Serial.begin(BAUD_RATE, SERIAL_8N1, SERIAL_FULL);
    Serial.setRxBufferSize(1024);
    Serial.println("");
    Serial.println("Swapping UART0 RX to inverted");
    Serial.flush();

    // Invert the RX serialport by setting a register value, this way the TX might continue normally allowing the serial monitor to read println's
    USC0(UART0) = USC0(UART0) | BIT(UCRXI);
    Serial.println("Serial port is ready to recieve.");

    // * Set led pin as output
    pinMode(LED_BUILTIN, OUTPUT);

    // * Start ticker with 0.5 because we start in AP mode and try to connect
    ticker.attach(0.6, tick);

    // * Get MQTT Server settings
    String settings_available = read_eeprom(134, 1);

    if (settings_available == "1")
    {
        read_eeprom(0, 64).toCharArray(MQTT_HOST, 64);   // * 0-63
        read_eeprom(64, 6).toCharArray(MQTT_PORT, 6);    // * 64-69
        read_eeprom(70, 32).toCharArray(MQTT_USER, 32);  // * 70-101
        read_eeprom(102, 32).toCharArray(MQTT_PASS, 32); // * 102-133
    }

    WiFiManagerParameter CUSTOM_MQTT_HOST("host", "MQTT hostname", MQTT_HOST, 64);
    WiFiManagerParameter CUSTOM_MQTT_PORT("port", "MQTT port",     MQTT_PORT, 6);
    WiFiManagerParameter CUSTOM_MQTT_USER("user", "MQTT user",     MQTT_USER, 32);
    WiFiManagerParameter CUSTOM_MQTT_PASS("pass", "MQTT pass",     MQTT_PASS, 32);

    // * WiFiManager local initialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;

    // * Reset settings - uncomment for testing
    // wifiManager.resetSettings();

    // * Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wifiManager.setAPCallback(configModeCallback);

    // * Set timeout
    wifiManager.setConfigPortalTimeout(WIFI_TIMEOUT);

    // * Set save config callback
    wifiManager.setSaveConfigCallback(save_wifi_config_callback);

    // * Add all your parameters here
    wifiManager.addParameter(&CUSTOM_MQTT_HOST);
    wifiManager.addParameter(&CUSTOM_MQTT_PORT);
    wifiManager.addParameter(&CUSTOM_MQTT_USER);
    wifiManager.addParameter(&CUSTOM_MQTT_PASS);

    // * Fetches SSID and pass and tries to connect
    // * Reset when no connection after 10 seconds
    if (!wifiManager.autoConnect())
    {
        Serial.println(F("Failed to connect to WIFI and hit timeout"));

        // * Reset and try again, or maybe put it to deep sleep
        ESP.reset();
        delay(WIFI_TIMEOUT);
    }

    // * Read updated parameters
    strcpy(MQTT_HOST, CUSTOM_MQTT_HOST.getValue());
    strcpy(MQTT_PORT, CUSTOM_MQTT_PORT.getValue());
    strcpy(MQTT_USER, CUSTOM_MQTT_USER.getValue());
    strcpy(MQTT_PASS, CUSTOM_MQTT_PASS.getValue());

    // * Save the custom parameters to FS
    if (shouldSaveConfig)
    {
        Serial.println(F("Saving WiFiManager config"));

        write_eeprom(0, 64, MQTT_HOST);   // * 0-63
        write_eeprom(64, 6, MQTT_PORT);   // * 64-69
        write_eeprom(70, 32, MQTT_USER);  // * 70-101
        write_eeprom(102, 32, MQTT_PASS); // * 102-133
        write_eeprom(134, 1, "1");        // * 134 --> always "1"
        EEPROM.commit();
    }

    // * If you get here you have connected to the WiFi
    Serial.println(F("Connected to WIFI..."));

    // * Keep LED on
    ticker.detach();
    digitalWrite(LED_BUILTIN, LOW);

    // * Configure OTA
    setup_ota();

    // * Startup MDNS Service
    setup_mdns();

    // * Setup MQTT
    Serial.printf("MQTT connecting to: %s:%s\n", MQTT_HOST, MQTT_PORT);

    // Disable SSL certificate validation for MQTT (insecure, but needed for self-signed certs)
    espClient.setInsecure();
    mqtt_client.setServer(MQTT_HOST, atoi(MQTT_PORT));

}

// **********************************
// * Loop                           *
// **********************************

void loop()
{
    ArduinoOTA.handle();
    long now = millis();

    if (!mqtt_client.connected())
    {
        if (now - LAST_RECONNECT_ATTEMPT > 5000)
        {
            LAST_RECONNECT_ATTEMPT = now;

            if (mqtt_reconnect())
            {
                LAST_RECONNECT_ATTEMPT = 0;
            }
        }
    }
    else
    {
        mqtt_client.loop();
    }
    
    if (now - LAST_UPDATE_SENT > UPDATE_INTERVAL) {
        read_p1_hardwareserial();
    }
}
