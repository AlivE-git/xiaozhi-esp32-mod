#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <esp_http_server.h>

#include <cstddef>
#include <string>

// HTTP server behind the device page. It owns the socket and the page itself and
// serves the endpoints of the device rather than of one feature: the page, the
// countdown timer, the RSS feeds, the skill settings and the system tab (volume,
// reboot, availability checks). Feature
// modules add their own endpoints after it starts, e.g. the radio registers
// /api/stations from RadioWebApi::Register().
//
// Pages: GET /          - the device page (radio, news, timer, skills, system, wake-on-lan)
//        GET  /api/timer                 - {running, remainingSeconds, remaining}
//        POST /api/timer?seconds=N       - start (or replace) the countdown timer
//        POST /api/timer/stop            - cancel the running countdown timer
//        GET  /api/rss                   - list RSS feeds as JSON (also used for export)
//        POST /api/rss                   - add a feed (optional key, name, url, category,
//                                          aliases); a {"feeds":[...]} body replaces the
//                                          whole list (import)
//        PUT  /api/rss                   - edit a feed by key
//        DELETE /api/rss?key=KEY         - delete a feed
//        GET  /api/skills                - every registered skill with its read_only flag
//        POST /api/skills                - replace the read-only skill list
//                                          ({"read_only":[...]})
//        GET  /api/system                - {volume}
//        POST /api/system                - set the speaker volume ({"volume":0..100})
//        POST /api/system/reboot         - reboot the device
//        GET  /api/system/check?service=joke|currency
//                                        - fetch from that service and report {ok} or 502
namespace web {

// Request bodies are small JSON documents; anything larger is rejected instead
// of being buffered.
constexpr size_t kMaxBodySize = 16384;

// Builds {"error":message}, the error shape every endpoint uses.
std::string ErrorJson(const std::string& message);

esp_err_t SendJson(httpd_req_t* req, int status, const std::string& body);
// Sends {"error":message} with the given status.
esp_err_t SendError(httpd_req_t* req, int status, const std::string& message);

// Reads the request body into `body`. Returns false after replying 400 itself
// when the body is empty, too large or cannot be read.
bool ReadJsonBody(httpd_req_t* req, std::string& body);

}  // namespace web

class WebServer {
public:
    WebServer();
    ~WebServer();

    // Starts httpd and registers the endpoints above plus those of the feature
    // modules. Safe to call again after a network restart: it is a no-op while
    // the server is running.
    bool Start(int port = 8081);
    void Stop();

    // Registers one endpoint of a feature module. Call it only while the server
    // is running; httpd keeps a copy of the arguments.
    bool AddHandler(const httpd_uri_t& uri);

private:
    httpd_handle_t server_handle_ = nullptr;

    static WebServer* instance_;

    static esp_err_t PageHandler(httpd_req_t* req);
    static esp_err_t TimerHandler(httpd_req_t* req);
    static esp_err_t TimerSetHandler(httpd_req_t* req);
    static esp_err_t TimerStopHandler(httpd_req_t* req);
    static esp_err_t RssHandler(httpd_req_t* req);
    static esp_err_t RssSaveHandler(httpd_req_t* req);
    static esp_err_t RssDeleteHandler(httpd_req_t* req);
    static esp_err_t SkillsHandler(httpd_req_t* req);
    static esp_err_t SystemHandler(httpd_req_t* req);
    static esp_err_t RebootHandler(httpd_req_t* req);
    static esp_err_t CheckHandler(httpd_req_t* req);

    esp_err_t HandlePage(httpd_req_t* req);
    esp_err_t HandleTimer(httpd_req_t* req);
    esp_err_t HandleTimerSet(httpd_req_t* req);
    esp_err_t HandleTimerStop(httpd_req_t* req);
    esp_err_t HandleRss(httpd_req_t* req);
    esp_err_t HandleRssSave(httpd_req_t* req);
    esp_err_t HandleRssDelete(httpd_req_t* req);
    esp_err_t HandleSkills(httpd_req_t* req);
    esp_err_t HandleSystem(httpd_req_t* req);
    esp_err_t HandleReboot(httpd_req_t* req);
    esp_err_t HandleCheck(httpd_req_t* req);

    std::string FeedsJson() const;
    std::string SkillsJson() const;
};

#endif  // WEB_SERVER_H
