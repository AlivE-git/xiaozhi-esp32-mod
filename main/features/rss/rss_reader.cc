#include "rss_reader.h"

#include "board.h"
#include "cjson_utils.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#define TAG "RssReader"

namespace {

constexpr char kNamespace[] = "rss";
constexpr int kMaxFeeds = 16;
constexpr int kMaxFeedsPerRequest = 4;
constexpr size_t kMaxFeedBytes = 256 * 1024;
constexpr int kFetchTimeoutMs = 8000;
constexpr size_t kMaxTitleBytes = 300;
constexpr size_t kMaxLinkBytes = 300;

std::string Trim(const std::string& value) {
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

std::string ToLowerAscii(const std::string& value) {
    std::string result = value;
    for (char& c : result) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x80) {
            c = static_cast<char>(std::tolower(u));
        }
    }
    return result;
}

void AppendUtf8(std::string& out, unsigned long code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

std::string DecodeEntities(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '&') {
            out.push_back(value[i]);
            continue;
        }
        size_t end = value.find(';', i + 1);
        if (end == std::string::npos || end - i > 10) {
            out.push_back('&');
            continue;
        }
        const std::string entity = value.substr(i + 1, end - i - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (entity == "nbsp") {
            out.push_back(' ');
        } else if (entity.size() > 1 && entity[0] == '#') {
            const bool hex = entity[1] == 'x' || entity[1] == 'X';
            unsigned long code =
                strtoul(entity.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            if (code > 0) {
                AppendUtf8(out, code);
            }
        } else {
            // Unknown entity: keep it verbatim instead of dropping the text.
            out += value.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}

// Cuts a UTF-8 string without splitting a multi-byte character.
std::string TruncateUtf8(std::string value, size_t max_bytes) {
    if (value.size() <= max_bytes) {
        return value;
    }
    size_t size = max_bytes;
    while (size > 0 && (static_cast<unsigned char>(value[size]) & 0xC0) == 0x80) {
        --size;
    }
    value.resize(size);
    return value + "...";
}

// Strips CDATA markers, inline markup and entities, then collapses whitespace so
// the headline reads well when spoken and fits on one line.
std::string CleanText(const std::string& raw) {
    std::string text = raw;
    size_t cdata = text.find("<![CDATA[");
    if (cdata != std::string::npos) {
        size_t end = text.find("]]>", cdata);
        text = text.substr(cdata + 9,
                           end == std::string::npos ? std::string::npos : end - cdata - 9);
    }

    std::string stripped;
    stripped.reserve(text.size());
    bool in_tag = false;
    for (char c : text) {
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag) {
            stripped.push_back(c);
        }
    }

    stripped = DecodeEntities(stripped);
    std::string out;
    out.reserve(stripped.size());
    bool pending_space = false;
    for (char c : stripped) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pending_space = true;
            continue;
        }
        if (pending_space && !out.empty()) {
            out.push_back(' ');
        }
        pending_space = false;
        out.push_back(c);
    }
    return out;
}

// Returns the text of <tag>...</tag> inside one item block. `tag` must be given
// in the spelling used by the feed, e.g. "title" or "pubDate".
std::string ExtractTagText(const std::string& block, const char* tag) {
    const std::string open = std::string("<") + tag;
    size_t pos = 0;
    while ((pos = block.find(open, pos)) != std::string::npos) {
        size_t after = pos + open.size();
        // Skip look-alike tags such as <titleImage>.
        if (after < block.size()) {
            unsigned char next = static_cast<unsigned char>(block[after]);
            if (std::isalnum(next) || next == '-' || next == ':' || next == '_') {
                pos = after;
                continue;
            }
        }
        size_t gt = block.find('>', after);
        if (gt == std::string::npos) {
            return "";
        }
        const std::string close = std::string("</") + tag;
        size_t end = block.find(close, gt + 1);
        if (end == std::string::npos) {
            return "";
        }
        return block.substr(gt + 1, end - gt - 1);
    }
    return "";
}

