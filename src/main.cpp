#include <Arduino.h>
#include <bluefruit.h>
#include <SPI.h>

#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_7C.h>
#include <fonts/GothamRoundedBook.h>
#include <fonts/GothamRoundedBold.h>
#include <fonts/GothamRoundedBoldBig.h>
#include "paper-config/GxEPD2_display_selection_new_style.h"
#include "home_assistant.h"
#include "epaper.h"
#include <ctime>

// battery level
#define VREF 2.4
#define ADC_MAX 4096

// Refresh interval of HA data and display
const size_t TIME_REFRESH = 5 * 60 * 1000;

// Retry interval if no peripheral (server) is found
const size_t TIME_RETRY_SCAN = 60 * 1000;

// Timeout values
const size_t SCAN_TIMEOUT = 30 * 1000;       // 30 seconds max scan time
const size_t CONNECTION_TIMEOUT = 15 * 1000; // 15 seconds for connection operations
const uint8_t MAX_RETRY_ATTEMPTS = 3;

// BLE state machine
enum BLE_STATE
{
    IDLE,
    SCANNING,
    CONNECTING,
    CONNECTED,
    READING_DATA,
    DISCONNECTING,
    ERROR_STATE
};

BLE_STATE current_state = IDLE;
unsigned long state_start_time = 0;
unsigned long last_refresh_time = 0;
uint8_t retry_count = 0;
bool data_read_complete = false;

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
void writeSerialWithState(String message, bool newLine = true);
void scan_callback(ble_gap_evt_adv_report_t *report);
void connect_callback(uint16_t conn_handle);
void disconnect_callback(uint16_t conn_handle, uint8_t reason);
void initDisplay();
void startScan();
void hibernateDisplay();
void writeDisplayData();
float getBatteryVoltage();
bool readCharacteristicsData(char *buffer, size_t buffer_size);
void setState(BLE_STATE new_state);
const char *getStateName(BLE_STATE state);
bool hasTimedOut(unsigned long timeout);

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
    Bluefruit.setName("SeedPaperBLE");
    Bluefruit.autoConnLed(false);
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);

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

    // battery level
    analogReference(AR_INTERNAL_2_4);
    analogReadResolution(ADC_RESOLUTION);
    pinMode(PIN_VBAT, INPUT);
    pinMode(VBAT_ENABLE, OUTPUT);
    digitalWrite(VBAT_ENABLE, LOW);

    // init display
    initDisplay();
    writeSerial("setup completed");

    // Initialize timing
    last_refresh_time = millis();
    setState(IDLE);

    startScan();
}

