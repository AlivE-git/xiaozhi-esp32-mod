/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <optional>

#include "application.h"
#include "board.h"
#include "display.h"
#include "features/currency/currency_converter.h"
#include "features/joke/joke_reader.h"
#include "features/music/esp32_radio.h"
#include "features/rss/rss_reader.h"
#include "features/timer/countdown_timer.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "MCP"

namespace {

// The read-only skill list is a user setting: the Web UI "Система" tab writes it and
// it decides whether a finished skill ends the voice session.
constexpr const char* kReadOnlyNamespace = "mcp";
constexpr const char* kReadOnlyKey = "read_only";

// Used until the user saves the list from the Web UI.
const char* const kDefaultReadOnlyTools[] = {"self.get_device_status", "self.radio.get_stations",
                                            "self.timer.get_remaining", "self.rss.list_feeds"};

std::set<std::string> DefaultReadOnlyTools() {
    std::set<std::string> names;
    for (const char* name : kDefaultReadOnlyTools) {
        names.emplace(name);
    }
    return names;
}

// Stored as {"tools":[...]}. An empty list still has to be distinguishable from
// an unset key, otherwise unchecking every skill would bring the defaults back.
bool ParseReadOnlyTools(const std::string& saved, std::set<std::string>* names) {
    CJsonUniquePtr root(cJSON_Parse(saved.c_str()));
    if (root == nullptr) {
        return false;
    }
    cJSON* tools = cJSON_GetObjectItem(root.get(), "tools");
    if (!cJSON_IsArray(tools)) {
        return false;
    }
    cJSON* tool = nullptr;
    cJSON_ArrayForEach(tool, tools) {
        if (cJSON_IsString(tool)) {
            names->emplace(tool->valuestring);
        }
    }
    return true;
}

std::string ReadOnlyToolsJson(const std::set<std::string>& names) {
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr tools(cJSON_CreateArray());
    if (root == nullptr || tools == nullptr) {
        return "";
    }
    for (const auto& name : names) {
        cJSON_AddItemToArray(tools.get(), cJSON_CreateString(name.c_str()));
    }
    cJSON_AddItemToObject(root.get(), "tools", tools.release());
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    return text != nullptr ? text.get() : "";
}

}  // namespace

McpServer::McpServer() { LoadReadOnlyTools(); }

void McpServer::LoadReadOnlyTools() {
    Settings settings(kReadOnlyNamespace);
    const std::string saved = settings.GetString(kReadOnlyKey);
    std::set<std::string> names;
    if (saved.empty() || !ParseReadOnlyTools(saved, &names)) {
        if (!saved.empty()) {
            ESP_LOGW(TAG, "Saved read-only skill list is not valid JSON, using defaults");
        }
        names = DefaultReadOnlyTools();
    }
    std::lock_guard<std::mutex> lock(read_only_mutex_);
    read_only_tools_ = std::move(names);
    ESP_LOGI(TAG, "Read-only skills: %d", static_cast<int>(read_only_tools_.size()));
}

std::vector<McpServer::ToolInfo> McpServer::GetToolInfos() const {
    std::lock_guard<std::mutex> lock(read_only_mutex_);
    std::vector<ToolInfo> infos;
    infos.reserve(tools_.size());
    for (const auto& tool : tools_) {
        infos.push_back(
            ToolInfo{tool->name(), tool->description(), read_only_tools_.count(tool->name()) > 0});
    }
    return infos;
}

bool McpServer::IsReadOnlyTool(const std::string& name) const {
    std::lock_guard<std::mutex> lock(read_only_mutex_);
    return read_only_tools_.count(name) > 0;
}

