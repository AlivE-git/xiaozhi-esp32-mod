#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
    eq_gains_db_.fill(0);
}

AudioCodec::~AudioCodec() {
    CloseEq();
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    ApplyEq(data);
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }
    LoadEqSettings();

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

void AudioCodec::CloseEq() {
    if (eq_handle_ != nullptr) {
        esp_ae_eq_close(eq_handle_);
        eq_handle_ = nullptr;
    }
    eq_sample_rate_cfg_ = 0;
    eq_channels_cfg_ = 0;
}

void AudioCodec::LoadEqSettings() {
    Settings settings("audio", false);
    for (int i = 0; i < kEqBandCount; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "eq_band_%d", i);
        int gain = settings.GetInt(key, 0);
        if (gain < -15) gain = -15;
        if (gain > 15) gain = 15;
        eq_gains_db_[i] = gain;
    }
}

void AudioCodec::EnsureEq() {
    const int sr = output_sample_rate_;
    const int ch = output_channels_;
    if (sr <= 0 || ch <= 0) {
        return;
    }
    if (eq_handle_ != nullptr && eq_sample_rate_cfg_ == sr && eq_channels_cfg_ == ch) {
        return;
    }

    CloseEq();

    esp_ae_eq_filter_para_t para[kEqBandCount] = {};
    for (int i = 0; i < kEqBandCount; ++i) {
        uint32_t fc = kEqBandFreqs[i];
        const uint32_t max_fc = static_cast<uint32_t>(sr / 2 > 1 ? sr / 2 - 1 : 1);
        if (fc > max_fc) {
            fc = max_fc;
        }
        para[i].filter_type = ESP_AE_EQ_FILTER_PEAK;
        para[i].fc = fc;
        para[i].q = 1.0f;
        para[i].gain = static_cast<float>(eq_gains_db_[i]);
    }

    esp_ae_eq_cfg_t cfg = {};
    cfg.sample_rate = static_cast<uint32_t>(sr);
    cfg.channel = static_cast<uint8_t>(ch);
    cfg.bits_per_sample = 16;
    cfg.filter_num = kEqBandCount;
    cfg.para = para;

    esp_ae_eq_handle_t handle = nullptr;
    esp_ae_err_t ret = esp_ae_eq_open(&cfg, &handle);
    if (ret != ESP_AE_ERR_OK || handle == nullptr) {
        ESP_LOGE(TAG, "Failed to open EQ: %d", ret);
        return;
    }

    for (int i = 0; i < kEqBandCount; ++i) {
        if (kEqBandFreqs[i] >= static_cast<uint32_t>(sr / 2)) {
            esp_ae_eq_disable_filter(handle, static_cast<uint8_t>(i));
        }
    }

    eq_handle_ = handle;
    eq_sample_rate_cfg_ = sr;
    eq_channels_cfg_ = ch;
    ESP_LOGI(TAG, "EQ opened: %d Hz, %d ch", sr, ch);
}

void AudioCodec::ApplyEq(std::vector<int16_t>& data) {
    if (data.empty()) {
        return;
    }
    bool has_gain = false;
    for (int i = 0; i < kEqBandCount; ++i) {
        if (eq_gains_db_[i] != 0) {
            has_gain = true;
            break;
        }
    }
    if (!has_gain) {
        return;
    }

    EnsureEq();
    if (eq_handle_ == nullptr || eq_channels_cfg_ <= 0) {
        return;
    }

    const uint32_t sample_num =
        static_cast<uint32_t>(data.size() / static_cast<size_t>(eq_channels_cfg_));
    if (sample_num == 0) {
        return;
    }
    esp_ae_err_t ret =
        esp_ae_eq_process(eq_handle_, sample_num, reinterpret_cast<esp_ae_sample_t>(data.data()),
                          reinterpret_cast<esp_ae_sample_t>(data.data()));
    if (ret != ESP_AE_ERR_OK) {
        ESP_LOGW(TAG, "EQ process failed: %d", ret);
    }
}

void AudioCodec::SetEqGains(const std::vector<int>& gains_db) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    for (int i = 0; i < kEqBandCount; ++i) {
        int gain = 0;
        if (i < static_cast<int>(gains_db.size())) {
            gain = gains_db[i];
        }
        if (gain < -15) gain = -15;
        if (gain > 15) gain = 15;
        eq_gains_db_[i] = gain;
    }

    Settings settings("audio", true);
    for (int i = 0; i < kEqBandCount; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "eq_band_%d", i);
        settings.SetInt(key, eq_gains_db_[i]);
    }

    if (eq_handle_ != nullptr) {
        for (int i = 0; i < kEqBandCount; ++i) {
            esp_ae_eq_filter_para_t para = {};
            uint32_t fc = kEqBandFreqs[i];
            const uint32_t max_fc = static_cast<uint32_t>(
                eq_sample_rate_cfg_ / 2 > 1 ? eq_sample_rate_cfg_ / 2 - 1 : 1);
            if (fc > max_fc) {
                fc = max_fc;
            }
            para.filter_type = ESP_AE_EQ_FILTER_PEAK;
            para.fc = fc;
            para.q = 1.0f;
            para.gain = static_cast<float>(eq_gains_db_[i]);
            esp_ae_err_t ret =
                esp_ae_eq_set_filter_para(eq_handle_, static_cast<uint8_t>(i), &para);
            if (ret != ESP_AE_ERR_OK) {
                ESP_LOGW(TAG, "Failed to set EQ band %d: %d", i, ret);
            }
        }
    }

    std::string gains_text;
    for (int i = 0; i < kEqBandCount; ++i) {
        if (!gains_text.empty()) {
            gains_text += ' ';
        }
        gains_text += std::to_string(eq_gains_db_[i]);
    }
    ESP_LOGI(TAG, "EQ gains set: %s dB", gains_text.c_str());
}

std::vector<int> AudioCodec::GetEqGains() const {
    // Accessed from MCP/main task; copy is small and values are stable enough.
    // The output mutex is mutable so a const query can still take it.
    std::lock_guard<std::mutex> lock(output_mutex_);
    return std::vector<int>(eq_gains_db_.begin(), eq_gains_db_.end());
}
