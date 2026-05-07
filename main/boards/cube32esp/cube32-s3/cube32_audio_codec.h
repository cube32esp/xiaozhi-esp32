#pragma once

#include "audio_codec.h"

// Thin xiaozhi `AudioCodec` adapter that forwards every call to
// `cube32::AudioCodec::instance()`. The cube32 BSP is responsible for
// initialising the codec (inside `cube32_init()`); this class never calls
// cube32 `begin()` / `end()`.
class Cube32AudioCodec : public AudioCodec {
public:
    Cube32AudioCodec();
    ~Cube32AudioCodec() override = default;

    void Start() override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
    void SetOutputVolume(int volume) override;
    void SetInputGain(float gain) override;

protected:
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;
};