bool McpServer::SetReadOnlyTools(const std::vector<std::string>& names) {
    std::set<std::string> next;
    for (const auto& name : names) {
        for (const auto& tool : tools_) {
            if (tool->name() == name) {
                next.emplace(name);
                break;
            }
        }
    }
    const std::string json = ReadOnlyToolsJson(next);
    // Writing needs the namespace opened read-write; a read-only handle makes
    // SetString fail.
    Settings settings(kReadOnlyNamespace, true);
    if (json.empty() || !settings.SetString(kReadOnlyKey, json)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(read_only_mutex_);
    read_only_tools_ = std::move(next);
    ESP_LOGI(TAG, "Saved %d read-only skills", static_cast<int>(read_only_tools_.size()));
    return true;
}

McpServer::~McpServer() = default;

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the "
            "audio speaker, screen, battery, network, etc.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of "
            "the audio speaker?)\n"
            "2. As the first step to control the device (e.g. turn up / down the volume of the "
            "audio speaker, etc.)",
            PropertyList(), [&board](const PropertyList& properties) -> ReturnValue {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker. If the current volume is unknown, you must call "
            "`self.get_device_status` tool first and then call this tool.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList& properties) -> ReturnValue {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness", "Set the brightness of the screen.",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList& properties) -> ReturnValue {
                    uint8_t brightness =
                        static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
                "Set the theme of the screen. The theme can be `light` or `dark`.",
                PropertyList({Property("theme", kPropertyTypeString)}),
                [display](const PropertyList& properties) -> ReturnValue {
                    auto theme_name = properties["theme"].value<std::string>();
                    auto& theme_manager = LvglThemeManager::GetInstance();
                    auto theme = theme_manager.GetTheme(theme_name);
                    if (theme != nullptr) {
                        display->SetTheme(theme);
                        return true;
                    }
                    return false;
                });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
                "Always remember you have a camera. If the user asks you to see something, use "
                "this tool to take a photo and then explain it.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [camera](const PropertyList& properties) -> ToolResult {
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture()) {
                        return std::unexpected("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    auto result = camera->Explain(question);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    return std::move(*result);
                });
    }
