#include "wol.h"

#include "cjson_utils.h"
#include "settings.h"

#include <esp_log.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#define TAG "WakeOnLan"

namespace {

constexpr const char* kNamespace = "wol";
constexpr const char* kListKey = "list";
// Keys of the first version of this feature, which kept a single computer.
// They are migrated on load and erased on the first save.
constexpr const char* kLegacyMacKey = "mac";
constexpr const char* kLegacyBroadcastKey = "broadcast";
constexpr const char* kLegacyPortKey = "port";

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string Trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string ToLower(const std::string& text) {
    std::string lower;
    lower.reserve(text.size());
    for (char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return lower;
}

std::string JoinNames(const std::vector<wol::Computer>& computers) {
    std::string names;
    for (const auto& computer : computers) {
        if (!names.empty()) {
            names += ", ";
        }
        names += computer.name;
        if (computer.is_default) {
            names += " (по умолчанию)";
        }
    }
    return names;
}

bool ParseComputers(const std::string& saved, std::vector<wol::Computer>& computers) {
    CJsonUniquePtr root(cJSON_Parse(saved.c_str()));
    if (root == nullptr || !cJSON_IsArray(root.get())) {
        return false;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root.get()) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        cJSON* key = cJSON_GetObjectItem(item, "key");
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* mac = cJSON_GetObjectItem(item, "mac");
        if (!cJSON_IsString(key) || !cJSON_IsString(name) || !cJSON_IsString(mac)) {
            continue;
        }
        wol::Computer computer;
        computer.key = key->valuestring;
        computer.name = name->valuestring;
        computer.mac = mac->valuestring;
        if (cJSON* broadcast = cJSON_GetObjectItem(item, "broadcast");
            cJSON_IsString(broadcast)) {
            computer.broadcast = broadcast->valuestring;
        }
        if (cJSON* port = cJSON_GetObjectItem(item, "port"); cJSON_IsNumber(port)) {
            computer.port = static_cast<int>(port->valuedouble);
        }
        if (cJSON* is_default = cJSON_GetObjectItem(item, "default"); cJSON_IsBool(is_default)) {
            computer.is_default = cJSON_IsTrue(is_default);
        }
        computers.push_back(std::move(computer));
    }
    return true;
}

}  // namespace

namespace wol {

std::string NextKey(const std::vector<Computer>& computers) {
    for (int index = 1;; ++index) {
        const std::string key = "pc" + std::to_string(index);
        const bool used = std::any_of(computers.begin(), computers.end(),
                                      [&key](const auto& computer) { return computer.key == key; });
        if (!used) {
            return key;
        }
    }
}

bool NormalizeMac(const std::string& text, std::string& mac) {
    std::string digits;
    digits.reserve(12);
    for (char c : text) {
        if (c == ':' || c == '-' || c == '.' || std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (HexValue(c) < 0) {
            return false;
        }
        digits.push_back(c);
    }
    if (digits.size() != 12) {
        return false;
    }

    std::string normalized;
    normalized.reserve(17);
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (!normalized.empty()) {
            normalized.push_back(':');
        }
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(digits[i]))));
        normalized.push_back(
            static_cast<char>(std::toupper(static_cast<unsigned char>(digits[i + 1]))));
    }
    mac = std::move(normalized);
    return true;
}

std::vector<Computer> LoadComputers() {
    Settings settings(kNamespace);
    std::vector<Computer> computers;
    const std::string saved = settings.GetString(kListKey);
    if (!saved.empty() && !ParseComputers(saved, computers)) {
        ESP_LOGW(TAG, "Saved computer list is not valid JSON, ignoring it");
    }
    if (computers.empty()) {
        // Migrate the single computer saved by the first version of the tab.
        Computer legacy;
        legacy.mac = settings.GetString(kLegacyMacKey);
        if (!legacy.mac.empty()) {
            legacy.key = "pc1";
            legacy.name = "Компьютер";
            legacy.broadcast = settings.GetString(kLegacyBroadcastKey, kDefaultBroadcast);
            legacy.port = settings.GetInt(kLegacyPortKey, kDefaultPort);
            computers.push_back(std::move(legacy));
            ESP_LOGI(TAG, "Migrating the single saved computer into the list");
        }
    }
    return computers;
}

