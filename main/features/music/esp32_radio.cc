#include "esp32_radio.h"
#include "application.h"
#include "audio/audio_codec.h"
#include "board.h"
#include "cjson_utils.h"
#include "device_state.h"
#include "display/display.h"
#include "settings.h"

#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define TAG "Esp32Radio"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }

Esp32Radio::Esp32Radio()
    : current_station_name_(),
      current_station_url_(),
      current_station_volume_(4.0f),
      radio_stations_(),
      is_playing_(false),
      is_downloading_(false),
      play_thread_(),
      download_thread_(),
      audio_buffer_(),
      buffer_mutex_(),
      buffer_cv_(),
      buffer_size_(0),
      aac_decoder_(nullptr),
      aac_info_(),
      aac_decoder_initialized_(false),
      aac_info_ready_(false),
      aac_out_buffer_() {}

Esp32Radio::~Esp32Radio() {
    ESP_LOGI(TAG, "Destroying radio player - stopping all operations");

    // Stop all operations
    is_downloading_ = false;
    is_playing_ = false;

    // Notify all waiting threads
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // Wait for the download thread to finish
    if (download_thread_.joinable()) {
        download_thread_.join();
        ESP_LOGI(TAG, "Download thread finished");
    }

    // Wait for the playback thread to finish
    if (play_thread_.joinable()) {
        play_thread_.join();
        ESP_LOGI(TAG, "Playback thread finished");
    }

    // Clear the buffer and clean up the decoder
    ClearAudioBuffer();
    CleanupAacDecoder();

    ESP_LOGI(TAG, "Radio player destroyed successfully");
}

void Esp32Radio::Initialize() {
    ESP_LOGI(TAG, "Radio player initialized with MP3/AAC decoder support");
    // Decoder will be initialized on-demand
    InitializeRadioStations();
}

void Esp32Radio::InitializeRadioStations() {
    if (!LoadStations()) {
        ESP_LOGW(TAG, "No saved stations found, seeding default stations");
        SeedDefaultStations();
        SaveStations();
    }
    ESP_LOGI(TAG, "Initialized %d radio stations", radio_stations_.size());
}

void Esp32Radio::SeedDefaultStations() {
    // Russian radio stations. Streams are MP3 unless noted, supported by the
    // ESP Audio simple decoder. Volume values: 1.0 = 100%, 2.0 = 200%, etc.
    // These defaults are saved to NVS on first boot and can afterwards be
    // edited from the Web UI; custom streams can also be started with PlayUrl().

    // === Новости / разговорные ===
    radio_stations_["MAYAK"] =
        RadioStation("Маяк", "http://icecast.vgtrk.cdnvideo.ru/mayakfm_mp3_192kbps",
                     "Новости 24/7 и музыка", "News", 1.0f);
    radio_stations_["VESTI"] =
        RadioStation("Вести FM", "http://icecast.vgtrk.cdnvideo.ru/vestifm_mp3_192kbps", "Новости",
                     "News", 1.0f);
    radio_stations_["RADIO_ROSSII"] =
        RadioStation("Радио России", "http://icecast.vgtrk.cdnvideo.ru/rrzonam_mp3_128kbps",
                     "Новости и разговорные программы", "News/Talk", 1.0f);
    radio_stations_["KP"] = RadioStation(
        "Комсомольская правда", "http://kpradio.hostingradio.ru:8000/russia.radiokp128.mp3",
        "Разговоры и новости", "Talk", 1.0f);

    // === Музыка / развлечения ===
    radio_stations_["EUROPA_PLUS"] =
        RadioStation("Европа Плюс", "http://ep256.hostingradio.ru:8052/europaplus256.mp3",
                     "Танцевальная и популярная музыка", "Pop/Dance", 1.0f);
    radio_stations_["RETRO_FM"] =
        RadioStation("Ретро FM", "http://retroserver.streamr.ru:8043/retro256.mp3",
                     "Хиты прошлых лет", "Pop", 1.0f);
    radio_stations_["ENERGY"] =
        RadioStation("Радио ENERGY", "https://srv02.gpmradio.ru:8443/stream/air/aac/64/99",
                     "Танцевальная музыка", "Dance", 1.0f);
    radio_stations_["AVTORADIO"] =
        RadioStation("Авторадио", "http://178.88.167.62:8080/AVTORADIO_192",
                     "Музыка и автомобильный юмор", "Pop", 1.0f);
    radio_stations_["LOVE_RADIO"] = RadioStation(
        "Love Radio", "http://radio.o2promo.ru:4060/stream", "Романтическая музыка", "Pop", 1.0f);
    radio_stations_["HUMOR_FM"] =
        RadioStation("Юмор FM", "https://gpm.hostingradio.ru/gpm-humorfm495.aacp", "Юмор и сатира",
                     "Comedy", 1.0f);
    radio_stations_["SHANSON"] =
        RadioStation("Радио Шансон", "http://chanson.hostingradio.ru:8041/chanson128.mp3", "Шансон",
                     "Chanson", 1.0f);
    radio_stations_["DACHA"] = RadioStation(
        "Радио Дача",
        "https://stream.n340.com/14_dacha_64?type=aac&UID=AF2C3AB947218127F59F62740712F61D",
        "Популярная музыка", "Pop", 1.0f);
    radio_stations_["DETSKOE"] =
        RadioStation("Детское радио", "https://gpm.hostingradio.ru/gpm-detifm495.aacp",
                     "Детские песни и сказки", "Kids", 1.0f);
    radio_stations_["DOROZHNOE"] =
        RadioStation("Дорожное радио", "http://dorognoe.hostingradio.ru:8000/radio",
                     "Музыка для дороги", "Pop", 1.0f);
    radio_stations_["ULTRA"] =
        RadioStation("Радио Ультра", "http://nashe1.hostingradio.ru:80/ultra-192.mp3",
                     "Рок музыка", "Rock", 1.0f);

    // === Авторские / нестандартные ===
    radio_stations_["MDS"] =
        RadioStation("Модель Для Сборки", "http://mds-station.com:8000/mds",
                     "Литературный аудиопроект МДС, станция I", "Talk/Literature", 1.0f);

    // Default keywords used as extra aliases for voice search. Editable from
    // the Web UI together with the stations.
    const std::map<std::string, std::vector<std::string>> default_aliases = {
        {"MAYAK", {"маяк", "mayak"}},
        {"VESTI", {"вести", "vesti"}},
        {"RADIO_ROSSII", {"россии"}},
        {"KP", {"комсомольск", "kp"}},
        {"EUROPA_PLUS", {"европа", "europa"}},
        {"RETRO_FM", {"ретро", "retro"}},
        {"ENERGY", {"энерджи", "energy", "энергия"}},
        {"AVTORADIO", {"авто", "avtoradio"}},
        {"LOVE_RADIO", {"love", "лав", "любов"}},
        {"HUMOR_FM", {"юмор", "humor"}},
        {"SHANSON", {"шансон", "shanson"}},
        {"DACHA", {"дача", "dacha"}},
        {"DETSKOE", {"детск", "detskoe"}},
        {"DOROZHNOE", {"дорожн", "doroghnoe", "дорога"}},
        {"ULTRA", {"ультра", "ultra"}},
        {"MDS", {"мдс", "mds", "модель"}},
    };
    for (const auto& [key, aliases] : default_aliases) {
        auto it = radio_stations_.find(key);
        if (it != radio_stations_.end()) {
            it->second.aliases = aliases;
        }
    }
}

