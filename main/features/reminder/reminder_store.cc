#include "reminder_store.h"

#include "cjson_utils.h"
#include "features/storage/list_storage.h"

#include <esp_log.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#define TAG "Reminder"

namespace {

constexpr const char* kFile = "reminders.json";
constexpr int kMaxItems = 32;
constexpr int kMaxTextLength = 120;
constexpr int kMaxMinutesAhead = 7 * 24 * 60;
constexpr const char* kDefaultText = "Напоминание";

// Serialises the list: the MCP and Web UI tasks create reminders while the
// notifier task removes the due ones.
std::mutex store_mutex;

std::string CleanText(const std::string& text) {
    std::string value;
    for (char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte == '\n' || byte == '\r' || byte == '\t') {
            value += ' ';
        } else if (byte >= 0x20) {
            value += c;
        }
    }
    // Collapse repeated spaces and trim the ends.
    std::string result;
    bool space = false;
    for (char c : value) {
        if (c == ' ') {
            space = !result.empty();
            continue;
        }
        if (space) {
            result += ' ';
            space = false;
        }
        result += c;
    }
    if (result.size() > kMaxTextLength) {
        result.resize(kMaxTextLength);
        result += "...";
    }
    return result;
}

bool Load(std::vector<reminder::Item>& items) {
    items.clear();
    std::string text;
    if (!list_storage::Read(kFile, text)) {
        return true;  // no file yet: an empty list
    }
    CJsonUniquePtr root(cJSON_Parse(text.c_str()));
    if (root == nullptr) {
        ESP_LOGW(TAG, "Stored reminders are unreadable, starting empty");
        return true;
    }
    cJSON* array = cJSON_GetObjectItem(root.get(), "items");
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, array) {
        cJSON* body = cJSON_GetObjectItem(entry, "text");
        cJSON* due = cJSON_GetObjectItem(entry, "due");
        if (!cJSON_IsString(body) || body->valuestring == nullptr || !cJSON_IsNumber(due)) {
            continue;
        }
        reminder::Item item;
        item.text = body->valuestring;
        item.due = static_cast<time_t>(due->valuedouble);
        if (!item.text.empty()) {
            items.push_back(std::move(item));
        }
    }
    return true;
}

bool Save(const std::vector<reminder::Item>& items) {
    CJsonUniquePtr root(cJSON_CreateObject());
    if (root == nullptr) {
        return false;
    }
    cJSON* array = cJSON_AddArrayToObject(root.get(), "items");
    if (array == nullptr) {
        return false;
    }
    for (const auto& item : items) {
        cJSON* entry = cJSON_CreateObject();
        if (entry == nullptr) {
            return false;
        }
        cJSON_AddStringToObject(entry, "text", item.text.c_str());
        cJSON_AddNumberToObject(entry, "due", static_cast<double>(item.due));
        cJSON_AddItemToArray(array, entry);
    }
    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    if (text == nullptr) {
        return false;
    }
    return list_storage::Write(kFile, text.get());
}

// Parses "8:05" or "22:40" into minutes since midnight.
bool ParseClock(const std::string& clock, int& minutes) {
    const size_t colon = clock.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 2 || clock.size() < colon + 3) {
        return false;
    }
    for (size_t i = 0; i < colon; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(clock[i]))) {
            return false;
        }
    }
    if (!std::isdigit(static_cast<unsigned char>(clock[colon + 1])) ||
        !std::isdigit(static_cast<unsigned char>(clock[colon + 2]))) {
        return false;
    }
    const int hour = std::atoi(clock.substr(0, colon).c_str());
    const int minute = std::atoi(clock.substr(colon + 1, 2).c_str());
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    minutes = hour * 60 + minute;
    return true;
}

