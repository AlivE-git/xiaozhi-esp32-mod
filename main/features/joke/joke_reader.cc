#include "joke_reader.h"

#include "board.h"
#include "cjson_utils.h"

#include <esp_log.h>

#include <cctype>
#include <string>

#define TAG "JokeReader"

namespace {

// Only the "Анекдоты" category is used: CType=1.
constexpr char kApiUrl[] = "https://rzhunemogu.ru/api/randjson?CType=1";
constexpr size_t kMaxResponseBytes = 8192;
constexpr int kTimeoutMs = 8000;

// The API returns the joke with a trailing space or line break, which is read
// aloud as an awkward pause.
void TrimTrailingWhitespace(std::string* text) {
    while (!text->empty() && std::isspace(static_cast<unsigned char>(text->back()))) {
        text->pop_back();
    }
}

}  // namespace

bool JokeReader::Fetch(std::string& joke, std::string& error) {
    joke.clear();
    error.clear();

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        error = "Нет подключения к сети";
        return false;
    }
    auto http = network->CreateHttp(1);
    if (!http) {
        error = "Не удалось создать HTTP-клиент";
        return false;
    }
    http->SetTimeout(kTimeoutMs);
    http->SetHeader("User-Agent", "XiaoZhi-Joke/1.0");
    http->SetHeader("Accept", "application/json");

    if (auto opened = http->Open("GET", kApiUrl); !opened) {
        error = "Ошибка сети: " + opened.error().ToString();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        http->Close();
        error = status.error().ToString();
        return false;
    }
    if (*status != 200) {
        http->Close();
        error = "Сервер вернул HTTP " + std::to_string(*status);
        return false;
    }

    std::string body;
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

    if (body.empty()) {
        error = "Пустой ответ от сервера";
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr) {
        ESP_LOGE(TAG, "Joke response is not valid JSON");
        error = "Сервер вернул не JSON";
        return false;
    }
    cJSON* content = cJSON_GetObjectItem(root.get(), "content");
    if (!cJSON_IsString(content) || content->valuestring == nullptr) {
        error = "В ответе нет текста анекдота";
        return false;
    }
    joke = content->valuestring;
    TrimTrailingWhitespace(&joke);
    if (joke.empty()) {
        error = "Сервер вернул пустой анекдот";
        return false;
    }
    ESP_LOGI(TAG, "Fetched a joke, %d bytes", static_cast<int>(joke.size()));
    return true;
}

std::string JokeReader::Tell() {
    std::string joke;
    std::string error;
    if (!Fetch(joke, error)) {
        return "Не удалось рассказать анекдот: " + error;
    }
    return joke;
}