namespace {

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string TrimAscii(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool IsValidStationKey(const std::string& key) {
    if (key.empty() || key.size() > 32) {
        return false;
    }
    for (char c : key) {
        unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

bool IsValidStation(const RadioStation& station) {
    if (station.name.empty() || station.name.size() > 64) {
        return false;
    }
    if (station.url.rfind("http://", 0) != 0 && station.url.rfind("https://", 0) != 0) {
        return false;
    }
    if (station.url.size() > 256 || station.description.size() > 128 || station.genre.size() > 32) {
        return false;
    }
    return true;
}

RadioStation NormalizeStation(RadioStation station) {
    station.name = TrimAscii(station.name);
    station.url = TrimAscii(station.url);
    station.description = TrimAscii(station.description);
    station.genre = TrimAscii(station.genre);
    if (station.volume < 0.1f) {
        station.volume = 0.1f;
    }
    if (station.volume > 4.0f) {
        station.volume = 4.0f;
    }
    std::vector<std::string> aliases;
    for (const auto& alias : station.aliases) {
        std::string normalized = ToLowerAscii(TrimAscii(alias));
        if (!normalized.empty() &&
            std::find(aliases.begin(), aliases.end(), normalized) == aliases.end()) {
            aliases.push_back(normalized);
        }
    }
    station.aliases = aliases;
    return station;
}

}  // namespace

// The station list JSON is stored in NVS in small chunks ("c0", "c1", ...
// plus a "n" chunk count). The NVS partition on small boards is only a few
// pages, and one multi-KB nvs_set_str needs several free pages at once and
// quickly exhausts it (ESP_ERR_NVS_NOT_ENOUGH_SPACE). Small chunks let GC
// reclaim pages one at a time. The legacy single "stations" key is still
// read for backward compatibility and erased after the first chunked save.
namespace {
constexpr size_t kNvsChunkSize = 1024;
}  // namespace

bool Esp32Radio::LoadStations() {
    Settings settings("radio");
    std::string saved;
    int32_t chunk_count = settings.GetInt("n", 0);
    if (chunk_count > 0) {
        for (int32_t i = 0; i < chunk_count; ++i) {
            saved += settings.GetString("c" + std::to_string(i));
        }
    } else {
        saved = settings.GetString("stations");
    }
    if (saved.empty()) {
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(saved.c_str()));
    if (root == nullptr) {
        ESP_LOGE(TAG, "Saved station list is not valid JSON, ignoring it");
        return false;
    }
    cJSON* stations = cJSON_GetObjectItem(root.get(), "stations");
    if (!cJSON_IsArray(stations)) {
        ESP_LOGE(TAG, "Saved station list has no stations array, ignoring it");
        return false;
    }

    std::map<std::string, RadioStation> loaded;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, stations) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        cJSON* key = cJSON_GetObjectItem(item, "key");
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* url = cJSON_GetObjectItem(item, "url");
        if (!cJSON_IsString(key) || !cJSON_IsString(name) || !cJSON_IsString(url)) {
            continue;
        }
        RadioStation station;
        station.name = name->valuestring;
        station.url = url->valuestring;
        if (cJSON* description = cJSON_GetObjectItem(item, "description"); cJSON_IsString(description)) {
            station.description = description->valuestring;
        }
        if (cJSON* genre = cJSON_GetObjectItem(item, "genre"); cJSON_IsString(genre)) {
            station.genre = genre->valuestring;
        }
        if (cJSON* volume = cJSON_GetObjectItem(item, "volume"); cJSON_IsNumber(volume)) {
            station.volume = static_cast<float>(volume->valuedouble);
        }
        if (cJSON* aliases = cJSON_GetObjectItem(item, "aliases"); cJSON_IsArray(aliases)) {
            cJSON* alias = nullptr;
            cJSON_ArrayForEach(alias, aliases) {
                if (cJSON_IsString(alias)) {
                    station.aliases.emplace_back(alias->valuestring);
                }
            }
        }
        station = NormalizeStation(station);
        if (IsValidStationKey(key->valuestring) && IsValidStation(station)) {
            loaded[key->valuestring] = station;
        }
    }

    if (loaded.empty()) {
        ESP_LOGE(TAG, "Saved station list contains no valid stations, ignoring it");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stations_mutex_);
        radio_stations_ = std::move(loaded);
    }
    ESP_LOGI(TAG, "Loaded %d radio stations from NVS", radio_stations_.size());
    return true;
}

