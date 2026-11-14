/*
 * MCP Server Implementation
 * Revisions: 2024-11-05 (legacy) and 2026-07-28 (per-request metadata)
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>

#include "application.h"
#include "board.h"
#include "display.h"
#include "features/currency/currency_converter.h"
#include "features/joke/joke_reader.h"
#include "features/reminder/reminder_store.h"
#include "features/radio/esp32_radio.h"
#include "features/rss/rss_reader.h"
#include "features/timer/countdown_timer.h"
#include "features/wol/wol.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "MCP"

namespace {

// The read-only skill list is a user setting: the Web UI "Навыки" tab writes it and
// it decides whether a finished skill ends the voice session.
constexpr const char* kReadOnlyNamespace = "mcp";
constexpr const char* kReadOnlyKey = "read_only";

// Used until the user saves the list from the Web UI.
const char* const kDefaultReadOnlyTools[] = {"self.radio.list_stations",
                                            "self.timer.get_remaining",
                                            "self.pc.list_computers"};

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
            "audio speaker, screen, battery, network, and why the device last restarted.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of "
            "the audio speaker?)\n"
            "2. Answering why the device rebooted or restarted - whether it was powered on, "
            "reset by its button, or crashed. This is about the device, not about the "
            "conversation.\n"
            "3. As the first step to control the device (e.g. turn up / down the volume of the "
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
                [](const PropertyList& properties, const McpRequestContext& context) -> ToolResult {
                    auto camera = Board::GetInstance().GetCamera();
                    if (camera == nullptr) {
                        return std::unexpected("Camera unavailable");
                    }
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture()) {
                        return std::unexpected("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    auto result = context.vision ? camera->Explain(question, context.vision->url,
                                                                   context.vision->token)
                                                 : camera->Explain(question);
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
                "Play a radio station by name or genre. Names and keywords are matched "
                "case-insensitively (partial match also works); a genre such as «джаз» plays a "
                "random station of that genre. Pass an empty `station_name` to play a random "
                "station. Use self.radio.list_stations to list all stations.",
                PropertyList({Property("station_name", kPropertyTypeString)}),
                [radio](const PropertyList& properties) -> ReturnValue {
                    auto station_name = properties["station_name"].value<std::string>();
                    if (radio->PlayStation(station_name)) {
                        const std::string played = radio->GetCurrentStation();
                        return "Playing radio station: " + (played.empty() ? station_name : played);
                    }
                    return std::string(
                        "Ошибка: станция не найдена. Используйте self.radio.list_stations для "
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
        AddTool("self.radio.list_stations",
                "List all available radio stations by name, with their keywords in brackets.",
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
                "Read the latest news headlines from one RSS feed aloud. Fetches the feed from "
                "the internet right now, so relay the returned headlines to the user. `feed` is "
                "required: the feed name, alias or category as spoken by the user; a category "
                "with several feeds reads a random one of them. There is no default feed, so "
                "when the user did not name one, call self.rss.list_feeds first and ask which "
                "feed to read. `count` is how many headlines to read (1-10).",
                PropertyList({Property("feed", kPropertyTypeString),
                              Property("count", kPropertyTypeInteger, 5, 1, 10)}),
                [rss](const PropertyList& properties) -> ReturnValue {
                    auto feed = properties["feed"].value<std::string>();
                    int count = properties["count"].value<int>();
                    return rss->ReadHeadlines(feed, count);
                });
    }

    // Reminders kept on the device, independent of the network.
    AddTool("self.reminder.add",
            "Set a reminder on the device itself: at that moment the device plays a sound "
            "and shows `text` on the screen. Use it whenever the user says \"напомни\" with "
            "their own wording, e.g. \"напомни через 10 минут выключить машинку\" or "
            "\"напомни в 22:40 позвонить маме\". `text` is the short action to remind about, "
            "in the user's words and without the time. Pass `minutes` when the user speaks "
            "in a delay (\"через 10 минут\", \"через полтора часа\" = 90) and `at` as "
            "\"HH:MM\" when they name a clock time. Pass `date` when the user names another "
            "day: \"today\", \"tomorrow\", \"послезавтра\", \"13.11\" or \"2026-11-13\"; "
            "with a date but no time the reminder is set for 09:00. Pass `minutes` alone, or "
            "`date` and `at` together. This does not create a Google Calendar event, so "
            "confirm the wording and the time to the user instead of asking for anything "
            "else.",
            PropertyList({Property("text", kPropertyTypeString),
                          Property("minutes", kPropertyTypeInteger, 0, 0, 10080),
                          Property("at", kPropertyTypeString, std::string("")),
                          Property("date", kPropertyTypeString, std::string(""))}),
            [](const PropertyList& properties) -> ReturnValue {
                const std::string text = properties["text"].value<std::string>();
                const std::string at = properties["at"].value<std::string>();
                const std::string date = properties["date"].value<std::string>();
                const int minutes = properties["minutes"].value<int>();
                reminder::Item added;
                std::string error;
                if (!at.empty() || !date.empty()) {
                    if (!reminder::AddAt(date, at, text, added, error)) {
                        return std::string("Ошибка: ") + error + ".";
                    }
                } else if (!reminder::AddIn(minutes > 0 ? minutes : 10, text, added,
                                            error)) {
                    return std::string("Ошибка: ") + error + ".";
                }
                return reminder::DescribeItem(added);
            });

    AddTool("self.reminder.list",
            "Read the reminders that are still waiting on the device to the user, soonest "
            "first. Use it when the user asks what they asked to be reminded about or asks "
            "to read the reminder list aloud; the returned text is meant to be spoken.",
            PropertyList({Property("max_items", kPropertyTypeInteger, 5, 1, 10)}),
            [](const PropertyList& properties) -> ReturnValue {
                return reminder::Describe(properties["max_items"].value<int>());
            });

    AddTool("self.reminder.remove",
            "Cancel reminders stored on the device. Use it when the user says \"отмени "
            "напоминание\", \"убери напоминание про машину\" or \"убери все напоминания\". "
            "Pass `match` with the words the user used to name the reminder; only the first "
            "matching reminder is cancelled, so repeat which one was removed. Set `all` to "
            "true only when the user clearly wants every reminder gone.",
            PropertyList({Property("match", kPropertyTypeString, std::string("")),
                          Property("all", kPropertyTypeBoolean, false)}),
            [](const PropertyList& properties) -> ReturnValue {
                if (properties["all"].value<bool>()) {
                    const int count = reminder::RemoveAll();
                    if (count == 0) {
                        return std::string("Напоминаний не было.");
                    }
                    return "Убрал все напоминания: " + std::to_string(count) + ".";
                }
                const std::string match = properties["match"].value<std::string>();
                reminder::Item removed;
                std::string error;
                if (!reminder::RemoveMatching(match, removed, error)) {
                    return std::string("Ошибка: ") + error + ".";
                }
                return "Убрал напоминание: " + reminder::WhenText(removed.due) + " — " +
                       removed.text + ".";
            });

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

    // Wake-on-LAN: the computer is off, so only its saved MAC address is needed.
    PropertyList wake_properties;
    wake_properties.AddProperty(Property("name", kPropertyTypeString, std::string("")));
    AddTool("self.pc.wake",
            "Turn on a computer in the local network with Wake-on-LAN. Use it when the user "
            "asks to turn on, wake up or start a computer. The computers are configured in the "
            "Web UI; pass the name of the one the user asked for in `name`, or leave `name` "
            "empty when the user did not name one: that wakes the computer marked as default.",
            wake_properties, [](const PropertyList& properties) -> ReturnValue {
                wol::Computer computer;
                std::string error;
                if (!wol::Wake(properties["name"].value<std::string>(), computer, error)) {
                    ESP_LOGW(TAG, "Wake-on-LAN failed: %s", error.c_str());
                    return std::string("Ошибка: ") + error;
                }
                return std::string("Магический пакет отправлен, включается «") + computer.name +
                       "».";
            });

    AddTool("self.pc.list_computers",
            "List the computers saved for Wake-on-LAN with their names. Use it when the user "
            "asks which computers can be turned on or what their names are.",
            PropertyList(), [](const PropertyList& properties) -> ReturnValue {
                const std::vector<wol::Computer> computers = wol::LoadComputers();
                if (computers.empty()) {
                    return std::string(
                        "Список компьютеров пуст: добавьте компьютер в веб-интерфейсе на "
                        "вкладке Wake-on-LAN.");
                }
                std::string text = "Сохранены компьютеры: ";
                for (size_t i = 0; i < computers.size(); ++i) {
                    if (i > 0) {
                        text += ", ";
                    }
                    text += computers[i].name;
                    if (computers[i].is_default) {
                        text += " (по умолчанию)";
                    }
                }
                text += ". Назовите нужный, чтобы включить его.";
                return text;
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

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ContextToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties, ToolCallback callback) {
    auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
    tool->set_user_only(true);
    AddTool(std::move(tool));
}

namespace {
constexpr const char* kProtocolVersion = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* kVisionExtension = "me.xiaozhi/vision";
constexpr const char* kModernVersion = "2026-07-28";
constexpr const char* kSupportedVersions = R"(["2026-07-28","2024-11-05"])";

std::string JsonString(const char* value) {
    CJsonUniquePtr string(cJSON_CreateString(value));
    CJsonStringUniquePtr encoded(string ? cJSON_PrintUnformatted(string.get()) : nullptr);
    return encoded ? std::string(encoded.get()) : std::string();
}

void AppendServerInfo(std::string& output) {
    output += "{\"name\":";
    output += JsonString(BOARD_NAME);
    output += ",\"version\":";
    output += JsonString(esp_app_get_description()->version);
    output += "}";
}

std::string ResultPrefix(const McpRequestContext& context) {
    std::string prefix = "{\"jsonrpc\":\"2.0\",\"id\":";
    prefix += context.id_json;
    prefix += ",\"result\":";
    if (context.modern) {
        prefix += "{\"resultType\":\"complete\",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":";
        AppendServerInfo(prefix);
        prefix += "}}";
    }
    return prefix;
}

std::optional<McpRequestContext::VisionConfig> ReadVisionConfig(const cJSON* vision) {
    auto url = cJSON_GetObjectItemCaseSensitive(vision, "url");
    auto token = cJSON_GetObjectItemCaseSensitive(vision, "token");
    if (!cJSON_IsString(url)) {
        return std::nullopt;
    }
    return McpRequestContext::VisionConfig{url->valuestring,
                                           cJSON_IsString(token) ? token->valuestring : ""};
}

bool ValidVisionConfig(const McpRequestContext::VisionConfig& vision) {
    const auto& url = vision.url;
    const auto scheme_size = url.starts_with("https://")  ? 8u
                             : url.starts_with("http://") ? 7u
                                                          : 0u;
    if (scheme_size == 0 || url.size() <= scheme_size || url[scheme_size] == '/' ||
        url[scheme_size] == '?' || url[scheme_size] == '#') {
        return false;
    }
    return std::none_of(url.begin(), url.end(),
                        [](unsigned char c) { return std::isspace(c) || std::iscntrl(c); }) &&
           std::none_of(vision.token.begin(), vision.token.end(),
                        [](unsigned char c) { return c == '\r' || c == '\n'; });
}
}  // namespace

void McpServer::ParseMessage(const std::string& message, ResponseSender response_sender) {
    CJsonUniquePtr json(cJSON_ParseWithOpts(message.c_str(), nullptr, true));
    if (json == nullptr) {
        // Requests may contain credentials. Never log the original message.
        ReplyError({}, -32700, "Parse error", response_sender);
        return;
    }
    ParseMessage(json.get(), std::move(response_sender));
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto config = ReadVisionConfig(cJSON_GetObjectItem(capabilities, "vision"));
    if (config) {
        if (auto camera = Board::GetInstance().GetCamera()) {
            camera->SetExplainUrl(config->url, config->token);
        }
    }
}

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    McpRequestContext context;
    auto id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (cJSON_IsString(id)) {
        context.id_json = JsonString(id->valuestring);
    } else if (cJSON_IsNumber(id) && std::isfinite(id->valuedouble) &&
               std::trunc(id->valuedouble) == id->valuedouble &&
               std::abs(id->valuedouble) <= 9007199254740991.0) {
        // cJSON's floating formatter can round large safe integers. IDs must round-trip exactly.
        context.id_json = std::to_string(static_cast<int64_t>(id->valuedouble));
    }
    auto version = cJSON_GetObjectItemCaseSensitive(json, "jsonrpc");
    auto method = cJSON_GetObjectItemCaseSensitive(json, "method");
    if (!cJSON_IsObject(json) || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0 || !cJSON_IsString(method)) {
        ReplyError(context, -32600, "Invalid Request", response_sender);
        return;
    }
    if (id == nullptr) {
        return;  // Notifications never execute request-only tools or receive a response.
    }
    if (context.id_json.empty()) {
        ReplyError(context, -32600, "Invalid request ID", response_sender);
        return;
    }

    const std::string_view method_name(method->valuestring);
    if (method_name.starts_with("notifications/")) {
        return;  // Retain compatibility with notifications/initialized.
    }
    auto params = cJSON_GetObjectItemCaseSensitive(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ReplyError(context, -32602, "Invalid params", response_sender);
        return;
    }
    auto meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    auto protocol_version = cJSON_GetObjectItemCaseSensitive(meta, kProtocolVersion);
    auto capabilities = cJSON_GetObjectItemCaseSensitive(meta, kClientCapabilities);
    context.modern = method_name == "server/discover" || protocol_version || capabilities ||
                     cJSON_GetObjectItemCaseSensitive(meta, "io.modelcontextprotocol/clientInfo") ||
                     cJSON_GetObjectItemCaseSensitive(meta, "io.modelcontextprotocol/logLevel");
    if ((meta && !cJSON_IsObject(meta)) ||
        (context.modern && (!cJSON_IsString(protocol_version) || !cJSON_IsObject(capabilities)))) {
        ReplyError(context, -32602, "Missing or invalid MCP request metadata", response_sender);
        return;
    }
    if (context.modern && strcmp(protocol_version->valuestring, kModernVersion) != 0) {
        std::string data = "{\"supported\":";
        data += kSupportedVersions;
        data += ",\"requested\":";
        data += JsonString(protocol_version->valuestring);
        data += "}";
        ReplyError(context, -32022, "Unsupported protocol version", response_sender, data);
        return;
    }
    auto extensions = cJSON_GetObjectItemCaseSensitive(capabilities, "extensions");
    auto client_info = cJSON_GetObjectItemCaseSensitive(meta, "io.modelcontextprotocol/clientInfo");
    if (context.modern &&
        ((extensions && !cJSON_IsObject(extensions)) ||
         (client_info &&
          (!cJSON_IsObject(client_info) ||
           !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(client_info, "name")) ||
           !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(client_info, "version")))))) {
        ReplyError(context, -32602, "Invalid MCP client capabilities or identity", response_sender);
        return;
    }

    if (method_name == "initialize" && !context.modern) {
        ParseCapabilities(cJSON_GetObjectItem(params, "capabilities"));
        std::string result =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":";
        AppendServerInfo(result);
        result += "}";
        ReplyResult(context, result, response_sender);
    } else if (method_name == "server/discover") {
        std::string result = "{\"supportedVersions\":";
        result += kSupportedVersions;
        result += ",\"capabilities\":{\"tools\":{}";
        if (std::any_of(tools_.begin(), tools_.end(), [](const auto& tool) {
                return tool->name() == "self.camera.take_photo";
            })) {
            result += ",\"extensions\":{\"me.xiaozhi/vision\":{}}";
        }
        result += "},\"ttlMs\":60000,\"cacheScope\":\"private\"}";
        ReplyResult(context, result, response_sender);
    } else if (method_name == "tools/list") {
        auto cursor = cJSON_GetObjectItemCaseSensitive(params, "cursor");
        auto with_user_tools = cJSON_GetObjectItemCaseSensitive(params, "withUserTools");
        if (context.modern && ((cursor && !cJSON_IsString(cursor)) ||
                               (with_user_tools && !cJSON_IsBool(with_user_tools)))) {
            ReplyError(context, -32602, "Invalid tool-list parameters", response_sender);
            return;
        }
        GetToolsList(context, cJSON_IsString(cursor) ? cursor->valuestring : "",
                     cJSON_IsTrue(with_user_tools), response_sender);
    } else if (method_name == "tools/call") {
        auto name = cJSON_GetObjectItemCaseSensitive(params, "name");
        auto arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (!cJSON_IsString(name) || (arguments && !cJSON_IsObject(arguments))) {
            ReplyError(context, -32602, "Invalid tool name or arguments", response_sender);
            return;
        }
        auto tool = std::find_if(tools_.begin(), tools_.end(), [&](const auto& item) {
            return item->name() == name->valuestring;
        });
        if (tool == tools_.end()) {
            std::string error = "Unknown tool: ";
            error += name->valuestring;
            ReplyError(context, -32602, error, response_sender);
            return;
        }
        if (context.modern && (*tool)->name() == "self.camera.take_photo") {
            auto vision = cJSON_GetObjectItemCaseSensitive(extensions, kVisionExtension);
            if (!vision) {
                ReplyError(context, -32021, "Missing required client capability", response_sender,
                           R"({"requiredCapabilities":{"extensions":{"me.xiaozhi/vision":{}}}})");
                return;
            }
            auto token = cJSON_GetObjectItemCaseSensitive(vision, "token");
            context.vision = ReadVisionConfig(vision);
            if (!cJSON_IsObject(vision) || !context.vision || (token && !cJSON_IsString(token)) ||
                !ValidVisionConfig(*context.vision)) {
                ReplyError(context, -32602, "Invalid vision configuration", response_sender);
                return;
            }
        }
        DoToolCall(std::move(context), tool->get(), arguments, std::move(response_sender));
    } else {
        ReplyError(context, -32601, "Method not implemented: " + std::string(method_name),
                   response_sender);
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) {
        response_sender(payload);
    } else {
        Application::GetInstance().SendMcpMessage(payload);
    }
}

void McpServer::ReplyResult(const McpRequestContext& context, const std::string& result,
                            const ResponseSender& response_sender) {
    auto payload = ResultPrefix(context);
    // Include the final brace so appending it cannot double a large response buffer.
    payload.reserve(payload.size() + result.size() + 1);
    if (context.modern) {
        // Merge known result objects into the envelope without parsing/copying a second JSON tree.
        payload.pop_back();
        if (result.size() > 2) {
            payload += ",";
        }
        payload.append(result, 1, std::string::npos);
    } else {
        payload += result;
    }
    payload += "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(const McpRequestContext& context, int code, const std::string& message,
                           const ResponseSender& response_sender, const std::string& data) {
    auto encoded = JsonString(message.c_str());
    if (encoded.empty()) {
        return;
    }
    std::string payload;
    payload.reserve(context.id_json.size() + encoded.size() + data.size() + 80);
    payload += "{\"jsonrpc\":\"2.0\"";
    if (!context.id_json.empty()) {
        payload += ",\"id\":";
        payload += context.id_json;
    }
    payload += ",\"error\":{\"code\":";
    payload += std::to_string(code);
    payload += ",\"message\":";
    payload += encoded;
    if (!data.empty()) {
        payload += ",\"data\":";
        payload += data;
    }
    payload += "}}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(const McpRequestContext& context, const std::string& message,
                           const ResponseSender& response_sender) {
    ReplyError(context, -32603, message, response_sender);
}

void McpServer::GetToolsList(const McpRequestContext& context, const std::string& cursor,
                             bool list_user_only_tools, const ResponseSender& response_sender) {
    constexpr size_t kMaxPayloadSize = 8000;
    const size_t envelope_size = ResultPrefix(context).size() + 2;
    const std::string suffix = context.modern ? R"(],"ttlMs":60000,"cacheScope":"private")" : "]";
    auto it = tools_.begin();
    if (!cursor.empty()) {
        it = std::find_if(it, tools_.end(),
                          [&](const auto& tool) { return tool->name() == cursor; });
        if (it == tools_.end()) {
            ReplyError(context, -32602, "Invalid cursor", response_sender);
            return;
        }
    }
    auto visible = [&](auto position) {
        while (position != tools_.end() && !list_user_only_tools && (*position)->user_only()) {
            ++position;
        }
        return position;
    };
    it = visible(it);
    std::string json = "{\"tools\":[";
    std::string next_cursor;
    bool added = false;
    for (; it != tools_.end();) {
        auto next = visible(it + 1);
        auto tool_json = (*it)->to_json();
        const auto cursor_json = next == tools_.end() ? "" : JsonString((*next)->name().c_str());
        const size_t reserved = cursor_json.empty() ? 0 : cursor_json.size() + 14;
        if (tool_json.empty() ||
            json.size() + tool_json.size() + suffix.size() + reserved + envelope_size + 2 >
                kMaxPayloadSize) {
            if (!added) {
                ReplyError(context, "Tool definition exceeds payload limit", response_sender);
                return;
            }
            next_cursor = JsonString((*it)->name().c_str());
            break;
        }
        if (added) {
            json += ",";
        }
        json += tool_json;
        added = true;
        it = next;
    }
    json += suffix;
    if (!next_cursor.empty()) {
        json += ",\"nextCursor\":";
        json += next_cursor;
    }
    json += "}";
    ReplyResult(context, json, response_sender);
}

void McpServer::DoToolCall(McpRequestContext context, McpTool* tool, const cJSON* tool_arguments,
                           ResponseSender response_sender) {
    PropertyList arguments = tool->properties();
    for (auto& argument : arguments) {
        bool found = false;
        std::expected<void, std::string> validation;
        if (cJSON_IsObject(tool_arguments)) {
            auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                validation = argument.set_value<bool>(value->valueint == 1);
                found = true;
            } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value) &&
                       (!context.modern ||
                        (std::isfinite(value->valuedouble) &&
                         std::trunc(value->valuedouble) == value->valuedouble &&
                         value->valuedouble >= INT_MIN && value->valuedouble <= INT_MAX))) {
                validation = argument.set_value<int>(value->valueint);
                found = true;
            } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                validation = argument.set_value<std::string>(value->valuestring);
                found = true;
            } else if (value != nullptr) {
                ReplyError(context, -32602, "Invalid type for argument: " + argument.name(),
                           response_sender);
                return;
            }
        }
        if (found && !validation) {
            ReplyError(context, -32602, validation.error(), response_sender);
            return;
        }
        if (!argument.has_default_value() && !found) {
            ReplyError(context, -32602, "Missing required argument: " + argument.name(),
                       response_sender);
            return;
        }
    }
    // Reuse the existing application task. Context owns ID/vision after input JSON is destroyed.
    Application::GetInstance().Schedule([this, context = std::move(context), tool,
                                         arguments = std::move(arguments),
                                         response_sender = std::move(response_sender)]() {
        auto result = tool->Call(arguments, context);
        if (!result) {
            if (context.modern) {
                std::string error = "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":";
                error += JsonString(result.error().c_str());
                error += "}]}";
                ReplyResult(context, error, response_sender);
            } else {
                ReplyError(context, result.error(), response_sender);
            }
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
        ReplyResult(context, *result, response_sender);
    });
}
