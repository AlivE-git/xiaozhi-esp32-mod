#include "currency_converter.h"

#include "board.h"
#include "cjson_utils.h"

#include <esp_log.h>

#include <cmath>
#include <cstdio>
#include <utility>

#define TAG "CurrencyConverter"

namespace {

constexpr int kTimeoutMs = 8000;
constexpr size_t kMaxResponseBytes = 16 * 1024;
constexpr char kUserAgent[] = "XiaoZhi-FX/1.0";
// Daily reference rates of the Russian Central Bank, published as JSON by the
// open mirror. The official endpoint (www.cbr.ru/scripts/XML_daily.asp) is XML
// over plain HTTP, so the mirror is used instead: same numbers, HTTPS, JSON.
constexpr char kCbrUrl[] = "https://www.cbr-xml-daily.ru/daily_json.js";

std::string FormatDate(const std::string& iso_date) {
    // The mirror uses ISO 8601 dates and appends the time to today's rates.
    if (iso_date.size() >= 10 && iso_date[4] == '-' && iso_date[7] == '-') {
        return iso_date.substr(8, 2) + "." + iso_date.substr(5, 2) + "." + iso_date.substr(0, 4);
    }
    return iso_date;
}

std::string FormatNumber(double value, int decimals) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text(buffer);
    // Russian decimal comma reads correctly through the TTS pipeline.
    for (char& c : text) {
        if (c == '.') {
            c = ',';
        }
    }
    return text;
}

std::string FormatRate(double value) { return FormatNumber(value, std::fabs(value) < 1.0 ? 4 : 2); }

std::string FormatAmount(double value) {
    if (std::fabs(value - std::round(value)) < 1e-9) {
        return FormatNumber(value, 0);
    }
    return FormatRate(value);
}

std::string NormalizeCode(const std::string& code) {
    std::string normalized;
    normalized.reserve(code.size());
    for (size_t i = 0; i < code.size(); ++i) {
        if (code[i] == ' ' || code[i] == '\t') {
            continue;
        }
        if (normalized.size() == 3) {
            return std::string();
        }
        char c = code[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        normalized.push_back(c);
    }
    // Some sources still spell the ruble as RUR.
    if (normalized == "RUR") {
        normalized = "RUB";
    }
    return normalized;
}

bool IsValidCode(const std::string& code) {
    if (code.size() != 3) {
        return false;
    }
    for (char c : code) {
        if (c < 'A' || c > 'Z') {
            return false;
        }
    }
    return true;
}

bool HttpGetJson(const std::string& url, std::string& body, std::string& error) {
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
    http->SetHeader("User-Agent", kUserAgent);
    http->SetHeader("Accept", "application/json");

    if (auto opened = http->Open("GET", url); !opened) {
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

    body.clear();
    body.reserve(2048);
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
    return true;
}

// How many rubles one unit of `code` costs; 0 when the Central Bank does not
// quote it. Currencies with a nominal above 1 (KZT, JPY, UAH...) are quoted for
// a bundle of units, so the nominal has to be divided out.
double CbrRubPerUnit(const cJSON* valute, const std::string& code) {
    if (code == "RUB") {
        return 1.0;
    }
    const cJSON* entry = cJSON_GetObjectItem(valute, code.c_str());
    if (!cJSON_IsObject(entry)) {
        return 0.0;
    }
    const cJSON* nominal = cJSON_GetObjectItem(entry, "Nominal");
    const cJSON* value = cJSON_GetObjectItem(entry, "Value");
    if (!cJSON_IsNumber(nominal) || !cJSON_IsNumber(value)) {
        return 0.0;
    }
    if (nominal->valuedouble <= 0 || value->valuedouble <= 0) {
        return 0.0;
    }
    return value->valuedouble / nominal->valuedouble;
}

bool FetchCbrRate(const std::string& from, const std::string& to, double& rate, std::string& date,
                  std::string& error) {
    std::string body;
    if (!HttpGetJson(kCbrUrl, body, error)) {
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr) {
        error = "Сервер отдал не JSON";
        return false;
    }
    const cJSON* valute = cJSON_GetObjectItem(root.get(), "Valute");
    if (!cJSON_IsObject(valute)) {
        error = "В ответе нет списка валют";
        return false;
    }
    const double from_rub = CbrRubPerUnit(valute, from);
    if (from_rub <= 0) {
        error = "ЦБ РФ не котирует валюту " + from;
        return false;
    }
    const double to_rub = CbrRubPerUnit(valute, to);
    if (to_rub <= 0) {
        error = "ЦБ РФ не котирует валюту " + to;
        return false;
    }
    rate = from_rub / to_rub;
    const cJSON* date_item = cJSON_GetObjectItem(root.get(), "Date");
    date = FormatDate(cJSON_IsString(date_item) ? date_item->valuestring : "");
    return true;
}

}  // namespace

bool CurrencyConverter::Convert(const std::string& from, const std::string& to, double amount,
                                Rate& rate, std::string& error) {
    rate = Rate{};
    error.clear();

    const std::string from_code = NormalizeCode(from);
    const std::string to_code = NormalizeCode(to);
    if (!IsValidCode(from_code) || !IsValidCode(to_code)) {
        error = "Нужны трёхбуквенные коды валют, например USD и RUB";
        return false;
    }

    rate.from = from_code;
    rate.to = to_code;
    rate.amount = amount > 0 ? amount : 1.0;
    if (from_code == to_code) {
        rate.rate = 1.0;
        rate.converted = rate.amount;
        return true;
    }

    double fetched_rate = 0.0;
    std::string date;
    if (!FetchCbrRate(from_code, to_code, fetched_rate, date, error)) {
        return false;
    }

    rate.rate = fetched_rate;
    rate.converted = rate.amount * fetched_rate;
    rate.date = std::move(date);
    rate.source = "ЦБ РФ";
    ESP_LOGI(TAG, "%s -> %s at %.6f", from_code.c_str(), to_code.c_str(), fetched_rate);
    return true;
}

std::string CurrencyConverter::Describe(const Rate& rate) {
    if (rate.from == rate.to) {
        return "1 " + rate.from + " = 1 " + rate.to;
    }

    std::string text;
    if (rate.amount == 1.0) {
        text = "1 " + rate.from + " = " + FormatRate(rate.rate) + " " + rate.to;
    } else {
        text = FormatAmount(rate.amount) + " " + rate.from + " = " + FormatRate(rate.converted) +
               " " + rate.to + " (1 " + rate.from + " = " + FormatRate(rate.rate) + " " +
               rate.to + ")";
    }
    if (rate.date.empty()) {
        text += ".";
    } else {
        text += ", курс " + rate.source + " на " + rate.date + ".";
    }
    return text;
}