bool Esp32Radio::SaveStations() {
    std::map<std::string, RadioStation> stations;
    {
        std::lock_guard<std::mutex> lock(stations_mutex_);
        stations = radio_stations_;
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return false;
    }
    for (const auto& [key, station] : stations) {
        CJsonUniquePtr item(cJSON_CreateObject());
        CJsonUniquePtr aliases(cJSON_CreateArray());
        if (item == nullptr || aliases == nullptr) {
            return false;
        }
        cJSON_AddStringToObject(item.get(), "key", key.c_str());
        cJSON_AddStringToObject(item.get(), "name", station.name.c_str());
        cJSON_AddStringToObject(item.get(), "url", station.url.c_str());
        cJSON_AddStringToObject(item.get(), "description", station.description.c_str());
        cJSON_AddStringToObject(item.get(), "genre", station.genre.c_str());
        cJSON_AddNumberToObject(item.get(), "volume", station.volume);
        for (const auto& alias : station.aliases) {
            cJSON_AddItemToArray(aliases.get(), cJSON_CreateString(alias.c_str()));
        }
        cJSON_AddItemToObject(item.get(), "aliases", aliases.release());
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "stations", array.release());

    CJsonStringUniquePtr json_str(cJSON_PrintUnformatted(root.get()));
    if (json_str == nullptr) {
        return false;
    }
    const std::string json = json_str.get();
    const int32_t chunk_count =
        static_cast<int32_t>((json.size() + kNvsChunkSize - 1) / kNvsChunkSize);

    Settings settings("radio", true);
    const int32_t old_chunk_count = settings.GetInt("n", 0);
    // Erase the legacy monolithic entry first: on the first chunked save it
    // would otherwise coexist with the new chunks and fill the tiny NVS
    // partition. Chunked data (if any) is authoritative, so this is safe.
    settings.EraseKey("stations");
    for (int32_t i = 0; i < chunk_count; ++i) {
        const size_t offset = static_cast<size_t>(i) * kNvsChunkSize;
        const std::string chunk = json.substr(offset, kNvsChunkSize);
        if (!settings.SetString("c" + std::to_string(i), chunk)) {
            ESP_LOGE(TAG, "Failed to save station chunk %d to NVS", static_cast<int>(i));
            return false;
        }
    }
    if (!settings.SetInt("n", chunk_count)) {
        ESP_LOGE(TAG, "Failed to save station chunk count to NVS");
        return false;
    }
    // Drop leftover chunks from a previously longer list, so they do not
    // keep occupying scarce NVS pages.
    for (int32_t i = chunk_count; i < old_chunk_count; ++i) {
        settings.EraseKey("c" + std::to_string(i));
    }
    ESP_LOGI(TAG, "Saved %d radio stations to NVS in %d chunks", stations.size(),
             static_cast<int>(chunk_count));
    return true;
}

bool Esp32Radio::PlayStation(const std::string& station_name) {
    ESP_LOGI(TAG, "Request to play radio station: %s", station_name.c_str());

    // Convert input to lowercase for case-insensitive search
    std::string lower_input = ToLowerAscii(station_name);

    RadioStation found;
    bool found_station = false;
    {
        std::lock_guard<std::mutex> lock(stations_mutex_);

        // Keywords used as extra aliases, stored per station
        for (const auto& station : radio_stations_) {
            for (const auto& alias : station.second.aliases) {
                std::string lower_alias = ToLowerAscii(alias);
                if (!lower_alias.empty() && lower_input.find(lower_alias) != std::string::npos) {
                    ESP_LOGI(TAG, "Found station by alias '%s' -> %s", alias.c_str(),
                             station.second.name.c_str());
                    found = station.second;
                    found_station = true;
                    break;
                }
            }
            if (found_station) {
                break;
            }
        }

        // Next, try to find by station display name (case insensitive partial match)
        if (!found_station) {
            for (const auto& station : radio_stations_) {
                std::string lower_station_name = ToLowerAscii(station.second.name);
                if (lower_station_name.find(lower_input) != std::string::npos ||
                    lower_input.find(lower_station_name) != std::string::npos) {
                    ESP_LOGI(TAG, "Found station by display name: '%s' -> %s", station_name.c_str(),
                             station.second.name.c_str());
                    found = station.second;
                    found_station = true;
                    break;
                }
            }
        }

        // Finally, try to find by station key (case insensitive)
        if (!found_station) {
            for (const auto& station : radio_stations_) {
                if (ToLowerAscii(station.first) == lower_input) {
                    ESP_LOGI(TAG, "Found station by key (case insensitive): '%s' -> %s",
                             station_name.c_str(), station.second.name.c_str());
                    found = station.second;
                    found_station = true;
                    break;
                }
            }
        }
    }

    if (!found_station) {
        ESP_LOGE(TAG, "Radio station not found: %s", station_name.c_str());
        return false;
    }

    current_station_volume_ = found.volume;
    return PlayUrl(found.url, found.name);
}

