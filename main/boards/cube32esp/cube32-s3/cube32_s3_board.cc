// CUBE32-S3 board
//
// Wraps the cube32 BSP (drivers/audio, drivers/display/st7789, drivers/lvgl,
// drivers/camera, drivers/button, drivers/ble) with the thin xiaozhi adapters
// required by `Board`. Initialisation is delegated entirely to a single
// `cube32_init()` call; xiaozhi adapters never re-initialise hardware.
//
// LVGL Mode A: cube32 owns only the `esp_lcd` panel; xiaozhi's stock
// `SpiLcdDisplay` owns LVGL.
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "assets/lang_config.h"

#include "cube32.h"
#include "cube32_audio_codec.h"
#ifdef CONFIG_CUBE32_CAMERA_ENABLED
#include "cube32_camera.h"
#endif

#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>

#define TAG "cube32-s3"

#if !defined(CONFIG_CUBE32_DISPLAY_ENABLED)
#error "CUBE32-S3 board requires CONFIG_CUBE32_DISPLAY_ENABLED=y in cube32_bsp"
#endif
#if !defined(CONFIG_CUBE32_AUDIO_ENABLED)
#error "CUBE32-S3 board requires CONFIG_CUBE32_AUDIO_ENABLED=y in cube32_bsp"
#endif
#if defined(CONFIG_CUBE32_LVGL_ENABLED)
#error "CUBE32-S3 (Phase 1) requires CONFIG_CUBE32_LVGL_ENABLED=n; LVGL must be owned by xiaozhi"
#endif

namespace {

// PMU-controlled on/off backlight (cube32 routes the LCD backlight through
// AXP2101 ALDO3 — there is no PWM pin available).
class Cube32Backlight : public Backlight {
public:
    Cube32Backlight() = default;
    ~Cube32Backlight() = default;

protected:
    void SetBrightnessImpl(uint8_t brightness) override {
#ifdef CONFIG_CUBE32_PMU_ENABLED
        auto& pmu = cube32::PMU::instance();
        if (pmu.isInitialized()) {
            pmu.setDisplayBacklight(brightness > 0);
        }
#else
        (void)brightness;
#endif
    }
};

}  // namespace

class Cube32S3Board : public WifiBoard {
private:
    Button boot_button_;
    LcdDisplay* display_ = nullptr;
    Cube32Backlight backlight_;
#ifdef CONFIG_CUBE32_CAMERA_ENABLED
    Cube32Camera* camera_ = nullptr;
#endif
#ifdef CONFIG_CUBE32_ADC_BUTTON_ENABLED
    // Wrap each cube32 ADC button handle as a xiaozhi `Button` so we get the
    // same OnClick / OnLongPress semantics as the rest of the codebase.
    Button* adc_buttons_[CUBE32_ADC_BUTTON_MAX_COUNT] = {};
#endif

    void InitializeDisplay() {
        auto& st = cube32::ST7789Display::instance();
        if (!st.isInitialized()) {
            ESP_LOGE(TAG, "ST7789 not initialised — cube32_init() failed earlier");
            return;
        }

        // cube32 deliberately keeps the panel OFF until the first frame is
        // drawn; in Mode A xiaozhi's LVGL pipeline will draw immediately, but
        // we still flip it on here so the panel is ready before LVGL flushes.
        esp_lcd_panel_disp_on_off(st.getPanelHandle(), true);

        // SpiLcdDisplay always uses the BASE dimensions and lets LVGL do the
        // rotation — same convention as cube32::LvglDisplay.
        const uint16_t w = st.getBaseWidth();
        const uint16_t h = st.getBaseHeight();
        display_ = new SpiLcdDisplay(st.getIOHandle(), st.getPanelHandle(),
                                     w, h,
                                     /*offset_x*/ 0, /*offset_y*/ 0,
                                     /*mirror_x*/ false,
                                     /*mirror_y*/ false,
                                     /*swap_xy*/  false,
                                     /*swap_bytes*/ false);

#if CUBE32_LCD_DEFAULT_ROTATION != 0
        // Apply the default rotation defined in cube32_config.h.
        // esp_lvgl_port will call esp_lcd_panel_swap_xy/mirror when
        // lv_display_set_rotation() fires its callback. The ST7789 gap
        // (offset between the 320-wide internal buffer and a 240-wide
        // panel) must be applied manually — esp_lvgl_port does not do it.
        {
            const int kGapOffset =
                (CUBE32_LCD_H_RES == 240 && CUBE32_LCD_V_RES == 240) ? 80 : 0;
#if CUBE32_LCD_DEFAULT_ROTATION == 90
            const lv_display_rotation_t kLvRot = LV_DISPLAY_ROTATION_90;
            const int kXGap = kGapOffset, kYGap = 0;
#elif CUBE32_LCD_DEFAULT_ROTATION == 180
            const lv_display_rotation_t kLvRot = LV_DISPLAY_ROTATION_180;
            const int kXGap = 0, kYGap = kGapOffset;
#elif CUBE32_LCD_DEFAULT_ROTATION == 270
            const lv_display_rotation_t kLvRot = LV_DISPLAY_ROTATION_270;
            const int kXGap = 0, kYGap = 0;
#else
#error "Unsupported CUBE32_LCD_DEFAULT_ROTATION value (expected 90, 180, or 270)"
#endif
            lvgl_port_lock(0);
            lv_display_set_rotation(lv_display_get_default(), kLvRot);
            esp_lcd_panel_set_gap(st.getPanelHandle(), kXGap, kYGap);
            lvgl_port_unlock();
        }
#endif

        // Turn the backlight on now that there is content (or will be in a
        // few ms when LVGL completes its first flush).
        backlight_.SetBrightness(100, /*permanent*/ true);
    }

