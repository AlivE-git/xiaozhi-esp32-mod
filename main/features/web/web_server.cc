#include "web_server.h"

#include "application.h"
#include "board.h"
#include "cjson_utils.h"
#include "features/currency/currency_converter.h"
#include "features/joke/joke_reader.h"
#include "features/reminder/reminder_web_api.h"
#include "features/radio/radio_web_api.h"
#include "features/rss/rss_reader.h"
#include "features/storage/list_storage.h"
#include "features/timer/countdown_timer.h"
#include "features/websearch/web_search_web_api.h"
#include "features/wol/wol_web_api.h"
#include "mcp_server.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define TAG "WebServer"

// The web UI is embedded gzipped (see scripts/pack_web_ui.py) and served with
// Content-Encoding: gzip, so the browser expands it and the image stays small.
extern const uint8_t kPageHtmlGzStart[] asm("_binary_index_html_gz_start");
extern const uint8_t kPageHtmlGzEnd[] asm("_binary_index_html_gz_end");

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

bool FeedFromJson(cJSON* root, RssFeed* feed) {
    if (root == nullptr || feed == nullptr || !cJSON_IsObject(root)) {
        return false;
    }
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* url = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(name) || !cJSON_IsString(url)) {
        return false;
    }
    feed->name = name->valuestring;
    feed->url = url->valuestring;
    if (cJSON* category = cJSON_GetObjectItem(root, "category"); cJSON_IsString(category)) {
        feed->category = category->valuestring;
    }
    // Aliases may arrive as a JSON array (Web UI) or a comma separated string.
    if (cJSON* aliases = cJSON_GetObjectItem(root, "aliases")) {
        if (cJSON_IsArray(aliases)) {
            cJSON* alias = nullptr;
            cJSON_ArrayForEach(alias, aliases) {
                if (cJSON_IsString(alias)) {
                    feed->aliases.emplace_back(alias->valuestring);
                }
            }
        } else if (cJSON_IsString(aliases)) {
            feed->aliases = SplitAliases(aliases->valuestring);
        }
    }
    return true;
}

}  // namespace

namespace web {

std::string ErrorJson(const std::string& message) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return "{}";
    }
    cJSON_AddStringToObject(root.get(), "error", message.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t SendJson(httpd_req_t* req, int status, const std::string& body) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status == 200   ? "200 OK"
                                         : status == 201 ? "201 Created"
                                         : status == 400 ? "400 Bad Request"
                                         : status == 404 ? "404 Not Found"
                                         : status == 409 ? "409 Conflict"
                                         : status == 502 ? "502 Bad Gateway"
                                         : status == 500 ? "500 Internal Server Error"
                                                         : "500 Internal Server Error");
    return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t SendError(httpd_req_t* req, int status, const std::string& message) {
    return SendJson(req, status, ErrorJson(message));
}

bool ReadJsonBody(httpd_req_t* req, std::string& body) {
    if (req->content_len <= 0 || req->content_len > static_cast<int>(kMaxBodySize)) {
        SendError(req, 400, "Invalid request body size");
        return false;
    }
    body.assign(static_cast<size_t>(req->content_len), '\0');
    size_t received = 0;
    while (received < body.size()) {
        int ret = httpd_req_recv(req, body.data() + received, body.size() - received);
        if (ret <= 0) {
            SendError(req, 400, "Failed to read request body");
            return false;
        }
        received += static_cast<size_t>(ret);
    }
    return true;
}

}  // namespace web

WebServer* WebServer::instance_ = nullptr;

WebServer::WebServer() { instance_ = this; }

