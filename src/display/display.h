#pragma once
//
// Display facade — one drawing interface, two backends.
//
// The weather-dashboard drawing code (main.cpp) talks only to this `epd::`
// interface. The build selects exactly one backend:
//
//   * GxEPD2   (default)  -> display_gxepd2.cpp   — Waveshare 7.5" HAT, paged
//   * Seeed_GFX           -> display_seeedgfx.cpp — XIAO ePaper Board EN04, sprite
//                            (enabled by -D DISPLAY_BACKEND_SEEEDGFX)
//
// The two libraries have incompatible drawing models (GxEPD2 re-runs the draw
// code once per page; Seeed_GFX draws once into a full framebuffer then pushes).
// epd::render() hides that difference behind a single callback so the dashboard
// layout is written once and renders identically on both — see the backends.
//
// Text is rendered with Adafruit-GFX fonts on both backends; the Seeed_GFX
// backend ports Adafruit's exact glyph/metrics algorithm so output is 1:1.

#if defined(DISPLAY_BACKEND_SEEEDGFX)
#include "TFT_eSPI.h" // provides GFXfont, TFT_BLACK/TFT_WHITE
#else
#include <Adafruit_GFX.h> // provides GFXfont
#endif

#include <Arduino.h>
#include <functional>
#include <cstdarg>
#include <cstdio>

namespace epd
{
    // --- lifecycle ---
    void begin();     // power on + full init + defaults (rotation, black text)
    void wake();      // re-init after hibernate, before a refresh
    void hibernate(); // enter low-power sleep and release pins

    // Render one frame. `draw` issues epd::* drawing calls; it may be invoked
    // once (Seeed_GFX) or several times (GxEPD2 paged), so it must be idempotent.
    void render(const std::function<void()> &draw);

    // --- geometry ---
    int16_t width();
    int16_t height();
    void setRotation(uint8_t r);
    void setPartialWindow(); // GxEPD2: partial-refresh full area; Seeed_GFX: no-op
    void setTextColor();     // monochrome: always black-on-white

    // --- text (Adafruit-GFX fonts) ---
    void setFont(const GFXfont *font);
    void setCursor(int16_t x, int16_t y);
    void getTextBounds(const char *str, int16_t x, int16_t y,
                       int16_t *x1, int16_t *y1, uint16_t *w, uint16_t *h);
    void print(const char *s);
    void print(const String &s);

    // --- graphics ---
    void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h); // outline, black

    // Formatted print, shared by both backends (routes to print()).
    inline void printf(const char *fmt, ...)
    {
        char buf[128];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        print(buf);
    }
} // namespace epd
