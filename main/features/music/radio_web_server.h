#ifndef RADIO_WEB_SERVER_H
#define RADIO_WEB_SERVER_H

#include <esp_http_server.h>

#include <string>

class Esp32Radio;
class RssReader;

// Small HTTP server with a Web UI for managing the radio station list
// (add / edit / delete stations and their voice-search aliases), the countdown
// timer and the RSS news feeds.
// Pages: GET /           - station management page
//        GET    /api/stations - list stations as JSON (also used for export)
//        POST   /api/stations - add a station
//        PUT    /api/stations?key=KEY - edit a station
//        DELETE /api/stations?key=KEY - delete a station
//        POST   /api/import   - replace the whole list from an exported JSON file
//        GET    /api/status   - {playing, currentKey, currentStation}
//        POST   /api/play?key=KEY - start playing a station by key
//                                 (also wakes the display from sleep)
//        POST   /api/stop     - stop playback
//        GET    /api/timer    - {running, remainingSeconds, remaining}
//        POST   /api/timer?seconds=N - start (or replace) the countdown timer
//        POST   /api/timer/stop - cancel the running countdown timer
//        GET    /api/rss      - list RSS feeds as JSON (also used for export)
//        GET    /api/rss?latest=1&feed=KEY&count=N - newest headlines
//                               (feed empty = all feeds)
//        POST   /api/rss      - add a feed (optional key, name, url, category,
//                               aliases); a {"feeds":[...]} body replaces the
//                               whole list (import)
//        PUT    /api/rss      - edit a feed by key
//        DELETE /api/rss?key=KEY - delete a feed
class RadioWebServer {
public:
    RadioWebServer();
    ~RadioWebServer();

    bool Start(Esp32Radio* radio, RssReader* rss = nullptr, int port = 8081);
    void Stop();

private:
    httpd_handle_t server_handle_ = nullptr;
    Esp32Radio* radio_ = nullptr;
    RssReader* rss_ = nullptr;

    static RadioWebServer* instance_;

    static esp_err_t PageHandler(httpd_req_t* req);
    static esp_err_t StationsHandler(httpd_req_t* req);
    static esp_err_t ImportHandler(httpd_req_t* req);
    static esp_err_t StatusHandler(httpd_req_t* req);
    static esp_err_t PlayHandler(httpd_req_t* req);
    static esp_err_t StopHandler(httpd_req_t* req);
    static esp_err_t TimerHandler(httpd_req_t* req);
    static esp_err_t TimerSetHandler(httpd_req_t* req);
    static esp_err_t TimerStopHandler(httpd_req_t* req);
    static esp_err_t RssHandler(httpd_req_t* req);
    static esp_err_t RssSaveHandler(httpd_req_t* req);
    static esp_err_t RssDeleteHandler(httpd_req_t* req);

    esp_err_t HandlePage(httpd_req_t* req);
    esp_err_t HandleStations(httpd_req_t* req);
    esp_err_t HandleImport(httpd_req_t* req);
    esp_err_t HandleStatus(httpd_req_t* req);
    esp_err_t HandlePlay(httpd_req_t* req);
    esp_err_t HandleStop(httpd_req_t* req);
    esp_err_t HandleTimer(httpd_req_t* req);
    esp_err_t HandleTimerSet(httpd_req_t* req);
    esp_err_t HandleTimerStop(httpd_req_t* req);
    esp_err_t HandleRss(httpd_req_t* req);
    esp_err_t HandleRssSave(httpd_req_t* req);
    esp_err_t HandleRssDelete(httpd_req_t* req);

    std::string StationsJson() const;
    std::string FeedsJson() const;
    esp_err_t SendJson(httpd_req_t* req, int status, const std::string& body);
};

#endif  // RADIO_WEB_SERVER_H
