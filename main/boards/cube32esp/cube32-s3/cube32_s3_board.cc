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
#ifdef CONFIG_CUBE32_LVGL_ENABLED
#include "cube32_lvgl_display.h"
#endif
#if defined(CONFIG_CUBE32_LVGL_ENABLED) && defined(CONFIG_CUBE32_FACE_EXPRESSION_ENABLED)
#include "cube32_face_display.h"
#endif

#include "mcp_server.h"
#include <ssid_manager.h>
#include <utils/config_manager.h>
#ifdef CONFIG_CUBE32_MODEM_ENABLED
#include "cube32_modem_network.h"
#include <cJSON.h>
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
// Mode A: CONFIG_CUBE32_LVGL_ENABLED=n  — xiaozhi's SpiLcdDisplay owns LVGL.
// Mode B: CONFIG_CUBE32_LVGL_ENABLED=y  — cube32 BSP owns LVGL;
//         Cube32AttachedLcdDisplay attaches to the existing lv_display_t.

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
#ifdef CONFIG_CUBE32_MODEM_ENABLED
    Cube32ModemNetwork modem_net_;
    NetworkEventCallback network_callback_;
#endif

    void InitializeDisplay() {
        auto& st = cube32::ST7789Display::instance();
        if (!st.isInitialized()) {
            ESP_LOGE(TAG, "ST7789 not initialised — cube32_init() failed earlier");
            return;
        }

        const uint16_t w = st.getBaseWidth();
        const uint16_t h = st.getBaseHeight();

#ifdef CONFIG_CUBE32_LVGL_ENABLED
        // ---- Mode B: cube32 BSP owns LVGL ------------------------------------
        // cube32_init() already called lv_init + lvgl_port_init +
        // lvgl_port_add_disp + lv_display_set_default + setRotation + backlight.
        // We only need to attach xiaozhi's LcdDisplay stack to the existing
        // lv_display_t.
#if defined(CONFIG_CUBE32_FACE_EXPRESSION_ENABLED)
        if (Cube32FaceDisplay::IsFaceModeEnabled()) {
            ESP_LOGI(TAG, "Display: face expression mode");
            display_ = new Cube32FaceDisplay(
                st.getIOHandle(), st.getPanelHandle(), w, h);
        } else {
            ESP_LOGI(TAG, "Display: chat mode");
            display_ = new Cube32AttachedLcdDisplay(
                st.getIOHandle(), st.getPanelHandle(), w, h);
        }
#else
        display_ = new Cube32AttachedLcdDisplay(
            st.getIOHandle(), st.getPanelHandle(), w, h);
#endif
#else
        // ---- Mode A: xiaozhi's SpiLcdDisplay owns LVGL -----------------------
        // cube32 deliberately keeps the panel OFF until the first frame is
        // drawn; flip it on here so the panel is ready before LVGL flushes.
        esp_lcd_panel_disp_on_off(st.getPanelHandle(), true);

        // SpiLcdDisplay uses BASE dimensions; LVGL handles rotation.
        display_ = new SpiLcdDisplay(st.getIOHandle(), st.getPanelHandle(),
                                     w, h,
                                     /*offset_x*/ 0, /*offset_y*/ 0,
                                     /*mirror_x*/ false,
                                     /*mirror_y*/ false,
                                     /*swap_xy*/  false,
                                     /*swap_bytes*/ false);

#if CUBE32_LCD_DEFAULT_ROTATION != 0
        // Apply the default rotation defined in cube32_config.h.
        // esp_lvgl_port handles swap_xy/mirror; the ST7789 gap (80px for
        // 240×240 displays) must be set manually — esp_lvgl_port does not.
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

        // Turn the backlight on now that LVGL is about to flush its first frame.
        backlight_.SetBrightness(100, /*permanent*/ true);
#endif  // CONFIG_CUBE32_LVGL_ENABLED
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

    void InitializeMcpTools() {
        auto& mcp = McpServer::GetInstance();

#ifdef CONFIG_CUBE32_MODEM_ENABLED
        // Switch between LTE modem and WiFi — persists to NVS, reboots.
        mcp.AddUserOnlyTool(
            "self.cube32.set_network_mode",
            "Switch the CUBE32 network between WiFi and LTE modem. "
            "The new mode is saved and takes effect after the device restarts.",
            PropertyList({
                Property("mode", kPropertyTypeString)  // required: "wifi" or "modem"
            }),
            [](const PropertyList& props) -> ReturnValue {
                bool use_modem = (props["mode"].value<std::string>() == "modem");
                Cube32ModemNetwork::SetModemActive(use_modem);
                auto& app = Application::GetInstance();
                app.Schedule([&app, use_modem]() {
                    ESP_LOGW(TAG, "Network mode set to %s — rebooting",
                             use_modem ? "modem" : "wifi");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    app.Reboot();
                });
                return std::string(use_modem
                    ? "Switching to LTE modem, rebooting..."
                    : "Switching to WiFi, rebooting...");
            });
#endif  // CONFIG_CUBE32_MODEM_ENABLED

#if defined(CONFIG_CUBE32_LVGL_ENABLED) && defined(CONFIG_CUBE32_FACE_EXPRESSION_ENABLED)
        // Switch between chat UI and face expression mode — persists to NVS, reboots.
        mcp.AddUserOnlyTool(
            "self.cube32.set_display_mode",
            "Switch the CUBE32 display between normal chat UI and face expression mode. "
            "The selection is saved and takes effect after the device restarts.",
            PropertyList({
                Property("mode", kPropertyTypeString)  // required: "chat" or "face"
            }),
            [](const PropertyList& props) -> ReturnValue {
                bool face = (props["mode"].value<std::string>() == "face");
                Cube32FaceDisplay::SetFaceMode(face);
                auto& app = Application::GetInstance();
                app.Schedule([&app, face]() {
                    ESP_LOGW(TAG, "Display mode set to %s — rebooting",
                             face ? "face" : "chat");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    app.Reboot();
                });
                return std::string(face
                    ? "Switching to face expression mode, rebooting..."
                    : "Switching to chat mode, rebooting...");
            });
#endif  // CONFIG_CUBE32_LVGL_ENABLED && CONFIG_CUBE32_FACE_EXPRESSION_ENABLED

        // Restart the device.
        mcp.AddUserOnlyTool(
            "self.cube32.restart",
            "Restart the CUBE32 system.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto& app = Application::GetInstance();
                app.Schedule([&app]() {
                    ESP_LOGW(TAG, "Restart requested via MCP");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    app.Reboot();
                });
                return true;
            });
    }

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
        InitializeMcpTools();
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