std::string ExtractAttribute(const std::string& tag, const char* name) {
    const std::string lower = ToLowerAscii(tag);
    const std::string key = ToLowerAscii(name);
    size_t pos = 0;
    while ((pos = lower.find(key, pos)) != std::string::npos) {
        size_t after = pos + key.size();
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(lower[pos - 1]))) {
            pos = after;
            continue;
        }
        while (after < lower.size() && std::isspace(static_cast<unsigned char>(lower[after]))) {
            ++after;
        }
        if (after >= lower.size() || lower[after] != '=') {
            pos = after;
            continue;
        }
        ++after;
        while (after < lower.size() && std::isspace(static_cast<unsigned char>(lower[after]))) {
            ++after;
        }
        if (after >= lower.size()) {
            return "";
        }
        const char quote = lower[after];
        if (quote != '"' && quote != '\'') {
            return "";
        }
        size_t end = tag.find(quote, after + 1);
        if (end == std::string::npos) {
            return "";
        }
        return tag.substr(after + 1, end - after - 1);
    }
    return "";
}

// RSS puts the article URL in <link>text</link>, Atom in <link href="..."/>.
std::string ExtractLink(const std::string& block) {
    std::string link = CleanText(ExtractTagText(block, "link"));
    if (!link.empty()) {
        return link;
    }
    size_t pos = 0;
    while ((pos = block.find("<link", pos)) != std::string::npos) {
        size_t gt = block.find('>', pos);
        if (gt == std::string::npos) {
            break;
        }
        const std::string tag = block.substr(pos, gt - pos + 1);
        const std::string href = DecodeEntities(ExtractAttribute(tag, "href"));
        const std::string rel = ToLowerAscii(ExtractAttribute(tag, "rel"));
        if (!href.empty() && (rel.empty() || rel == "alternate")) {
            return href;
        }
        pos = gt + 1;
    }
    return "";
}

bool ParseFeedItems(const std::string& body, int count, std::vector<RssItem>& items) {
    items.clear();
    size_t pos = 0;
    while (static_cast<int>(items.size()) < count) {
        const size_t item_pos = body.find("<item", pos);
        const size_t entry_pos = body.find("<entry", pos);
        if (item_pos == std::string::npos && entry_pos == std::string::npos) {
            break;
        }
        const bool atom = item_pos == std::string::npos ||
                          (entry_pos != std::string::npos && entry_pos < item_pos);
        const size_t start = atom ? entry_pos : item_pos;
        const char* close_tag = atom ? "</entry>" : "</item>";
        const size_t end = body.find(close_tag, start);
        if (end == std::string::npos) {
            break;
        }

        const std::string block = body.substr(start, end - start);
        RssItem item;
        item.title = TruncateUtf8(CleanText(ExtractTagText(block, "title")), kMaxTitleBytes);
        item.link = TruncateUtf8(ExtractLink(block), kMaxLinkBytes);
        const char* date_tags[] = {"pubDate", "published", "updated", "dc:date", "date"};
        for (const char* tag : date_tags) {
            item.published = CleanText(ExtractTagText(block, tag));
            if (!item.published.empty()) {
                break;
            }
        }
        if (!item.title.empty()) {
            items.push_back(std::move(item));
        }
        pos = end + strlen(close_tag);
    }
    return !items.empty();
}

bool IsValidFeed(const RssFeed& feed) {
    if (feed.name.empty() || feed.name.size() > 64) {
        return false;
    }
    if (feed.url.size() < 8 || feed.url.size() > 256) {
        return false;
    }
    if (feed.url.rfind("http://", 0) != 0 && feed.url.rfind("https://", 0) != 0) {
        return false;
    }
    return feed.url.find(' ') == std::string::npos;
}

// Trims the editable fields and drops empty or duplicate aliases.
void NormalizeFeed(RssFeed* feed) {
    feed->name = Trim(feed->name);
    feed->url = Trim(feed->url);
    feed->category = Trim(feed->category);
    // Bytes, not characters: 32 was only ~16 Cyrillic letters, so a category
    // like "Технологии/Наука" was silently cut in half. Keep it in line with
    // the radio genre and station name limits.
    if (feed->category.size() > 64) {
        feed->category = TruncateUtf8(feed->category, 64);
    }
    std::vector<std::string> aliases;
    for (const auto& raw : feed->aliases) {
        std::string alias = Trim(raw);
        if (alias.empty() || alias.size() > 32) {
            continue;
        }
        if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end()) {
            aliases.push_back(std::move(alias));
        }
        if (aliases.size() >= 8) {
            break;
        }
    }
    feed->aliases = std::move(aliases);
}

}  // namespace

