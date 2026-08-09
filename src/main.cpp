#include <Arduino.h>
#include <bluefruit.h>

// Display facade — selects GxEPD2 (legacy HAT) or Seeed_GFX (EN04) at build time.
// Must precede the font headers so the GFXfont type is defined for them.
#include "display/display.h"
#include <fonts/GothamRoundedBook.h>
#include <fonts/GothamRoundedBold.h>
#include <fonts/GothamRoundedBoldBig.h>
#include "home_assistant.h"
#include "epaper.h"

// battery level
#define VREF 2.4
#define ADC_MAX 4096

// Battery ADC configuration.
// - XIAO nRF52840 (Sense): PIN_VBAT via VBAT_ENABLE, active LOW, 2.4V reference.
// - XIAO ePaper Display Board EN04: A0 reads the divider, A5 enables it active
//   HIGH, default reference + 12-bit, per the Seeed wiki battery example:
//   https://wiki.seeedstudio.com/epaper_EN04/#user-battery-on-xiao-epaper-display-boardnrf52840---en04
#if defined(DISPLAY_BACKEND_SEEEDGFX)
#define BATTERY_PIN A0
#define BATTERY_ENABLE_PIN A5
#define BATTERY_ENABLE_ACTIVE HIGH
#else
#define BATTERY_PIN PIN_VBAT
#define BATTERY_ENABLE_PIN VBAT_ENABLE
#define BATTERY_ENABLE_ACTIVE LOW
#endif

// User 1 button (EN04 KEY1), active-low, internal pull-up.
// https://wiki.seeedstudio.com/epaper_EN04/
#if defined(DISPLAY_BACKEND_SEEEDGFX)
#define BUTTON_REFRESH_PIN 1
#endif

// Debug: draw calibration borders (full panel outline + the OFFSET_* content
// window) so the display can be aligned inside the physical picture frame.
// Set to 1 to enable, or add `-D DEBUG_DISPLAY_BORDER=1` to the env's build_flags.
#ifndef DEBUG_DISPLAY_BORDER
#define DEBUG_DISPLAY_BORDER 0
#endif

// Refresh interval of HA data and display
const size_t TIME_REFRESH = 5 * 60 * 1000;

// Retry interval if no peripheral (server) is found
const size_t TIME_RETRY_SCAN = 60 * 1000;

// Timeout values
const size_t SCAN_TIMEOUT = 30 * 1000;       // 30 seconds max scan time
const size_t CONNECTION_TIMEOUT = 15 * 1000; // 15 seconds for connection operations
const uint8_t MAX_RETRY_ATTEMPTS = 6;

// Radio TX power (dBm). nRF52840 max is +8. A stronger transmit toward the peer can
// harden this marginal link and help connection establishment. RX sensitivity is fixed
// in hardware and NOT tunable. Valid levels: -40,-20,-16,-12,-8,-4,0,2,3,4,5,6,7,8.
const int8_t BLE_TX_POWER_DBM = 8;

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
bool refresh_failed = false; // on-screen data is stale (last refresh episode gave up)

// User 1 button (EN04 KEY1): set from a GPIO interrupt, consumed in loop(). volatile
// because it's written in ISR context. button_last_ms debounces inside the ISR.
#if defined(DISPLAY_BACKEND_SEEEDGFX)
volatile bool button_refresh_requested = false;
volatile unsigned long button_last_ms = 0;
#endif

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
void writeDisplayData(bool stale = false);
void markRefreshFailed();
#if defined(DISPLAY_BACKEND_SEEEDGFX)
void onUserButton();
#endif
void drawDebugBorder();
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

    // EXPERIMENT: connect at a faster FIXED interval than the Bluefruit 20ms default.
    // More frequent connection events = faster GATT setup AND more packet-retransmit
    // opportunities per unit time, which may harden the marginal link to the flaky
    // Realtek/BlueZ peer. Keep min == max (FIXED): a *range* let the link come up at
    // the slow 75ms max and broke service discovery (btmon-confirmed). If this doesn't
    // help, delete this line to fall back to the proven-good 20ms default.
    Bluefruit.Central.setConnIntervalMS(15, 15);

    // Crank radio TX power to the nRF52840 max. Bluefruit.setTxPower() alone only drives
    // the advertising role (unused by a central), so set the scanner/initiator role
    // directly — that governs scanning and the connect. The live-connection role is set
    // per-connection in the CONNECTED state once we have a handle.
    Bluefruit.setTxPower(BLE_TX_POWER_DBM);
    sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_SCAN_INIT, 0, BLE_TX_POWER_DBM);

    service.begin();
    characteristic1.begin();
    characteristic2.begin();
    characteristic3.begin();
    characteristic4.begin();

    // power management
    sd_power_dcdc_mode_set(NRF_POWER_DCDC_ENABLE);
    sd_power_mode_set(NRF_POWER_MODE_LOWPWR);

    // battery level
