#pragma once
//
// Intentionally-empty stub for Seeed_GFX on the XIAO nRF52840.
//
// Seeed_GFX's nRF52840 processor header (Processors/TFT_eSPI_nRF52840.h)
// unconditionally does `#include <Seeed_Arduino_FS.h>` whenever SMOOTH_FONT is
// enabled — and Setup502 (our 7.5" combo) always enables SMOOTH_FONT.
//
// However, the filesystem-backed smooth-font code is gated on FONT_FS_AVAILABLE,
// which is defined ONLY for ESP8266/ESP32/RP2040 — never for nRF52840. On this
// target `fontFile` degrades to a bool and no `fs::` type is ever referenced, so
// the include is vestigial. We render Adafruit-GFX fonts via our own rasteriser
// (see display_seeedgfx.cpp) and never load VLW fonts, so pulling in the real
// Seeed_Arduino_FS / SdFat stack would add flash and SPI baggage for nothing.
//
// This stub satisfies the include with zero functional impact. Remove it only if
// you actually enable filesystem-backed smooth fonts on this board.
