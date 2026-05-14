// Cube32FaceDisplay — face expression mode for CUBE32-S3.
//
// Replaces Xiaozhi's central emoji image with cube32::FaceDisplay procedural
// animated eyes.  Xiaozhi's top status bar, status text, bottom chat-message
// bar, notifications and battery popup wiring are all inherited unchanged from
// LcdDisplay/Cube32AttachedLcdDisplay.
//
// Compile guard: requires both CONFIG_CUBE32_LVGL_ENABLED and
// CONFIG_CUBE32_FACE_EXPRESSION_ENABLED.  The whole file is excluded by the
// preprocessor when either flag is absent, keeping binary size unaffected.
#include "sdkconfig.h"
#if defined(CONFIG_CUBE32_LVGL_ENABLED) && defined(CONFIG_CUBE32_FACE_EXPRESSION_ENABLED)

#include "cube32_face_display.h"

// cube32.h is the BSP umbrella header; when CONFIG_CUBE32_FACE_EXPRESSION_ENABLED
// is set it conditionally includes <drivers/robot/face_expression.h>, which
// provides cube32::FaceDisplay, cube32::FaceConfig, cube32::FaceExpressionId
// and CUBE32_FACE_CONFIG_DEFAULT().
#include "cube32.h"

#include "display/lvgl_display/lvgl_theme.h"  // LvglThemeManager
#include "settings.h"                          // Settings (NVS)

#include <esp_log.h>

#define TAG "Cube32FaceDisplay"

// ---------------------------------------------------------------------------
// Emotion-string → FaceExpressionId look-up table
// Covers all 21 xiaozhi emotion strings using the 16 current BSP IDs.
// Update to the richer IDs once the BSP ships the extended enum.
// ---------------------------------------------------------------------------
using cube32::FaceExpressionId;

static const struct { const char* name; FaceExpressionId id; } kEmotionMap[] = {
    { "neutral",     FaceExpressionId::IDLE        },
    { "happy",       FaceExpressionId::EXCITED      },
    { "laughing",    FaceExpressionId::EXCITED      },
    { "funny",       FaceExpressionId::EXCITED      },
    { "sad",         FaceExpressionId::SAD          },
    { "angry",       FaceExpressionId::SHAKE        },
    { "crying",      FaceExpressionId::SAD          },
    { "loving",      FaceExpressionId::BOW          },
    { "embarrassed", FaceExpressionId::BOW          },
    { "surprised",   FaceExpressionId::DOUBLE_TAKE  },
    { "shocked",     FaceExpressionId::DOUBLE_TAKE  },
    { "thinking",    FaceExpressionId::CURIOUS      },
    { "winking",     FaceExpressionId::NOD          },
    { "cool",        FaceExpressionId::ATTENTION    },
    { "relaxed",     FaceExpressionId::IDLE         },
    { "delicious",   FaceExpressionId::EXCITED      },
    { "kissy",       FaceExpressionId::BOW          },
    { "confident",   FaceExpressionId::ATTENTION    },
    { "sleepy",      FaceExpressionId::SAD          },
    { "silly",       FaceExpressionId::DIZZY        },
    { "confused",    FaceExpressionId::CURIOUS      },
};

