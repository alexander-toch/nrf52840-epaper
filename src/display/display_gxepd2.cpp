// GxEPD2 backend for the display facade (legacy Waveshare 7.5" HAT path).
//
// Delegates every epd:: call to the global GxEPD2 `display` object, preserving
// the original rendering behaviour of the XIAO nRF52840 + Waveshare HAT setup.
#include "display.h"

#include <SPI.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>
#include <GxEPD2_7C.h>
// Defines the global `display` object for the active board (see NRF52840 branch).
#include "paper-config/GxEPD2_display_selection_new_style.h"

namespace epd
{
    void begin()
    {
        // Pull PWR (D0) high — power for the ePaper Driver HAT.
        pinMode(D0, OUTPUT);
        digitalWrite(D0, HIGH);
        display.init(115200, true, 2, true);
        display.setRotation(1);
        display.setTextColor(GxEPD_BLACK);
    }

    void wake()
    {
        digitalWrite(D0, HIGH);
        display.init(115200, false, 2, true);
    }

    void hibernate()
    {
        // These steps are what gets the board below ~30 uA in sleep.
        display.hibernate();
        pinMode(D4, INPUT); // RST
        SPI.end();
        digitalWrite(D0, LOW);
    }

    void render(const std::function<void()> &draw)
    {
        display.firstPage();
        do
        {
            draw();
        } while (display.nextPage());
    }

    int16_t width() { return display.width(); }
    int16_t height() { return display.height(); }
    void setRotation(uint8_t r) { display.setRotation(r); }
    void setPartialWindow() { display.setPartialWindow(0, 0, display.width(), display.height()); }
    void setTextColor() { display.setTextColor(GxEPD_BLACK); }

    void setFont(const GFXfont *font) { display.setFont(font); }
    void setCursor(int16_t x, int16_t y) { display.setCursor(x, y); }

    void getTextBounds(const char *str, int16_t x, int16_t y,
                       int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h)
    {
        display.getTextBounds(str, x, y, x1, y1, w, h);
    }

    void print(const char *s) { display.print(s); }
    void print(const String &s) { display.print(s); }

    void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint8_t scale)
    {
        if (scale <= 1)
        {
            display.drawBitmap(x, y, bitmap, w, h, GxEPD_BLACK);
            return;
        }
        // No higher-resolution asset for icons that need to render larger, so
        // replicate each source pixel scale×scale times (nearest-neighbor).
        int16_t byteWidth = (w + 7) / 8;
        for (int16_t by = 0; by < h; by++)
            for (int16_t bx = 0; bx < w; bx++)
                if (pgm_read_byte(&bitmap[by * byteWidth + bx / 8]) & (0x80 >> (bx & 7)))
                    display.fillRect(x + bx * scale, y + by * scale, scale, scale, GxEPD_BLACK);
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h)
    {
        display.drawRect(x, y, w, h, GxEPD_BLACK);
    }
} // namespace epd
