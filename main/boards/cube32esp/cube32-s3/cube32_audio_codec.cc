#include "cube32_audio_codec.h"
#include "drivers/audio/audio_codec.h"

#include <esp_log.h>

#define TAG "Cube32AudioCodec"

Cube32AudioCodec::Cube32AudioCodec() {
    auto& ac = cube32::AudioCodec::instance();
    if (!ac.isInitialized()) {
        ESP_LOGE(TAG, "cube32::AudioCodec not initialised; cube32_init() must run first");
    }
    duplex_           = true;
    input_sample_rate_  = ac.getInputSampleRate();
    output_sample_rate_ = ac.getOutputSampleRate();
    input_channels_   = ac.getInputChannels();
    output_channels_  = 1;
    output_volume_    = ac.getOutputVolume();
    input_gain_       = static_cast<float>(ac.getInputGain());
    input_reference_  = ac.hasInputReference();
    ESP_LOGI(TAG,
             "init in=%d Hz / %d ch out=%d Hz vol=%d ref=%d",
             input_sample_rate_, input_channels_,
             output_sample_rate_, output_volume_, input_reference_ ? 1 : 0);
}

void Cube32AudioCodec::Start() {
    // cube32_init() already started the codec; nothing to do here.
}

void Cube32AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    cube32::AudioCodec::instance().enableInput(enable);
    AudioCodec::EnableInput(enable);
}

void Cube32AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    cube32::AudioCodec::instance().enableOutput(enable, output_sample_rate_);
    AudioCodec::EnableOutput(enable);
}

void Cube32AudioCodec::SetOutputVolume(int volume) {
    cube32::AudioCodec::instance().setOutputVolume(volume);
    AudioCodec::SetOutputVolume(volume);
}

void Cube32AudioCodec::SetInputGain(float gain) {
    cube32::AudioCodec::instance().setInputGain(static_cast<int>(gain));
    AudioCodec::SetInputGain(gain);
}

int Cube32AudioCodec::Read(int16_t* dest, int samples) {
    return cube32::AudioCodec::instance().read(dest, samples);
}

int Cube32AudioCodec::Write(const int16_t* data, int samples) {
    return cube32::AudioCodec::instance().write(data, samples);
}
