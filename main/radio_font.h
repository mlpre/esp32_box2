#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Project-local 16 px Unicode bitmap font; no external font component. */
extern const lv_font_t radio_font_16;

/** 24 px presentation variant, scaled from the same project-local bitmap. */
extern const lv_font_t radio_font_24;

#ifdef __cplusplus
}
#endif
