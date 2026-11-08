#ifndef ESP32_RADIO_H
#define ESP32_RADIO_H

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "radio.h"

// Internet radio player for XiaoZhi using the ESP Audio simple decoder.
// Supports MP3/AAC/OGG/FLAC streams. Sample rate is converted to the codec
// output sample rate in software.
extern "C" {
#include "esp_audio_simple_dec_default.h"
}

// Audio data chunk structure
struct RadioAudioChunk {
    uint8_t* data;
    size_t size;

    RadioAudioChunk() : data(nullptr), size(0) {}
    RadioAudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

// Radio station information structure
struct RadioStation {
    std::string name;                 // Radio station name
    std::string url;                  // Streaming URL
    std::string description;          // Description
    std::string genre;                // Genre
    float volume;                     // Volume amplification factor (default 1.0 = 100%)
    std::vector<std::string> aliases;  // Keywords used as extra aliases (voice search)

    RadioStation() : volume(1.0f) {}
    RadioStation(const std::string& n, const std::string& u, const std::string& d = "",
                 const std::string& g = "", float v = 1.0f,
                 const std::vector<std::string>& a = {})
        : name(n), url(u), description(d), genre(g), volume(v), aliases(a) {}
};

class Esp32Radio : public Radio {
private:
    std::string current_station_name_;
    std::string current_station_key_;  // Key of the playing station, empty for custom URLs
    std::string current_station_url_;
    float current_station_volume_;  // Current station's volume amplification factor

    // Station list keyed by station key. Loaded from NVS; seeded with the
    // default stations on first boot. Guarded by stations_mutex_ because the
    // MCP task and the Web UI task both access it.
    mutable std::mutex stations_mutex_;
    std::map<std::string, RadioStation> radio_stations_;

    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    // Serializes PlayUrl()/Stop() so thread start/join never races
    std::mutex control_mutex_;
    std::thread play_thread_;
    std::thread download_thread_;

    // Audio buffer
    std::queue<RadioAudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB buffer
    static constexpr size_t MIN_BUFFER_SIZE = 32 * 1024;   // 32KB minimum playback buffer

    // Simple decoder for radio streams
    esp_audio_simple_dec_handle_t aac_decoder_;
    esp_audio_simple_dec_cfg_t aac_cfg_;
    esp_audio_simple_dec_info_t aac_info_;
    bool aac_decoder_initialized_;
    bool aac_info_ready_;
    std::vector<uint8_t> aac_out_buffer_;

    // Private methods
    void InitializeRadioStations();
    void SeedDefaultStations();
    bool LoadStations();
    bool SaveStations();
    void DownloadRadioStream(const std::string& radio_url);
    void PlayRadioStream();
    void ClearAudioBuffer();
    void CleanupAacDecoder();
    void StopLocked();

public:
    Esp32Radio();
    ~Esp32Radio();

    void Initialize();

    // Show the current station name on the display while the radio is playing.
    // On the 128x32 OLED layout the name goes to the subtitle line under the
    // status bar, which is the only text line left free next to the clock.
    void ShowCurrentStation();

    // Play a specific station
    virtual bool PlayStation(const std::string& station_name) override;

    // Play a station by its exact key (used by the Web UI, where the key is
    // already known and fuzzy name/alias matching must not kick in). Applies
    // the station's volume like PlayStation() does.
    bool PlayStationByKey(const std::string& key);

    // Play a station from a specific URL
    virtual bool PlayUrl(const std::string& radio_url,
                         const std::string& station_name = "") override;

    // Stop playback
    virtual bool Stop() override;

    // Get the list of stations
    virtual std::vector<std::string> GetStationList() const override;

    // Station list management (used by the Web UI). Keys are unique ids;
    // Add/Update validate the station and persist the list to NVS.
    std::map<std::string, RadioStation> GetStations() const;
    bool AddStation(const std::string& key, const RadioStation& station);
    bool UpdateStation(const std::string& key, const RadioStation& station);
    bool DeleteStation(const std::string& key);

    // Replaces the whole station list (used by the Web UI import). Every
    // station is validated first; on any invalid entry or failed NVS save
    // the current list is left untouched and false is returned.
    bool ReplaceStations(const std::map<std::string, RadioStation>& stations);

    // Get current playback status
    virtual bool IsPlaying() const override { return is_playing_; }
    virtual std::string GetCurrentStation() const override { return current_station_name_; }
    std::string GetCurrentStationKey() const { return current_station_key_; }

    // Buffer status
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual int16_t* GetAudioData() override { return nullptr; }
};

#endif  // ESP32_RADIO_H
