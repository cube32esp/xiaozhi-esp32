#pragma once
#include "sdkconfig.h"

// Cube32ModemNetwork — A7670 modem init helper for Cube32S3Board.
// Only compiled when CONFIG_CUBE32_MODEM_ENABLED is set.
#ifdef CONFIG_CUBE32_MODEM_ENABLED

#include "board.h"  // NetworkEventCallback, NetworkEvent
#include <string>

// Encapsulates the A7670 modem init sequence, PPP bring-up, and runtime
// signal / device-status queries.  Cube32S3Board owns one instance and
// delegates all modem-related work to it.
class Cube32ModemNetwork {
public:
    // Three-gate check at startup time:
    //   CONFIG_CUBE32_MODEM_ENABLED (compile-time)
    //   && modem_module_present      (TCA9554 @ I2C 0x22 detected by BSP)
    //   && modem_active              (NVS "cube32_cfg"/"act_modem", default=true)
    // Deterministic for the lifetime of a boot; call once.
    static bool ShouldUseModem();

    // Persist the modem-active preference to NVS ("cube32_cfg"/"act_modem").
    // Takes effect on the next reboot.
    static void SetModemActive(bool active);

    // Spawn the modem-init FreeRTOS task.  event_cb receives NetworkEvent
    // notifications as the sequence progresses (ModemDetecting → Connecting
    // → Connected, or ModemError* on failure).
    void StartAsync(NetworkEventCallback event_cb);

    // Map A7670 signal quality (0–31; 99 = unknown) to a FONT_AWESOME_SIGNAL_*
    // constant, using the same thresholds as Ml307Board.
    static const char* GetSignalIcon();

    // Returns the "network" JSON object string for GetDeviceStatusJson().
    std::string GetNetworkJson() const;

    bool is_active() const { return m_active_; }

private:
    volatile bool m_active_ = false;
    NetworkEventCallback m_event_cb_;

    static void ModemTaskEntry(void* arg);
    void ModemTask();
    void FireEvent(NetworkEvent event, const std::string& data = "");
};

#endif  // CONFIG_CUBE32_MODEM_ENABLED