bool Esp32Radio::PlayUrl(const std::string& radio_url, const std::string& station_name) {
    std::lock_guard<std::mutex> control_lock(control_mutex_);

    if (radio_url.empty()) {
        ESP_LOGE(TAG, "Radio URL is empty");
        return false;
    }

    ESP_LOGI(TAG, "Starting radio stream: %s (%s)",
             station_name.empty() ? "Custom URL" : station_name.c_str(), radio_url.c_str());

    // Stop previous playback without re-acquiring the control mutex
    // (the caller already holds control_mutex_)
    if (is_playing_ || is_downloading_) {
        StopLocked();
    }

    // Set current station information
    current_station_url_ = radio_url;
    current_station_name_ = station_name.empty() ? "Custom Radio" : station_name;

    // If current_station_volume_ wasn't set by PlayStation(), use default volume
    if (current_station_volume_ <= 0.0f) {
        current_station_volume_ = 4.0f;  // Default volume for custom URLs
    }

    // Clear the buffer
    ClearAudioBuffer();

    // Configure download thread stack size
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 1024 * 3 + 512;
    cfg.prio = 5;
    cfg.thread_name = "radio_download";
    esp_pthread_set_cfg(&cfg);

    // Start download thread
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Radio::DownloadRadioStream, this, radio_url);

    // Configure playback thread stack size
    cfg.stack_size = 1024 * 8;
    cfg.prio = 6;
    cfg.thread_name = "radio_playback";
    esp_pthread_set_cfg(&cfg);

    // Start playback thread
    is_playing_ = true;
    play_thread_ = std::thread(&Esp32Radio::PlayRadioStream, this);

    ESP_LOGI(TAG, "Radio streaming threads started successfully");
    return true;
}

void Esp32Radio::ShowCurrentStation() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr || current_station_name_.empty()) {
        return;
    }
    // Persistent subtitle line: unlike ShowNotification() it is not cleared by a
    // timer, so the station name stays visible for the whole playback.
    display->SetChatMessage("assistant", current_station_name_.c_str());
}

bool Esp32Radio::Stop() {
    std::lock_guard<std::mutex> control_lock(control_mutex_);

    if (!is_playing_ && !is_downloading_) {
        ESP_LOGW(TAG, "No streaming in progress to stop");
        return true;
    }

    StopLocked();
    return true;
}

void Esp32Radio::StopLocked() {
    ESP_LOGI(TAG, "Stopping radio streaming - downloading=%d, playing=%d", is_downloading_.load(),
             is_playing_.load());

    // Stop download and playback flags
    is_downloading_ = false;
    is_playing_ = false;

    // Notify all waiting threads
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    // Wait for threads to finish
    if (download_thread_.joinable()) {
        download_thread_.join();
        ESP_LOGI(TAG, "Download thread joined in Stop");
    }

    if (play_thread_.joinable()) {
        play_thread_.join();
        ESP_LOGI(TAG, "Play thread joined in Stop");
    }

    // Drop the "now playing" line so a stopped station is not left on screen.
    // RemoveChatMessage() only clears the line if it still shows this station.
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && !current_station_name_.empty()) {
        display->RemoveChatMessage("assistant", current_station_name_.c_str());
    }

    ESP_LOGI(TAG, "Radio streaming stopped successfully");
}

std::vector<std::string> Esp32Radio::GetStationList() const {
    std::vector<std::string> station_list;
    std::lock_guard<std::mutex> lock(stations_mutex_);
    for (const auto& station : radio_stations_) {
        std::string entry = station.first + " - " + station.second.name;
        if (!station.second.aliases.empty()) {
            entry += " (";
            for (size_t i = 0; i < station.second.aliases.size(); ++i) {
                if (i > 0) {
                    entry += ", ";
                }
                entry += station.second.aliases[i];
            }
            entry += ")";
        }
        station_list.push_back(entry);
    }
    return station_list;
}

std::map<std::string, RadioStation> Esp32Radio::GetStations() const {
    std::lock_guard<std::mutex> lock(stations_mutex_);
    return radio_stations_;
}