#endif

    // Internet radio control
    auto radio = Application::GetInstance().GetRadio();
    if (radio) {
        AddTool("self.radio.play_station",
                "Play a radio station by name. Station names are in Russian; names/keywords are "
                "matched case-insensitively (partial match also works). Use "
                "self.radio.get_stations to list all stations.",
                PropertyList({Property("station_name", kPropertyTypeString)}),
                [radio](const PropertyList& properties) -> ReturnValue {
                    auto station_name = properties["station_name"].value<std::string>();
                    if (radio->PlayStation(station_name)) {
                        return "Playing radio station: " + station_name;
                    }
                    return std::string(
                        "Ошибка: станция не найдена. Используйте self.radio.get_stations для "
                        "списка станций.");
                });
        AddTool("self.radio.play_url",
                "Play a custom radio stream by direct URL (http/https mp3 or aac stream).",
                PropertyList({Property("url", kPropertyTypeString)}),
                [radio](const PropertyList& properties) -> ReturnValue {
                    auto url = properties["url"].value<std::string>();
                    if (radio->PlayUrl(url, "")) {
                        return "Playing radio url: " + url;
                    }
                    return std::string("Ошибка: не удалось начать воспроизведение URL");
                });
        AddTool("self.radio.stop", "Stop the currently playing radio stream.", PropertyList(),
                [radio](const PropertyList& properties) -> ReturnValue {
                    radio->Stop();
                    return std::string("Stopping radio");
                });
        AddTool("self.radio.get_stations",
                "List all available radio stations with their keys and display names.",
                PropertyList(), [radio](const PropertyList& properties) -> ReturnValue {
                    std::string response = "Available radio stations:\n";
                    for (const auto& s : radio->GetStationList()) {
                        response += "- " + s + "\n";
                    }
                    return response;
                });
    }

    // Countdown timer control
    auto timer = Application::GetInstance().GetTimer();
    if (timer) {
        AddTool("self.timer.set",
                "Set a countdown timer. Convert the requested time into seconds: 1 minute = 60, "
                "5 minutes = 300, 1 hour = 3600. The remaining time is shown on the device "
                "display and the device rings when the timer ends. Use self.timer.stop to cancel "
                "it and self.timer.get_remaining to check how much time is left.",
                PropertyList({Property("duration_seconds", kPropertyTypeInteger, 1, 86400)}),
                [timer](const PropertyList& properties) -> ReturnValue {
                    int seconds = properties["duration_seconds"].value<int>();
                    if (!timer->Start(seconds)) {
                        return std::string("Ошибка: не удалось установить таймер");
                    }
                    return "Таймер установлен на " + timer->FormatRemaining();
                });
        AddTool("self.timer.stop", "Stop and cancel the running countdown timer.", PropertyList(),
                [timer](const PropertyList& properties) -> ReturnValue {
                    if (timer->Stop()) {
                        return std::string("Таймер остановлен");
                    }
                    return std::string("Активного таймера нет");
                });
        AddTool("self.timer.get_remaining",
                "Get how much time is left on the countdown timer. Returns the remaining time "
                "as MM:SS plus the number of seconds.",
                PropertyList(), [timer](const PropertyList& properties) -> ReturnValue {
                    if (!timer->IsRunning()) {
                        return std::string("Активного таймера нет");
                    }
                    return "Осталось " + timer->FormatRemaining() + " (" +
                           std::to_string(timer->GetRemainingSeconds()) + " секунд)";
                });
    }

    // RSS news reader
    auto rss = Application::GetInstance().GetRssReader();
    if (rss) {
        AddTool("self.rss.list_feeds",
                "List the RSS news feeds configured on the device. Use it when the user asks "
                "which news sources are available.",
                PropertyList(), [rss](const PropertyList& properties) -> ReturnValue {
                    auto feeds = rss->GetFeeds();
                    if (feeds.empty()) {
                        return std::string(
                            "RSS-ленты не настроены. Их можно добавить в веб-интерфейсе устройства.");
                    }
                    std::string response = "RSS-ленты устройства:\n";
                    for (const auto& [key, feed] : feeds) {
                        response += "- " + feed.name;
                        if (!feed.category.empty()) {
                            response += " [" + feed.category + "]";
                        }
                        if (!feed.aliases.empty()) {
                            response += " (" + feed.aliases.front() + ")";
                        }
                        response += ": " + feed.url + "\n";
                    }
                    return response;
                });
        AddTool("self.rss.read_latest",
                "Read the latest news headlines from an RSS feed aloud. Fetches the feed from "
                "the internet right now, so relay the returned headlines to the user. `feed` is "
                "the feed name, alias or category as spoken by the user; leave it empty to use "
                "the default feed. "
                "`count` is how many headlines to read (1-10).",
                PropertyList({Property("feed", kPropertyTypeString, std::string("")),
                              Property("count", kPropertyTypeInteger, 5, 1, 10)}),
                [rss](const PropertyList& properties) -> ReturnValue {
                    auto feed = properties["feed"].value<std::string>();
                    int count = properties["count"].value<int>();
                    return rss->ReadHeadlines(feed, count);
                });
    }

    // Random joke reader
    AddTool("self.joke.tell",
            "Tell a random Russian joke aloud. Fetches a fresh joke from the internet right "
            "now, so read the returned text to the user as it is. Call it again when the user "
            "asks for another joke.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                return JokeReader::Tell();
            });

    // Currency converter
    AddTool("self.currency.convert",
            "Convert money between currencies using current daily exchange rates. Use it for "
            "exchange-rate questions and conversions, e.g. \"курс доллара\", \"курс евро к "
            "тенге\" or \"100 тенге в рублях\". Pass ISO 4217 codes: рубль=RUB, доллар=USD, "
            "тенге=KZT, евро=EUR, юань=CNY. For a plain rate question keep `amount` at 1; `to` "
            "defaults to RUB, so pass it explicitly for any other target currency. Read the "
            "returned text to the user.",
            PropertyList({Property("from", kPropertyTypeString),
                          Property("to", kPropertyTypeString, std::string("RUB")),
                          Property("amount", kPropertyTypeInteger, 1, 1, 1000000000)}),
            [](const PropertyList& properties) -> ReturnValue {
                CurrencyConverter::Rate rate;
                std::string error;
                if (!CurrencyConverter::Convert(properties["from"].value<std::string>(),
                                                properties["to"].value<std::string>(),
                                                properties["amount"].value<int>(), rate, error)) {
                    return std::string("Ошибка: ") + error;
                }
                return CurrencyConverter::Describe(rate);
            });

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), std::make_move_iterator(original_tools.begin()),
                  std::make_move_iterator(original_tools.end()));
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info", "Get the system information", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& board = Board::GetInstance();
                        return board.GetSystemInfoJson();
                    });

    AddUserOnlyTool("self.reboot", "Reboot the system", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& app = Application::GetInstance();
                        app.Schedule([&app]() {
                            ESP_LOGW(TAG, "User requested reboot");
                            vTaskDelay(pdMS_TO_TICKS(1000));

                            app.Reboot();
                        });
                        return true;
                    });

    // Firmware upgrade
    AddUserOnlyTool(
        "self.upgrade_firmware",
        "Upgrade firmware from a specific URL. This will download and install the firmware, then "
        "reboot the device.",
        PropertyList({Property("url", kPropertyTypeString,
                               "The URL of the firmware binary file to download and install")}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());

            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });

            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && display->SupportsGuiOperations()) {
        AddUserOnlyTool("self.screen.get_info",
                        "Information about the screen, including width, height, etc.",
                        PropertyList(), [display](const PropertyList& properties) -> ReturnValue {
                            cJSON* json = cJSON_CreateObject();
                            cJSON_AddNumberToObject(json, "width", display->width());
                            cJSON_AddNumberToObject(json, "height", display->height());
                            cJSON_AddBoolToObject(json, "monochrome", display->IsMonochrome());
                            return json;
                        });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool(
            "self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("quality", kPropertyTypeInteger, 80, 1, 100)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    return std::unexpected("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());

                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (auto opened = http->Open("POST", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                auto write_or_fail = [&http](const char* data,
                                             size_t size) -> std::optional<std::string> {
                    if (auto written = http->Write(data, size); !written) {
                        return written.error().ToString();
                    }
                    return std::nullopt;
                };
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header +=
                        "Content-Disposition: form-data; name=\"file\"; "
                        "filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    if (auto error = write_or_fail(file_header.c_str(), file_header.size());
                        error) {
                        http->Close();
                        return std::unexpected("Failed to upload screenshot: " + *error);
                    }
                }

                // JPEG数据
                if (auto error = write_or_fail((const char*)jpeg_data.data(), jpeg_data.size());
                    error) {
                    http->Close();
                    return std::unexpected("Failed to upload screenshot: " + *error);
                }

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    if (auto error =
                            write_or_fail(multipart_footer.c_str(), multipart_footer.size());
                        error) {
                        http->Close();
                        return std::unexpected("Failed to upload screenshot: " + *error);
                    }
                }
                if (auto error = write_or_fail("", 0); error) {
                    http->Close();
                    return std::unexpected("Failed to upload screenshot: " + *error);
                }

                auto upload_status = http->GetStatusCode();
                if (!upload_status) {
                    return std::unexpected(upload_status.error().ToString());
                }
                if (*upload_status != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*upload_status));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });

        AddUserOnlyTool(
            "self.screen.preview_image", "Preview an image on the screen",
            PropertyList({Property("url", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (auto opened = http->Open("GET", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                auto status_code = http->GetStatusCode();
                if (!status_code) {
                    return std::unexpected(status_code.error().ToString());
                }
                if (*status_code != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*status_code));
                }

                size_t content_length = http->GetBodyLength();
                using BufferPtr = std::unique_ptr<char, decltype(&heap_caps_free)>;
                BufferPtr data(
                    static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT)),
                    heap_caps_free);
                if (data == nullptr) {
                    return std::unexpected("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    auto ret = http->Read(data.get() + total_read, content_length - total_read);
                    if (!ret) {
                        return std::unexpected("Failed to download image: " + url);
                    }
                    if (*ret == 0) {
                        break;
                    }
                    total_read += *ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data.release(), total_read);
                if (!image->IsValid()) {
                    return std::unexpected("Downloaded image is invalid: " + url);
                }
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif  // CONFIG_LV_USE_SNAPSHOT
    }
#endif  // HAVE_LVGL

    // Assets download url (always registered — Settings storage works regardless of partition
    // layout)
    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [](const PropertyList& properties) -> ReturnValue {
                        auto url = properties["url"].value<std::string>();
                        Settings settings("assets", true);
                        settings.SetString("download_url", url);
                        return true;
                    });
}

