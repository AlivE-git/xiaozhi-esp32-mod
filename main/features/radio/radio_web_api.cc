#include "radio_web_api.h"

#include "application.h"
#include "board.h"
#include "cjson_utils.h"
#include "esp32_radio.h"
#include "features/web/web_server.h"

#include <esp_log.h>

#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#define TAG "RadioWebApi"

namespace {

// "маяк, mayak" -> {"маяк", "mayak"}. The Web UI sends lists as comma separated
// form values in some places and as JSON arrays in others.
std::vector<std::string> SplitAliases(const std::string& value) {
    std::vector<std::string> aliases;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) {
            comma = value.size();
        }
        std::string alias = value.substr(start, comma - start);
        size_t first = alias.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            size_t last = alias.find_last_not_of(" \t\r\n");
            aliases.push_back(alias.substr(first, last - first + 1));
        }
        if (comma == value.size()) {
            break;
        }
        start = comma + 1;
    }
    return aliases;
}

// Keys are the internal identity of the list, the page does not show them, so a
// station or a file without one gets a key made from its name: "Маяк" -> "ST".
std::string MakeStationKey(const std::string& name,
                           const std::map<std::string, RadioStation>& existing) {
    std::string base;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) && u < 0x80) {
            base.push_back(static_cast<char>(std::toupper(u)));
        } else if (c == '_' || c == '-') {
            base.push_back(c);
        }
        if (base.size() >= 24) {
            break;
        }
    }
    if (base.empty()) {
        base = "ST";
    }
    std::string key = base;
    for (int i = 2; existing.count(key) > 0 && i < 1000; ++i) {
        key = base + std::to_string(i);
    }
    return key;
}

bool StationFromJson(cJSON* root, std::string* key, RadioStation* station) {
    if (root == nullptr || key == nullptr || station == nullptr || !cJSON_IsObject(root)) {
        return false;
    }
    cJSON* key_item = cJSON_GetObjectItem(root, "key");
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(name) || !cJSON_IsString(url)) {
        return false;
    }
    *key = cJSON_IsString(key_item) ? key_item->valuestring : "";
    station->name = name->valuestring;
    station->url = url->valuestring;
    if (cJSON* genre = cJSON_GetObjectItem(root, "genre"); cJSON_IsString(genre)) {
        station->genre = genre->valuestring;
    }
    if (cJSON* volume = cJSON_GetObjectItem(root, "volume"); cJSON_IsNumber(volume)) {
        station->volume = static_cast<float>(volume->valuedouble);
    }
    // Aliases may arrive as a JSON array (Web UI) or a comma separated string.
    if (cJSON* aliases = cJSON_GetObjectItem(root, "aliases")) {
        if (cJSON_IsArray(aliases)) {
            cJSON* alias = nullptr;
            cJSON_ArrayForEach(alias, aliases) {
                if (cJSON_IsString(alias)) {
                    station->aliases.emplace_back(alias->valuestring);
                }
            }
        } else if (cJSON_IsString(aliases)) {
            station->aliases = SplitAliases(aliases->valuestring);
        }
    }
    return true;
}