bool Esp32Radio::AddStation(const std::string& key, const RadioStation& station) {
    RadioStation normalized = NormalizeStation(station);
    if (!IsValidStationKey(key) || !IsValidStation(normalized)) {
        ESP_LOGE(TAG, "Rejected invalid station: key=%s name=%s", key.c_str(),
                 normalized.name.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(stations_mutex_);
        if (radio_stations_.find(key) != radio_stations_.end()) {
            ESP_LOGE(TAG, "Station key already exists: %s", key.c_str());
            return false;
        }
        radio_stations_[key] = normalized;
    }
    if (!SaveStations()) {
        // Keep RAM in sync with NVS: drop the station we just added.
        std::lock_guard<std::mutex> lock(stations_mutex_);
        radio_stations_.erase(key);
        return false;
    }
    return true;
}

bool Esp32Radio::UpdateStation(const std::string& key, const RadioStation& station) {
    RadioStation normalized = NormalizeStation(station);
    if (!IsValidStationKey(key) || !IsValidStation(normalized)) {
        ESP_LOGE(TAG, "Rejected invalid station: key=%s name=%s", key.c_str(),
                 normalized.name.c_str());
        return false;
    }
    RadioStation previous;
    {
        std::lock_guard<std::mutex> lock(stations_mutex_);
        auto it = radio_stations_.find(key);
        if (it == radio_stations_.end()) {
            ESP_LOGE(TAG, "Station not found for update: %s", key.c_str());
            return false;
        }
        previous = it->second;
        it->second = normalized;
    }
    if (!SaveStations()) {
        // Keep RAM in sync with NVS: restore the previous station data.
        std::lock_guard<std::mutex> lock(stations_mutex_);
        radio_stations_[key] = previous;
        return false;
    }
    return true;
}

bool Esp32Radio::DeleteStation(const std::string& key) {
    bool was_playing_station = false;
    RadioStation removed;
    {
        std::lock_guard<std::mutex> lock(stations_mutex_);
        auto it = radio_stations_.find(key);
        if (it == radio_stations_.end()) {
            ESP_LOGE(TAG, "Station not found for delete: %s", key.c_str());
            return false;
        }
        was_playing_station = IsPlaying() && current_station_name_ == it->second.name;
        removed = it->second;
        radio_stations_.erase(it);
    }
    if (!SaveStations()) {
        // Keep RAM in sync with NVS: put the station back.
        std::lock_guard<std::mutex> lock(stations_mutex_);
        radio_stations_[key] = removed;
        return false;
    }
    if (was_playing_station) {
        Stop();
    }
    return true;
}

bool Esp32Radio::ReplaceStations(const std::map<std::string, RadioStation>& stations) {
    if (stations.empty()) {
        ESP_LOGE(TAG, "Rejected empty station list for import");
        return false;
    }
    std::map<std::string, RadioStation> validated;
    for (const auto& [key, station] : stations) {
        RadioStation normalized = NormalizeStation(station);
        if (!IsValidStationKey(key) || !IsValidStation(normalized)) {
            ESP_LOGE(TAG, "Rejected invalid station in import: key=%s name=%s", key.c_str(),
                     normalized.name.c_str());
            return false;
        }
        validated[key] = normalized;
    }

    std::map<std::string, RadioStation> previous;
    bool was_playing_removed_station = false;
    {
        std::lock_guard<std::mutex> lock(stations_mutex_);
        previous = radio_stations_;
        if (IsPlaying() && !current_station_name_.empty()) {
            was_playing_removed_station = true;
            for (const auto& [key, station] : validated) {
                if (station.name == current_station_name_) {
                    was_playing_removed_station = false;
                    break;
                }
            }
        }
        radio_stations_ = validated;
    }
    if (!SaveStations()) {
        // Keep RAM in sync with NVS: restore the previous list.
        std::lock_guard<std::mutex> lock(stations_mutex_);
        radio_stations_ = previous;
        return false;
    }
    if (was_playing_removed_station) {
        Stop();
    }
    ESP_LOGI(TAG, "Replaced radio stations with imported list of %d stations",
             static_cast<int>(validated.size()));
    return true;
}

void Esp32Radio::DownloadRadioStream(const std::string& radio_url) {
    ESP_LOGD(TAG, "Starting radio stream download from: %s", radio_url.c_str());

    if (radio_url.empty() || radio_url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL format: %s", radio_url.c_str());
        is_downloading_ = false;
        return;
    }

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        is_downloading_ = false;
        return;
    }

    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Accept", "*/*");

    bool is_https = (radio_url.find("https://") == 0);
    ESP_LOGI(TAG, "Connecting to %s stream: %s", is_https ? "HTTPS" : "HTTP", radio_url.c_str());

    auto display = Board::GetInstance().GetDisplay();

    std::string current_url = radio_url;
    int redirects = 0;

retry_open: {
    auto opened = http->Open("GET", current_url);
    if (!opened) {
        ESP_LOGE(TAG, "Failed to connect to radio stream URL: %s (%s)", current_url.c_str(),
                 opened.error().ToString().c_str());
        is_downloading_ = false;
        if (display)
            display->ShowNotification("Радио: ошибка подключения", 3000);
        return;
    }

    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "Failed to read radio stream status: %s",
                 status.error().ToString().c_str());
        http->Close();
        is_downloading_ = false;
        if (display)
            display->ShowNotification("Радио: сервер недоступен", 3000);
        return;
    }

    int status_code = *status;
    if (status_code >= 300 && status_code < 400) {
        std::string location = http->GetResponseHeader("Location");
        http->Close();
        if (!location.empty() && redirects < 3) {
            // Location may be scheme-relative (//host/path) or a path-only redirect
            if (location.rfind("//", 0) == 0) {
                auto scheme_end = current_url.find("://");
                location = current_url.substr(0, scheme_end + 3) + location.substr(2);
            } else if (location[0] == '/') {
                auto scheme_pos = current_url.find("://");
                auto path_pos = current_url.find('/', scheme_pos + 3);
                location = (path_pos == std::string::npos ? current_url
                                                          : current_url.substr(0, path_pos)) +
                           location;
            }
            redirects++;
            ESP_LOGI(TAG, "Following redirect %d to: %s", redirects, location.c_str());
            current_url = location;
            goto retry_open;
        }
        ESP_LOGW(TAG, "HTTP %d redirect cannot be followed, status: %d", status_code, status_code);
        is_downloading_ = false;
        return;
    }
    if (status_code != 200 && status_code != 206) {
        ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
        http->Close();
        is_downloading_ = false;
        if (display)
            display->ShowNotification("Радио: сервер недоступен", 3000);
        return;
    }
}  // retry_open

    ESP_LOGI(TAG, "Started downloading radio stream, status: %d",
             *http->GetStatusCode());

    const size_t chunk_size = 4096;
    char* buffer = new char[chunk_size];
    size_t total_downloaded = 0;
    size_t total_print_bytes = 0;
    const int kMaxReconnectAttempts = 3;
    int reconnect_attempts = 0;

    while (is_downloading_ && is_playing_) {
        int bytes_read = 0;
        auto read_result = http->Read(buffer, chunk_size);
        if (!read_result) {
            ESP_LOGW(TAG, "Stream read failed: %s", read_result.error().ToString().c_str());
            bytes_read = -1;
        } else {
            bytes_read = *read_result;
        }

        // ---- HANDLE READ ERRORS & RECONNECT ----
        if (bytes_read < 0 || bytes_read == 0) {
            reconnect_attempts++;
            ESP_LOGW(TAG, "Stream lost (bytes_read=%d), trying reconnect (%d/%d)...", bytes_read,
                     reconnect_attempts, kMaxReconnectAttempts);

            if (display)
                display->ShowNotification("Радио: потеря связи, переподключение...", 3000);

            if (reconnect_attempts > kMaxReconnectAttempts) {
                ESP_LOGE(TAG, "Exceeded max reconnect attempts");
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(1500));  // Wait 1.5s
            http->Close();

            auto reopened = http->Open("GET", current_url);
            if (!reopened) {
                ESP_LOGE(TAG, "Reconnect failed at attempt %d: %s", reconnect_attempts,
                         reopened.error().ToString().c_str());
                continue;
            } else {
                ESP_LOGI(TAG, "Reconnect success at attempt %d", reconnect_attempts);
                continue;
            }
        }

        reconnect_attempts = 0;

        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for radio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);

        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock,
                            [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });

            if (is_downloading_) {
                audio_buffer_.push(RadioAudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                total_print_bytes += bytes_read;
                buffer_cv_.notify_one();

                if (total_print_bytes >= (128 * 1024)) {
                    total_print_bytes = 0;
                    ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded,
                             buffer_size_);
                }
            } else {
                heap_caps_free(chunk_data);
                break;
            }
        }
    }

    delete[] buffer;
    http->Close();

    if (is_downloading_) {
        ESP_LOGI(TAG, "Radio stream download completed");
    } else {
        ESP_LOGI(TAG, "Radio stream download stopped by user");
    }

    is_downloading_ = false;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    if (total_downloaded < 1024 && display) {
        display->ShowNotification("Радио: не удалось подключиться", 3000);
    }

    ESP_LOGI(TAG, "Radio stream download thread finished");
}

void Esp32Radio::PlayRadioStream() {
    ESP_LOGI(TAG, "Starting radio stream playback with MP3/AAC decoder");

    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    auto display = board.GetDisplay();
    if (!codec) {
        ESP_LOGE(TAG, "Audio codec not available");
        is_playing_ = false;
        return;
    }

    // Ensure audio output is enabled
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    // Wait for the buffer to have enough data to start playback
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this] {
            return buffer_size_ >= MIN_BUFFER_SIZE || (!is_downloading_ && !audio_buffer_.empty());
        });
    }

    ESP_LOGI(TAG, "Starting radio playback with buffer size: %d", buffer_size_);

    const size_t kInputBufferSize = 8192;
    size_t total_played_bytes = 0;
    size_t total_print_bytes = 0;
    uint8_t* input_buffer = (uint8_t*)heap_caps_malloc(kInputBufferSize, MALLOC_CAP_SPIRAM);
    if (!input_buffer) {
        input_buffer = (uint8_t*)heap_caps_malloc(kInputBufferSize, MALLOC_CAP_DEFAULT);
    }
    if (!input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate input buffer");
        is_playing_ = false;
        return;
    }
    int bytes_left = 0;
    uint8_t* read_ptr = input_buffer;
    void* resampler = nullptr;

    auto& app = Application::GetInstance();

    // The OLED has a single subtitle line that conversation text also uses, so the
    // station name is (re)applied whenever playback starts from the idle state and
    // whenever the device returns to idle after a conversation.
    auto last_app_state = app.GetDeviceState();
    if (last_app_state == kDeviceStateIdle) {
        ShowCurrentStation();
    }

    while (is_playing_) {
        // Pause radio playback while the device is busy talking or connecting.
        // Radio only plays in the idle state so it never interferes with the mic.
        auto state = app.GetDeviceState();
        if (state != kDeviceStateIdle) {
            ESP_LOGD(TAG, "Device state is %d, pausing radio playback", state);
            last_app_state = state;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (last_app_state != kDeviceStateIdle) {
            ESP_LOGI(TAG, "Resuming radio playback, showing station name: %s",
                     current_station_name_.c_str());
            ShowCurrentStation();
            last_app_state = kDeviceStateIdle;
        }

        // If more audio data is needed, read from the buffer
        if (bytes_left < (int)kInputBufferSize) {
            RadioAudioChunk chunk;

            // Retrieve audio data from the buffer
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        ESP_LOGI(TAG, "Radio stream ended, total played: %d bytes",
                                 total_played_bytes);
                        break;
                    }
                    // Wait for new data
                    buffer_cv_.wait(lock,
                                    [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }

                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                total_played_bytes += chunk.size;
                total_print_bytes += chunk.size;

                // Notify download thread that buffer has space
                buffer_cv_.notify_one();
            }

            // Add new data to the input buffer
            if (chunk.data && chunk.size > 0) {
                // Move remaining data to the beginning of the buffer
                if (bytes_left > 0 && read_ptr != input_buffer) {
                    memmove(input_buffer, read_ptr, bytes_left);
                }

                // Check buffer space
                size_t space_available = kInputBufferSize - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);

                // Copy new data
                memcpy(input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = input_buffer;

                // Free chunk memory
                heap_caps_free(chunk.data);
            }
        }

        if (bytes_left <= 0) {
            continue;  // Need more data
        }

        // Initialize the decoder based on the detected format
        if (!aac_decoder_initialized_) {
            ESP_LOGI(TAG, "Detecting stream format from first bytes: %02X %02X %02X %02X",
                     read_ptr[0], read_ptr[1], read_ptr[2], read_ptr[3]);

            esp_audio_simple_dec_type_t dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
            if (bytes_left >= 4 && read_ptr[0] == 0xFF && (read_ptr[1] & 0xF6) == 0xF0) {
                dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;  // ADTS AAC
            } else if (bytes_left >= 4 && memcmp(read_ptr, "fLaC", 4) == 0) {
                dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
            } else if (bytes_left >= 4 && memcmp(read_ptr, "OggS", 4) == 0) {
                dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
            } else if (bytes_left >= 4 && memcmp(read_ptr, "RIFF", 4) == 0) {
                dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
            }
            // MP3 default covers ID3 tags and MPA/MPEG frame syncs

            aac_cfg_ = {};
            aac_cfg_.dec_type = dec_type;
            aac_cfg_.dec_cfg = nullptr;
            aac_cfg_.cfg_size = 0;
            aac_cfg_.use_frame_dec = false;

            esp_audio_dec_register_default();
            esp_audio_simple_dec_register_default();

            esp_audio_err_t dec_ret = esp_audio_simple_dec_open(&aac_cfg_, &aac_decoder_);
            if (dec_ret != ESP_AUDIO_ERR_OK || !aac_decoder_) {
                ESP_LOGE(TAG, "Failed to open simple decoder (type=%d), ret=%d", aac_cfg_.dec_type,
                         dec_ret);
                esp_audio_simple_dec_unregister_default();
                esp_audio_dec_unregister_default();
                is_playing_ = false;
                break;
            }

            aac_out_buffer_.resize(4096);
            aac_info_ready_ = false;
            aac_decoder_initialized_ = true;
            ESP_LOGI(TAG, "Simple decoder initialized successfully (type=%d)", aac_cfg_.dec_type);
        }

        bool input_eos = (!is_downloading_ && audio_buffer_.empty());

        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = read_ptr;
        raw.len = bytes_left;
        raw.eos = input_eos;

        esp_audio_simple_dec_out_t out_frame = {};
        out_frame.buffer = aac_out_buffer_.data();
        out_frame.len = aac_out_buffer_.size();

        while (raw.len > 0 && is_playing_) {
            esp_audio_err_t dec_ret = esp_audio_simple_dec_process(aac_decoder_, &raw, &out_frame);
            if (dec_ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                // Output buffer not enough, expand and retry
                aac_out_buffer_.resize(out_frame.needed_size);
                out_frame.buffer = aac_out_buffer_.data();
                out_frame.len = out_frame.needed_size;
                continue;
            }
            if (dec_ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Audio decode error: %d", dec_ret);
                is_playing_ = false;
                break;
            }

            // Safety: consumed nothing and produced nothing, wait for more data
            if (raw.consumed == 0 && out_frame.decoded_size == 0) {
                break;
            }

            if (out_frame.decoded_size > 0) {
                // First decode -> get stream info
                if (!aac_info_ready_) {
                    esp_audio_simple_dec_get_info(aac_decoder_, &aac_info_);
                    aac_info_ready_ = true;

                    ESP_LOGI(TAG, "Stream info: %d Hz, %d bits, %d ch", aac_info_.sample_rate,
                             aac_info_.bits_per_sample, aac_info_.channel);

                    // Create resampler if stream rate differs from the codec rate
                    if (resampler == nullptr &&
                        aac_info_.sample_rate != codec->output_sample_rate()) {
                        esp_ae_rate_cvt_cfg_t resampler_cfg = RATE_CVT_CFG(
                            aac_info_.sample_rate, codec->output_sample_rate(), ESP_AUDIO_MONO);
                        ESP_LOGI(TAG, "Resampling audio from %d to %d", aac_info_.sample_rate,
                                 codec->output_sample_rate());
                        esp_ae_rate_cvt_open(&resampler_cfg, &resampler);
                        if (resampler == nullptr) {
                            ESP_LOGE(TAG, "Failed to create output resampler");
                        }
                    }
                }

                int bits_per_sample =
                    (aac_info_.bits_per_sample > 0) ? aac_info_.bits_per_sample : 16;
                int bytes_per_sample = bits_per_sample / 8;
                if (bytes_per_sample < 2)
                    bytes_per_sample = 2;
                int channels = (aac_info_.channel > 0) ? aac_info_.channel : 2;

                int total_samples = out_frame.decoded_size / bytes_per_sample;
                int samples_per_channel =
                    (channels > 0) ? (total_samples / channels) : total_samples;

                int16_t* pcm_in = reinterpret_cast<int16_t*>(out_frame.buffer);
                std::vector<int16_t> mono_buffer;
                int16_t* final_pcm_data = nullptr;
                int final_sample_count = 0;

                if (aac_info_.channel == 2) {
                    // Downmix stereo -> mono
                    mono_buffer.resize(samples_per_channel);
                    for (int i = 0; i < samples_per_channel; ++i) {
                        int32_t left = pcm_in[i * 2];
                        int32_t right = pcm_in[i * 2 + 1];
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = samples_per_channel;
                } else {
                    final_pcm_data = pcm_in;
                    final_sample_count = total_samples;
                }

                // Amplify audio using station-specific volume setting
                std::vector<int16_t> amplified_buffer(final_sample_count);
                const float amplification_factor = current_station_volume_;
                for (int i = 0; i < final_sample_count; ++i) {
                    int32_t amplified_sample = (int32_t)(final_pcm_data[i] * amplification_factor);
                    // Clamp to prevent overflow
                    if (amplified_sample > INT16_MAX) {
                        amplified_sample = INT16_MAX;
                    } else if (amplified_sample < INT16_MIN) {
                        amplified_sample = INT16_MIN;
                    }
                    amplified_buffer[i] = (int16_t)amplified_sample;
                }

                // Resample to the codec sample rate if needed
                std::vector<int16_t>* out_pcm = &amplified_buffer;
                std::vector<int16_t> resampled;
                if (resampler != nullptr) {
                    uint32_t target_size = 0;
                    esp_ae_rate_cvt_get_max_out_sample_num(resampler, amplified_buffer.size(),
                                                           &target_size);
                    resampled.resize(target_size);
                    uint32_t actual_output = target_size;
                    esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)amplified_buffer.data(),
                                            amplified_buffer.size(),
                                            (esp_ae_sample_t)resampled.data(), &actual_output);
                    resampled.resize(actual_output);
                    out_pcm = &resampled;
                    if (actual_output == 0) {
                        ESP_LOGI(TAG, "Resampler produced no samples yet");
                    }
                }

                auto rt_state = app.GetDeviceState();
                if (!out_pcm->empty() && rt_state == kDeviceStateIdle) {
                    app.GetAudioService().NotifyExternalOutputActivity();
                    codec->OutputData(*out_pcm);
                }

                if (total_print_bytes >= (128 * 1024)) {
                    total_print_bytes = 0;
                    ESP_LOGI(TAG, "AAC/MP3: Played %d bytes, buffer size: %d", total_played_bytes,
                             buffer_size_);
                }
            }

            // Update input pointer based on consumed bytes
            raw.len -= raw.consumed;
            raw.buffer += raw.consumed;
        }

        // Update read_ptr and bytes_left for main loop
        bytes_left = raw.len;
        read_ptr = const_cast<uint8_t*>(raw.buffer);

        // Check for end of stream
        if (input_eos && bytes_left == 0) {
            ESP_LOGI(TAG, "Radio stream ended");
            break;
        }
    }

    // Cleanup
    if (input_buffer) {
        heap_caps_free(input_buffer);
    }
    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }

    if (is_playing_) {
        ESP_LOGI(TAG, "Radio stream playback finished successfully");
        ClearAudioBuffer();  // Clear remaining buffer data
        if (display) {
            display->ShowNotification("Радио: поток завершён", 3000);
        }
    } else {
        ESP_LOGI(TAG, "Radio stream playback stopped by user");
    }

    // The stream is over (finished or stopped), so drop the station name line.
    if (display) {
        display->RemoveChatMessage("assistant", current_station_name_.c_str());
    }

    // Cleanup decoder
    CleanupAacDecoder();

    ESP_LOGI(TAG, "Radio stream playback finished, total played: %d bytes", total_played_bytes);
    is_playing_ = false;
}

void Esp32Radio::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    while (!audio_buffer_.empty()) {
        RadioAudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
    }

    buffer_size_ = 0;
    ESP_LOGI(TAG, "Radio audio buffer cleared");
}

// Simple Decoder methods
void Esp32Radio::CleanupAacDecoder() {
    if (!aac_decoder_initialized_) {
        return;
    }

    if (aac_decoder_) {
        esp_audio_simple_dec_close(aac_decoder_);
        aac_decoder_ = nullptr;
    }

    esp_audio_simple_dec_unregister_default();
    esp_audio_dec_unregister_default();

    aac_out_buffer_.clear();
    aac_info_ready_ = false;
    aac_decoder_initialized_ = false;
    aac_cfg_ = {};

    ESP_LOGI(TAG, "Simple decoder cleaned up");
}