void loop()
{
    serial_enabled = bitRead(NRF_POWER->USBREGSTATUS, 0);

    switch (current_state)
    {
    case IDLE:
        // Waiting state, should transition to scanning
        break;

    case SCANNING:
        if (Bluefruit.Scanner.isRunning())
        {
            writeSerialWithState("Scanning for devices...");
            // Check for scan timeout
            if (hasTimedOut(SCAN_TIMEOUT))
            {
                writeSerialWithState("Scan timeout, no device found");
                Bluefruit.Scanner.stop();

                if (retry_count >= MAX_RETRY_ATTEMPTS)
                {
                    writeSerialWithState("Max retry attempts reached, waiting longer");
                    retry_count = 0;
                    setState(IDLE);
                    // Wait longer before next attempt
                    delay(TIME_RETRY_SCAN);
                    startScan();
                }
                else
                {
                    retry_count++;
                    unsigned long backoff = 5000 * retry_count; // Exponential backoff
                    writeSerialWithState("Retrying scan in " + String(backoff / 1000) + "s (attempt " + String(retry_count) + "/" + String(MAX_RETRY_ATTEMPTS) + ")");
                    delay(backoff);
                    startScan();
                }
            }
        }
        else
        {
            // Scanner stopped but not connected - retry
            setState(IDLE);
            delay(TIME_RETRY_SCAN);
            startScan();
        }
        break;

    case CONNECTING:
        // Wait for connection callback, check timeout
        if (hasTimedOut(CONNECTION_TIMEOUT))
        {
            writeSerialWithState("Connection timeout");
            if (connection != nullptr)
            {
                connection->disconnect();
            }
            setState(ERROR_STATE);
        }
        break;

    case CONNECTED:
        // Connection established, move to reading data
        if (!data_read_complete)
        {
            setState(READING_DATA);
        }
        break;

    case READING_DATA:
    {
        if (connection == nullptr)
        {
            writeSerialWithState("Error: Connection lost during data read");
            setState(ERROR_STATE);
            break;
        }

        if (hasTimedOut(CONNECTION_TIMEOUT))
        {
            writeSerialWithState("Data read timeout");
            connection->disconnect();
            setState(ERROR_STATE);
            break;
        }

        // Discover and read data
        if (!service.discover(connection->handle()))
        {
            writeSerialWithState("Service discovery failed");
            connection->disconnect();
            setState(ERROR_STATE);
            break;
        }

        if (!service.discovered())
        {
            writeSerialWithState("Service not discovered");
            connection->disconnect();
            setState(ERROR_STATE);
            break;
        }

        // Discover all characteristics
        if (!characteristic1.discover() || !characteristic2.discover() ||
            !characteristic3.discover() || !characteristic4.discover())
        {
            writeSerialWithState("Characteristic discovery failed");
            connection->disconnect();
            setState(ERROR_STATE);
            break;
        }

        // Read data into buffer with safety checks
        const size_t CHARACTERISTIC_MAX_DATA_LEN = connection->getMtu() - 3;
        const size_t BUFFER_SIZE = 4 * CHARACTERISTIC_MAX_DATA_LEN;
        char buffer[BUFFER_SIZE] = {0};

        if (!readCharacteristicsData(buffer, BUFFER_SIZE))
        {
            writeSerialWithState("Failed to read characteristics data");
            connection->disconnect();
            setState(ERROR_STATE);
            break;
        }

        // Disconnect after successful read
        setState(DISCONNECTING);
        Bluefruit.disconnect(connection->handle());

        // Parse and validate data
        String data = String(buffer);
        parseHomeAssistantData(data, haData);
        writeSerialWithState("Data received: " + String(data.length()) + " bytes");

        if (haData.last_updated == "null" || haData.last_updated == "")
        {
            writeSerialWithState("Invalid data received (null last_updated)");
            if (retry_count < MAX_RETRY_ATTEMPTS)
            {
                retry_count++;
                setState(IDLE);
                delay(5000 * retry_count);
                startScan();
            }
            else
            {
                writeSerialWithState("Max retries reached for invalid data");
                retry_count = 0;
                setState(IDLE);
                delay(TIME_RETRY_SCAN);
                startScan();
            }
            break;
        }

        writeSerialWithState("Last updated: " + String(haData.last_updated));
        writeSerialWithState("Temperature inside: " + String(haData.temperature_inside));

        // Update display
        digitalWrite(D0, HIGH);
        display.init(115200, false, 2, true);
        writeDisplayData();
        hibernateDisplay();

        // Success - reset retry counter
        retry_count = 0;
        refresh_count++;
        data_read_complete = true;

        // Wait for refresh interval
        last_refresh_time = millis();
        setState(IDLE);
        break;
    }

    case DISCONNECTING:
        // Wait for disconnect callback
        if (hasTimedOut(5000))
        {
            writeSerialWithState("Disconnect timeout, forcing state change");
            setState(IDLE);
        }
        break;

    case ERROR_STATE:
        writeSerialWithState("Error state, attempting recovery");
        if (connection != nullptr && Bluefruit.Central.connected())
        {
            connection->disconnect();
            delay(1000);
        }
        retry_count++;
        if (retry_count >= MAX_RETRY_ATTEMPTS)
        {
            writeSerialWithState("Max errors reached, long delay");
            retry_count = 0;
            delay(TIME_RETRY_SCAN);
        }
        else
        {
            delay(5000 * retry_count);
        }
        setState(IDLE);
        startScan();
        break;
    }

    // Check if it's time for next refresh (when in IDLE state)
    if (current_state == IDLE && data_read_complete)
    {
        if (millis() - last_refresh_time >= TIME_REFRESH)
        {
            writeSerialWithState("Refresh interval elapsed, starting new scan");
            data_read_complete = false;
            startScan();
        }
    }
    delay(100); // Small delay to prevent tight loop
}

void startScan()
{
    writeSerialWithState("Starting BLE scan");
    setState(SCANNING);

    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);
    Bluefruit.Scanner.restartOnDisconnect(false);
    Bluefruit.Scanner.useActiveScan(false);
    Bluefruit.Scanner.filterUuid(service.uuid);
    Bluefruit.Scanner.start(0); // 0 = no timeout, we handle it ourselves
}

