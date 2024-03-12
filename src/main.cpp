#include <Arduino.h>
#include <bluefruit.h>
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
#include "paper-config/GxEPD2_display_selection_new_style.h"
#include "home_assistant.h"

// Refresh interval of HA data and display
const size_t TIME_REFRESH = 5 * 1000;

// Retry interval if no peripheral (server) is found
const size_t TIME_RETRY_SCAN = 60 * 1000;

// Advertising/Central parameters should have a global scope. Do NOT define them in 'setup' or in 'loop'
BLEClientService service("D2EA587F-19C8-4F4C-8179-3BA0BC150B01");

// we use 4 characteristics in order to fit the whole HA JSON data
// as we are limited to 247 bytes per characteristic
// see https://devzone.nordicsemi.com/f/nordic-q-a/35927/max-data-length-over-ble
BLEClientCharacteristic characteristic1("0DF8D897-33FE-4AF4-9E7A-63D24664C94C");
BLEClientCharacteristic characteristic2("0DF8D897-33FE-4AF4-9E7A-63D24664C94D");
BLEClientCharacteristic characteristic3("0DF8D897-33FE-4AF4-9E7A-63D24664C94E");
BLEClientCharacteristic characteristic4("0DF8D897-33FE-4AF4-9E7A-63D24664C94F");

// Handle for current connection
BLEConnection *connection;

// forward declarations
void writeSerial(String message, bool newLine = true);
void scan_callback(ble_gap_evt_adv_report_t *report);
void connect_callback(uint16_t conn_handle);
void disconnect_callback(uint16_t conn_handle, uint8_t reason);
void initDisplay();
void startScan();
void hibernateDisplay();

int serial_enabled = 0;
size_t refresh_count = 0;
size_t scan_count = 0;

void setup()
{
    serial_enabled = bitRead(NRF_POWER->USBREGSTATUS, 0); // VBUSDETECT - USB supply status
    if (serial_enabled == 1)
    {
        Serial.begin(9600);
        while (!Serial)
            ;
        serial_enabled = 1;
    }

    // bluetooth
    Bluefruit.configCentralBandwidth(BANDWIDTH_MAX);

    if (!Bluefruit.begin(0, 1))
    {
        writeSerial("failed to initialize BLE!");
        return;
    }
    service.begin();
    characteristic1.begin();
    characteristic2.begin();
    characteristic3.begin();
    characteristic4.begin();

    // power management
    sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
    sd_power_mode_set(NRF_POWER_MODE_LOWPWR);

    // init display
    initDisplay();
    hibernateDisplay();
    writeSerial("setup completed");

    startScan();
}

void loop()
{
    serial_enabled = bitRead(NRF_POWER->USBREGSTATUS, 0);

    if (Bluefruit.Scanner.isRunning())
    {
        writeSerial("Scanning...");
        scan_count++;

        if (scan_count > 500)
        {
            Bluefruit.Scanner.stop();
            delay(500);
            scan_count = 0;
            return;
        }
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
        if (!service.discovered())
        {
            writeSerial("Service not discovered. Trying again.");
            delay(500);
            startScan();
            return;
        }
        characteristic1.discover();
        characteristic2.discover();
        characteristic3.discover();
        characteristic4.discover();

        const size_t CHARACTERISTIC_MAX_DATA_LEN = connection->getMtu() - 3;
        char buffer[4 * CHARACTERISTIC_MAX_DATA_LEN] = {0};
        char *current_pos = buffer;

        size_t bytes_received = characteristic1.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received;
        bytes_received = characteristic2.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received;
        bytes_received = characteristic3.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received;
        bytes_received = characteristic4.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
        current_pos += bytes_received;
        memset(current_pos, 0, buffer + sizeof(buffer) - current_pos);
        Bluefruit.disconnect(connection->handle());

        String data = String(buffer);
        parseHomeAssistantData(String(data), haData);
        writeSerial(String(data));

        if (haData.last_updated == "null")
        {
            writeSerial("Last updated is null. Trying again.");
            delay(500);
            startScan();
            return;
        }
        writeSerial("Last updated: " + String(haData.last_updated));
        writeSerial("Temperature inside: " + String(haData.temperature_inside));

        display.init(115200, false, 10, false); // wake up
        // display.setPartialWindow(0, 0, display.width(), display.height());
        display.firstPage();
        do
        {
            display.setCursor(0, 0);
            display.setFont(&GothamRounded_Book14pt8b);
            display.setTextSize(1);
            display.setTextColor(GxEPD_BLACK);
            display.write("\n");
            display.write("Count: ");
            display.write(String(refresh_count).c_str());
        } while (display.nextPage());
        hibernateDisplay();

        refresh_count++;
        delay(TIME_REFRESH);
        startScan();
    }
    else
    {
        writeSerial("Nothing found during BLE scan. Trying again in some seconds.");
        delay(TIME_RETRY_SCAN);
        startScan();
    }
}

void startScan()
{
    Bluefruit.setName("SeedPaperBLE");
    Bluefruit.autoConnLed(false);
    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.filterUuid(service.uuid);
    Bluefruit.Scanner.start(500); // Scan timeout in 10 ms units
    delay(100);
}

void hibernateDisplay()
{
    display.hibernate();
    pinMode(D4, INPUT); // RST
    SPI.end();
}

void writeSerial(String message, bool newLine)
{
    if (serial_enabled == 1)
    {
        if (newLine == true)
        {
            Serial.println(message);
        }
        else
        {
            Serial.print(message);
        }
    }
}

void initDisplay()
{
    display.init(115200, true, 10, false); //, true, 2, false); // initial = true
    display.setPartialWindow(0, 0, display.width(), display.height());
    display.setRotation(1);

    display.firstPage();
    do
    {
        display.setCursor(0, 0);
        display.setFont(&GothamRounded_Book14pt8b);
        display.setTextSize(1);
        display.setTextColor(GxEPD_BLACK);
        display.write("\n");
        display.write("Scanning...");
    } while (display.nextPage());
}

void scan_callback(ble_gap_evt_adv_report_t *report)
{
    writeSerial("Connecting...");
    Bluefruit.Central.connect(report);
}

void connect_callback(uint16_t conn_handle)
{
    writeSerial("Connected");
    connection = Bluefruit.Connection(conn_handle);
    delay(100);
    writeSerial("set connection to handle");
    // FIXME: move characteristic read + display update here?
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    (void)reason;

    writeSerial("Disconnected");
    Bluefruit.Scanner.stop();
}