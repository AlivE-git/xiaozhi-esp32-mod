#ifndef WEB_SEARCH_WEB_API_H
#define WEB_SEARCH_WEB_API_H

class WebServer;

// Endpoints behind the "Веб-поиск" card of the System tab:
//   GET  /api/websearch - {deployment_id, token, configured}
//   POST /api/websearch - save them ({"deployment_id":"...","token":"..."})
class WebSearchWebApi {
public:
    static void Register(WebServer& server);
};

#endif  // WEB_SEARCH_WEB_API_H