void hibernateDisplay()
{
    // I don't know why these steps are needed, but there's no
    // other chance to get below 30 uA
    display.hibernate();
    pinMode(D4, INPUT); // RST
    SPI.end();
    digitalWrite(D0, LOW);
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
    // pull PWR (D0) high (Power for the ePaper Driver hat)
    pinMode(D0, OUTPUT);
    digitalWrite(D0, HIGH);

    // display.init(115200, true, 10, false); //, true, 2, false); // initial = true this is for the small display
    display.init(115200, true, 2, true);
    display.setPartialWindow(0, 0, display.width(), display.height());
    display.setTextColor(GxEPD_BLACK);
    display.setRotation(1);

    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.setFont(&GothamRounded_Bold32pt7b); // title
    display.getTextBounds(title, 0, 0, &tbx, &tby, &tbw, &tbh);

    display.firstPage();

    do
    {
        // print Title
        display.setCursor(((display.width() - tbw) / 2) - tbx - 25, OFFSET_TOP + 80);
        display.print("Scanning...");
    } while (display.nextPage());
}

void scan_callback(ble_gap_evt_adv_report_t *report)
{
    Serial.printf("Packet received from ");
    Serial.printBuffer(report->peer_addr.addr, 6, ':');
    if (Bluefruit.Scanner.checkReportForUuid(report, service.uuid))
    {
        Bluefruit.Scanner.stop();
        writeSerialWithState("Service found, initiating connection");
        setState(CONNECTING);

        Bluefruit.Central.connect(report);
    }
}

void connect_callback(uint16_t conn_handle)
{
    connection = Bluefruit.Connection(conn_handle);
    if (connection == nullptr)
    {
        writeSerialWithState("Error: Failed to get connection object");
        setState(ERROR_STATE);
        return;
    }

    // Perform connection setup operations once
    connection->requestPHY();
    delay(100);
    connection->requestDataLengthUpdate();
    delay(100);
    connection->requestMtuExchange(BLE_GATT_ATT_MTU_MAX);
    delay(100);

    char peer_name[32] = {0};
    connection->getPeerName(peer_name, sizeof(peer_name));
    writeSerialWithState("Connected to " + String(peer_name) + " with MTU: " + String(connection->getMtu()));
    delay(1000);
    setState(CONNECTED);
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;

    writeSerialWithState("Disconnected (reason: " + String(reason) + ")");
    connection = nullptr;

    if (current_state != ERROR_STATE && current_state == CONNECTED)
    {
        writeSerialWithState("Error: Connection lost unexpectedly");
        setState(ERROR_STATE);
    }
    else if (current_state != ERROR_STATE && current_state != IDLE)
    {
        setState(IDLE);
    }
}

void printForecast(int offset_x, int offset_y, weather_icon icon, float temperature, String time)
{
    display.setFont(&GothamRounded_Book14pt8b);

    display.setCursor(OFFSET_LEFT + offset_x, OFFSET_TOP + offset_y);
    display.print(time);

    display.drawBitmap(OFFSET_LEFT + offset_x + 15, OFFSET_TOP + offset_y + 8, icon, GLYPH_SIZE_WEATHER_SMALL, GLYPH_SIZE_WEATHER_SMALL, GxEPD_BLACK);

    if (temperature > 9.99)
    {
        display.setCursor(OFFSET_LEFT + offset_x + 10, OFFSET_TOP + offset_y + 10 + GLYPH_SIZE_WEATHER_SMALL + 28);
    }
    else
    {
        display.setCursor(OFFSET_LEFT + offset_x + 15, OFFSET_TOP + offset_y + 10 + GLYPH_SIZE_WEATHER_SMALL + 28);
    }
    display.setFont(&GothamRounded_Bold14pt8b);
    display.printf("%.0f°C", temperature);
}