void RssReader::Initialize() {
    std::map<std::string, RssFeed> feeds;
    const bool loaded = LoadFeedsLocked(feeds);
    std::lock_guard<std::mutex> lock(mutex_);
    feeds_ = std::move(feeds);
    if (!loaded) {
        ESP_LOGI(TAG, "No saved RSS feeds, starting with an empty list");
    }
    ESP_LOGI(TAG, "Initialized %d RSS feeds", static_cast<int>(feeds_.size()));
}

bool RssReader::LoadFeedsLocked(std::map<std::string, RssFeed>& feeds) const {
    Settings settings(kNamespace);
    const std::string saved = settings.GetString("feeds");
    if (saved.empty()) {
        return false;
    }

    CJsonUniquePtr root(cJSON_Parse(saved.c_str()));
    if (root == nullptr) {
        ESP_LOGE(TAG, "Saved feed list is not valid JSON, ignoring it");
        return false;
    }
    cJSON* array = cJSON_GetObjectItem(root.get(), "feeds");
    if (!cJSON_IsArray(array)) {
        ESP_LOGE(TAG, "Saved feed list has no feeds array, ignoring it");
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }
        cJSON* key = cJSON_GetObjectItem(item, "key");
        cJSON* name = cJSON_GetObjectItem(item, "name");
        cJSON* url = cJSON_GetObjectItem(item, "url");
        if (!cJSON_IsString(key) || !cJSON_IsString(name) || !cJSON_IsString(url)) {
            continue;
        }
        RssFeed feed;
        feed.name = name->valuestring;
        feed.url = url->valuestring;
        if (cJSON* category = cJSON_GetObjectItem(item, "category");
            cJSON_IsString(category)) {
            feed.category = category->valuestring;
        }
        if (cJSON* aliases = cJSON_GetObjectItem(item, "aliases"); cJSON_IsArray(aliases)) {
            cJSON* alias = nullptr;
            cJSON_ArrayForEach(alias, aliases) {
                if (cJSON_IsString(alias)) {
                    feed.aliases.emplace_back(alias->valuestring);
                }
            }
        }
        NormalizeFeed(&feed);
        const std::string feed_key = key->valuestring;
        if (feed_key.empty() || feed_key.size() > 32 || !IsValidFeed(feed)) {
            continue;
        }
        feeds[feed_key] = std::move(feed);
    }
    return !feeds.empty();
}

bool RssReader::SaveFeedsLocked(const std::map<std::string, RssFeed>& feeds) const {
    CJsonUniquePtr root(cJSON_CreateObject());
    CJsonUniquePtr array(cJSON_CreateArray());
    if (root == nullptr || array == nullptr) {
        return false;
    }
    for (const auto& [key, feed] : feeds) {
        CJsonUniquePtr item(cJSON_CreateObject());
        if (item == nullptr) {
            return false;
        }
        CJsonUniquePtr aliases(cJSON_CreateArray());
        if (aliases == nullptr) {
            return false;
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
    if (text == nullptr) {
        return false;
    }
    Settings settings(kNamespace, true);
    if (!settings.SetString("feeds", text.get())) {
        ESP_LOGE(TAG, "Failed to save RSS feed list to NVS");
        return false;
    }
    return true;
}

std::vector<std::pair<std::string, RssFeed>> RssReader::GetFeeds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::vector<std::pair<std::string, RssFeed>>(feeds_.begin(), feeds_.end());
}

bool RssReader::GetFeed(const std::string& key, RssFeed* feed) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = feeds_.find(key);
    if (it == feeds_.end()) {
        return false;
    }
    if (feed != nullptr) {
        *feed = it->second;
    }
    return true;
}

std::string RssReader::MakeKey(const std::string& name,
                               const std::map<std::string, RssFeed>& existing) {
    std::string base;
    for (char c : name) {
        unsigned char u = static_cast<unsigned char>(c);
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
        base = "FEED";
    }
    std::string key = base;
    for (int i = 2; existing.count(key) > 0 && i < 1000; ++i) {
        key = base + std::to_string(i);
    }
    return key;
}

std::string RssReader::MakeUniqueKeyLocked(const std::string& name) const {
    return MakeKey(name, feeds_);
}

