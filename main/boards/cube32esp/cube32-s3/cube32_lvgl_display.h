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

#include <functional>
#include <string>

// Link information shown on the second status row; supplied by the board.
struct Cube32StripInfo {
    std::string mode_text;   // e.g. "WiFi", "A7671G USB", "A7671G UART"
    bool link_up = false;    // Wi-Fi associated / modem PPP up
};

// Status layout (width <= 240 uses a 16 px font for row 2, larger screens the 20 px font):
//   row 1 (xiaozhi top bar): [signal][link icon]  status / clock  [mute][battery icon]
//   row 2 (info strip):      mode text                                    battery %
class Cube32AttachedLcdDisplay : public LcdDisplay {
public:
    // panel_io and panel are passed straight to LcdDisplay for bookkeeping;
    // no LVGL port calls are made here — cube32_init() already did that.
    Cube32AttachedLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                             esp_lcd_panel_handle_t panel,
                             int width, int height);

    using StripInfoProvider = std::function<Cube32StripInfo()>;
    void SetStripInfoProvider(StripInfoProvider provider) { strip_info_provider_ = std::move(provider); }

    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool update_all = false) override;

protected:
    lv_obj_t* info_strip_ = nullptr;

    // Height occupied at the top by the status bar plus the info strip.
    // The caller must hold the display lock.
    int GetTopInset();

private:
    lv_obj_t* mode_label_ = nullptr;
    lv_obj_t* link_label_ = nullptr;
    lv_obj_t* battery_pct_label_ = nullptr;
    StripInfoProvider strip_info_provider_;
    std::string last_mode_text_;
    const char* last_link_icon_ = nullptr;
    std::string last_pct_text_;
    bool last_charging_ = false;
    uint32_t last_link_color_ = 0;

    void BuildInfoStrip();
    void UpdateInfoStrip();
};
