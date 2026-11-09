#ifndef RSS_READER_H
#define RSS_READER_H

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// One RSS/Atom subscription. The device keeps only the feed list; articles are
// fetched on demand so the device never stores news on flash.
struct RssFeed {
    std::string name;
    std::string url;
    // Optional grouping label, e.g. "новости" or "технологии".
    std::string category;
    // Extra spoken names used to pick this feed by voice.
    std::vector<std::string> aliases;
};

// One parsed article. Only the fields that can be spoken to the user are kept.
struct RssItem {
    std::string title;
    std::string link;
    std::string published;
};

// Reads RSS 2.0 and Atom feeds over the board's NetworkInterface.
//
// The feed list lives in NVS (namespace "rss") and is managed from the device
// Web UI. Fetching blocks the calling task for up to a few seconds, so call it
// from the MCP or HTTP server task, never from the main event loop or audio
// tasks.
class RssReader {
public:
    // Loads the saved feed list. The list starts empty on a fresh device.
    void Initialize();

    std::vector<std::pair<std::string, RssFeed>> GetFeeds() const;
    bool GetFeed(const std::string& key, RssFeed* feed) const;

    // Feed keys are storage identities: 1-32 chars of A-Z a-z 0-9 _ -.
    static bool IsValidKey(const std::string& key);

    // Resolves a user-spoken name ("лента", "РИА") to a feed key; empty if unknown.
    std::string ResolveFeedKey(const std::string& query) const;

    // Adds a feed and returns its key. An empty `key` is generated from the feed
    // name; an explicit key must be valid and unused. Returns false when the
    // feed is invalid, the key is taken, the list is full or NVS cannot be
    // written.
    bool AddFeed(const RssFeed& feed, const std::string& key, std::string* key_out);
    bool UpdateFeed(const std::string& key, const RssFeed& feed);
    bool DeleteFeed(const std::string& key);

    // Replaces the whole list (Web UI import). An empty key is generated from
    // the feed name; provided keys must be valid and unique.
    bool ReplaceFeeds(const std::vector<std::pair<std::string, RssFeed>>& feeds, int* imported,
                      std::string& error);

    // Fetches one feed and returns up to `count` newest entries.
    bool FetchFeed(const std::string& key, int count, std::vector<RssItem>& items,
                   std::string& error);

    // Newest entries from one feed (or from every feed when `feed_query` is
    // empty or "all"), at most `count` in total. "feed" in the result is the
    // feed display name.
    bool FetchLatest(const std::string& feed_query, int count,
                     std::vector<std::pair<std::string, RssItem>>& items,
                     std::vector<std::string>& errors);

    // Headlines as plain text for the assistant to read aloud.
    std::string ReadHeadlines(const std::string& feed_query, int count);

private:
    bool LoadFeedsLocked(std::map<std::string, RssFeed>& feeds) const;
    bool SaveFeedsLocked(const std::map<std::string, RssFeed>& feeds) const;
    std::string ResolveFeedKeyLocked(const std::string& query) const;
    std::string MakeUniqueKeyLocked(const std::string& name) const;
    static std::string MakeKey(const std::string& name,
                               const std::map<std::string, RssFeed>& existing);

    mutable std::mutex mutex_;
    std::map<std::string, RssFeed> feeds_;
};

#endif  // RSS_READER_H