std::string RssReader::ResolveFeedKeyLocked(const std::string& query) const {
    if (feeds_.empty()) {
        return "";
    }
    const std::string needle = ToLowerAscii(Trim(query));
    if (needle.empty()) {
        return feeds_.begin()->first;
    }
    // Same order as the radio: aliases first, then the name, both by substring.
    // Keys are internal identities and never take part in the search.
    for (const auto& [key, feed] : feeds_) {
        for (const auto& alias : feed.aliases) {
            const std::string alias_lower = ToLowerAscii(alias);
            if (!alias_lower.empty() && alias_lower.find(needle) != std::string::npos) {
                return key;
            }
        }
    }
    for (const auto& [key, feed] : feeds_) {
        const std::string name = ToLowerAscii(feed.name);
        if (!name.empty() && (name.find(needle) != std::string::npos ||
                              needle.find(name) != std::string::npos)) {
            return key;
        }
    }
    // A category name works too ("спорт"): one matching feed is taken as is,
    // several feeds in the category mean a random one of them is read.
    std::vector<std::string> category_keys;
    for (const auto& [key, feed] : feeds_) {
        if (feed.category.empty()) {
            continue;
        }
        const std::string category = ToLowerAscii(feed.category);
        if (category == needle || category.find(needle) != std::string::npos) {
            category_keys.push_back(key);
        }
    }
    if (category_keys.size() == 1) {
        return category_keys.front();
    }
    if (!category_keys.empty()) {
        return category_keys[esp_random() % category_keys.size()];
    }
    return "";
}

std::string RssReader::ResolveFeedKey(const std::string& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ResolveFeedKeyLocked(query);
}

bool RssReader::IsValidKey(const std::string& key) {
    if (key.empty() || key.size() > 32) {
        return false;
    }
    for (char c : key) {
        unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '_' && c != '-') {
            return false;
        }
        if (u >= 0x80) {
            return false;
        }
    }
    return true;
}

bool RssReader::AddFeed(const RssFeed& feed_in, const std::string& key_in,
                        std::string* key_out) {
    RssFeed feed = feed_in;
    NormalizeFeed(&feed);
    if (!IsValidFeed(feed)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(feeds_.size()) >= kMaxFeeds) {
        return false;
    }
    std::string key = Trim(key_in);
    if (key.empty()) {
        key = MakeUniqueKeyLocked(feed.name);
    } else if (!IsValidKey(key) || feeds_.count(key) > 0) {
        return false;
    }
    std::map<std::string, RssFeed> next = feeds_;
    next[key] = feed;
    if (!SaveFeedsLocked(next)) {
        return false;
    }
    feeds_.swap(next);
    if (key_out != nullptr) {
        *key_out = key;
    }
    return true;
}

