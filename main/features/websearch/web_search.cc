#include "web_search.h"

#include "board.h"
#include "settings.h"

#include <esp_log.h>

#include <string>

#define TAG "WebSearch"

namespace websearch {
namespace {

// NVS keys are part of the persistent API: keep the names when changing the code.
constexpr const char* kNamespace = "websearch";
constexpr const char* kKeyDeployment = "deployment_id";
constexpr const char* kKeyToken = "token";

constexpr const char* kScriptUrlPrefix = "https://script.google.com/macros/s/";

// The script answers with plain text: a Tavily answer is a few hundred characters,
// so a megabyte-scale buffer is not needed here.
constexpr size_t kMaxResponseBytes = 8192;

// The script waits for Tavily and Apps Script adds a cold start on top, so the
// device waits longer than for the joke and currency services.
constexpr int kTimeoutMs = 15000;

// Apps Script answers 302 to script.googleusercontent.com. The HTTP client of the
// board does not follow redirects by itself, so they are followed here.
constexpr int kMaxRedirects = 3;

bool IsUrlSafe(const std::string& text) {
    for (unsigned char c : text) {
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '~' ||
                          c == '-';
        if (!safe) {
            return false;
        }
    }
    return true;
}

// Percent-encodes every byte outside the unreserved set, so a Russian query
// survives the trip through the script URL as UTF-8.
std::string Escape(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(text.size() * 3);
    for (unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                                c == '_' || c == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[c >> 4]);
        encoded.push_back(kHex[c & 0x0F]);
    }
    return encoded;
}

std::string BuildUrl(const Config& config, const std::string& query, bool detailed) {
    std::string url = kScriptUrlPrefix;
    url += Escape(config.deployment_id);
    url += "/exec?token=";
    url += Escape(config.token);
    url += "&q=";
    url += Escape(query);
    url += "&only=1";
    if (detailed) {
        url += "&adv=1";
    }
    return url;
}

// Reads the body of a 200 answer, or follows a redirect and tries again.
bool Fetch(const std::string& url, std::string& body, std::string& error) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "Нет подключения к сети";
        return false;
    }

    std::string current = url;
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        auto http = network->CreateHttp(1);
        if (!http) {
            error = "Не удалось создать HTTP-клиент";
            return false;
        }
        http->SetTimeout(kTimeoutMs);
        http->SetHeader("User-Agent", "XiaoZhi-WebSearch/1.0");
        http->SetHeader("Accept", "text/plain");

        if (auto opened = http->Open("GET", current); !opened) {
            http->Close();
            error = "Ошибка сети: " + opened.error().ToString();
            return false;
        }
        auto status = http->GetStatusCode();
        if (!status) {
            const std::string reason = status.error().ToString();
            http->Close();
            error = reason;
            return false;
        }

        if (*status == 301 || *status == 302 || *status == 303 || *status == 307 ||
            *status == 308) {
            std::string location = http->GetResponseHeader("Location");
            http->Close();
            if (location.rfind("http", 0) != 0) {
                error = "Сервис вернул редирект без адреса";
                return false;
            }
            current = location;
            continue;
        }

        if (*status != 200) {
            http->Close();
            error = "Сервис вернул HTTP " + std::to_string(*status);
            return false;
        }

        body.clear();
        body.reserve(1024);
        char buffer[256];
        while (body.size() < kMaxResponseBytes) {
            auto read = http->Read(buffer, sizeof(buffer));
            if (!read) {
                ESP_LOGW(TAG, "Read failed: %s", read.error().ToString().c_str());
                break;
            }
            if (*read == 0) {
                break;
            }
            body.append(buffer, static_cast<size_t>(*read));
        }
        http->Close();
        return true;
    }

    error = "Слишком много редиректов";
    return false;
}

// Line breaks and repeated spaces are read aloud as awkward pauses.
std::string NormalizeAnswer(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool previous_space = true;
    for (char c : text) {
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
        if (c == ' ') {
            if (previous_space) {
                continue;
            }
            previous_space = true;
        } else {
            previous_space = false;
        }
        normalized.push_back(c);
    }
    while (!normalized.empty() && normalized.back() == ' ') {
        normalized.pop_back();
    }
    return normalized;
}

}  // namespace

Config Load() {
    Settings settings(kNamespace);
    Config config;
    config.deployment_id = settings.GetString(kKeyDeployment);
    config.token = settings.GetString(kKeyToken);
    return config;
}

bool Save(const std::string& deployment_id, const std::string& token, std::string& error) {
    error.clear();
    if (!deployment_id.empty() && !IsUrlSafe(deployment_id)) {
        error = "Идентификатор развертывания: разрешены латиница, цифры, точка, дефис, подчёркивание";
        return false;
    }
    if (!token.empty() && !IsUrlSafe(token)) {
        error = "TOKEN: разрешены латиница, цифры, точка, дефис, подчёркивание";
        return false;
    }
    if (deployment_id.size() > 200 || token.size() > 128) {
        error = "Слишком длинное значение";
        return false;
    }
    Settings settings(kNamespace, true);
    if (!settings.SetString(kKeyDeployment, deployment_id) ||
        !settings.SetString(kKeyToken, token)) {
        error = "Не удалось сохранить настройки в NVS";
        return false;
    }
    ESP_LOGI(TAG, "Web search settings saved (configured=%d)",
             (!deployment_id.empty() && !token.empty()) ? 1 : 0);
    return true;
}

bool IsConfigured() {
    const Config config = Load();
    return !config.deployment_id.empty() && !config.token.empty();
}

bool Search(const std::string& query, bool detailed, std::string& answer, std::string& error) {
    answer.clear();
    error.clear();

    const Config config = Load();
    if (config.deployment_id.empty() || config.token.empty()) {
        error = "Веб-поиск не настроен: впишите идентификатор развертывания и TOKEN в веб-интерфейсе устройства, вкладка «Система»";
        return false;
    }
    if (query.empty()) {
        error = "Пустой поисковый запрос";
        return false;
    }

    std::string body;
    if (!Fetch(BuildUrl(config, query, detailed), body, error)) {
        ESP_LOGW(TAG, "Search failed: %s", error.c_str());
        return false;
    }

    answer = NormalizeAnswer(body);
    if (answer.empty()) {
        error = "Сервис вернул пустой ответ";
        return false;
    }
    ESP_LOGI(TAG, "Search answered, %d bytes (detailed=%d)", static_cast<int>(answer.size()),
             detailed ? 1 : 0);
    return true;
}

std::string Ask(const std::string& query, bool detailed) {
    std::string answer;
    std::string error;
    if (!Search(query, detailed, answer, error)) {
        return "Не удалось найти: " + error;
    }
    return answer;
}

}  // namespace websearch