void writeDisplayData()
{
    display.setRotation(1);

    display.setTextColor(GxEPD_BLACK);
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.setFont(&GothamRounded_Bold32pt7b); // title
    display.getTextBounds(title, 0, 0, &tbx, &tby, &tbw, &tbh);

    display.firstPage();

    do
    {
        // print Title
        display.setCursor(((display.width() - tbw) / 2) - tbx, OFFSET_TOP + 80);
        display.print(title);

        // print big weather icon and temperature
        weather_icon weather_icon = get_weather_icon(haData.weather_forecast_now);
        display.drawBitmap(OFFSET_LEFT + 10, OFFSET_TOP + 110, weather_icon, GLYPH_SIZE_WEATHER, GLYPH_SIZE_WEATHER, GxEPD_BLACK);
        display.setCursor(OFFSET_LEFT + 110, OFFSET_TOP + 190);
        display.setFont(&GothamRounded_Bold48pt8b);
        display.printf("%.1f°C", haData.temperature_outside);

        // print humidity and wind
        display.setFont(&GothamRounded_Bold14pt8b);
        size_t weatherdetails_offset = 250;
        display.drawBitmap(OFFSET_LEFT + 30, OFFSET_TOP + weatherdetails_offset - GLYPH_SIZE_WEATHER_SMALL / 2, weather_small_wind, GLYPH_SIZE_WEATHER_SMALL, GLYPH_SIZE_WEATHER_SMALL, GxEPD_BLACK);
        display.setCursor(OFFSET_LEFT + 90, OFFSET_TOP + weatherdetails_offset + GLYPH_SIZE_WEATHER_SMALL / 4);
        display.printf("%.1fkm/h", haData.wind_speed);

        display.drawBitmap(OFFSET_LEFT + 230, OFFSET_TOP + weatherdetails_offset - GLYPH_SIZE_WEATHER_SMALL / 2, icon_humidity, GLYPH_SIZE_WEATHER_SMALL, GLYPH_SIZE_WEATHER_SMALL, GxEPD_BLACK);
        display.setCursor(OFFSET_LEFT + 290, OFFSET_TOP + weatherdetails_offset + GLYPH_SIZE_WEATHER_SMALL / 4);
        display.printf("%.1f%%", haData.humidity_outside);

        // print forecasts
        size_t forecast_offset_y = weatherdetails_offset + 60;
        printForecast(30, forecast_offset_y, get_weather_icon(haData.weather_forecast_2h, true), haData.weather_forecast_2h_temp, haData.weather_forecast_2h_time);
        printForecast(130, forecast_offset_y, get_weather_icon(haData.weather_forecast_4h, true), haData.weather_forecast_4h_temp, haData.weather_forecast_4h_time);
        printForecast(230, forecast_offset_y, get_weather_icon(haData.weather_forecast_6h, true), haData.weather_forecast_6h_temp, haData.weather_forecast_6h_time);
        printForecast(330, forecast_offset_y, get_weather_icon(haData.weather_forecast_8h, true), haData.weather_forecast_8h_temp, haData.weather_forecast_8h_time);

        // living room temperature
        display.drawBitmap(OFFSET_LEFT + 30, OFFSET_TOP + 430, icon_living_room, 80, 80, GxEPD_BLACK);
        display.drawBitmap(OFFSET_LEFT + 120, OFFSET_TOP + 430, icon40_thermometer, 40, 40, GxEPD_BLACK);
        display.drawBitmap(OFFSET_LEFT + 120, OFFSET_TOP + 470, icon40_humidity, 40, 40, GxEPD_BLACK);

        display.setFont(&GothamRounded_Bold14pt8b);
        display.setCursor(OFFSET_LEFT + 165, OFFSET_TOP + 460);
        display.printf("%.1f°C", haData.temperature_inside);

        display.setCursor(OFFSET_LEFT + 165, OFFSET_TOP + 500);
        display.printf("%.1f%%", haData.humidity_inside);

        // dog age
        int y, M, d, h, m;
        float s;
        sscanf(haData.last_updated.c_str(), "%d-%d-%dT%d:%d:%f+00:00", &y, &M, &d, &h, &m, &s); // "2024-04-15T15:26:00.392326+00:00"

        std::tm now_tm = {0, 0, 0, d, M - 1, y - 1900};
        std::time_t now = std::mktime(&now_tm);
        std::tm birthday_tm = {0, 0, 0, 25, 2, 2024 - 1900}; /* March 25, 2024 */
        std::time_t birthday = std::mktime(&birthday_tm);

        int years_old = std::difftime(now, birthday) / (365.25 * 24 * 3600);
        int months_old = (std::difftime(now, birthday) - (years_old * 365.25 * 24 * 3600)) / (30.44 * 24 * 3600);
        int total_months_old = years_old * 12 + months_old;

        display.drawBitmap(OFFSET_LEFT + 30, OFFSET_TOP + 550, image_dog, 80, 50, GxEPD_BLACK);
        display.setCursor(OFFSET_LEFT + 120, OFFSET_TOP + 585);
        display.printf("%d Monate", total_months_old);

        display.setFont(&GothamRounded_Book14pt8b);

        // battery level
        float batteryVoltage = getBatteryVoltage();
        display.setCursor(OFFSET_LEFT + 5, OFFSET_TOP + 670);
        display.printf("%.1fV", batteryVoltage);

        // last update
        display.getTextBounds("STAND 11:11", 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor(((display.width() - tbw) / 2) - tbx - 10, OFFSET_TOP + 670);
        display.printf("STAND %s", haData.time.c_str());

        // calibrate borders
        // display.drawRect(OFFSET_LEFT, OFFSET_TOP, display.width() - OFFSET_LEFT - OFFSET_RIGHT, display.height() - OFFSET_TOP - OFFSET_BOTTOM, GxEPD_BLACK);
    } while (display.nextPage());
}

bool readCharacteristicsData(char *buffer, size_t buffer_size)
{
    if (connection == nullptr || buffer == nullptr)
    {
        return false;
    }

    const size_t CHARACTERISTIC_MAX_DATA_LEN = connection->getMtu() - 3;
    size_t total_bytes = 0;
    char *current_pos = buffer;

    // Read characteristic 1
    size_t bytes_received = characteristic1.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
    if (bytes_received == 0)
    {
        writeSerialWithState("Failed to read characteristic 1");
        return false;
    }
    total_bytes += bytes_received;
    current_pos += bytes_received;

    // Check buffer bounds
    if (total_bytes + CHARACTERISTIC_MAX_DATA_LEN > buffer_size)
    {
        writeSerialWithState("Buffer overflow prevented");
        return false;
    }

    // Read characteristic 2
    bytes_received = characteristic2.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
    total_bytes += bytes_received;
    current_pos += bytes_received;

    if (total_bytes + CHARACTERISTIC_MAX_DATA_LEN > buffer_size)
    {
        writeSerialWithState("Buffer overflow prevented");
        return false;
    }

    // Read characteristic 3
    bytes_received = characteristic3.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
    total_bytes += bytes_received;
    current_pos += bytes_received;

    if (total_bytes + CHARACTERISTIC_MAX_DATA_LEN > buffer_size)
    {
        writeSerialWithState("Buffer overflow prevented");
        return false;
    }

    // Read characteristic 4
    bytes_received = characteristic4.read(current_pos, CHARACTERISTIC_MAX_DATA_LEN);
    total_bytes += bytes_received;
    current_pos += bytes_received;

    // Null terminate
    if (total_bytes < buffer_size)
    {
        buffer[total_bytes] = '\0';
    }
    else
    {
        buffer[buffer_size - 1] = '\0';
    }

    writeSerialWithState("Read " + String(total_bytes) + " bytes from characteristics");
    return true;
}

void setState(BLE_STATE new_state)
{
    if (current_state != new_state)
    {
        writeSerial("State: " + String(getStateName(current_state)) + " -> " + String(getStateName(new_state)));
        current_state = new_state;
        state_start_time = millis();
    }
}

const char *getStateName(BLE_STATE state)
{
    switch (state)
    {
    case IDLE:
        return "IDLE";
    case SCANNING:
        return "SCANNING";
    case CONNECTING:
        return "CONNECTING";
    case CONNECTED:
        return "CONNECTED";
    case READING_DATA:
        return "READING_DATA";
    case DISCONNECTING:
        return "DISCONNECTING";
    case ERROR_STATE:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

bool hasTimedOut(unsigned long timeout)
{
    return (millis() - state_start_time) >= timeout;
}

void writeSerialWithState(String message, bool newLine)
{
    if (serial_enabled == 1)
    {
        String prefix = "[" + String(getStateName(current_state)) + "] ";
        writeSerial(prefix + message, newLine);
    }
}

float getBatteryVoltage()
{
    unsigned int adcCount = analogRead(PIN_VBAT);
    float adcVoltage = adcCount * VREF / ADC_MAX;
    return ((510e3 + 1000e3) / 510e3) * adcVoltage;
}