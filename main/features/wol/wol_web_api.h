#ifndef WOL_WEB_API_H
#define WOL_WEB_API_H

class WebServer;

// HTTP API of the Wake-on-LAN computer list, used by the "Wake-on-LAN" tab of
// the device page. The list itself, its storage and the packet belong to the
// wol module; this only translates between HTTP/JSON and it.
//
// Endpoints:
//   GET    /api/wol          - {computers:[{key, name, mac, broadcast, port, default}]}
//   POST   /api/wol          - add a computer ({name, mac, broadcast?, port?, default?});
//                              a {"computers":[...]} body (or a bare array) replaces the
//                              whole list, which is what the import button sends
//   PUT    /api/wol?key=KEY  - edit a computer; default:true also makes it the one
//                              that "включи компьютер" without a name wakes
//   DELETE /api/wol?key=KEY  - delete a computer
//   POST   /api/wol/wake     - wake the default computer, or ?key=KEY to pick one
namespace WolWebApi {

// Adds the endpoints above to a running WebServer.
void Register(WebServer& server);

}  // namespace WolWebApi

#endif  // WOL_WEB_API_H