void McpServer::LogToolInventory() const {
    size_t user_only = 0;
    for (const auto& tool : tools_) {
        if (tool->user_only()) {
            ++user_only;
        }
    }
    ESP_LOGI(TAG, "%u tools registered: %u visible to the model, %u user-only",
             (unsigned)tools_.size(), (unsigned)(tools_.size() - user_only), (unsigned)user_only);
}

void McpServer::AddTool(std::unique_ptr<McpTool> tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [&tool](const auto& existing) {
            return existing->name() == tool->name();
        }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(std::move(tool));
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, std::move(callback)));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties, ToolCallback callback) {
    auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
    tool->set_user_only(true);
    AddTool(std::move(tool));
}

void McpServer::ParseMessage(const std::string& message, ResponseSender response_sender) {
    CJsonUniquePtr json(cJSON_Parse(message.c_str()));
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json.get(), std::move(response_sender));
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }

    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{"
            "\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message, response_sender);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools, response_sender);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, -32602, "Missing params", response_sender);
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, -32602, "Missing tool name", response_sender);
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, -32602, "Invalid arguments: expected object", response_sender);
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments,
                   std::move(response_sender));
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, -32601, "Method not implemented: " + method_str, response_sender);
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) {
        response_sender(payload);
    } else {
        Application::GetInstance().SendMcpMessage(payload);
    }
}

