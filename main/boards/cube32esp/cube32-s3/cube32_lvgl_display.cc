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

#include "application.h"
#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"

#include <esp_log.h>
#include <algorithm>

#define TAG "Cube32AttachedLcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(font_puhui_basic_16_4);

namespace {
constexpr uint32_t kColorOk    = 0x2EA043;
constexpr uint32_t kColorWarn  = 0xD29922;
constexpr uint32_t kColorError = 0xE5484D;
}  // namespace

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

void Cube32AttachedLcdDisplay::SetupUI() {
    const bool first_call = !IsSetupUICalled();
    LcdDisplay::SetupUI();
    if (first_call) {
        BuildInfoStrip();
    }
}

void Cube32AttachedLcdDisplay::BuildInfoStrip() {
    DisplayLockGuard lock(this);
    if (top_bar_ == nullptr || info_strip_ != nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* font = (width_ <= 240) ? &font_puhui_basic_16_4 : &BUILTIN_TEXT_FONT;

    lv_obj_update_layout(top_bar_);
    const int top_h = lv_obj_get_height(top_bar_);

    info_strip_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(info_strip_, LV_HOR_RES,
                    font->line_height + theme->spacing(1) * 2);
    lv_obj_set_style_radius(info_strip_, 0, 0);
    lv_obj_set_style_bg_opa(info_strip_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(info_strip_, theme->background_color(), 0);
    lv_obj_set_style_border_width(info_strip_, 0, 0);
    lv_obj_set_style_pad_all(info_strip_, 0, 0);
    lv_obj_set_style_pad_left(info_strip_, theme->spacing(2), 0);
    lv_obj_set_style_pad_right(info_strip_, theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(info_strip_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(info_strip_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(info_strip_, LV_ALIGN_TOP_MID, 0, top_h);

    auto make_label = [&](lv_obj_t*& label) {
        label = lv_label_create(info_strip_);
        lv_label_set_text(label, "");
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, theme->text_color(), 0);
    };
    make_label(mode_label_);
    make_label(link_label_);
    make_label(battery_pct_label_);
    LayoutInfoStrip();
}

// Mode text sits on the left, battery % on the right, and the link label is
// centred on the screen, moved aside only as far as needed to avoid the others.
void Cube32AttachedLcdDisplay::LayoutInfoStrip() {
    constexpr int kGap = 4;
    lv_obj_update_layout(info_strip_);
    const int area_w = lv_obj_get_content_width(info_strip_);
    const int left_w = lv_obj_get_width(mode_label_);
    const int right_w = lv_obj_get_width(battery_pct_label_);
    const int link_w = lv_obj_get_width(link_label_);

    int x = (area_w - link_w) / 2;
    const int min_x = left_w + kGap;
    const int max_x = area_w - right_w - link_w - kGap;
    if (min_x > max_x) {
        x = (min_x + max_x) / 2;
    } else {
        x = std::clamp(x, min_x, max_x);
    }

    lv_obj_align(mode_label_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(battery_pct_label_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_align(link_label_, LV_ALIGN_LEFT_MID, x, 0);
}

int Cube32AttachedLcdDisplay::GetTopInset() {
    int inset = 0;
    if (top_bar_ != nullptr) {
        lv_obj_update_layout(top_bar_);
        inset += lv_obj_get_height(top_bar_);
    }
    if (info_strip_ != nullptr) {
        lv_obj_update_layout(info_strip_);
        inset += lv_obj_get_height(info_strip_);
    }
    return inset;
}

void Cube32AttachedLcdDisplay::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    if (info_strip_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    auto* lvgl_theme = static_cast<LvglTheme*>(theme);
    lv_obj_set_style_bg_color(info_strip_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(mode_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_pct_label_,
        last_charging_ ? lv_color_hex(kColorOk) : lvgl_theme->text_color(), 0);
}

void Cube32AttachedLcdDisplay::UpdateStatusBar(bool update_all) {
    LcdDisplay::UpdateStatusBar(update_all);
    UpdateInfoStrip();
}

void Cube32AttachedLcdDisplay::UpdateInfoStrip() {
    if (info_strip_ == nullptr) {
        return;
    }

    Cube32StripInfo info;
    if (strip_info_provider_) {
        info = strip_info_provider_();
    }

    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    const char* link_text;
    uint32_t link_color;
    if (!info.link_up) {
        if (state == kDeviceStateWifiConfiguring) {
            link_text = "WiFi setup";
            link_color = kColorWarn;
        } else if (state == kDeviceStateStarting) {
            link_text = "Linking";
            link_color = kColorWarn;
        } else {
            link_text = "No link";
            link_color = kColorError;
        }
    } else if (app.IsServerLinkLost()) {
        link_text = "MQTT lost";
        link_color = kColorError;
    } else {
        link_text = "Online";
        link_color = kColorOk;
    }

    char pct[8] = "";
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        snprintf(pct, sizeof(pct), "%d%%", level);
    }

    if (info.mode_text == last_mode_text_ && last_link_text_ == link_text &&
        last_link_color_ == link_color && last_pct_text_ == pct && last_charging_ == charging) {
        return;
    }

    DisplayLockGuard lock(this);
    if (info.mode_text != last_mode_text_) {
        last_mode_text_ = info.mode_text;
        lv_label_set_text(mode_label_, last_mode_text_.c_str());
    }
    if (last_link_text_ != link_text || last_link_color_ != link_color) {
        last_link_text_ = link_text;
        last_link_color_ = link_color;
        lv_label_set_text(link_label_, link_text);
        lv_obj_set_style_text_color(link_label_, lv_color_hex(link_color), 0);
    }
    if (last_pct_text_ != pct || last_charging_ != charging) {
        last_pct_text_ = pct;
        last_charging_ = charging;
        lv_label_set_text(battery_pct_label_, pct);
        // Green while charging, theme text colour otherwise.
        lv_obj_set_style_text_color(battery_pct_label_,
            charging ? lv_color_hex(kColorOk)
                     : static_cast<LvglTheme*>(current_theme_)->text_color(), 0);
    }
    LayoutInfoStrip();
}

#endif // CONFIG_CUBE32_LVGL_ENABLED