#if defined(DISPLAY_BACKEND_SEEEDGFX)
    // EN04: default analog reference + 12-bit ADC, matching the wiki's 7.16 factor.
    analogReadResolution(12);
#else
    analogReference(AR_INTERNAL_2_4);
    analogReadResolution(ADC_RESOLUTION);
#endif
    pinMode(BATTERY_PIN, INPUT);
    pinMode(BATTERY_ENABLE_PIN, OUTPUT);
    digitalWrite(BATTERY_ENABLE_PIN, BATTERY_ENABLE_ACTIVE);

#if defined(DISPLAY_BACKEND_SEEEDGFX)
    // User 1 button (EN04 KEY1): press to force an immediate BLE refresh. The ISR only
    // sets a flag; loop() acts on it. Active-low, so trigger on the falling edge.
    pinMode(BUTTON_REFRESH_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_REFRESH_PIN), onUserButton, FALLING);
#endif

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

#if defined(DISPLAY_BACKEND_SEEEDGFX)
    // User 1 button: force a refresh now. Only acted on when IDLE (waiting for the next
    // interval); if a refresh cycle is already running, the press is ignored.
    if (button_refresh_requested)
    {
        button_refresh_requested = false;
        if (current_state == IDLE)
        {
            writeSerialWithState("User button: forcing refresh");
            retry_count = 0;
            data_read_complete = false;
            startScan();
        }
        else
        {
            writeSerialWithState("User button: ignored (refresh already in progress)");
        }
    }