// Lowercases ASCII and the Cyrillic letters, which is what the spoken reminder
// text and the spoken match phrase differ in most often.
std::string ToLower(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            out += static_cast<char>(std::tolower(c));
            continue;
        }
        if (c == 0xD0 && i + 1 < text.size()) {
            const unsigned char next = static_cast<unsigned char>(text[i + 1]);
            if (next == 0x81) {  // Ё -> ё
                out += "\xD1\x91";
                ++i;
                continue;
            }
            if (next >= 0x90 && next <= 0x9F) {  // А-П -> а-п
                out += "\xD0";
                out += static_cast<char>(next + 0x20);
                ++i;
                continue;
            }
            if (next >= 0xA0 && next <= 0xAF) {  // Р-Я -> р-я
                out += "\xD1";
                out += static_cast<char>(next - 0x20);
                ++i;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

std::string WhenLabel(time_t due) {
    struct tm when = {};
    localtime_r(&due, &when);
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", when.tm_hour, when.tm_min);

    const time_t now = time(nullptr);
    struct tm today = {};
    localtime_r(&now, &today);
    if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday) {
        return clock;
    }
    if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday + 1) {
        return std::string("завтра в ") + clock;
    }
    char text[32];
    snprintf(text, sizeof(text), "%02d.%02d в %s", when.tm_mday, when.tm_mon + 1, clock);
    return text;
}

bool Append(const std::string& text, time_t due, reminder::Item& added, std::string& error) {
    std::lock_guard<std::mutex> lock(store_mutex);
    std::vector<reminder::Item> items;
    Load(items);
    if (static_cast<int>(items.size()) >= kMaxItems) {
        error = "слишком много напоминаний, дождитесь срабатывания старых";
        return false;
    }
    reminder::Item item;
    item.text = CleanText(text);
    if (item.text.empty()) {
        item.text = kDefaultText;
    }
    item.due = due;
    items.push_back(item);
    if (!Save(items)) {
        error = "не удалось сохранить напоминание в памяти устройства";
        return false;
    }
    added = item;
    ESP_LOGI(TAG, "Reminder at %s: %s", WhenLabel(item.due).c_str(), item.text.c_str());
    return true;
}

}  // namespace

namespace reminder {

std::string WhenText(time_t due) { return WhenLabel(due); }

bool AddIn(int minutes, const std::string& text, Item& added, std::string& error) {
    if (minutes < 1 || minutes > kMaxMinutesAhead) {
        error = "напомнить можно от 1 минуты до 7 дней";
        return false;
    }
    return Append(text, time(nullptr) + static_cast<time_t>(minutes) * 60, added, error);
}

bool AddAt(const std::string& day, const std::string& clock, const std::string& text,
           Item& added, std::string& error) {
    // Only the day given: a sensible default so "напомни завтра" just works.
    int minutes = 9 * 60;
    if (!clock.empty() && !ParseClock(clock, minutes)) {
        error = "время задаётся как 22:40";
        return false;
    }

    const time_t now = time(nullptr);
    struct tm today = {};
    localtime_r(&now, &today);

    std::string word = ToLower(day);
    while (!word.empty() && (word.front() == ' ' || word.front() == '\t')) {
        word.erase(word.begin());
    }
    while (!word.empty() && (word.back() == ' ' || word.back() == '\t')) {
        word.pop_back();
    }

    struct tm target = today;
    bool fixed_day = false;  // the day was named, so a passed time is an error
    bool has_year = false;

    if (word.empty()) {
        // Today, or tomorrow when that time already passed.
    } else if (word == "today" || word == "сегодня") {
        fixed_day = true;
    } else if (word == "tomorrow" || word == "завтра") {
        fixed_day = true;
        target.tm_mday += 1;
    } else if (word == "послезавтра") {
        fixed_day = true;
        target.tm_mday += 2;
    } else {
        int a = 0, b = 0, c = 0;
        const int dotted = sscanf(word.c_str(), "%d.%d.%d", &a, &b, &c);
        if (dotted >= 2) {
            int year = today.tm_year + 1900;
            if (dotted == 3) {
                year = c < 100 ? 2000 + c : c;
                has_year = true;
            }
            if (a < 1 || a > 31 || b < 1 || b > 12 || year < 1970) {
                error = "дата задаётся как 13.11 или 2026-11-13";
                return false;
            }
            target.tm_year = year - 1900;
            target.tm_mon = b - 1;
            target.tm_mday = a;
            fixed_day = true;
        } else if (sscanf(word.c_str(), "%d-%d-%d", &a, &b, &c) == 3) {
            if (a < 1970 || b < 1 || b > 12 || c < 1 || c > 31) {
                error = "дата задаётся как 2026-11-13";
                return false;
            }
            target.tm_year = a - 1900;
            target.tm_mon = b - 1;
            target.tm_mday = c;
            fixed_day = true;
            has_year = true;
        } else {
            error = "день задаётся как today, tomorrow, послезавтра или 13.11";
            return false;
        }
    }

    target.tm_hour = minutes / 60;
    target.tm_min = minutes % 60;
    target.tm_sec = 0;
    target.tm_isdst = -1;
    time_t due = mktime(&target);

    if (due <= now) {
        if (fixed_day) {
            if (has_year) {
                error = "это время уже прошло";
                return false;
            }
            // "13.11" said after that day of this year means next year.
            target.tm_year += 1;
            target.tm_isdst = -1;
            due = mktime(&target);
        } else {
            target.tm_mday += 1;
            target.tm_isdst = -1;
            due = mktime(&target);
        }
    }
    constexpr time_t kMaxSecondsAhead = 366 * 24 * 60 * 60;
    if (due - now > kMaxSecondsAhead) {
        error = "напомнить можно не дальше года";
        return false;
    }
    return Append(text, due, added, error);
}

bool List(std::vector<Item>& items) {
    std::lock_guard<std::mutex> lock(store_mutex);
    Load(items);
    return !items.empty();
}

bool RemoveMatching(const std::string& match, Item& removed, std::string& error) {
    const std::string needle = ToLower(match);
    if (needle.empty()) {
        error = "не указано, какое напоминание отменить";
        return false;
    }
    std::lock_guard<std::mutex> lock(store_mutex);
    std::vector<Item> items;
    Load(items);
    size_t found = items.size();
    for (size_t i = 0; i < items.size(); ++i) {
        if (ToLower(items[i].text).find(needle) != std::string::npos) {
            found = i;
            break;
        }
    }
    if (found == items.size()) {
        error = "не нашёл напоминание со словами \"" + match + "\"";
        return false;
    }
    removed = items[found];
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(found));
    if (!Save(items)) {
        error = "не удалось сохранить список напоминаний";
        return false;
    }
    ESP_LOGI(TAG, "Removed reminder: %s", removed.text.c_str());
    return true;
}