#ifdef CONFIG_CUBE32_MODEM_ENABLED
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        if (Cube32ModemNetwork::ShouldUseModem()) {
            // Defer forwarding — the modem task will deliver events directly.
            network_callback_ = std::move(callback);
            return;
        }
        // Modem built but not active/present: take the WiFi path.
        WifiBoard::SetNetworkEventCallback(std::move(callback));
    }

    void StartNetwork() override {
        if (Cube32ModemNetwork::ShouldUseModem()) {
            if (auto* d = GetDisplay()) {
                d->SetStatus(Lang::Strings::DETECTING_MODULE);
            }
            modem_net_.StartAsync(network_callback_);
            return;
        }
        // Modem built but not active/present: forward stored callback then start WiFi.
        WifiBoard::SetNetworkEventCallback(std::move(network_callback_));
        SyncCube32WiFiCredentials_();
        WifiBoard::StartNetwork();
    }

    const char* GetNetworkStateIcon() override {
        if (modem_net_.is_active()) {
            return Cube32ModemNetwork::GetSignalIcon();
        }
        return WifiBoard::GetNetworkStateIcon();
    }

    std::string GetDeviceStatusJson() override {
        if (modem_net_.is_active()) {
            // Replace the WiFi "network" section with cellular info.
            std::string base = WifiBoard::GetDeviceStatusJson();
            auto* root = cJSON_Parse(base.c_str());
            if (root) {
                cJSON_DeleteItemFromObject(root, "network");
                auto* net = cJSON_Parse(modem_net_.GetNetworkJson().c_str());
                if (net) cJSON_AddItemToObject(root, "network", net);
                auto* raw = cJSON_PrintUnformatted(root);
                std::string result(raw);
                cJSON_free(raw);
                cJSON_Delete(root);
                return result;
            }
        }
        return WifiBoard::GetDeviceStatusJson();
    }
#else
    // WiFi-only build: still sync CUBE32 BSP credentials into SsidManager.
    void StartNetwork() override {
        SyncCube32WiFiCredentials_();
        WifiBoard::StartNetwork();
    }
#endif  // CONFIG_CUBE32_MODEM_ENABLED

private:
    // Copies CUBE32 BSP WiFi credentials into Xiaozhi's SsidManager (slot 0 / highest priority).
    // No-op when the BSP SSID is empty (user hasn't provisioned via Mobile App yet).
    static void SyncCube32WiFiCredentials_() {
        const cube32_cfg_t* cfg = cube32_cfg();
        if (cfg->wifi_ssid[0] != '\0') {
            SsidManager::GetInstance().AddSsid(cfg->wifi_ssid, cfg->wifi_pass);
        }
    }
};

DECLARE_BOARD(Cube32S3Board);
