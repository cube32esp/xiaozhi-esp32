#pragma once

#include "esp32_camera.h"

// Camera adapter that reuses xiaozhi's `Esp32Camera` Capture/Explain logic but
// does NOT call `esp_camera_init` — the cube32 BSP already initialised the
// camera driver inside `cube32_init()`. We simply wrap the running driver
// with the protected no-init constructor introduced in `Esp32Camera`.
class Cube32Camera : public Esp32Camera {
public:
    Cube32Camera();
    ~Cube32Camera() = default;
};
