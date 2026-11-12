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
#include "assets.h"

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
#include <wifi_manager.h>
#include <utils/config_manager.h>
#ifdef CONFIG_CUBE32_MODEM_ENABLED
#include "cube32_modem_network.h"
#include <cJSON.h>
#include <font_awesome.h>
#endif

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <utility>
#include <algorithm>

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

#ifdef CONFIG_CUBE32_BLE_OTA_ENABLED
// BLE OTA and xiaozhi share the internal RAM, so while a BLE OTA transfer runs (data OTA of the
// memory-mapped "assets" partition, or an app OTA that ends with a restart) the wake word, the audio
// processor and the assets are released. They come back when the update has finished or failed and
// no restart follows. Outside an OTA the services are untouched, also while a BLE client is connected.
//
// Data OTA: BleOta::setDataOtaHandler (BEGIN on the NimBLE host task, END on the BSP worker task).
// App OTA: BSP command callback (START_OTA / OTA_END / OTA_ABORT) and connection callback.
constexpr int kResumeAfterFollowUpMs = 3000;   // Studio's partitions / hash commands follow a data OTA
constexpr int kResumeWaitForRestartMs = 60000;

struct DataOtaBeginState {
    enum : int { kPending, kRunning, kCancelled };
    std::atomic<int> phase{kPending};
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    bool accepted = false;
    ~DataOtaBeginState() { vSemaphoreDelete(done); }
};

// Written on the main task only (g_suspended also read by the data OTA worker)
std::atomic<bool> g_suspended{false};   // assets and models released (data OTA)
std::atomic<bool> g_app_ota_active{false};
bool g_audio_stopped = false;           // conversation ended and wake word stopped (app OTA)
int g_suspend_tries = 0;
esp_timer_handle_t g_suspend_timer = nullptr;
esp_timer_handle_t g_resume_timer = nullptr;