int RemoveAll() {
    std::lock_guard<std::mutex> lock(store_mutex);
    std::vector<Item> items;
    Load(items);
    const int count = static_cast<int>(items.size());
    if (count > 0 && !Save({})) {
        ESP_LOGW(TAG, "Failed to clear the reminder list");
        return 0;
    }
    ESP_LOGI(TAG, "Cleared %d reminders", count);
    return count;
}

bool Remove(size_t index, std::string& error) {
    std::lock_guard<std::mutex> lock(store_mutex);
    std::vector<Item> items;
    Load(items);
    if (index >= items.size()) {
        error = "напоминание уже сработало или удалено";
        return false;
    }
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
    if (!Save(items)) {
        error = "не удалось сохранить список напоминаний";
        return false;
    }
    return true;
}

std::string DescribeItem(const Item& item) {
    return "Напомню в " + WhenLabel(item.due) + ": " + item.text + ".";
}

std::string Describe(int max_items) {
    if (max_items < 1) {
        max_items = 1;
    }
    std::lock_guard<std::mutex> lock(store_mutex);
    std::vector<Item> items;
    Load(items);
    if (items.empty()) {
        return "Напоминаний нет.";
    }
    std::string text = "Напоминания: ";
    int shown = 0;
    for (const auto& item : items) {
        if (shown >= max_items) {
            break;
        }
        if (shown > 0) {
            text += "; ";
        }
        text += WhenLabel(item.due) + " — " + item.text;
        ++shown;
    }
    if (static_cast<int>(items.size()) > shown) {
        text += "; и ещё " + std::to_string(items.size() - shown);
    }
    return text + ".";
}

bool TakeDue(time_t now, std::vector<Item>& due) {
    std::lock_guard<std::mutex> lock(store_mutex);
    std::vector<Item> items;
    Load(items);
    due.clear();
    std::vector<Item> rest;
    for (const auto& item : items) {
        if (item.due <= now) {
            due.push_back(item);
        } else {
            rest.push_back(item);
        }
    }
    if (due.empty()) {
        return false;
    }
    if (!Save(rest)) {
        ESP_LOGW(TAG, "Failed to save the reminder list after a due reminder");
    }
    return true;
}

}  // namespace reminder
