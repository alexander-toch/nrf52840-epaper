#include <Arduino.h>
#include <bluefruit.h>
#include <ArduinoJson.h>
#include <SPI.h>

#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_7C.h>
#include <fonts/GothamRoundedBook.h>
#include <fonts/GothamRoundedBold.h>
#include <fonts/GothamRoundedBoldBig.h>
#include <glyphs/weather.h>
#include <glyphs/icons.h>
#include <glyphs/weather_small.h>

#define SERIAL_DEBUG

// select the display class and display driver class in the following file (new style):
#include "paper-config/GxEPD2_display_selection_new_style.h"

// Advertising parameters should have a global scope. Do NOT define them in 'setup' or in 'loop'
BLEClientService service("D2EA587F-19C8-4F4C-8179-3BA0BC150B01");
BLEClientCharacteristic characteristic1("0DF8D897-33FE-4AF4-9E7A-63D24664C94C");
BLEClientCharacteristic characteristic2("0DF8D897-33FE-4AF4-9E7A-63D24664C94D");
BLEClientCharacteristic characteristic3("0DF8D897-33FE-4AF4-9E7A-63D24664C94E");
BLEClientCharacteristic characteristic4("0DF8D897-33FE-4AF4-9E7A-63D24664C94F");
BLEConnection *connection;

const size_t TIME_REFRESH = 10 * 1000;
const size_t TIME_RETRY_SCAN = 60 * 1000;

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
    String last_updated;
} HAData;
HAData haData;

void parseData(String input, HAData &haData);

void scan_callback(ble_gap_evt_adv_report_t *report)
{
    writeSerial("Connecting...");
    Bluefruit.Central.connect(report);
}

void connect_callback(uint16_t conn_handle)
{
    digitalWrite(LED_GREEN, LOW);

    connection = Bluefruit.Connection(conn_handle);
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    (void)reason;

    writeSerial("Disconnected");
    digitalWrite(LED_GREEN, HIGH);
    // Bluefruit.Scanner.stop();
}

void setup()
{

#ifdef SERIAL_DEBUG
    Serial.begin(9600);
    delay(1000);
#endif

    pinMode(LED_RED, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_BLUE, HIGH);
    digitalWrite(LED_GREEN, HIGH);

    // bluetooth
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);

    if (!Bluefruit.begin(0, 1))
    {
        writeSerial("failed to initialize BLE!");
        return;
    }
    delay(1000);
    writeSerial("SeedPaperBLEClient initialized");

    // power management
    sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
    sd_power_mode_set(NRF_POWER_MODE_LOWPWR);

    // display
    display.init(115200, true, 10, false); //, true, 2, false); // initial = true
    display.setFullWindow();
    display.setRotation(1);

    // TODO: deep sleep
    // https://github.com/waveshareteam/e-Paper/issues/15
    // nrf_gpio_cfg_input(D4, NRF_GPIO_PIN_PULLUP); // RST needs to be pulled up for deepsleep to work properly.
    // prevent parasitic current consumption
    // pinMode(D3, INPUT);  // BUSY
    // pinMode(D5, INPUT);  // RST
    // pinMode(D7, INPUT);  // CS
    // pinMode(D8, INPUT);  // CLK
    // pinMode(D10, INPUT); // DIN

    // GxEPD2_213_B74.cpp
    // delay(500);
    // display.powerOff();
}

void parseData(String input, HAData &haData)
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
    haData.last_updated = doc["last_updated"].as<String>();
}
bool firstRun = true;
size_t refreshCount = 0;
void loop()
{
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
    Bluefruit.Advertising.setFastTimeout(30);
    // Bluefruit.Scanner.setInterval(32, 244); // in unit of 0.625 ms
    Bluefruit.Scanner.filterUuid(service.uuid);
    Bluefruit.Scanner.useActiveScan(false);

    if (firstRun)
    {
        Bluefruit.Scanner.start(500); // Scan timeout in 10 ms units
        firstRun = false;
    }
    else if (Bluefruit.Scanner.isRunning())
    {
        writeSerial("Scanning...");
        delay(100);
    }
    else if (Bluefruit.Central.connected())
    {
        connection->requestPHY();
        connection->requestDataLengthUpdate();
        connection->requestMtuExchange(BLE_GATT_ATT_MTU_MAX);
        delay(500);

        char peer_name[32] = {0};
        connection->getPeerName(peer_name, sizeof(peer_name));

        writeSerial("Connected to ", false);
        writeSerial(peer_name, false);
        writeSerial(" with max Mtu: ", false);
        writeSerial(String(connection->getMtu()));

        service.discover(connection->handle());
        characteristic1.discover();
        characteristic2.discover();
        characteristic3.discover();
        characteristic4.discover();

        const size_t CHARACTERISTIC_MAX_DATA_LEN = connection->getMtu() - 3;
        char buffer[4 * CHARACTERISTIC_MAX_DATA_LEN] = {0};
        char *current_pos = buffer;

        size_t bytes_received = characteristic1.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received - 4; // oh god why is it 4?
        bytes_received = characteristic2.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received - 4;
        bytes_received = characteristic3.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received - 4;
        bytes_received = characteristic4.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received - 4;
        memset(current_pos, 0, buffer + sizeof(buffer) - current_pos);
        Bluefruit.disconnect(connection->handle());

        String data = String(buffer);
        parseData(String(data), haData);
        writeSerial(String(data));

        if (haData.last_updated == "null")
        {
            writeSerial("Last updated is null. Trying again.");
            Bluefruit.Scanner.start(500);
            return;
        }
        writeSerial("Last updated: " + String(haData.last_updated));
        writeSerial("Temperature inside: " + String(haData.temperature_inside));
        refreshCount++;

        pinMode(D4, OUTPUT);                   // RST pin
        display.init(115200, false, 2, false); // wake up
        if (refreshCount > 1)
        {
            // 122x250 -> 120x248
            // TODO: check partial refresh
            // display.setPartialWindow(0, 0, 248, 120); // TODO: case for multiple of 8 needs to be fixed
        }
        display.firstPage();
        do
        {
            display.setCursor(0, 0);
            display.setFont(&GothamRounded_Book14pt8b);
            display.setTextSize(1);
            display.setTextColor(GxEPD_BLACK);
            display.write("\n");
            display.write("Count: ");
            display.write(String(refreshCount).c_str());
        } while (display.nextPage());
        display.hibernate();

        delay(100);
        // force power saving
        pinMode(D4, INPUT); // RST
        SPI.end();

        delay(TIME_REFRESH);
        Bluefruit.Scanner.start(500);
    }
    else
    {
        writeSerial("Nothing found. Trying again in some seconds.");
        delay(TIME_RETRY_SCAN);
        Bluefruit.Scanner.start(500); // Scan timeout in 10 ms units
    }
}