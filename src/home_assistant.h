
#include <Arduino.h>
#include <ArduinoJson.h>

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

void parseHomeAssistantData(String input, HAData &haData)
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