std::string StationsJson(const Esp32Radio& radio) {
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return "{}";
    }
    for (const auto& [key, station] : radio.GetStations()) {
        CJsonUniquePtr item(cJSON_CreateObject());
        CJsonUniquePtr aliases(cJSON_CreateArray());
        if (item == nullptr || aliases == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(item.get(), "key", key.c_str());
        cJSON_AddStringToObject(item.get(), "name", station.name.c_str());
        cJSON_AddStringToObject(item.get(), "url", station.url.c_str());
        cJSON_AddStringToObject(item.get(), "genre", station.genre.c_str());
        cJSON_AddNumberToObject(item.get(), "volume", station.volume);
        for (const auto& alias : station.aliases) {
            cJSON_AddItemToArray(aliases.get(), cJSON_CreateString(alias.c_str()));
        }
        cJSON_AddItemToObject(item.get(), "aliases", aliases.release());
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "stations", array.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t HandleStations(httpd_req_t* req) {
    auto* radio = Application::GetInstance().GetRadio();
    if (radio == nullptr) {
        return web::SendError(req, 500, "Radio unavailable");
    }
    if (req->method == HTTP_GET) {
        return web::SendJson(req, 200, StationsJson(*radio));
    }

    if (req->method == HTTP_DELETE) {
        char key[64] = {0};
        // httpd_query_key_value wants the query string itself ("key=..."),
        // not the full URI with the path in front of it.
        const char* query = strchr(req->uri, '?');
        const char* query_str = (query != nullptr) ? query + 1 : "";
        if (httpd_query_key_value(query_str, "key", key, sizeof(key)) != ESP_OK ||
            key[0] == '\0') {
            return web::SendError(req, 400, "Missing key query parameter");
        }
        ESP_LOGI(TAG, "Delete station request: key='%s'", key);
        // Distinguish "no such station" from a failed NVS save for the user.
        if (radio->GetStations().find(key) == radio->GetStations().end()) {
            ESP_LOGW(TAG, "Delete station: key not present in station list");
            return web::SendError(req, 404, "Station not found");
        }
        if (!radio->DeleteStation(key)) {
            ESP_LOGE(TAG, "Delete station: failed to persist station list to NVS");
            return web::SendError(req, 500, "Failed to save station list");
        }
        return web::SendJson(req, 200, "{\"ok\":true}");
    }

    // POST (add) and PUT (edit) both carry the station as a JSON body.
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    std::string key;
    RadioStation station;
    if (!StationFromJson(root.get(), &key, &station)) {
        return web::SendError(req, 400, "Expected JSON with name and url");
    }

    if (req->method == HTTP_PUT) {
        char query_key[64] = {0};
        const char* query = strchr(req->uri, '?');
        const char* query_str = (query != nullptr) ? query + 1 : "";
        if (httpd_query_key_value(query_str, "key", query_key, sizeof(query_key)) == ESP_OK &&
            query_key[0] != '\0') {
            key = query_key;
        }
        if (!radio->UpdateStation(key, station)) {
            return web::SendError(req, 404, "Station not found or invalid data");
        }
        return web::SendJson(req, 200, "{\"ok\":true}");
    }

    if (req->method == HTTP_POST) {
        if (key.empty()) {
            key = MakeStationKey(station.name, radio->GetStations());
        }
        if (!radio->AddStation(key, station)) {
            return web::SendError(req, 409, "Station key already exists or invalid data");
        }
        return web::SendJson(req, 201, "{\"ok\":true}");
    }

    return web::SendError(req, 400, "Unsupported method");
}

esp_err_t HandleImport(httpd_req_t* req) {
    auto* radio = Application::GetInstance().GetRadio();
    if (radio == nullptr) {
        return web::SendError(req, 500, "Radio unavailable");
    }
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* stations = root != nullptr ? cJSON_GetObjectItem(root.get(), "stations") : nullptr;
    if (!cJSON_IsArray(stations) || cJSON_GetArraySize(stations) == 0) {
        return web::SendError(req, 400, "Expected a non-empty \"stations\" array");
    }

    std::map<std::string, RadioStation> imported;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, stations) {
        std::string key;
        RadioStation station;
        if (!StationFromJson(item, &key, &station)) {
            return web::SendError(req, 400,
                                  "Invalid station entry: name and url are required");
        }
        // Exports carry keys; a file edited by hand may not, and duplicate keys
        // are regenerated instead of failing the whole import.
        if (key.empty() || imported.count(key) > 0) {
            key = MakeStationKey(station.name, imported);
        }
        imported.emplace(key, std::move(station));
    }

    ESP_LOGI(TAG, "Import station request: %d stations", static_cast<int>(imported.size()));
    if (!radio->ReplaceStations(imported)) {
        ESP_LOGE(TAG, "Import station request: failed to persist station list to NVS");
        return web::SendError(req, 500, "Failed to save imported station list");
    }
    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddNumberToObject(result.get(), "imported", static_cast<double>(imported.size()));
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t HandleStatus(httpd_req_t* req) {
    auto* radio = Application::GetInstance().GetRadio();
    if (radio == nullptr) {
        return web::SendError(req, 500, "Radio unavailable");
    }
    bool playing = radio->IsPlaying() || radio->IsDownloading();
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(root.get(), "playing", playing);
    cJSON_AddStringToObject(root.get(), "currentKey",
                            playing ? radio->GetCurrentStationKey().c_str() : "");
    cJSON_AddStringToObject(root.get(), "currentStation",
                            playing ? radio->GetCurrentStation().c_str() : "");
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t HandlePlay(httpd_req_t* req) {
    auto* radio = Application::GetInstance().GetRadio();
    if (radio == nullptr) {
        return web::SendError(req, 500, "Radio unavailable");
    }
    char key[64] = {0};
    const char* query = strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    if (httpd_query_key_value(query_str, "key", key, sizeof(key)) != ESP_OK || key[0] == '\0') {
        return web::SendError(req, 400, "Missing key query parameter");
    }
    ESP_LOGI(TAG, "Play station request: key='%s'", key);
    if (radio->GetStations().find(key) == radio->GetStations().end()) {
        return web::SendError(req, 404, "Station not found");
    }
    // PlayUrl() reports a dead stream (404, DNS error) instead of pretending to
    // play, so the page can show the failure right away.
    if (!radio->PlayStationByKey(key)) {
        return web::SendError(req, 502, "Не удалось открыть поток: станция недоступна");
    }
    // Starting playback from the Web UI counts as user activity: wake the
    // display if it went to sleep.
    Board::GetInstance().WakeUp();
    return web::SendJson(req, 200, "{\"ok\":true}");
}

esp_err_t HandleStop(httpd_req_t* req) {
    auto* radio = Application::GetInstance().GetRadio();
    if (radio == nullptr) {
        return web::SendError(req, 500, "Radio unavailable");
    }
    ESP_LOGI(TAG, "Stop playback request");
    if (!radio->Stop()) {
        return web::SendError(req, 500, "Failed to stop playback");
    }
    return web::SendJson(req, 200, "{\"ok\":true}");
}

const httpd_uri_t kRoutes[] = {
    {.uri = "/api/stations", .method = HTTP_GET, .handler = HandleStations, .user_ctx = nullptr},
    {.uri = "/api/stations", .method = HTTP_POST, .handler = HandleStations, .user_ctx = nullptr},
    {.uri = "/api/stations", .method = HTTP_PUT, .handler = HandleStations, .user_ctx = nullptr},
    {.uri = "/api/stations", .method = HTTP_DELETE, .handler = HandleStations, .user_ctx = nullptr},
    {.uri = "/api/import", .method = HTTP_POST, .handler = HandleImport, .user_ctx = nullptr},
    {.uri = "/api/status", .method = HTTP_GET, .handler = HandleStatus, .user_ctx = nullptr},
    {.uri = "/api/play", .method = HTTP_POST, .handler = HandlePlay, .user_ctx = nullptr},
    {.uri = "/api/stop", .method = HTTP_POST, .handler = HandleStop, .user_ctx = nullptr},
};

}  // namespace

void RadioWebApi::Register(WebServer& server) {
    for (const auto& route : kRoutes) {
        server.AddHandler(route);
    }
}