bool RssReader::UpdateFeed(const std::string& key, const RssFeed& feed_in) {
    RssFeed feed = feed_in;
    NormalizeFeed(&feed);
    if (!IsValidFeed(feed)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (feeds_.find(key) == feeds_.end()) {
        return false;
    }
    std::map<std::string, RssFeed> next = feeds_;
    next[key] = feed;
    if (!SaveFeedsLocked(next)) {
        return false;
    }
    feeds_.swap(next);
    return true;
}

bool RssReader::DeleteFeed(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (feeds_.find(key) == feeds_.end()) {
        return false;
    }
    std::map<std::string, RssFeed> next = feeds_;
    next.erase(key);
    if (!SaveFeedsLocked(next)) {
        return false;
    }
    feeds_.swap(next);
    return true;
}

bool RssReader::ReplaceFeeds(const std::vector<std::pair<std::string, RssFeed>>& feeds,
                             int* imported, std::string& error) {
    error.clear();
    std::map<std::string, RssFeed> next;
    for (const auto& [raw_key, raw_feed] : feeds) {
        RssFeed feed = raw_feed;
        NormalizeFeed(&feed);
        if (!IsValidFeed(feed)) {
            error = "Лента без названия или с неверным адресом: " +
                    (feed.name.empty() ? feed.url : feed.name);
            return false;
        }
        if (static_cast<int>(next.size()) >= kMaxFeeds) {
            error = "Слишком много лент: не больше " + std::to_string(kMaxFeeds);
            return false;
        }
        const std::string key_in = Trim(raw_key);
        std::string key = key_in.empty() ? MakeKey(feed.name, next) : key_in;
        if (!IsValidKey(key)) {
            error = "Неверный ключ ленты: " + key + " (латиница, цифры, _ и -, до 32)";
            return false;
        }
        if (next.count(key) > 0) {
            key = MakeKey(feed.name, next);
        }
        next[key] = std::move(feed);
    }
    if (next.empty()) {
        error = "Список лент пуст";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!SaveFeedsLocked(next)) {
        error = "Не удалось сохранить список лент";
        return false;
    }
    feeds_.swap(next);
    if (imported != nullptr) {
        *imported = static_cast<int>(feeds_.size());
    }
    return true;
}

bool RssReader::FetchFeed(const std::string& key, int count, std::vector<RssItem>& items,
                          std::string& error) {
    RssFeed feed;
    if (!GetFeed(key, &feed)) {
        error = "Лента не найдена";
        return false;
    }
    count = std::clamp(count, 1, 20);

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
    http->SetTimeout(kFetchTimeoutMs);
    http->SetHeader("User-Agent", "XiaoZhi-RSS/1.0");
    http->SetHeader("Accept",
                    "application/rss+xml, application/atom+xml, application/xml, text/xml, */*");

    if (auto opened = http->Open("GET", feed.url); !opened) {
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

    // Read at most kMaxFeedBytes: some feeds carry long article bodies, but the
    // first entries always come first and only their titles matter here.
    std::string body;
    body.reserve(16384);
    char buffer[512];
    while (body.size() < kMaxFeedBytes) {
        auto read = http->Read(buffer, sizeof(buffer));
        if (!read) {
            ESP_LOGW(TAG, "Read from %s failed: %s", feed.url.c_str(),
                     read.error().ToString().c_str());
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
    if (!ParseFeedItems(body, count, items)) {
        error = "В ответе не нашлось новостей (это точно RSS-лента?)";
        return false;
    }
    ESP_LOGI(TAG, "Fetched %d items from %s", static_cast<int>(items.size()), feed.name.c_str());
    return true;
}

bool RssReader::FetchLatest(const std::string& feed_query, int count,
                            std::vector<std::pair<std::string, RssItem>>& items,
                            std::vector<std::string>& errors) {
    count = std::clamp(count, 1, 20);
    items.clear();
    errors.clear();

    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (feeds_.empty()) {
            errors.push_back("Ленты не настроены");
            return false;
        }
        const std::string query = Trim(feed_query);
        if (query.empty() || ToLowerAscii(query) == "all") {
            for (const auto& [key, feed] : feeds_) {
                keys.push_back(key);
                if (static_cast<int>(keys.size()) >= kMaxFeedsPerRequest) {
                    break;
                }
            }
        } else {
            const std::string key = ResolveFeedKeyLocked(query);
            if (key.empty()) {
                errors.push_back("Лента не найдена");
                return false;
            }
            keys.push_back(key);
        }
    }

    // Spread the requested number of headlines over the feeds so that reading
    // "all" stays fast instead of paginating through every feed.
    const int per_feed = std::max(1, count / static_cast<int>(keys.size()) + 1);
    bool any = false;
    for (const auto& key : keys) {
        RssFeed feed;
        if (!GetFeed(key, &feed)) {
            continue;
        }
        std::vector<RssItem> feed_items;
        std::string error;
        if (!FetchFeed(key, std::min(per_feed, count), feed_items, error)) {
            errors.push_back(feed.name + ": " + error);
            continue;
        }
        any = true;
        for (auto& item : feed_items) {
            if (static_cast<int>(items.size()) >= count) {
                break;
            }
            items.emplace_back(feed.name, std::move(item));
        }
        if (static_cast<int>(items.size()) >= count) {
            break;
        }
    }
    return any;
}

std::string RssReader::ReadHeadlines(const std::string& feed_query, int count) {
    count = std::clamp(count, 1, 10);

    std::string key;
    RssFeed feed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (feeds_.empty()) {
            return "RSS-ленты не настроены. Их можно добавить в веб-интерфейсе устройства.";
        }
        key = ResolveFeedKeyLocked(feed_query);
        if (key.empty()) {
            std::string names;
            for (const auto& [feed_key, feed_value] : feeds_) {
                if (!names.empty()) {
                    names += ", ";
                }
                names += feed_value.name;
            }
            return "Лента не найдена. Доступные RSS-ленты: " + names;
        }
        feed = feeds_.at(key);
    }

    std::vector<RssItem> items;
    std::string error;
    if (!FetchFeed(key, count, items, error)) {
        return "Не удалось прочитать ленту " + feed.name + ": " + error;
    }

    std::string text = "Последние новости — " + feed.name + ":";
    for (size_t i = 0; i < items.size(); ++i) {
        text += "\n" + std::to_string(i + 1) + ". " + items[i].title;
    }
    return text;
}
