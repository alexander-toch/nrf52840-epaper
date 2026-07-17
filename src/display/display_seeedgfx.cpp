// Seeed_GFX backend for the display facade (XIAO ePaper Display Board EN04).
//
// Seeed_GFX's EPaper class is a full-frame 1bpp sprite (TFT_eSprite): you draw
// into the buffer, then update() pushes it and returns the panel to sleep.
//
// The panel/board/pin combo is selected by include/driver.h
// (BOARD_SCREEN_COMBO 502 -> 7.5" UC8179, USE_XIAO_EPAPER_DISPLAY_BOARD_EN04).
//
// ORIENTATION: the dashboard layout is portrait (480 x 800), but Seeed_GFX's
// 1bpp sprite corrupts odd (portrait) rotations — at rotation 1 only half the
// panel is drawn. Even rotations render the whole panel. So we keep the panel at
// its native landscape rotation (0) and map our portrait logical coordinates
// into the native buffer ourselves via putPixel(), a 90° rotation that matches
// the GxEPD2 backend's rotation(1). Every pixel-producing primitive goes through
// putPixel, so main.cpp's portrait layout renders unchanged and 1:1 with GxEPD2.
//
// TEXT: TFT_eSPI's own free-font renderer positions GFX fonts differently from
// Adafruit-GFX, which would shift the hand-tuned layout, so we port Adafruit's
// exact glyph rasteriser and text metrics here.
#include "display.h"

namespace
{
    EPaper epaper; // constructor allocates the 800x480 1bpp framebuffer

    // Panel is driven at its native landscape rotation; portrait is synthesised
    // by putPixel() below (see the ORIENTATION note above).
    constexpr uint8_t kNativePanelRotation = 0;

    constexpr int16_t kNativeW = EPD_WIDTH;  // 800 (panel native = landscape)
    constexpr int16_t kNativeH = EPD_HEIGHT; // 480
    constexpr int16_t kLogicalW = kNativeH;  // 480 (portrait width the app sees)
    constexpr int16_t kLogicalH = kNativeW;  // 800 (portrait height the app sees)

    // Set true if the image comes out upside-down in your physical frame.
    constexpr bool kFlip180 = false;

    // Map portrait logical coords -> native landscape buffer (90° rotation that
    // matches GxEPD2/Adafruit rotation 1). epaper stays at rotation 0, so its
    // drawPixel writes the native buffer directly.
    inline void putPixel(int16_t lx, int16_t ly)
    {
        if (lx < 0 || ly < 0 || lx >= kLogicalW || ly >= kLogicalH)
            return;
        int16_t nx, ny;
        if (kFlip180)
        {
            nx = ly;
            ny = kNativeH - 1 - lx;
        }
        else
        {
            nx = kNativeW - 1 - ly;
            ny = lx;
        }
        epaper.drawPixel(nx, ny, TFT_BLACK);
    }

    const GFXfont *g_font = nullptr;
    int16_t g_cursor_x = 0;
    int16_t g_cursor_y = 0;

    // Rasterise one glyph at (x, y) as an Adafruit-GFX baseline draw, textsize 1.
    void drawGlyph(int16_t x, int16_t y, unsigned char c)
    {
        if (!g_font || c < g_font->first || c > g_font->last)
            return;
        const GFXglyph *glyph = &g_font->glyph[c - g_font->first];
        const uint8_t *bitmap = g_font->bitmap;
        uint32_t bo = glyph->bitmapOffset;
        uint8_t w = glyph->width, h = glyph->height;
        int8_t xo = glyph->xOffset, yo = glyph->yOffset;
        uint8_t bits = 0, bit = 0;
        for (uint8_t yy = 0; yy < h; yy++)
        {
            for (uint8_t xx = 0; xx < w; xx++)
            {
                if (!(bit++ & 7))
                    bits = pgm_read_byte(&bitmap[bo++]);
                if (bits & 0x80)
                    putPixel(x + xo + xx, y + yo + yy);
                bits <<= 1;
            }
        }
    }

    // Advance the cursor and draw one character (Adafruit-GFX write(), textsize 1).
    void writeChar(unsigned char c)
    {
        if (!g_font)
            return;
        if (c == '\n')
        {
            g_cursor_x = 0;
            g_cursor_y += (int16_t)g_font->yAdvance;
            return;
        }
        if (c == '\r' || c < g_font->first || c > g_font->last)
            return;
        const GFXglyph *glyph = &g_font->glyph[c - g_font->first];
        uint8_t w = glyph->width, h = glyph->height;
        if (w > 0 && h > 0)
        {
            int16_t xo = glyph->xOffset;
            if (g_cursor_x + (xo + w) > kLogicalW) // wrap (Adafruit default)
            {
                g_cursor_x = 0;
                g_cursor_y += (int16_t)g_font->yAdvance;
            }
            drawGlyph(g_cursor_x, g_cursor_y, c);
        }
        g_cursor_x += glyph->xAdvance;
    }

