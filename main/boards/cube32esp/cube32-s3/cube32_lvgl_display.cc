// Cube32AttachedLcdDisplay — Mode B LVGL adapter for CUBE32-S3.
//
// cube32::LvglDisplay owns the LVGL port (lv_init + lvgl_port_init +
// lvgl_port_add_disp + lv_display_set_default). This constructor just wires
// xiaozhi's LcdDisplay to the existing lv_display_t handle so that
// SetupUI(), SetEmotion(), SetChatMessage(), etc. work unchanged.
#include "sdkconfig.h"
#ifdef CONFIG_CUBE32_LVGL_ENABLED

#include "cube32_lvgl_display.h"

#include "cube32.h"  // cube32::LvglDisplay

#include <esp_log.h>

#define TAG "Cube32AttachedLcdDisplay"

Cube32AttachedLcdDisplay::Cube32AttachedLcdDisplay(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_handle_t    panel,
    int width, int height)
    : LcdDisplay(panel_io, panel, width, height)
{
    auto& bsp_lvgl = cube32::LvglDisplay::instance();
    if (!bsp_lvgl.isInitialized()) {
        ESP_LOGE(TAG, "cube32::LvglDisplay not initialised — cube32_init() must be called first");
        return;
    }

    // Attach to the BSP-owned display so all LVGL draw calls land on the
    // correct lv_display_t.  The BSP already called lv_display_set_default()
    // on this handle inside addDisplay(), so lv_screen_active() is correct.
    display_ = bsp_lvgl.getDisplay();

    ESP_LOGI(TAG, "Attached to cube32 LVGL display (Mode B)");
}

#endif // CONFIG_CUBE32_LVGL_ENABLED
