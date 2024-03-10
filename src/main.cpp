#include <ArduinoBLE.h>
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

// select the display class and display driver class in the following file (new style):
#include "paper-config/GxEPD2_display_selection_new_style.h"

BLEService myService("fff0");
BLEIntCharacteristic myCharacteristic("fff1", BLERead | BLEBroadcast);

// Advertising parameters should have a global scope. Do NOT define them in 'setup' or in 'loop'
const String SERVER_NAME = "SeedPaperBLEServer";
const String SERVICE_UUID = "D2EA587F-19C8-4F4C-8179-3BA0BC150B01";
const String CHARACTERISTIC1_UUID = "0DF8D897-33FE-4AF4-9E7A-63D24664C94C";
const String CHARACTERISTIC2_UUID = "0DF8D897-33FE-4AF4-9E7A-63D24664C94D";
const String CHARACTERISTIC3_UUID = "0DF8D897-33FE-4AF4-9E7A-63D24664C94E";
const String CHARACTERISTIC4_UUID = "0DF8D897-33FE-4AF4-9E7A-63D24664C94F";

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

void setup()
{
  Serial.begin(9600);
  while (!Serial)
    ;

  if (!BLE.begin())
  {
    Serial.println("failed to initialize BLE!");
    while (1)
      ;
  }

  // display.init(115200, false, 2, false); // initial = false
  // display.setPartialWindow(0, 0, display.width(), display.height());
}

String readCharacteristic(BLEDevice peripheral, BLEService service, String characteristic_uuid)
{
  BLECharacteristic characteristic = service.characteristic(characteristic_uuid.c_str());
  char val[1024] = {0};
  if (characteristic.canRead())
  {
    int success = characteristic.read();
    if (success == 0)
    {
      Serial.println("Characteristic read failed.");
    }
    else
    {
      Serial.print("Characteristic read success. Bytes read: ");
      Serial.println(characteristic.valueLength());
      characteristic.readValue(val, characteristic.valueLength());
    }
  }
  return String(val);
}

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
  BLEService service;
  BLEDevice peripheral;
  do
  {
    Serial.println("Scanning for server...");
    BLE.scanForName(SERVER_NAME);
    peripheral = BLE.available();
  } while (!peripheral);

  BLE.stopScan();
  Serial.println("Server found. Connecting...");
  peripheral.connect();

  while (peripheral.connected())
  {
    peripheral.discoverService(SERVICE_UUID.c_str());

    Serial.print("Connected to server/peripheral: ");
    Serial.print(peripheral.address());
    Serial.print(" '");
    Serial.print(peripheral.localName());
    Serial.print("' Characteristics: ");
    Serial.print(peripheral.characteristicCount());
    Serial.print(" Services: ");
    Serial.print(peripheral.serviceCount());
    Serial.println();

    service = peripheral.service(SERVICE_UUID.c_str());
    if (!service.hasCharacteristic(CHARACTERISTIC1_UUID.c_str()) || !service.hasCharacteristic(CHARACTERISTIC2_UUID.c_str()))
    {
      Serial.println("Characteristics not found.");
      Serial.print(peripheral.serviceCount());
      Serial.println(" services found.");
      peripheral.disconnect();
      continue;
    }

    Serial.println("Reading characteristics...");

    String c1 = readCharacteristic(peripheral, service, CHARACTERISTIC1_UUID);
    String c2 = readCharacteristic(peripheral, service, CHARACTERISTIC2_UUID);
    String c3 = readCharacteristic(peripheral, service, CHARACTERISTIC3_UUID);
    String c4 = readCharacteristic(peripheral, service, CHARACTERISTIC4_UUID);

    String final_val = c1 + c2 + c3 + c4;
    Serial.println(final_val);

    HAData haData;
    getData(final_val, haData);

    // // check if the value of the simple key characteristic has been updated
    // if (simpleKeyCharacteristic.valueUpdated())
    // {
    //   // yes, get the value, characteristic is 1 byte so use byte value
    //   byte value = 0;

    //   simpleKeyCharacteristic.readValue(value);

    //   if (value & 0x01)
    //   {
    //     // first bit corresponds to the right button
    //     Serial.println("Right button pressed");
    //   }

    //   if (value & 0x02)
    //   {
    //     // second bit corresponds to the left button
    //     Serial.println("Left button pressed");
    //   }
    // }
    delay(30000);
  }
}