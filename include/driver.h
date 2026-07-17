#pragma once
// Seeed_GFX hardware configuration for the XIAO ePaper Display Board EN04.
//
// Seeed_GFX resolves its board/panel combo by #include "driver.h"; this file is
// found via the project `include/` path. It only takes effect in the
// `xiao_epaper_en04` build (the only env that lists Seeed_GFX in lib_deps).
//
// See https://wiki.seeedstudio.com/epaper_EN04/

#define BOARD_SCREEN_COMBO 502 // 7.5 inch monochrome ePaper Screen （UC8179）
#define USE_XIAO_EPAPER_DISPLAY_BOARD_EN04
