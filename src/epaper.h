#include <map>
#include <stddef.h>
#include <glyphs/weather.h>
#include <glyphs/icons.h>
#include <glyphs/weather_small.h>
#include <WString.h>

// set borders of display inside frame
#define OFFSET_LEFT 18
#define OFFSET_TOP 35
#define OFFSET_RIGHT 23
#define OFFSET_BOTTOM 80

const char *title = "WETTER";
const size_t GLYPH_SIZE_WEATHER = 100;
const size_t GLYPH_SIZE_WEATHER_SMALL = 45;

typedef const unsigned char *weather_icon;
std::map<std::string, weather_icon> weather_icon_map{
    {"clear-night", weather_clear_night},
    {"cloudy", weather_cloudy},
    {"fog", weather_foggy},
    {"hail", weather_thunderstorm},
    {"lightning", weather_thunderstorm},
    {"lightning-rainy", weather_thunderstorm},
    {"partlycloudy", weather_partly_cloudy},
    {"night-partly-cloudy", weather_partly_cloudy_night},
    {"pouring", weather_rainy},
    {"rainy", weather_rainy},
    {"snowy", weather_snowing},
    {"snowy-rainy", weather_snowing},
    {"sunny", weather_sunny},
    {"windy", weather_wind},
    {"windy-variant", weather_wind}};

typedef const unsigned char *weather_icon;
std::map<std::string, weather_icon> weather_icon_map_small{
    {"clear-night", weather_small_clear_night},
    {"cloudy", weather_small_cloudy},
    {"fog", weather_small_foggy},
    {"hail", weather_small_thunderstorm},
    {"lightning", weather_small_thunderstorm},
    {"lightning-rainy", weather_small_thunderstorm},
    {"partlycloudy", weather_small_partly_cloudy},
    {"night-partly-cloudy", weather_small_partly_cloudy_night},
    {"pouring", weather_small_rainy},
    {"rainy", weather_small_rainy},
    {"snowy", weather_small_snowing},
    {"snowy-rainy", weather_small_snowing},
    {"sunny", weather_small_sunny},
    {"windy", weather_small_wind},
    {"windy-variant", weather_small_wind}};

const unsigned char *get_weather_icon(String forecast, bool small = false)
{
  if (small)
  {
    if (weather_icon_map_small.find(forecast.c_str()) != weather_icon_map_small.end())
    {
      return weather_icon_map_small[forecast.c_str()];
    }
    else
    {
      return weather_small_sunny;
    }
  }
  else
  {
    if (weather_icon_map.find(forecast.c_str()) != weather_icon_map.end())
    {
      return weather_icon_map[forecast.c_str()];
    }
    else
    {
      return weather_sunny;
    }
  }
}