void ArmTimer(esp_timer_handle_t& handle, const char* name, esp_timer_cb_t cb, int ms) {
    if (handle == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = cb;
        args.name = name;
        if (esp_timer_create(&args, &handle) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(handle);
    esp_timer_start_once(handle, (uint64_t)ms * 1000);
}

void LogRam(const char* what) {
    ESP_LOGI(TAG, "%s: internal RAM free %u, largest block %u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

// Main task. Returns true when the services are released.
bool ReleaseServices() {
    if (g_suspended) {
        return true;
    }
    auto& app = Application::GetInstance();
    DeviceState state = app.GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateWifiConfiguring) {
        return false;
    }
    Assets::GetInstance().Release();
    g_suspended = true;
    // The BSP worker tasks need a free internal block; let the idle task free deleted tasks
    vTaskDelay(pdMS_TO_TICKS(300));
    LogRam("BLE OTA: services released");
    return true;
}

void ResumeTick(void*) {
    Application::GetInstance().Schedule([]() {
        if (!g_suspended && !g_audio_stopped) {
            return;
        }
        // A new transfer, or a worker for a follow-up command, is still running
        if (cube32::BleOta::isDataOtaActive() ||
            (g_app_ota_active &&
             cube32::BleOta::instance().getStatus().state == cube32::BleOtaState::OTA_IN_PROGRESS)) {
            ArmTimer(g_resume_timer, "ota_resume", ResumeTick, 1000);
            return;
        }
        auto& app = Application::GetInstance();
        // Maps and applies the assets again, also after a failed transfer, so the device keeps working
        bool reloaded = !g_suspended || Assets::GetInstance().Reload();
        g_suspended = false;
        g_audio_stopped = false;
        g_app_ota_active = false;
        if (app.GetDeviceState() == kDeviceStateIdle) {
            app.GetAudioService().EnableWakeWordDetection(true);
        }
        ESP_LOGI(TAG, "BLE OTA: services resumed, assets %s", reloaded ? "applied" : "not valid");
    });
}

void ScheduleResume(int ms) {
    ArmTimer(g_resume_timer, "ota_resume", ResumeTick, ms);
}

// App OTA: the transfer needs no extra RAM, but the AFE tasks (core 1, higher priority than the NimBLE
// host) can starve the BLE host and lose chunks. End the conversation and stop the wake word without
// blocking the NimBLE host task. Nothing heavy runs here: the chunks start right after the START_OTA reply.
void SuspendTick(void*) {
    Application::GetInstance().Schedule([]() {
        if (g_audio_stopped || !g_app_ota_active) {
            return;
        }
        auto& app = Application::GetInstance();
        DeviceState state = app.GetDeviceState();
        if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
            state == kDeviceStateSpeaking) {
            app.CloseAudioChannel();
            if (++g_suspend_tries < 30) {
                ArmTimer(g_suspend_timer, "ota_suspend", SuspendTick, 500);
                return;
            }
        }
        app.GetAudioService().EnableWakeWordDetection(false);
        app.GetAudioService().EnableVoiceProcessing(false);
        g_audio_stopped = true;
        ESP_LOGI(TAG, "BLE OTA: wake word stopped for the app update");
    });
}

bool OnDataOta(cube32::DataOtaEvent event, const char* label, bool ok) {
    if (strcmp(label, "assets") != 0) {
        return true;
    }
    auto& app = Application::GetInstance();

    if (event == cube32::DataOtaEvent::BEGIN) {
        if (g_resume_timer != nullptr) {
            esp_timer_stop(g_resume_timer);
        }
        // A conversation holds the audio models and the server link: end it. Startup and activation
        // finish on their own. Wait for Idle in both cases.
        DeviceState current = app.GetDeviceState();
        if (current == kDeviceStateConnecting || current == kDeviceStateListening ||
            current == kDeviceStateSpeaking) {
            app.Schedule([]() { Application::GetInstance().CloseAudioChannel(); });
        }
        if (current == kDeviceStateStarting || current == kDeviceStateActivating ||
            current == kDeviceStateConnecting || current == kDeviceStateListening ||
            current == kDeviceStateSpeaking) {
            for (int i = 0; i < 200 && app.GetDeviceState() != kDeviceStateIdle; i++) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        auto state = std::make_shared<DataOtaBeginState>();
        // The audio service and the display are owned by the main task
        app.Schedule([state]() {
            int expected = DataOtaBeginState::kPending;
            if (!state->phase.compare_exchange_strong(expected, DataOtaBeginState::kRunning)) {
                return;
            }
            state->accepted = ReleaseServices();
            if (!state->accepted) {
                ESP_LOGW(TAG, "Assets data OTA refused: device is busy (state %d)",
                         (int)Application::GetInstance().GetDeviceState());
            }
            xSemaphoreGive(state->done);
        });

        if (xSemaphoreTake(state->done, pdMS_TO_TICKS(5000)) != pdTRUE) {
            int expected = DataOtaBeginState::kPending;
            if (state->phase.compare_exchange_strong(expected, DataOtaBeginState::kCancelled)) {
                ESP_LOGW(TAG, "Assets data OTA refused: main task did not respond");
                return false;
            }
            xSemaphoreTake(state->done, portMAX_DELAY);
        }
        return state->accepted;
    }

    if (g_suspended) {
        ESP_LOGI(TAG, "Assets data OTA finished: transfer %s", ok ? "verified" : "failed");
        ScheduleResume(kResumeAfterFollowUpMs);
    }
    return true;
}

void OnBleCommand(uint8_t cmd_id, const uint8_t*, size_t) {
    auto state = cube32::BleOta::instance().getStatus().state;
    switch (cmd_id) {
    case CUBE32_BLE_CMD_START_OTA:
        if (state == cube32::BleOtaState::OTA_IN_PROGRESS && !g_app_ota_active) {
            g_app_ota_active = true;
            g_suspend_tries = 0;
            if (g_resume_timer != nullptr) {
                esp_timer_stop(g_resume_timer);
            }
            ArmTimer(g_suspend_timer, "ota_suspend", SuspendTick, 100);
        }
        break;
    case CUBE32_BLE_CMD_OTA_END:
        if (g_app_ota_active) {
            // A completed app OTA is followed by a restart; resume only if it does not come
            ScheduleResume(state == cube32::BleOtaState::OTA_COMPLETE ? kResumeWaitForRestartMs
                                                                      : kResumeAfterFollowUpMs);
        }
        break;
    case CUBE32_BLE_CMD_OTA_ABORT:
        if (g_app_ota_active) {
            ScheduleResume(kResumeAfterFollowUpMs);
        }
        break;
    default:
        break;
    }
}

void OnBleConnection(cube32::BleConnectionState state) {
    if (state == cube32::BleConnectionState::DISCONNECTED && g_app_ota_active &&
        cube32::BleOta::instance().getStatus().state != cube32::BleOtaState::OTA_COMPLETE) {
        ScheduleResume(kResumeAfterFollowUpMs);   // the transfer was cut; no restart will follow
    }
}
#endif  // CONFIG_CUBE32_BLE_OTA_ENABLED

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

#ifdef CONFIG_CUBE32_LVGL_ENABLED
    Cube32StripInfo GetStripInfo() {
        Cube32StripInfo info;
#ifdef CONFIG_CUBE32_MODEM_ENABLED
        if (Cube32ModemNetwork::ShouldUseModem()) {
            info.mode_text = Cube32ModemNetwork::GetModeText();
            info.link_up = Cube32ModemNetwork::IsLinkUp();
            return info;
        }
#endif
        info.mode_text = "WiFi";
        info.link_up = WifiManager::GetInstance().IsConnected();
        return info;
    }
#endif

    void InitializeDisplay() {
        // The BSP selects the controller from the detected touch/display
        // model. GT911 320x320 panels initialize ST7796S; legacy panels use
        // ST7789. Keep the panel handles separate from the singleton type so
        // the rest of the adapter can serve either controller.
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        uint16_t w = 0;
        uint16_t h = 0;

        auto& st7796 = cube32::ST7796Display::instance();
        if (st7796.isInitialized()) {
            panel_io = st7796.getIOHandle();
            panel = st7796.getPanelHandle();
            w = st7796.getBaseWidth();
            h = st7796.getBaseHeight();
        } else {
            auto& st7789 = cube32::ST7789Display::instance();
            if (!st7789.isInitialized()) {
                ESP_LOGE(TAG, "No CUBE32 display was initialised — cube32_init() failed earlier");
                return;
            }
            panel_io = st7789.getIOHandle();
            panel = st7789.getPanelHandle();
            w = st7789.getBaseWidth();
            h = st7789.getBaseHeight();
        }

#ifdef CONFIG_CUBE32_LVGL_ENABLED
        // ---- Mode B: cube32 BSP owns LVGL ------------------------------------
        // cube32_init() already called lv_init + lvgl_port_init +
        // lvgl_port_add_disp + lv_display_set_default + setRotation + backlight.
        // We only need to attach xiaozhi's LcdDisplay stack to the existing
        // lv_display_t.
#if defined(CONFIG_CUBE32_FACE_EXPRESSION_ENABLED)
        if (Cube32FaceDisplay::IsFaceModeEnabled()) {
            ESP_LOGI(TAG, "Display: face expression mode");
            display_ = new Cube32FaceDisplay(panel_io, panel, w, h);
        } else {
            ESP_LOGI(TAG, "Display: chat mode");
            display_ = new Cube32AttachedLcdDisplay(panel_io, panel, w, h);
        }
#else
        display_ = new Cube32AttachedLcdDisplay(panel_io, panel, w, h);
#endif
        // Feed the second status row (mode text and link state).
        static_cast<Cube32AttachedLcdDisplay*>(display_)->SetStripInfoProvider(
            [this]() { return GetStripInfo(); });
#else
        // ---- Mode A: xiaozhi's SpiLcdDisplay owns LVGL -----------------------
        // cube32 deliberately keeps the panel OFF until the first frame is
        // drawn; flip it on here so the panel is ready before LVGL flushes.
        esp_lcd_panel_disp_on_off(panel, true);

        // SpiLcdDisplay uses BASE dimensions; LVGL handles rotation.
        display_ = new SpiLcdDisplay(panel_io, panel, w, h,
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
            esp_lcd_panel_set_gap(panel, kXGap, kYGap);
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
#ifdef CONFIG_CUBE32_BLE_OTA_ENABLED
        cube32::BleOta::instance().setDataOtaHandler(OnDataOta);
        cube32::BleOta::instance().setCommandCallback(OnBleCommand);
        cube32::BleOta::instance().setConnectionCallback(OnBleConnection);
#endif

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
        if (!pmu.isInitialized() || !pmu.isBatteryPresent()) {
            return false;
        }
        const int percent = pmu.getBatteryPercent();
        if (percent < 0) {
            return false;
        }
        // Clamp: the shared status bar indexes its icon table with level / 20.
        level       = std::min(percent, 100);
        charging    = pmu.isCharging();
        // Not simply !charging: a full battery on USB power is "standby", and must
        // not trigger the shared low-battery popup logic.
        discharging = pmu.isDischarging();
        return true;
    }
#endif

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
#ifdef CONFIG_CUBE32_MODEM_ENABLED
        if (Cube32ModemNetwork::ShouldUseModem()) {
            // Defer forwarding — the modem task will deliver events directly.
            network_callback_ = std::move(callback);
            return;
        }
#endif
        // Modem built but not active/present: take the WiFi path.
        WifiBoard::SetNetworkEventCallback(
            [this, callback = std::move(callback)](NetworkEvent event, const std::string& data) {
                // The base Wi-Fi station starts in modem power-save mode. On
                // this board, the first DNS lookup during activation can then
                // time out despite DHCP having completed. Disable Wi-Fi power
                // save before the application starts its activation task.
                if (event == NetworkEvent::Connected) {
                    ESP_LOGI(TAG, "WiFi connected; disabling modem power save for activation");
                    WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                }
                if (callback) {
                    callback(event, data);
                }
            });
    }

#ifdef CONFIG_CUBE32_MODEM_ENABLED
    void StartNetwork() override {
        if (Cube32ModemNetwork::ShouldUseModem()) {
            if (auto* d = GetDisplay()) {
                d->SetStatus(Lang::Strings::DETECTING_MODULE);
            }
            modem_net_.StartAsync(network_callback_);
            return;
        }
        // Modem built but not present/active: fall back to WiFi.
        // Only forward network_callback_ if it was deferred by SetNetworkEventCallback
        // (i.e. ShouldUseModem() was true at that call).  When ShouldUseModem() was
        // already false at SetNetworkEventCallback time, the callback was forwarded
        // directly to WifiBoard then — forwarding an empty network_callback_ here would
        // silently overwrite it and drop all WiFi events, hanging Application::Run().
        if (network_callback_) {
            SetNetworkEventCallback(std::move(network_callback_));
        }
        SyncCube32WiFiCredentials_();
        WifiBoard::StartNetwork();
    }

    const char* GetNetworkStateIcon() override {
        if (modem_net_.is_active()) {
            // The signal quality alone stays valid after the PPP link drops.
            if (!Cube32ModemNetwork::IsLinkUp()) {
                return FONT_AWESOME_SIGNAL_OFF;
            }
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