static FaceExpressionId LookupEmotion(const char* emotion) {
    for (const auto& entry : kEmotionMap) {
        if (strcmp(entry.name, emotion) == 0) {
            return entry.id;
        }
    }
    return FaceExpressionId::IDLE;  // safe default for unknown strings
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------

void Cube32FaceDisplay::SetFaceMode(bool enable) {
    Settings settings("display", /*read_write=*/true);
    settings.SetInt("face_mode", enable ? 1 : 0);
}

bool Cube32FaceDisplay::IsFaceModeEnabled() {
    Settings settings("display", /*read_write=*/false);
    // Default = 1 (face mode) when CONFIG_CUBE32_FACE_EXPRESSION_ENABLED is
    // compiled in and no NVS key has ever been written.  Use the MCP tool
    // "self.cube32.set_display_mode mode=chat" to revert to chat UI.
    return settings.GetInt("face_mode", 1) != 0;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Cube32FaceDisplay::Cube32FaceDisplay(
        esp_lcd_panel_io_handle_t panel_io,
        esp_lcd_panel_handle_t    panel,
        int width, int height)
    : Cube32AttachedLcdDisplay(panel_io, panel, width, height)
{
    // The LcdDisplay base constructor loads the theme from NVS (default
    // "light").  Override it here so the face always runs on a dark canvas.
    auto* dark = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark != nullptr) {
        current_theme_ = dark;
    } else {
        // Themes are registered in InitializeLcdThemes() which runs inside the
        // LcdDisplay constructor, so "dark" should always be present.
        ESP_LOGW(TAG, "Dark theme not found — using whatever theme was loaded from NVS");
    }
    ESP_LOGI(TAG, "Cube32FaceDisplay created (dark theme forced)");
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

Cube32FaceDisplay::~Cube32FaceDisplay() {
    cube32::FaceDisplay::instance().end();
}

// ---------------------------------------------------------------------------
// SetupUI
// ---------------------------------------------------------------------------

void Cube32FaceDisplay::SetupUI() {
    // Step 1 — Build the full xiaozhi widget tree (status bar, bottom bar,
    //          container, notification/status labels, battery popup …).
    //          The dark theme is already set so all colours are applied dark.
    Cube32AttachedLcdDisplay::SetupUI();

    // Step 2 — Suppress the emoji area and chat scroll content.
    //          Capture the active screen handle while holding the LVGL lock.
    lv_obj_t* screen = nullptr;
    {
        DisplayLockGuard lock(this);

        // emoji_box_ is the parent of emoji_label_ + emoji_image_ in the
        // standard (non-wechat) SetupUI layout.  Hiding it hides both children.
        if (emoji_box_   != nullptr) lv_obj_add_flag(emoji_box_,   LV_OBJ_FLAG_HIDDEN);
        // Defensive: also hide individually in case the wechat-style layout
        // (where emoji_box_ == nullptr) is compiled in.
        if (emoji_image_ != nullptr) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_ != nullptr) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        // content_ is the scrollable chat bubble area; only non-null in the
        // wechat-style SetupUI.  Hiding it frees the full middle band for the face.
        if (content_     != nullptr) lv_obj_add_flag(content_,     LV_OBJ_FLAG_HIDDEN);

        screen = lv_screen_active();
    }

    // Step 3 — Start the face engine bounded to the middle band of the screen,
    //          leaving the top and bottom bars untouched.
    //          FaceDisplay::begin() calls lv_obj_create internally; we MUST hold
    //          the LVGL lock so the LVGL task cannot render a partially-built
    //          object tree mid-creation (which would cause one-eye glitches).
    auto& face = cube32::FaceDisplay::instance();
    if (!face.isInitialized()) {
        cube32::FaceConfig cfg = CUBE32_FACE_CONFIG_DEFAULT();
        cfg.screen_w = static_cast<uint16_t>(width_);
        cfg.screen_h = static_cast<uint16_t>(height_);
        // Match the dark theme background (0x000000) so the face area, status
        // bar and bottom bar all share the same black background.
        // lid_color must also match bg_color — the eyelid rectangles park above/
        // below each eye when open and would otherwise show as a contrasting band.
        cfg.bg_color  = 0x000000;
        cfg.lid_color = 0x000000;

        // Measure bar heights and start face engine under a single LVGL lock.
        // Holding the lock through begin() prevents the LVGL task from rendering
        // a partially-constructed object tree (which causes one-eye glitches).
        int16_t top_h    = 0;
        int16_t bottom_h = 0;
        cube32_result_t rc = CUBE32_OK;
        {
            DisplayLockGuard lock(this);

            if (top_bar_    != nullptr) top_h    = (int16_t)lv_obj_get_height(top_bar_);
            if (bottom_bar_ != nullptr) bottom_h = (int16_t)lv_obj_get_height(bottom_bar_);

            // Guard: if layout hasn't resolved yet (height==0), use a safe fallback
            if (top_h    <= 0) top_h    = 24;
            if (bottom_h <= 0) bottom_h = 24;

            cfg.face_x = 0;
            cfg.face_y = top_h;
            cfg.face_w = static_cast<uint16_t>(width_);
            int16_t mid_h = static_cast<int16_t>(height_) - top_h - bottom_h;
            if (mid_h <= 0) {
                cfg.face_y = 0;
                mid_h = static_cast<int16_t>(height_);
            }
            cfg.face_h = static_cast<uint16_t>(mid_h);

            ESP_LOGI(TAG, "Face area: y=%d h=%d (top_bar=%dpx bottom_bar=%dpx)",
                     cfg.face_y, cfg.face_h, top_h, bottom_h);

            rc = face.begin(cfg, screen);
        }
        if (rc != CUBE32_OK) {
            ESP_LOGE(TAG, "FaceDisplay::begin() failed (%d)", static_cast<int>(rc));
            return;
        }
    }

    // Step 4 — face_container is a new screen-level child added after the bars,
    //          so it is already in front of them.  Move the bars to the very
    //          foreground so they always overlay the face regardless of z-order.
    {
        DisplayLockGuard lock(this);
        if (top_bar_    != nullptr) lv_obj_move_foreground(top_bar_);
        if (status_bar_ != nullptr) lv_obj_move_foreground(status_bar_);
        if (bottom_bar_ != nullptr) lv_obj_move_foreground(bottom_bar_);
    }

    ESP_LOGI(TAG, "Face mode active — eyes on screen");
}

// ---------------------------------------------------------------------------
// SetEmotion
// ---------------------------------------------------------------------------

void Cube32FaceDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return;

    auto& face = cube32::FaceDisplay::instance();
    if (!face.isInitialized()) return;

    FaceExpressionId id = LookupEmotion(emotion);
    ESP_LOGI(TAG, "SetEmotion('%s') → FaceExpressionId %d", emotion, static_cast<int>(id));
    face.playExpression(id);
}

// ---------------------------------------------------------------------------
// SetTheme — face mode is always dark, but we MUST forward the call so that
// font / colour updates propagate to the status & bottom bars.  In particular
// `Assets::LvglStrategy::Apply()` calls `display->SetTheme(current_theme)`
// after loading the custom CJK text_font from the assets partition.  If we
// short-circuit that call, the screen-level local `text_font` style baked in
// by `LcdDisplay::SetupUI()` keeps pointing at the builtin basic font and
// Chinese glyphs (which only live in the asset font) render as blanks.
// ---------------------------------------------------------------------------

void Cube32FaceDisplay::SetTheme(Theme* /*theme*/) {
    // Force-dark regardless of what the caller requested: face mode looks
    // wrong on a light background (the face container is hard-coded dark).
    auto* dark = LvglThemeManager::GetInstance().GetTheme("dark");
    if (dark == nullptr) {
        ESP_LOGW(TAG, "SetTheme: dark theme missing — skipping refresh");
        return;
    }
    Cube32AttachedLcdDisplay::SetTheme(dark);
}

#endif // CONFIG_CUBE32_LVGL_ENABLED && CONFIG_CUBE32_FACE_EXPRESSION_ENABLED