WebServer::~WebServer() {
    Stop();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

bool WebServer::Start(int port) {
    if (server_handle_ != nullptr) {
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    // The whole device has only CONFIG_LWIP_MAX_SOCKETS (10) sockets, and the
    // MQTT/UDP audio channel, the radio stream and OTA all need some. Reserving
    // 7 for the Web UI left nothing for voice input, which then failed with
    // "Failed to create socket" (ENFILE) after browsing the UI. Keep a few
    // sessions and recycle the oldest instead of refusing new connections.
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.ctrl_port = 32770;
    // The availability checks fetch HTTPS APIs from this task, and the TLS
    // handshake does not fit in the default 4 KB stack.
    config.stack_size = 12 * 1024;
    // The TLS handshake is CPU-heavy. httpd defaults to priority 5, above the
    // AFE fetch task (priority 3), so a request starved the audio pipeline: the
    // codec task kept feeding while nothing drained the AFE ring buffer
    // ("Ringbuffer of AFE(FEED) is full"). Keep the Web UI below the audio
    // tasks; handlers still get CPU because they block on the network.
    config.task_priority = 2;
    // httpd counts every uri/method pair, and the modules registered after this
    // point need slots too: sixteen endpoints here, five for Wake-on-LAN, eight
    // for the radio API, two for the web search and one for the reminders. Keep a
    // margin, since a handler that does not fit is silently missing at runtime.
    config.max_uri_handlers = 40;

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start the Web UI on port %d", port);
        server_handle_ = nullptr;
        return false;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = PageHandler, .user_ctx = nullptr},
        {.uri = "/api/timer", .method = HTTP_GET, .handler = TimerHandler, .user_ctx = nullptr},
        {.uri = "/api/timer", .method = HTTP_POST, .handler = TimerSetHandler, .user_ctx = nullptr},
        {.uri = "/api/timer/stop",
         .method = HTTP_POST,
         .handler = TimerStopHandler,
         .user_ctx = nullptr},
        {.uri = "/api/rss", .method = HTTP_GET, .handler = RssHandler, .user_ctx = nullptr},
        {.uri = "/api/rss", .method = HTTP_POST, .handler = RssSaveHandler, .user_ctx = nullptr},
        {.uri = "/api/rss", .method = HTTP_PUT, .handler = RssSaveHandler, .user_ctx = nullptr},
        {.uri = "/api/rss",
         .method = HTTP_DELETE,
         .handler = RssDeleteHandler,
         .user_ctx = nullptr},
        {.uri = "/api/skills", .method = HTTP_GET, .handler = SkillsHandler, .user_ctx = nullptr},
        {.uri = "/api/skills", .method = HTTP_POST, .handler = SkillsHandler, .user_ctx = nullptr},
        {.uri = "/api/system", .method = HTTP_GET, .handler = SystemHandler, .user_ctx = nullptr},
        {.uri = "/api/system", .method = HTTP_POST, .handler = SystemHandler, .user_ctx = nullptr},
        {.uri = "/api/system/reboot", .method = HTTP_POST, .handler = RebootHandler,
         .user_ctx = nullptr},
        {.uri = "/api/system/check", .method = HTTP_GET, .handler = CheckHandler,
         .user_ctx = nullptr},
        {.uri = "/api/eq", .method = HTTP_GET, .handler = EqHandler, .user_ctx = nullptr},
        {.uri = "/api/eq", .method = HTTP_POST, .handler = EqHandler, .user_ctx = nullptr},
    };
    for (const auto& route : routes) {
        AddHandler(route);
    }
    // The radio and computer tabs ship with the device page, so their endpoints
    // are registered here too instead of from every board that has a radio.
    RadioWebApi::Register(*this);
    reminder::ReminderWebApi::Register(*this);
    WolWebApi::Register(*this);
    WebSearchWebApi::Register(*this);

    ESP_LOGI(TAG, "Web UI started on port %d (http://<device-ip>:%d/), build %s %s", port, port,
             __DATE__, __TIME__);
    return true;
}

void WebServer::Stop() {
    if (server_handle_ != nullptr) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        ESP_LOGI(TAG, "Web UI stopped");
    }
}

bool WebServer::AddHandler(const httpd_uri_t& uri) {
    if (server_handle_ == nullptr) {
        ESP_LOGE(TAG, "Cannot register %s before the Web UI starts", uri.uri);
        return false;
    }
    if (httpd_register_uri_handler(server_handle_, &uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler: %s", uri.uri);
        return false;
    }
    return true;
}

esp_err_t WebServer::PageHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandlePage(req);
}

esp_err_t WebServer::TimerHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleTimer(req);
}

esp_err_t WebServer::TimerSetHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleTimerSet(req);
}

esp_err_t WebServer::TimerStopHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleTimerStop(req);
}

esp_err_t WebServer::RssHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleRss(req);
}

esp_err_t WebServer::RssSaveHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleRssSave(req);
}

esp_err_t WebServer::RssDeleteHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleRssDelete(req);
}

esp_err_t WebServer::SkillsHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleSkills(req);
}

esp_err_t WebServer::SystemHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleSystem(req);
}

esp_err_t WebServer::RebootHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleReboot(req);
}

esp_err_t WebServer::CheckHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleCheck(req);
}

esp_err_t WebServer::EqHandler(httpd_req_t* req) {
    if (instance_ == nullptr) {
        return ESP_FAIL;
    }
    return instance_->HandleEq(req);
}

