#ifndef RADIO_WEB_API_H
#define RADIO_WEB_API_H

class WebServer;

// HTTP API of the radio station list, used by the radio tab of the device page.
// The station list itself, its playback and its storage belong to Esp32Radio;
// this module only translates between HTTP/JSON and that class.
//
// Endpoints:
//   GET    /api/stations          - stations as JSON (also used for export)
//   POST   /api/stations          - add a station
//   PUT    /api/stations?key=KEY  - edit a station
//   DELETE /api/stations?key=KEY  - delete a station
//   POST   /api/import            - replace the whole list from an exported JSON file
//   GET    /api/status            - {playing, currentKey, currentStation}
//   POST   /api/play?key=KEY      - start playing a station by key
//                                   (also wakes the display from sleep)
//   POST   /api/stop              - stop playback
namespace RadioWebApi {

// Adds the endpoints above to a running WebServer. They all answer with an error
// while the radio does not exist yet, so only call this when the device page is
// up rather than when the radio is created.
void Register(WebServer& server);

}  // namespace RadioWebApi

#endif  // RADIO_WEB_API_H