bool SaveComputers(const std::vector<Computer>& computers, std::string& error) {
    CJsonUniquePtr root(cJSON_CreateArray());
    if (root == nullptr) {
        error = "Не хватает памяти";
        return false;
    }
    // Only the first entry keeps the flag, so the saved list never has two defaults.
    bool default_written = false;
    for (const auto& computer : computers) {
        Computer normalized = computer;
        normalized.name = Trim(normalized.name);
        if (normalized.key.empty()) {
            error = "У компьютера нет идентификатора";
            return false;
        }
        if (normalized.name.empty()) {
            error = "Укажите название компьютера";
            return false;
        }
        std::string mac;
        if (!NormalizeMac(normalized.mac, mac)) {
            error = "MAC-адрес «" + normalized.name + "» должен быть в виде AA:BB:CC:DD:EE:FF";
            return false;
        }
        normalized.mac = mac;
        struct in_addr address = {};
        if (normalized.broadcast.empty() ||
            inet_aton(normalized.broadcast.c_str(), &address) == 0) {
            error = "Для «" + normalized.name + "» укажите IPv4 широковещательный адрес";
            return false;
        }
        if (normalized.port < 1 || normalized.port > 65535) {
            error = "Для «" + normalized.name + "» порт должен быть от 1 до 65535";
            return false;
        }

        CJsonUniquePtr item(cJSON_CreateObject());
        if (item == nullptr) {
            error = "Не хватает памяти";
            return false;
        }
        const bool is_default = normalized.is_default && !default_written;
        default_written = default_written || is_default;
        cJSON_AddStringToObject(item.get(), "key", normalized.key.c_str());
        cJSON_AddStringToObject(item.get(), "name", normalized.name.c_str());
        cJSON_AddStringToObject(item.get(), "mac", normalized.mac.c_str());
        cJSON_AddStringToObject(item.get(), "broadcast", normalized.broadcast.c_str());
        cJSON_AddNumberToObject(item.get(), "port", normalized.port);
        cJSON_AddBoolToObject(item.get(), "default", is_default);
        cJSON_AddItemToArray(root.get(), item.release());
    }

    CJsonStringUniquePtr text(cJSON_PrintUnformatted(root.get()));
    if (text == nullptr) {
        error = "Не хватает памяти";
        return false;
    }
    Settings settings(kNamespace, /*read_write=*/true);
    if (!settings.SetString(kListKey, text.get())) {
        error = "Не удалось сохранить список компьютеров в NVS";
        return false;
    }
    settings.EraseKey(kLegacyMacKey);
    settings.EraseKey(kLegacyBroadcastKey);
    settings.EraseKey(kLegacyPortKey);
    ESP_LOGI(TAG, "Saved %d computers", static_cast<int>(computers.size()));
    return true;
}

bool SendMagicPacket(const std::string& mac, const std::string& broadcast, int port,
                     std::string& error) {
    if (mac.size() != 17) {
        error = "MAC-адрес компьютера не задан";
        return false;
    }
    unsigned char mac_bytes[6] = {};
    for (size_t i = 0; i < 6; ++i) {
        const int high = HexValue(mac[i * 3]);
        const int low = HexValue(mac[i * 3 + 1]);
        if (high < 0 || low < 0) {
            error = "Сохранённый MAC-адрес повреждён";
            return false;
        }
        mac_bytes[i] = static_cast<unsigned char>((high << 4) | low);
    }

    unsigned char packet[6 + 16 * 6];
    std::memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; ++i) {
        std::memcpy(packet + 6 + i * 6, mac_bytes, sizeof(mac_bytes));
    }

    struct sockaddr_in destination = {};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_aton(broadcast.c_str(), &destination.sin_addr) == 0) {
        error = "Широковещательный адрес неверный";
        return false;
    }

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        error = "Не удалось создать UDP-сокет";
        return false;
    }
    const int broadcast_enabled = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enabled, sizeof(broadcast_enabled));
    const ssize_t sent =
        sendto(sock, packet, sizeof(packet), 0,
               reinterpret_cast<const struct sockaddr*>(&destination), sizeof(destination));
    close(sock);
    if (sent < 0) {
        error = "Не удалось отправить магический пакет: проверьте подключение к сети";
        return false;
    }
    ESP_LOGI(TAG, "Magic packet sent to %s:%d", broadcast.c_str(), port);
    return true;
}

bool FindComputer(const std::vector<Computer>& computers, const std::string& selector,
                  Computer& computer, std::string& error) {
    const std::string wanted = ToLower(Trim(selector));
    if (!wanted.empty()) {
        for (const auto& candidate : computers) {
            if (ToLower(candidate.key) == wanted || ToLower(candidate.name) == wanted) {
                computer = candidate;
                return true;
            }
        }
        error = "Компьютер «" + Trim(selector) + "» не найден. Сохранены: " + JoinNames(computers);
        return false;
    }
    if (computers.empty()) {
        error = "Список компьютеров пуст: добавьте компьютер в веб-интерфейсе на вкладке "
                "Wake-on-LAN";
        return false;
    }
    if (computers.size() > 1) {
        const auto marked = std::find_if(computers.begin(), computers.end(),
                                         [](const auto& candidate) { return candidate.is_default; });
        if (marked != computers.end()) {
            computer = *marked;
            return true;
        }
        error = "Уточните, какой компьютер включить: " + JoinNames(computers) +
                ". Компьютер по умолчанию можно выбрать в веб-интерфейсе.";
        return false;
    }
    computer = computers.front();
    return true;
}

bool Wake(const std::string& selector, Computer& computer, std::string& error) {
    const std::vector<Computer> computers = LoadComputers();
    if (!FindComputer(computers, selector, computer, error)) {
        return false;
    }
    return SendMagicPacket(computer.mac, computer.broadcast, computer.port, error);
}

}  // namespace wol
