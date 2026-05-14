// CUBE32 Face Expression display adapter.
//
// Gated by CONFIG_CUBE32_FACE_EXPRESSION_ENABLED (requires
// CONFIG_CUBE32_LVGL_ENABLED).  Replaces Xiaozhi's central emoji area with
// cube32::FaceDisplay (procedural animated eyes).  The top status bar, bottom
// chat-message bar and all notifications are retained unchanged.
//
// When face mode is not selected at boot (NVS key "face_mode" in "display"
// namespace is 0), the board falls back to the regular Cube32AttachedLcdDisplay.
// The mode is toggled via the "self.cube32.set_display_mode" MCP tool, which
// persists the choice and reboots.
#pragma once

#include "cube32_lvgl_display.h"

#if defined(CONFIG_CUBE32_LVGL_ENABLED) && defined(CONFIG_CUBE32_FACE_EXPRESSION_ENABLED)

class Cube32FaceDisplay : public Cube32AttachedLcdDisplay {
public:
    Cube32FaceDisplay(esp_lcd_panel_io_handle_t panel_io,
                      esp_lcd_panel_handle_t    panel,
                      int width, int height);
    ~Cube32FaceDisplay();

    // Maps the xiaozhi emotion string to a FaceExpressionId and calls
    // cube32::FaceDisplay::instance().playExpression().
    // Unknown strings fall back to IDLE; the base LcdDisplay emoji path
    // is never invoked.
    void SetEmotion(const char* emotion) override;

    // No-op: face mode is locked to dark theme regardless of the caller.
    void SetTheme(Theme* theme) override;

    // Extends the base SetupUI() by:
    //   1. Hiding emoji_box_, emoji_image_, emoji_label_, content_
    //   2. Starting cube32::FaceDisplay on the active LVGL screen
    //   3. Bringing top_bar_, status_bar_, bottom_bar_ to foreground
    void SetupUI() override;

    // --- NVS helpers (used by the "self.cube32.set_display_mode" MCP tool) --
    // Persist face_mode (1 = face, 0 = chat) to the "display" NVS namespace.
    static void SetFaceMode(bool enable);
    static bool IsFaceModeEnabled();
};

#endif // CONFIG_CUBE32_LVGL_ENABLED && CONFIG_CUBE32_FACE_EXPRESSION_ENABLED
