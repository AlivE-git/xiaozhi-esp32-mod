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
    std::string genre;                // Genre
    float volume;                     // Volume amplification factor (default 1.0 = 100%)
    std::vector<std::string> aliases;  // Keywords used as extra aliases (voice search)

    RadioStation() : volume(1.0f) {}
    RadioStation(const std::string& n, const std::string& u, const std::string& g = "",
                 float v = 1.0f, const std::vector<std::string>& a = {})
        : name(n), url(u), genre(g), volume(v), aliases(a) {}
};

class Esp32Radio : public Radio {
private:
    std::string current_station_name_;
    std::string current_station_key_;  // Key of the playing station, empty for custom URLs
    std::string current_station_url_;
    float current_station_volume_;  // Current station's volume amplification factor

    // Station list keyed by station key. Loaded from NVS and empty on first
    // boot. Guarded by stations_mutex_ because the MCP task and the Web UI task
    // both access it.
    mutable std::mutex stations_mutex_;
    std::map<std::string, RadioStation> radio_stations_;

    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;

    // A start requested during a voice session: the radio waits for the session to
    // end so its audio and network work never overlap the assistant's answer.
    // Guarded by control_mutex_.
    struct PendingStart {
        bool active = false;
        std::string url;
        std::string name;
        std::string key;
    };
    PendingStart pending_start_;
    // The station that was playing when a voice session started. The radio does not
    // just pause for a session: a paused stream kept its TLS socket, decoder and
    // 256 KB buffer alive next to the assistant's audio, which made the answer
    // stutter. Guarded by control_mutex_.
    struct SuspendedStream {
        bool active = false;
        std::string url;
        std::string name;
        std::string key;
        float volume = 1.0f;
    };
    SuspendedStream suspended_;
    // Serializes PlayUrl()/Stop() so thread start/join never races
    mutable std::mutex control_mutex_;
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
    bool LoadStations();
    bool SaveStations();
    void DownloadRadioStream(const std::string& radio_url);
    void PlayRadioStream();
    void ClearAudioBuffer();
    void CleanupAacDecoder();
    void StopLocked();
    // Ends the download side of the stream. A failed stream also drops the
    // playing flag and wakes both threads, so a dead URL never leaves a
    // thread waiting for audio that will not arrive.
    void FinishDownload(bool failed);
    // Starts a stream now when the device is idle, otherwise remembers the request
    // so Application can start it after the conversation.
    bool StartOrDefer(const std::string& radio_url, const std::string& station_name,
                      const std::string& station_key);

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

    // Starts a station that was requested while the assistant owned the speaker.
    // Application calls it when the device returns to idle.
    bool StartPendingIfAny();

    // True while a start is waiting for the assistant to finish. The idle decision
    // after an answer uses it so the station can begin right away.
    bool HasPendingStart();

    // Stops both streaming threads for the duration of a voice session and saves the
    // station so it can be restarted afterwards. Application calls it whenever a
    // session begins (button, push-to-talk, wake word).
    bool SuspendForVoiceSession();

    // Restarts the station saved by SuspendForVoiceSession(). Returns false when
    // nothing was suspended or a station requested during the session takes over.
    bool ResumeAfterVoiceSession();

    // True while a station is stopped for a voice session and waiting to restart.
    // The idle decision after an answer uses it so the device returns to standby.
    bool IsSuspended() const;

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