    // Accumulate bounding box for one character (Adafruit-GFX charBounds(), size 1).
    void charBounds(unsigned char c, int16_t *x, int16_t *y,
                    int16_t *minx, int16_t *miny, int16_t *maxx, int16_t *maxy)
    {
        if (!g_font)
            return;
        if (c == '\n')
        {
            *x = 0;
            *y += (int16_t)g_font->yAdvance;
            return;
        }
        if (c == '\r' || c < g_font->first || c > g_font->last)
            return;
        const GFXglyph *glyph = &g_font->glyph[c - g_font->first];
        uint8_t gw = glyph->width, gh = glyph->height, xa = glyph->xAdvance;
        int8_t xo = glyph->xOffset, yo = glyph->yOffset;
        if (*x + (xo + gw) > kLogicalW)
        {
            *x = 0;
            *y += (int16_t)g_font->yAdvance;
        }
        int16_t x1 = *x + xo, y1 = *y + yo, x2 = x1 + gw - 1, y2 = y1 + gh - 1;
        if (x1 < *minx)
            *minx = x1;
        if (y1 < *miny)
            *miny = y1;
        if (x2 > *maxx)
            *maxx = x2;
        if (y2 > *maxy)
            *maxy = y2;
        *x += xa;
    }
} // namespace

namespace epd
{
    void begin()
    {
        epaper.begin();
        epaper.setRotation(kNativePanelRotation); // native landscape; putPixel() does portrait
    }

    void wake()
    {
        // EPaper::update() wakes the panel before pushing, so nothing to do here.
    }

    void hibernate() { epaper.sleep(); }

    void render(const std::function<void()> &draw)
    {
        epaper.fillScreen(TFT_WHITE); // uniform fill of the whole native buffer
        draw();
        epaper.update(); // wake -> push -> sleep
    }

    int16_t width() { return kLogicalW; }   // portrait, 480
    int16_t height() { return kLogicalH; }  // portrait, 800

    // Orientation is fixed (portrait via putPixel); the app's requested rotation
    // is ignored so the panel stays at its working native rotation.
    void setRotation(uint8_t) {}
    void setPartialWindow() { /* full-frame update model; nothing to do */ }
    void setTextColor() { /* monochrome raster path is always black */ }

    void setFont(const GFXfont *font) { g_font = font; }
    void setCursor(int16_t x, int16_t y)
    {
        g_cursor_x = x;
        g_cursor_y = y;
    }

    void getTextBounds(const char *str, int16_t x, int16_t y,
                       int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h)
    {
        int16_t minx = 0x7FFF, miny = 0x7FFF, maxx = -1, maxy = -1;
        int16_t cx = x, cy = y;
        *x1 = x;
        *y1 = y;
        *w = 0;
        *h = 0;
        for (const char *p = str; *p; ++p)
            charBounds((unsigned char)*p, &cx, &cy, &minx, &miny, &maxx, &maxy);
        if (maxx >= minx)
        {
            *x1 = minx;
            *w = (uint16_t)(maxx - minx + 1);
        }
        if (maxy >= miny)
        {
            *y1 = miny;
            *h = (uint16_t)(maxy - miny + 1);
        }
    }

    void print(const char *s)
    {
        for (const char *p = s; *p; ++p)
            writeChar((unsigned char)*p);
    }
    void print(const String &s) { print(s.c_str()); }

    void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h)
    {
        // 1bpp bitmap, MSB-first, rows byte-aligned (Adafruit-GFX layout). Plot
        // set bits as black through the portrait transform.
        int16_t byteWidth = (w + 7) / 8;
        for (int16_t by = 0; by < h; by++)
            for (int16_t bx = 0; bx < w; bx++)
                if (pgm_read_byte(&bitmap[by * byteWidth + bx / 8]) & (0x80 >> (bx & 7)))
                    putPixel(x + bx, y + by);
    }

    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h)
    {
        for (int16_t i = 0; i < w; i++)
        {
            putPixel(x + i, y);
            putPixel(x + i, y + h - 1);
        }
        for (int16_t i = 0; i < h; i++)
        {
            putPixel(x, y + i);
            putPixel(x + w - 1, y + i);
        }
    }
} // namespace epd