esp_err_t WebServer::HandlePage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // The page is stored gzipped, so let the browser expand it and never cache it:
    // after an OTA the UI must come from the new image, not the old cache.
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, reinterpret_cast<const char*>(kPageHtmlGzStart),
                           kPageHtmlGzEnd - kPageHtmlGzStart);
}

esp_err_t WebServer::HandleTimer(httpd_req_t* req) {
    auto* timer = Application::GetInstance().GetTimer();
    bool running = timer != nullptr && timer->IsRunning();
    int remaining_seconds = running ? timer->GetRemainingSeconds() : 0;
    // The countdown can reach zero between IsRunning() and the remaining query.
    if (remaining_seconds <= 0) {
        running = false;
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    std::string remaining = running ? timer->FormatRemaining() : std::string();
    cJSON_AddBoolToObject(root.get(), "running", running);
    cJSON_AddNumberToObject(root.get(), "remainingSeconds", remaining_seconds);
    cJSON_AddStringToObject(root.get(), "remaining", remaining.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t WebServer::HandleTimerSet(httpd_req_t* req) {
    // httpd_query_key_value wants the query string itself, not the full URI.
    char seconds_text[16] = {0};
    const char* query = strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    if (httpd_query_key_value(query_str, "seconds", seconds_text, sizeof(seconds_text)) != ESP_OK ||
        seconds_text[0] == '\0') {
        return web::SendError(req, 400, "Missing seconds query parameter");
    }

    char* end = nullptr;
    long seconds = strtol(seconds_text, &end, 10);
    if (end == nullptr || *end != '\0' || seconds <= 0 ||
        seconds > static_cast<long>(CountdownTimer::kMaxDurationSeconds)) {
        return web::SendError(req, 400, "Seconds must be between 1 and 86400");
    }

    auto* timer = Application::GetInstance().GetTimer();
    if (timer == nullptr) {
        return web::SendError(req, 500, "Timer unavailable");
    }
    ESP_LOGI(TAG, "Start timer request: %ld s", seconds);
    if (!timer->Start(static_cast<int>(seconds))) {
        return web::SendError(req, 400, "Failed to start timer");
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    std::string remaining = timer->FormatRemaining();
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddNumberToObject(root.get(), "seconds", static_cast<double>(seconds));
    cJSON_AddStringToObject(root.get(), "remaining", remaining.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    // Setting a timer from the Web UI counts as user activity: wake the display
    // so the countdown in the status line becomes visible. CountdownTimer shows
    // its own short confirmation on the subtitle line and clears it by itself.
    Board::GetInstance().WakeUp();
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
}

esp_err_t WebServer::HandleTimerStop(httpd_req_t* req) {
    auto* timer = Application::GetInstance().GetTimer();
    if (timer == nullptr) {
        return web::SendError(req, 500, "Timer unavailable");
    }
    ESP_LOGI(TAG, "Stop timer request");
    if (!timer->Stop()) {
        return web::SendError(req, 409, "No active timer");
    }
    return web::SendJson(req, 200, "{\"ok\":true}");
}

std::string WebServer::FeedsJson() const {
    auto* rss = Application::GetInstance().GetRssReader();
    if (rss == nullptr) {
        return "{}";
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return "{}";
    }
    for (const auto& [key, feed] : rss->GetFeeds()) {
        CJsonUniquePtr item(cJSON_CreateObject());
        CJsonUniquePtr aliases(cJSON_CreateArray());
        if (item == nullptr || aliases == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(item.get(), "key", key.c_str());
        cJSON_AddStringToObject(item.get(), "name", feed.name.c_str());
        cJSON_AddStringToObject(item.get(), "url", feed.url.c_str());
        cJSON_AddStringToObject(item.get(), "category", feed.category.c_str());
        for (const auto& alias : feed.aliases) {
            cJSON_AddItemToArray(aliases.get(), cJSON_CreateString(alias.c_str()));
        }
        cJSON_AddItemToObject(item.get(), "aliases", aliases.release());
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "feeds", array.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t WebServer::HandleRss(httpd_req_t* req) {
    if (Application::GetInstance().GetRssReader() == nullptr) {
        return web::SendError(req, 500, "RSS reader unavailable");
    }
    return web::SendJson(req, 200, FeedsJson());
}

esp_err_t WebServer::HandleRssSave(httpd_req_t* req) {
    auto* rss = Application::GetInstance().GetRssReader();
    if (rss == nullptr) {
        return web::SendError(req, 500, "RSS reader unavailable");
    }
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr) {
        return web::SendError(req, 400, "Invalid JSON body");
    }

    // Import replaces the whole list and is sent as {"feeds": [...]}.
    cJSON* feeds = cJSON_GetObjectItem(root.get(), "feeds");
    if (cJSON_IsArray(feeds)) {
        std::vector<std::pair<std::string, RssFeed>> imported_feeds;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, feeds) {
            RssFeed feed;
            if (!FeedFromJson(item, &feed)) {
                return web::SendError(req, 400,
                                      "Invalid feed entry: name and url are required");
            }
            cJSON* item_key = cJSON_GetObjectItem(item, "key");
            imported_feeds.emplace_back(cJSON_IsString(item_key) ? item_key->valuestring : "",
                                        std::move(feed));
        }
        ESP_LOGI(TAG, "Import RSS feeds request: %d feeds",
                 static_cast<int>(imported_feeds.size()));
        int imported = 0;
        std::string error;
        if (!rss->ReplaceFeeds(imported_feeds, &imported, error)) {
            return web::SendError(req, 400, error);
        }
        CJsonUniquePtr result(cJSON_CreateObject());
        if (result == nullptr) {
            return web::SendError(req, 500, "Out of memory");
        }
        cJSON_AddBoolToObject(result.get(), "ok", true);
        cJSON_AddNumberToObject(result.get(), "imported", imported);
        CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
        return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
    }

    RssFeed feed;
    if (!FeedFromJson(root.get(), &feed)) {
        return web::SendError(req, 400, "Expected JSON with name and url");
    }
    // The device only fetches plain http(s); anything else is a typo in the UI.
    if (feed.name.empty() || feed.name.size() > 64 || feed.url.size() < 8 ||
        feed.url.size() > 256 ||
        (feed.url.rfind("http://", 0) != 0 && feed.url.rfind("https://", 0) != 0)) {
        return web::SendError(req, 400, "Feed needs a name (up to 64 chars) and an http(s) URL");
    }

    // PUT edits an existing feed (key unchanged), POST adds a new one and may
    // carry an explicit key; without a key one is generated from the name.
    cJSON* key_item = cJSON_GetObjectItem(root.get(), "key");
    std::string key = cJSON_IsString(key_item) ? key_item->valuestring : "";
    const bool is_edit = req->method == HTTP_PUT;
    if (is_edit && key.empty()) {
        return web::SendError(req, 400, "Missing key for feed update");
    }
    if (!key.empty() && !RssReader::IsValidKey(key)) {
        return web::SendError(req, 400, "Feed key must be 1-32 chars of A-Z a-z 0-9 _ -");
    }
    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }

    if (is_edit) {
        ESP_LOGI(TAG, "Update RSS feed request: key='%s'", key.c_str());
        if (!rss->UpdateFeed(key, feed)) {
            return web::SendError(req, 404, "Feed not found or failed to save it");
        }
    } else {
        ESP_LOGI(TAG, "Add RSS feed request: key='%s' url='%s'", key.c_str(), feed.url.c_str());
        if (!key.empty() && rss->GetFeed(key, nullptr)) {
            return web::SendError(req, 409, "Feed key already exists");
        }
        std::string new_key;
        if (!rss->AddFeed(feed, key, &new_key)) {
            return web::SendError(req, 409, "Feed list is full or it could not be saved");
        }
        key = std::move(new_key);
    }

    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddStringToObject(result.get(), "key", key.c_str());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
    return web::SendJson(req, is_edit ? 200 : 201, text != nullptr ? text.get() : "{}");
}

esp_err_t WebServer::HandleRssDelete(httpd_req_t* req) {
    auto* rss = Application::GetInstance().GetRssReader();
    if (rss == nullptr) {
        return web::SendError(req, 500, "RSS reader unavailable");
    }
    char key[64] = {0};
    const char* query = strchr(req->uri, '?');
    const char* query_str = (query != nullptr) ? query + 1 : "";
    if (httpd_query_key_value(query_str, "key", key, sizeof(key)) != ESP_OK || key[0] == '\0') {
        return web::SendError(req, 400, "Missing key query parameter");
    }
    ESP_LOGI(TAG, "Delete RSS feed request: key='%s'", key);
    if (!rss->DeleteFeed(key)) {
        return web::SendError(req, 404, "Feed not found or failed to save the list");
    }
    return web::SendJson(req, 200, "{\"ok\":true}");
}

std::string WebServer::SkillsJson() const {
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return "{}";
    }
    for (const auto& tool : McpServer::GetInstance().GetToolInfos()) {
        CJsonUniquePtr item(cJSON_CreateObject());
        if (item == nullptr) {
            continue;
        }
        cJSON_AddStringToObject(item.get(), "name", tool.name.c_str());
        cJSON_AddStringToObject(item.get(), "description", tool.description.c_str());
        cJSON_AddBoolToObject(item.get(), "read_only", tool.read_only);
        cJSON_AddItemToArray(array.get(), item.release());
    }
    cJSON_AddItemToObject(root.get(), "tools", array.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "{}";
}

esp_err_t WebServer::HandleSkills(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        return web::SendJson(req, 200, SkillsJson());
    }
    // POST carries {"read_only":["self.get_device_status", ...]}: the full list,
    // so skills the user unchecked are missing from it.
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* read_only = root != nullptr ? cJSON_GetObjectItem(root.get(), "read_only") : nullptr;
    if (!cJSON_IsArray(read_only)) {
        return web::SendError(req, 400, "Expected a \"read_only\" array");
    }
    std::vector<std::string> names;
    cJSON* name = nullptr;
    cJSON_ArrayForEach(name, read_only) {
        if (cJSON_IsString(name)) {
            names.emplace_back(name->valuestring);
        }
    }
    ESP_LOGI(TAG, "Save read-only skill list: %d skills", static_cast<int>(names.size()));
    if (!McpServer::GetInstance().SetReadOnlyTools(names)) {
        ESP_LOGE(TAG, "Save read-only skill list: failed to persist it to NVS");
        return web::SendError(req, 500, "Failed to save the skill list");
    }
    return web::SendJson(req, 200, "{\"ok\":true}");
}

esp_err_t WebServer::HandleSystem(httpd_req_t* req) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return web::SendError(req, 503, "Audio codec is not available");
    }
    if (req->method == HTTP_GET) {
        CJsonUniquePtr root(cJSON_CreateObject());
        if (root == nullptr) {
            return web::SendError(req, 500, "Out of memory");
        }
        cJSON_AddNumberToObject(root.get(), "volume", codec->output_volume());

        // Status comes from data the device already has for the MCP status tool:
        // the app descriptor and the board status JSON. No new route.
        const esp_app_desc_t* app = esp_app_get_description();
        cJSON* status = cJSON_AddObjectToObject(root.get(), "status");
        if (status != nullptr && app != nullptr) {
            cJSON_AddStringToObject(status, "version", app->version);
            cJSON_AddStringToObject(status, "buildDate", app->date);
            cJSON_AddStringToObject(status, "buildTime", app->time);
            cJSON_AddNumberToObject(status, "uptimeSeconds",
                                    static_cast<double>(esp_timer_get_time()) / 1000000.0);
        }
        // Bytes used on the storage partition, so the page shows how full the
        // files with the radio, RSS and Wake-on-LAN lists are.
        size_t storage_used = 0;
        size_t storage_total = 0;
        if (status != nullptr && list_storage::GetStats(storage_used, storage_total)) {
            cJSON* storage = cJSON_AddObjectToObject(status, "storage");
            if (storage != nullptr) {
                cJSON_AddNumberToObject(storage, "used", static_cast<double>(storage_used));
                cJSON_AddNumberToObject(storage, "total", static_cast<double>(storage_total));
            }
        }
        CJsonUniquePtr device(cJSON_Parse(Board::GetInstance().GetDeviceStatusJson().c_str()));
        if (device != nullptr) {
            cJSON_AddItemToObject(root.get(), "device", device.release());
        }

        CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
        return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
    }

    // POST carries {"volume":0..100}.
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* volume = root != nullptr ? cJSON_GetObjectItem(root.get(), "volume") : nullptr;
    if (!cJSON_IsNumber(volume)) {
        return web::SendError(req, 400, "Expected a \"volume\" number");
    }
    const int value = static_cast<int>(volume->valuedouble);
    if (value < 0 || value > 100) {
        return web::SendError(req, 400, "Volume must be between 0 and 100");
    }
    ESP_LOGI(TAG, "Set speaker volume: %d", value);
    codec->SetOutputVolume(value);

    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(result.get(), "ok", true);
    cJSON_AddNumberToObject(result.get(), "volume", codec->output_volume());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{\"ok\":true}");
}

esp_err_t WebServer::HandleReboot(httpd_req_t* req) {
    ESP_LOGW(TAG, "Reboot requested from the Web UI");
    // Reply first: the reboot itself runs from the main task a moment later.
    esp_err_t err = web::SendJson(req, 200, "{\"ok\":true}");
    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        vTaskDelay(pdMS_TO_TICKS(1000));
        app.Reboot();
    });
    return err;
}

