// CUBE32 Mode B display adapter.
//
// When CONFIG_CUBE32_LVGL_ENABLED=y the cube32 BSP owns LVGL (lv_init,
// lvgl_port_init, lvgl_port_add_disp are all called inside cube32_init()).
// This thin subclass attaches xiaozhi's LcdDisplay stack to the
// already-initialised lv_display_t provided by cube32::LvglDisplay, without
// re-initialising any part of the LVGL port.
//
// Lock/Unlock are inherited from LcdDisplay and delegate to
// lvgl_port_lock/unlock — the same esp_lvgl_port mutex that the cube32 BSP
// task holds.
#pragma once

#include "display/lcd_display.h"

class Cube32AttachedLcdDisplay : public LcdDisplay {
public:
    // panel_io and panel are passed straight to LcdDisplay for bookkeeping;
    // no LVGL port calls are made here — cube32_init() already did that.
    Cube32AttachedLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_handle_t panel,
                             int width, int height);
};
