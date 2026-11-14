#pragma once
#include "lvgl.h"
inline void bsp_display_rotate(lv_display_t *d, lv_display_rotation_t r) { lv_display_set_rotation(d, r); }
inline bool bsp_display_lock(unsigned) { return true; }
inline void bsp_display_unlock() {}
