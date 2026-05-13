#include "sdkconfig.h"
#ifdef CONFIG_CUBE32_MODEM_ENABLED

#include "cube32_modem_network.h"

#include <drivers/modem/a7670_modem.h>
#include <utils/hw_manifest.h>
#include <utils/config_manager.h>
#include <font_awesome.h>
#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Cube32ModemNetwork"

// Maximum polling durations (each tick = 1 second)
static constexpr int MODEM_INIT_TIMEOUT_S    = 60;
static constexpr int MODEM_NETWORK_TIMEOUT_S = 60;
static constexpr int MODEM_PPP_TIMEOUT_S     = 30;

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

bool Cube32ModemNetwork::ShouldUseModem() {
    const cube32_hw_manifest_t* hw = cube32_hw_manifest();
    return hw->modem_built && hw->modem_module_present && hw->modem_active;
}

void Cube32ModemNetwork::SetModemActive(bool active) {
    cube32_cfg_set_active_modem(active);
}

// ---------------------------------------------------------------------------
// Async entry point
// ---------------------------------------------------------------------------

void Cube32ModemNetwork::StartAsync(NetworkEventCallback event_cb) {
    m_event_cb_ = std::move(event_cb);
    xTaskCreate(ModemTaskEntry, "modem_net", 4096, this, 5, nullptr);
}

void Cube32ModemNetwork::ModemTaskEntry(void* arg) {
    static_cast<Cube32ModemNetwork*>(arg)->ModemTask();
    vTaskDelete(nullptr);
}

void Cube32ModemNetwork::FireEvent(NetworkEvent event, const std::string& data) {
    if (m_event_cb_) {
        m_event_cb_(event, data);
    }
}

// ---------------------------------------------------------------------------
// Modem init sequence (mirrors hello_rtc_ntp / hello_mqtt pattern)
// ---------------------------------------------------------------------------

void Cube32ModemNetwork::ModemTask() {
    auto& modem = cube32::A7670Modem::instance();

    // ---- Step 1: wait for BSP beginAsync() to complete --------------------
    FireEvent(NetworkEvent::ModemDetecting);
    ESP_LOGI(TAG, "Waiting for modem initialisation...");
    for (int i = 0; i < MODEM_INIT_TIMEOUT_S; ++i) {
        if (modem.isInitialized()) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i == MODEM_INIT_TIMEOUT_S - 1) {
            ESP_LOGE(TAG, "Modem init timeout after %ds", MODEM_INIT_TIMEOUT_S);
            FireEvent(NetworkEvent::ModemErrorInitFailed);
            return;
        }
    }
    ESP_LOGI(TAG, "Modem initialised");

    // ---- Step 2: wait for network registration ----------------------------
    FireEvent(NetworkEvent::Connecting);  // empty data → shows REGISTERING_NETWORK
    ESP_LOGI(TAG, "Waiting for network registration...");
    for (int i = 0; i < MODEM_NETWORK_TIMEOUT_S; ++i) {
        if (modem.isNetworkReady()) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i == MODEM_NETWORK_TIMEOUT_S - 1) {
            ESP_LOGE(TAG, "Network registration timeout after %ds", MODEM_NETWORK_TIMEOUT_S);
            FireEvent(NetworkEvent::ModemErrorTimeout);
            return;
        }
    }
    ESP_LOGI(TAG, "Network registered");

    // ---- Step 3: enter PPP data mode and wait for IP ----------------------
    modem.setDataMode();
    ESP_LOGI(TAG, "Waiting for PPP link...");
    for (int i = 0; i < MODEM_PPP_TIMEOUT_S; ++i) {
        if (modem.isPPPConnected()) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i == MODEM_PPP_TIMEOUT_S - 1) {
            ESP_LOGE(TAG, "PPP timeout after %ds", MODEM_PPP_TIMEOUT_S);
            FireEvent(NetworkEvent::ModemErrorInitFailed);
            return;
        }
    }

    // Retrieve operator name for the "Connected to <carrier>" notification
    cube32::ModemInfo info = {};
    modem.getModemInfo(info);
    ESP_LOGI(TAG, "PPP connected — carrier: %s  IMEI: %s",
             info.operatorName.c_str(), info.imei.c_str());

    m_active_ = true;
    FireEvent(NetworkEvent::Connected, info.operatorName);
}

// ---------------------------------------------------------------------------
// Runtime queries
// ---------------------------------------------------------------------------

const char* Cube32ModemNetwork::GetSignalIcon() {
    int sq = cube32::A7670Modem::instance().getSignalQuality();
    if (sq < 0 || sq == 99) return FONT_AWESOME_SIGNAL_OFF;
    if (sq <= 9)             return FONT_AWESOME_SIGNAL_WEAK;
    if (sq <= 14)            return FONT_AWESOME_SIGNAL_FAIR;
    if (sq <= 19)            return FONT_AWESOME_SIGNAL_GOOD;
    return                          FONT_AWESOME_SIGNAL_STRONG;
}

std::string Cube32ModemNetwork::GetNetworkJson() const {
    auto& modem = cube32::A7670Modem::instance();
    cube32::ModemInfo info = {};
    modem.getModemInfo(info);

    int sq = modem.getSignalQuality();
    const char* signal_str;
    if (sq < 0 || sq == 99) signal_str = "unknown";
    else if (sq <= 9)        signal_str = "weak";
    else if (sq <= 14)       signal_str = "fair";
    else if (sq <= 19)       signal_str = "good";
    else                     signal_str = "strong";

    auto* net = cJSON_CreateObject();
    cJSON_AddStringToObject(net, "type",    "cellular");
    cJSON_AddStringToObject(net, "carrier", info.operatorName.c_str());
    cJSON_AddStringToObject(net, "signal",  signal_str);
    cJSON_AddNumberToObject(net, "csq",     sq);
    cJSON_AddStringToObject(net, "imei",    info.imei.c_str());
    cJSON_AddStringToObject(net, "iccid",   info.iccid.c_str());

    auto* raw = cJSON_PrintUnformatted(net);
    std::string result(raw);
    cJSON_free(raw);
    cJSON_Delete(net);
    return result;
}

#endif  // CONFIG_CUBE32_MODEM_ENABLED
