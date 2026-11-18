#ifndef WEB_SEARCH_H
#define WEB_SEARCH_H

#include <string>

// Web search through a Google Apps Script proxy. Tavily answers 403 from Russian
// IPs, so the script calls it from Google's servers and returns the ready answer
// as plain text (only=1, plus adv=1 for a detailed one).
//
// Both values are set by the user in the "Система" tab of the device page: the
// deployment id is the part of the script URL between /s/ and /exec.
//
// Search() blocks the calling task for up to ~15 seconds, so call it from the MCP
// or HTTP server task, never from the main event loop or audio tasks.
namespace websearch {

struct Config {
    std::string deployment_id;
    std::string token;
};

// Reads the saved values; both are empty until the user fills the System tab.
Config Load();

// Validates and saves the values. An empty pair turns the feature off. Values
// must be URL-safe (latin letters, digits, '.', '_', '~', '-') because they end
// up in the request URL.
bool Save(const std::string& deployment_id, const std::string& token, std::string& error);

bool IsConfigured();

// Asks the script and returns its plain-text answer. `detailed` sends adv=1.
// On failure returns false and fills `error` with a short Russian message.
bool Search(const std::string& query, bool detailed, std::string& answer, std::string& error);

// The answer as the assistant should read it aloud; on failure a spoken-style
// message explaining what to fix.
std::string Ask(const std::string& query, bool detailed);

}  // namespace websearch

#endif  // WEB_SEARCH_H