void McpServer::ReplyResult(int id, const std::string& result,
                            const ResponseSender& response_sender) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, int code, const std::string& message,
                           const ResponseSender& response_sender) {
    CJsonUniquePtr json_message(cJSON_CreateString(message.c_str()));
    if (json_message == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate MCP error message");
        return;
    }
    CJsonStringUniquePtr encoded_message(cJSON_PrintUnformatted(json_message.get()));
    if (encoded_message == nullptr) {
        ESP_LOGE(TAG, "Failed to serialize MCP error message");
        return;
    }

    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"code\":";
    payload += std::to_string(code);
    payload += ",\"message\":";
    payload += encoded_message.get();
    payload += "}}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(int id, const std::string& message,
                           const ResponseSender& response_sender) {
    ReplyError(id, -32603, message, response_sender);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools,
                             const ResponseSender& response_sender) {
    const int max_payload_size = 20000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }

        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',') {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit",
                 next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit",
                   response_sender);
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json, response_sender);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments,
                           ResponseSender response_sender) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), [&tool_name](const auto& tool) {
        return tool->name() == tool_name;
    });

    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, -32602, "Unknown tool: " + tool_name, response_sender);
        return;
    }

    McpTool* tool = tool_iter->get();
    PropertyList arguments = tool->properties();
    for (auto& argument : arguments) {
        bool found = false;
        std::expected<void, std::string> validation;
        if (cJSON_IsObject(tool_arguments)) {
            auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                validation = argument.set_value<bool>(value->valueint == 1);
                found = true;
            } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                validation = argument.set_value<int>(value->valueint);
                found = true;
            } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                validation = argument.set_value<std::string>(value->valuestring);
                found = true;
            } else if (value != nullptr) {
                ESP_LOGE(TAG, "tools/call: Invalid type for argument: %s", argument.name().c_str());
                ReplyError(id, -32602, "Invalid type for argument: " + argument.name(),
                           response_sender);
                return;
            }
        }

        if (found && !validation) {
            ESP_LOGE(TAG, "tools/call: %s", validation.error().c_str());
            ReplyError(id, -32602, validation.error(), response_sender);
            return;
        }

        if (!argument.has_default_value() && !found) {
            ESP_LOGE(TAG, "tools/call: Missing required argument: %s", argument.name().c_str());
            ReplyError(id, -32602, "Missing required argument: " + argument.name(),
                       response_sender);
            return;
        }
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool, arguments = std::move(arguments),
                  response_sender = std::move(response_sender)]() {
        auto result = tool->Call(arguments);
        if (!result) {
            ESP_LOGE(TAG, "tools/call: %s", result.error().c_str());
            ReplyError(id, result.error(), response_sender);
            return;
        }
        // An action skill owns the voice session: once its result has been spoken,
        // the device goes to standby instead of reopening the mic in listening
        // mode. Read-only queries keep the conversation going. The flag is
        // consumed by the TTS "stop" handler in Application.
        auto& application = Application::GetInstance();
        auto state = application.GetDeviceState();
        if (!this->IsReadOnlyTool(tool->name()) &&
            (state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
            application.RequestIdleAfterSpeaking();
        }
        ReplyResult(id, *result, response_sender);
    });
}