    void ChangeVolume(int delta) {
        auto codec = GetAudioCodec();
        int volume = codec->output_volume() + delta;
        if (volume < 0)   volume = 0;
        if (volume > 100) volume = 100;
        codec->SetOutputVolume(volume);
        if (auto* d = GetDisplay()) {
            d->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        }
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !IsInWifiConfigMode()) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#ifdef CONFIG_CUBE32_ADC_BUTTON_ENABLED
        auto& adc = cube32::ADCButton::instance();
        if (!adc.isInitialized()) {
            ESP_LOGW(TAG, "cube32 ADC button driver not initialised — skipping");
            return;
        }
        for (size_t i = 0; i < CUBE32_ADC_BUTTON_MAX_COUNT; ++i) {
            auto* h = adc.getButtonHandle(static_cast<cube32::ADCButtonIndex>(i));
            if (h != nullptr) {
                adc_buttons_[i] = new Button(h);
            }
        }

        // Mapping (matches the cube32 ADC ladder ordering):
        //   0 → Volume +    1 → Volume −    2 → Mute
        //   3 → Toggle chat 4 → Stop / abort speak
        //   5 → reserved (Wi-Fi config via long-press on chat button instead)
        if (adc_buttons_[0]) adc_buttons_[0]->OnClick([this]() { ChangeVolume(+10); });
        if (adc_buttons_[1]) adc_buttons_[1]->OnClick([this]() { ChangeVolume(-10); });
        if (adc_buttons_[2]) adc_buttons_[2]->OnClick([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            if (auto* d = GetDisplay()) d->ShowNotification(Lang::Strings::MUTED);
        });
        if (adc_buttons_[3]) {
            adc_buttons_[3]->OnClick([]() { Application::GetInstance().ToggleChatState(); });
            adc_buttons_[3]->OnLongPress([this]() { EnterWifiConfigMode(); });
        }
        if (adc_buttons_[4]) adc_buttons_[4]->OnClick([]() {
            // "Stop" — abort current speech if any.
            Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        });
#endif  // CONFIG_CUBE32_ADC_BUTTON_ENABLED
    }

#ifdef CONFIG_CUBE32_CAMERA_ENABLED
    void InitializeCamera() {
        if (!cube32::Camera::instance().isInitialized()) {
            ESP_LOGW(TAG, "cube32 camera driver not initialised — skipping");
            return;
        }
        camera_ = new Cube32Camera();
    }
#endif

public:
    Cube32S3Board() : boot_button_(BOOT_BUTTON_GPIO) {
        ESP_LOGI(TAG, "Initializing CUBE32-S3 board");

        // One-shot BSP initialisation. Every subsystem comes up here according
        // to the cube32 Kconfig matrix.
        ESP_ERROR_CHECK(cube32_init());

        InitializeDisplay();
#ifdef CONFIG_CUBE32_CAMERA_ENABLED
        InitializeCamera();
#endif
        InitializeButtons();
    }

    std::string GetBoardType() override { return "cube32-s3"; }

    AudioCodec* GetAudioCodec() override {
        static Cube32AudioCodec codec;
        return &codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override { return &backlight_; }

#ifdef CONFIG_CUBE32_CAMERA_ENABLED
    Camera* GetCamera() override { return camera_; }
#endif

#ifdef CONFIG_CUBE32_PMU_ENABLED
    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        auto& pmu = cube32::PMU::instance();
        if (!pmu.isInitialized()) {
            return false;
        }
        level       = pmu.getBatteryPercent();
        charging    = pmu.isCharging();
        discharging = !charging;
        return true;
    }
#endif
};

DECLARE_BOARD(Cube32S3Board);
