#include "cube32_camera.h"
#include <esp_log.h>

#define TAG "Cube32Camera"

Cube32Camera::Cube32Camera() : Esp32Camera() {
    ESP_LOGI(TAG, "Reusing cube32-initialised esp32-camera driver");
}