esp_err_t WebServer::HandleCheck(httpd_req_t* req) {
    const char* query = std::strchr(req->uri, '?');
    char service[16] = {0};
    if (query == nullptr ||
        httpd_query_key_value(query + 1, "service", service, sizeof(service)) != ESP_OK) {
        return web::SendError(req, 400, "Missing \"service\" query parameter");
    }
    std::string error;
    if (std::strcmp(service, "joke") == 0) {
        std::string joke;
        if (!JokeReader::Fetch(joke, error)) {
            ESP_LOGW(TAG, "Joke service is unavailable: %s", error.c_str());
            return web::SendError(req, 502, error);
        }
    } else if (std::strcmp(service, "currency") == 0) {
        CurrencyConverter::Rate rate;
        if (!CurrencyConverter::Convert("USD", "RUB", 1, rate, error)) {
            ESP_LOGW(TAG, "Currency service is unavailable: %s", error.c_str());
            return web::SendError(req, 502, error);
        }
    } else {
        return web::SendError(req, 400, "Unknown service");
    }
    ESP_LOGI(TAG, "Service check passed: %s", service);

    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(root.get(), "ok", true);
    cJSON_AddStringToObject(root.get(), "service", service);
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{\"ok\":true}");
}

esp_err_t WebServer::HandleEq(httpd_req_t* req) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return web::SendError(req, 503, "Audio codec is not available");
    }

    if (req->method == HTTP_GET) {
        CJsonUniquePtr root(cJSON_CreateObject());
        if (root == nullptr) {
            return web::SendError(req, 500, "Out of memory");
        }
        cJSON* bands = cJSON_AddArrayToObject(root.get(), "bands_hz");
        cJSON* gains_json = cJSON_AddArrayToObject(root.get(), "gains_db");
        if (bands == nullptr || gains_json == nullptr) {
            return web::SendError(req, 500, "Out of memory");
        }
        const std::vector<int> gains = codec->GetEqGains();
        for (int i = 0; i < AudioCodec::eq_band_count(); ++i) {
            cJSON_AddItemToArray(bands, cJSON_CreateNumber(AudioCodec::eq_band_freq(i)));
            const int gain = i < static_cast<int>(gains.size()) ? gains[i] : 0;
            cJSON_AddItemToArray(gains_json, cJSON_CreateNumber(gain));
        }
        CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
        return web::SendJson(req, 200, text != nullptr ? text.get() : "{}");
    }

    // POST carries {"gains":[-15..15, ...]} with one value per band, in the order
    // the getter above reports. The codec applies them to the output path and
    // keeps them in NVS, so the page and the device never disagree after a boot.
    std::string body;
    if (!web::ReadJsonBody(req, body)) {
        return ESP_OK;
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    cJSON* gains_json = root != nullptr ? cJSON_GetObjectItem(root.get(), "gains") : nullptr;
    if (!cJSON_IsArray(gains_json)) {
        return web::SendError(req, 400, "Expected a \"gains\" array");
    }
    if (cJSON_GetArraySize(gains_json) != AudioCodec::eq_band_count()) {
        return web::SendError(req, 400, "Expected one gain per band");
    }
    std::vector<int> gains;
    gains.reserve(AudioCodec::eq_band_count());
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, gains_json) {
        if (!cJSON_IsNumber(item)) {
            return web::SendError(req, 400, "Every gain must be a number");
        }
        const int gain = static_cast<int>(item->valuedouble);
        if (gain < -15 || gain > 15) {
            return web::SendError(req, 400, "Gain must be between -15 and 15 dB");
        }
        gains.push_back(gain);
    }
    ESP_LOGI(TAG, "Equalizer updated from the Web UI");
    codec->SetEqGains(gains);

    CJsonUniquePtr result(cJSON_CreateObject());
    if (result == nullptr) {
        return web::SendError(req, 500, "Out of memory");
    }
    cJSON_AddBoolToObject(result.get(), "ok", true);
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(result.get()));
    return web::SendJson(req, 200, text != nullptr ? text.get() : "{\"ok\":true}");
}