#endif

    switch (current_state)
    {
    case IDLE:
        // Waiting state, should transition to scanning
        break;

    case SCANNING:
        if (Bluefruit.Scanner.isRunning())
        {
            // Check for scan timeout
            if (hasTimedOut(SCAN_TIMEOUT))
            {
                writeSerialWithState("Scan timeout, no device found");
                Bluefruit.Scanner.stop();

                if (retry_count >= MAX_RETRY_ATTEMPTS)
                {
                    writeSerialWithState("Max retry attempts reached, waiting longer");
                    retry_count = 0;
                    markRefreshFailed(); // server not found — flag on-screen data stale
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
    {
        // Connection setup runs here (main task context), NOT in connect_callback.
        // Negotiating PHY / data length / MTU from the event callback caused the
        // link to fail to establish (HCI reason 0x3E) with a garbage MTU readback.
        if (connection == nullptr)
        {
            writeSerialWithState("Error: Connection lost before setup");
            setState(ERROR_STATE);
            break;
        }

        // Max TX power on the live connection too (SCAN_INIT set in setup() only covers
        // scanning/initiating, not the established link where the data transfer happens).
        connection->setTxPower(BLE_TX_POWER_DBM);

        connection->requestPHY();
        connection->requestDataLengthUpdate();
        connection->requestMtuExchange(BLE_GATT_ATT_MTU_MAX);

        // Wait for the MTU exchange to actually complete instead of blocking a flat
        // 500ms. The SoftDevice processes the exchange on its own task, so getMtu()
        // rises above the 23-byte default as soon as it lands — usually well under
        // 500ms. Cap the wait so a stalled negotiation can't hang the state machine,
        // and bail the moment the link drops mid-negotiation. (PHY/data-length are
        // link-layer optimisations that settle in the background; only the MTU gates
        // the reads below, so it's the one worth waiting on.)
        unsigned long negotiate_deadline = millis() + 500;
        while (millis() < negotiate_deadline)
        {
            if (connection == nullptr || connection->getMtu() > BLE_GATT_ATT_MTU_DEFAULT)
            {
                break;
            }
            delay(10);
        }

        // The link can drop during the negotiation window above. disconnect_callback
        // runs in the SoftDevice context and may already have nulled `connection` and
        // moved us out of CONNECTED. Re-validate before dereferencing `connection` or
        // advancing — otherwise we deref a null pointer (garbage peer name / MTU 0) and
        // clobber the ERROR state the callback just set by forcing READING_DATA.
        if (connection == nullptr || current_state != CONNECTED)
        {
            writeSerialWithState("Connection dropped during setup, aborting");
            break; // let disconnect_callback's state drive recovery
        }

        // A link whose MTU never rose past the 23-byte default is marginal: the
        // negotiation packets didn't get through (seen with the flaky Realtek/BlueZ
        // peer, which also leaves the peer name empty). Discovery + reads would only
        // crawl and fail on 20-byte chunks — the 240-byte characteristics can't even be
        // read whole — so bail and retry now instead of limping into a doomed read.
        if (connection->getMtu() <= BLE_GATT_ATT_MTU_DEFAULT)
        {
            writeSerialWithState("MTU not negotiated (still " + String(connection->getMtu()) + "), marginal link — retrying");
            connection->disconnect();
            setState(ERROR_STATE);
            break;
        }

        char peer_name[32] = {0};
        connection->getPeerName(peer_name, sizeof(peer_name));
        writeSerialWithState("Connected to " + String(peer_name) + " with MTU: " + String(connection->getMtu()));

        // Connection established, move to reading data
        if (!data_read_complete)
        {
            setState(READING_DATA);
        }
        break;
    }

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
            if (connection != nullptr)
            {
                connection->disconnect();
            }
            setState(ERROR_STATE);
            break;
        }

        // Discover and read data
        if (!service.discover(connection->handle()))
        {
            writeSerialWithState("Service discovery failed");
            if (connection != nullptr)
            {
                connection->disconnect();
            }
            setState(ERROR_STATE);
            break;
        }

        if (!service.discovered())
        {
            writeSerialWithState("Service not discovered");
            if (connection != nullptr)
            {
                connection->disconnect();
            }
            setState(ERROR_STATE);
            break;
        }

        // Discover all characteristics
        if (!characteristic1.discover() || !characteristic2.discover() ||
            !characteristic3.discover() || !characteristic4.discover())
        {
            writeSerialWithState("Characteristic discovery failed");
            if (connection != nullptr)
            {
                connection->disconnect();
            }
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
            if (connection != nullptr)
            {
                connection->disconnect();
            }
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
                markRefreshFailed(); // couldn't get valid data — flag on-screen data stale
                setState(IDLE);
                delay(TIME_RETRY_SCAN);
                startScan();
            }
            break;
        }

        writeSerialWithState("Last updated: " + String(haData.last_updated));
        writeSerialWithState("Temperature inside: " + String(haData.temperature_inside));

        // Update display
        epd::wake();
        writeDisplayData();
        hibernateDisplay();

        // Success - reset retry counter and clear the stale flag (the render above,
        // writeDisplayData() with stale=false, has already replaced any "(!)").
        retry_count = 0;
        refresh_failed = false;
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
            markRefreshFailed(); // connection kept failing — flag on-screen data stale
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
    // Backend-specific low-power sequence (see the display backends).
    epd::hibernate();
}

#if defined(DISPLAY_BACKEND_SEEEDGFX)
// GPIO interrupt for the EN04 User 1 button (KEY1, active-low). Keep it minimal: debounce
// mechanical bounce with a coarse millis() window and set a flag consumed by loop().
void onUserButton()
{
    unsigned long now = millis();
    if (now - button_last_ms < 300)
    {
        return;
    }
    button_last_ms = now;
    button_refresh_requested = true;
}
#endif

// Flag the on-screen data as stale after a refresh episode is abandoned. Does ONE full
// re-render of the last-good data with "(!)" appended — the UC8179 has no true partial
// refresh, so this is a full-panel update. Guarded so repeated failures don't re-flash
// the panel, and a no-op until we've shown real data at least once (refresh_count > 0)
// so the boot "Scanning..." screen isn't clobbered with empty fields.
void markRefreshFailed()
{
    if (refresh_failed || refresh_count == 0)
    {
        return;
    }
    refresh_failed = true;
    writeSerialWithState("Refresh failed — re-rendering display as stale (!)");
    epd::wake();
    writeDisplayData(true);
    hibernateDisplay();
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

void drawDebugBorder()
{
#if DEBUG_DISPLAY_BORDER
    // Full physical panel extent. If the bottom edge of THIS rectangle is missing
    // on hardware, the panel isn't refreshing its full height — a rendering issue,
    // not frame placement (adjusting OFFSET_* won't recover it).
    epd::drawRect(0, 0, epd::width(), epd::height());
    // Content window bounded by the OFFSET_* margins — tune those until this
    // rectangle lines up with the visible opening of your picture frame.
    epd::drawRect(OFFSET_LEFT, OFFSET_TOP,
                  epd::width() - OFFSET_LEFT - OFFSET_RIGHT,
                  epd::height() - OFFSET_TOP - OFFSET_BOTTOM);
#endif
}

void initDisplay()
{
    epd::begin(); // power on + init + defaults (rotation, black text)
    epd::setPartialWindow();
    epd::setTextColor();
    epd::setRotation(1);

    int16_t tbx, tby;
    uint16_t tbw, tbh;
    epd::setFont(&GothamRounded_Bold32pt7b); // title
    epd::getTextBounds(title, 0, 0, &tbx, &tby, &tbw, &tbh);

    epd::render([&]()
                {
        // print Title
        epd::setCursor(((epd::width() - tbw) / 2) - tbx - 25, OFFSET_TOP + 80);
        epd::print("Scanning...");
        drawDebugBorder(); });
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

    // Keep this callback minimal: it runs in the Bluefruit event-handler context.
    // Blocking here (delays) or issuing PHY/DataLength/MTU negotiations from the
    // callback destabilises the freshly-formed link and it fails to establish
    // (HCI disconnect reason 0x3E). All connection setup is deferred to the
    // CONNECTED state in loop(), which runs in the main task context.
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
    epd::setFont(&GothamRounded_Book14pt8b);

    epd::setCursor(OFFSET_LEFT + offset_x, OFFSET_TOP + offset_y);
    epd::print(time);

    epd::drawBitmap(OFFSET_LEFT + offset_x + 15, OFFSET_TOP + offset_y + 8, icon, GLYPH_SIZE_WEATHER_SMALL, GLYPH_SIZE_WEATHER_SMALL);

    if (temperature > 9.99)
    {
        epd::setCursor(OFFSET_LEFT + offset_x + 10, OFFSET_TOP + offset_y + 10 + GLYPH_SIZE_WEATHER_SMALL + 28);
    }
    else
    {
        epd::setCursor(OFFSET_LEFT + offset_x + 15, OFFSET_TOP + offset_y + 10 + GLYPH_SIZE_WEATHER_SMALL + 28);
    }
    epd::setFont(&GothamRounded_Bold14pt8b);
    epd::printf("%.0f°C", temperature);
}

void writeDisplayData(bool stale)
{
    epd::setRotation(1);

    epd::setTextColor();
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    epd::setFont(&GothamRounded_Bold32pt7b); // title
    epd::getTextBounds(title, 0, 0, &tbx, &tby, &tbw, &tbh);

    epd::render([&]()
                {
        // print Title
        epd::setCursor(((epd::width() - tbw) / 2) - tbx, OFFSET_TOP + 80);
        epd::print(title);

        // print big weather icon and temperature
        weather_icon weather_icon = get_weather_icon(haData.weather_forecast_now);
        epd::drawBitmap(OFFSET_LEFT + 10, OFFSET_TOP + 110, weather_icon, GLYPH_SIZE_WEATHER, GLYPH_SIZE_WEATHER);
        epd::setCursor(OFFSET_LEFT + 110, OFFSET_TOP + 190);
        epd::setFont(&GothamRounded_Bold48pt8b);
        epd::printf("%.1f°C", haData.temperature_outside);

        // print humidity and wind
        epd::setFont(&GothamRounded_Bold14pt8b);
        size_t weatherdetails_offset = 250;
        epd::drawBitmap(OFFSET_LEFT + 30, OFFSET_TOP + weatherdetails_offset - GLYPH_SIZE_WEATHER_SMALL / 2, weather_small_wind, GLYPH_SIZE_WEATHER_SMALL, GLYPH_SIZE_WEATHER_SMALL);
        epd::setCursor(OFFSET_LEFT + 90, OFFSET_TOP + weatherdetails_offset + GLYPH_SIZE_WEATHER_SMALL / 4);
        epd::printf("%.1fkm/h", haData.wind_speed);

        epd::drawBitmap(OFFSET_LEFT + 230, OFFSET_TOP + weatherdetails_offset - GLYPH_SIZE_WEATHER_SMALL / 2, icon_humidity, GLYPH_SIZE_WEATHER_SMALL, GLYPH_SIZE_WEATHER_SMALL);
        epd::setCursor(OFFSET_LEFT + 290, OFFSET_TOP + weatherdetails_offset + GLYPH_SIZE_WEATHER_SMALL / 4);
        epd::printf("%.1f%%", haData.humidity_outside);

        // print forecasts
        size_t forecast_offset_y = weatherdetails_offset + 60;
        printForecast(30, forecast_offset_y, get_weather_icon(haData.weather_forecast_2h, true), haData.weather_forecast_2h_temp, haData.weather_forecast_2h_time);
        printForecast(130, forecast_offset_y, get_weather_icon(haData.weather_forecast_4h, true), haData.weather_forecast_4h_temp, haData.weather_forecast_4h_time);
        printForecast(230, forecast_offset_y, get_weather_icon(haData.weather_forecast_6h, true), haData.weather_forecast_6h_temp, haData.weather_forecast_6h_time);
        printForecast(330, forecast_offset_y, get_weather_icon(haData.weather_forecast_8h, true), haData.weather_forecast_8h_temp, haData.weather_forecast_8h_time);

        // living room temperature — values print at native Bold32pt7b size (same
        // asset resolution as everywhere else, so they stay just as crisp) rather
        // than upscaling the small Bold14pt8b glyphs, which looked chunky.
        epd::drawBitmap(OFFSET_LEFT + 30, OFFSET_TOP + 430, icon_living_room, 80, 80);
        epd::drawBitmap(OFFSET_LEFT + 140, OFFSET_TOP + 440, icon_thermometer, 45, 45);
        epd::drawBitmap(OFFSET_LEFT + 140, OFFSET_TOP + 520, icon_humidity, 45, 45);

        epd::setFont(&GothamRounded_Bold32pt7b);

        // GothamRounded_Bold32pt7b has no ° glyph (its table stops at 0x7E), so
        // the degree ring is spliced in as a small bitmap between the number and "C".
        char tempBuf[8];
        snprintf(tempBuf, sizeof(tempBuf), "%.1f", haData.temperature_inside);
        int16_t tempX = OFFSET_LEFT + 200, tempY = OFFSET_TOP + 484;
        epd::setCursor(tempX, tempY);
        epd::print(tempBuf);
        epd::getTextBounds(tempBuf, tempX, tempY, &tbx, &tby, &tbw, &tbh);
        int16_t degreeX = tbx + tbw + 4;
        epd::drawBitmap(degreeX, tempY - 40, icon_degree, 10, 10, 2);
        epd::setCursor(degreeX + 20 + 4, tempY);
        epd::print("C");

        epd::setCursor(OFFSET_LEFT + 200, OFFSET_TOP + 564);
        epd::printf("%.1f%%", haData.humidity_inside);

        epd::setFont(&GothamRounded_Book14pt8b);

        // battery level
        float batteryVoltage = getBatteryVoltage();
        epd::setCursor(OFFSET_LEFT + 5, OFFSET_TOP + 670);
        epd::printf("%.1fV", batteryVoltage);

        // last update — append "(!)" when the shown data is stale (last refresh failed)
        epd::getTextBounds("STAND 11:11", 0, 0, &tbx, &tby, &tbw, &tbh);
        epd::setCursor(((epd::width() - tbw) / 2) - tbx - 10, OFFSET_TOP + 670);
        epd::printf("STAND %s", haData.time.c_str());
        if (stale)
        {
            epd::print(" (!)");
        }

        // calibration borders (no-op unless DEBUG_DISPLAY_BORDER is enabled)
        drawDebugBorder(); });
}

// Read a characteristic, retrying a few times before giving up. The peer's flaky
// Realtek/BlueZ controller intermittently drops a fragment of a large multi-packet
// read response, so the SoftDevice errors the read and returns 0 (fast, not a timeout).
// A fresh read of the same value almost always succeeds — retrying here recovers it on
// the still-live link instead of tearing down the whole connection and paying the full
// re-scan + re-discover cost. Returns 0 only if every attempt failed (or the link drops).
static size_t readCharacteristicWithRetry(BLEClientCharacteristic &chr, char *dst,
                                          size_t maxlen, const char *label)
{
    const uint8_t READ_ATTEMPTS = 4;
    for (uint8_t attempt = 1; attempt <= READ_ATTEMPTS; attempt++)
    {
        size_t n = chr.read(dst, maxlen);
        if (n > 0)
        {
            return n;
        }
        if (connection == nullptr)
        {
            break; // link gone — retrying is pointless
        }
        writeSerialWithState(String("Read ") + label + " returned 0, retry " +
                             String(attempt) + "/" + String(READ_ATTEMPTS));
        delay(20);
    }
    return 0;
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
    size_t bytes_received = readCharacteristicWithRetry(characteristic1, current_pos, CHARACTERISTIC_MAX_DATA_LEN, "characteristic 1");
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
    bytes_received = readCharacteristicWithRetry(characteristic2, current_pos, CHARACTERISTIC_MAX_DATA_LEN, "characteristic 2");
    total_bytes += bytes_received;
    current_pos += bytes_received;

    if (total_bytes + CHARACTERISTIC_MAX_DATA_LEN > buffer_size)
    {
        writeSerialWithState("Buffer overflow prevented");
        return false;
    }

    // Read characteristic 3
    bytes_received = readCharacteristicWithRetry(characteristic3, current_pos, CHARACTERISTIC_MAX_DATA_LEN, "characteristic 3");
    total_bytes += bytes_received;
    current_pos += bytes_received;

    if (total_bytes + CHARACTERISTIC_MAX_DATA_LEN > buffer_size)
    {
        writeSerialWithState("Buffer overflow prevented");
        return false;
    }

    // Read characteristic 4
    bytes_received = readCharacteristicWithRetry(characteristic4, current_pos, CHARACTERISTIC_MAX_DATA_LEN, "characteristic 4");
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
#if defined(DISPLAY_BACKEND_SEEEDGFX)
    // XIAO ePaper Display Board EN04: A5 (active HIGH) enables the on-board
    // divider on A0; the 7.16 factor folds the divider ratio and the default
    // 12-bit reference into one calibration constant (Seeed wiki battery example).
    delay(10); // wiki: a short settle before analogRead improves precision
    unsigned int adcCount = analogRead(BATTERY_PIN);
    return (adcCount / 4096.0f) * 7.16f;
#else
    unsigned int adcCount = analogRead(BATTERY_PIN);
    float adcVoltage = adcCount * VREF / ADC_MAX;
    return ((510e3 + 1000e3) / 510e3) * adcVoltage;
#endif
}