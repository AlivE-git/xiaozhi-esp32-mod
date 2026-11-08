#ifndef RADIO_WEB_SERVER_H
#define RADIO_WEB_SERVER_H

#include <esp_http_server.h>

#include <string>

class Esp32Radio;

// Small HTTP server with a Web UI for managing the radio station list
// (add / edit / delete stations and their voice-search aliases).
// Pages: GET /           - station management page
//        GET    /api/stations - list stations as JSON (also used for export)
//        POST   /api/stations - add a station
//        PUT    /api/stations?key=KEY - edit a station
//        DELETE /api/stations?key=KEY - delete a station
//        POST   /api/import   - replace the whole list from an exported JSON file
class RadioWebServer {
public:
    RadioWebServer();
    ~RadioWebServer();

    bool Start(Esp32Radio* radio, int port = 8081);
    void Stop();

private:
    httpd_handle_t server_handle_ = nullptr;
    Esp32Radio* radio_ = nullptr;

    static RadioWebServer* instance_;

    static esp_err_t PageHandler(httpd_req_t* req);
    static esp_err_t StationsHandler(httpd_req_t* req);
    static esp_err_t ImportHandler(httpd_req_t* req);

    esp_err_t HandlePage(httpd_req_t* req);
    esp_err_t HandleStations(httpd_req_t* req);
    esp_err_t HandleImport(httpd_req_t* req);

    std::string StationsJson() const;
    esp_err_t SendJson(httpd_req_t* req, int status, const std::string& body);
};

#endif  // RADIO_WEB_SERVER_H
