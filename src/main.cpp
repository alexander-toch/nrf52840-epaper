#include <Arduino.h>
#include <bluefruit.h>
#include <ArduinoJson.h>

#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_7C.h>
#include <fonts/GothamRoundedBook.h>
#include <fonts/GothamRoundedBold.h>
#include <fonts/GothamRoundedBoldBig.h>
#include <glyphs/weather.h>
#include <glyphs/icons.h>
#include <glyphs/weather_small.h>

// #define SERIAL_DEBUG

// select the display class and display driver class in the following file (new style):
#include "paper-config/GxEPD2_display_selection_new_style.h"

// Advertising parameters should have a global scope. Do NOT define them in 'setup' or in 'loop'
BLEClientService service("D2EA587F-19C8-4F4C-8179-3BA0BC150B01");
BLEClientCharacteristic characteristic1("0DF8D897-33FE-4AF4-9E7A-63D24664C94C");
BLEClientCharacteristic characteristic2("0DF8D897-33FE-4AF4-9E7A-63D24664C94D");
BLEClientCharacteristic characteristic3("0DF8D897-33FE-4AF4-9E7A-63D24664C94E");
BLEClientCharacteristic characteristic4("0DF8D897-33FE-4AF4-9E7A-63D24664C94F");

void writeSerial(String message, bool newLine = true)
{
#ifdef SERIAL_DEBUG
    if (newLine == true)
    {
        Serial.println(message);
    }
    else
    {
        Serial.print(message);
    }
#endif
}

typedef struct
{
    float temperature_inside;
    float humidity_inside;
    float temperature_outside;
    float humidity_outside;
    float wind_speed;
    String weather_forecast_now;
    String weather_forecast_2h;
    float weather_forecast_2h_temp;
    String weather_forecast_2h_time;
    String weather_forecast_4h;
    float weather_forecast_4h_temp;
    String weather_forecast_4h_time;
    String weather_forecast_6h;
    float weather_forecast_6h_temp;
    String weather_forecast_6h_time;
    String weather_forecast_8h;
    float weather_forecast_8h_temp;
    String weather_forecast_8h_time;
    String time;
    String timestamp;
} HAData;

void scan_callback(ble_gap_evt_adv_report_t *report)
{
    writeSerial("Connecting...");
    Bluefruit.Central.connect(report);
}

void connect_callback(uint16_t conn_handle)
{
    BLEConnection *connection = Bluefruit.Connection(conn_handle);
    char peer_name[32] = {0};
    connection->getPeerName(peer_name, sizeof(peer_name));

    writeSerial("Connected to ", false);
    writeSerial(peer_name);

    service.discover(conn_handle);
    characteristic1.discover();
    characteristic2.discover();
    characteristic3.discover();
    characteristic4.discover();

    HAData haData;
    char data[256] = {0};
    int bytes_received = characteristic1.read(data, 1);
    data[255] = 0;

    writeSerial("Data: ", false);
    writeSerial(String(data));

    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_GREEN, LOW);

    Bluefruit.disconnect(conn_handle);
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    (void)reason;

    writeSerial("Disconnected");

    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_GREEN, HIGH);

    Bluefruit.Scanner.stop();
    // NRF_POWER->SYSTEMOFF = 1;
    sd_power_mode_set(NRF_POWER_MODE_LOWPWR);
    delay(300 * 1000);
}

void setup()
{
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_GREEN, HIGH);

    // power management
    NRF_POWER->DCDCEN = 1;
    NRF_POWER->TASKS_LOWPWR = 1;

#ifdef SERIAL_DEBUG
    Serial.begin(9600);
    while (!Serial)
        delay(10);
#endif

    if (!Bluefruit.begin(0, 1))
    {
        writeSerial("failed to initialize BLE!");
        return;
    }
    writeSerial("SeedPaperBLEClient initialized");

    service.begin();
    Bluefruit.setName("SeedPaperBLECentral");

    characteristic1.begin();
    characteristic2.begin();
    characteristic3.begin();
    characteristic4.begin();

    Bluefruit.autoConnLed(false);
    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    // Bluefruit.Scanner.setInterval(32, 244); // in unit of 0.625 ms
    Bluefruit.Scanner.filterUuid(service.uuid);
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.start(0);
    writeSerial("Scanning for devices...");
}

// String readCharacteristic(BLEDevice peripheral, BLEService service, String characteristic_uuid)
// {
//     BLECharacteristic characteristic = service.characteristic(characteristic_uuid.c_str());
//     char val[1024] = {0};
//     if (characteristic.canRead())
//     {
//         int success = characteristic.read();
//         if (success == 0)
//         {
// #ifdef SERIAL_DEBUG
//             writeSerial"Characteristic read failed.");
// #endif
//         }
//         else
//         {
// #ifdef SERIAL_DEBUG
//            writeSerial("Characteristic read success. Bytes read: ");
//             writeSerialcharacteristic.valueLength());
// #endif
//             characteristic.readValue(val, characteristic.valueLength());
//         }
//     }
//     return String(val);
// }

void getData(String input, HAData &haData)
{
    JsonDocument doc;
    deserializeJson(doc, input);
    haData.temperature_inside = doc["attributes"]["temperature_inside"].as<float>();
    haData.humidity_inside = doc["attributes"]["humidity_inside"].as<float>();
    haData.temperature_outside = doc["attributes"]["temperature_outside"].as<float>();
    haData.humidity_outside = doc["attributes"]["humidity_outside"].as<float>();
    haData.wind_speed = doc["attributes"]["wind_speed"].as<float>();
    haData.weather_forecast_now = doc["attributes"]["weather_forecast_now"].as<String>();
    haData.weather_forecast_2h = doc["attributes"]["weather_forecast_2h"].as<String>();
    haData.weather_forecast_2h_temp = doc["attributes"]["weather_forecast_2h_temp"].as<float>();
    haData.weather_forecast_2h_time = doc["attributes"]["weather_forecast_2h_time"].as<String>();
    haData.weather_forecast_4h = doc["attributes"]["weather_forecast_4h"].as<String>();
    haData.weather_forecast_4h_temp = doc["attributes"]["weather_forecast_4h_temp"].as<float>();
    haData.weather_forecast_4h_time = doc["attributes"]["weather_forecast_4h_time"].as<String>();
    haData.weather_forecast_6h = doc["attributes"]["weather_forecast_6h"].as<String>();
    haData.weather_forecast_6h_temp = doc["attributes"]["weather_forecast_6h_temp"].as<float>();
    haData.weather_forecast_6h_time = doc["attributes"]["weather_forecast_6h_time"].as<String>();
    haData.weather_forecast_8h = doc["attributes"]["weather_forecast_8h"].as<String>();
    haData.weather_forecast_8h_temp = doc["attributes"]["weather_forecast_8h_temp"].as<float>();
    haData.weather_forecast_8h_time = doc["attributes"]["weather_forecast_8h_time"].as<String>();
    haData.time = doc["attributes"]["time"].as<String>();
    haData.timestamp = doc["attributes"]["timestamp"].as<String>();
}

void loop()
{
}