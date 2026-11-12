#ifndef REMINDER_WEB_API_H
#define REMINDER_WEB_API_H

class WebServer;

// Registers the reminder endpoints of the device page:
//   GET    /api/reminders              - {reminders:[{text, when, due}]}
//   POST   /api/reminders              - add one from {text, minutes} or {text, at}
//   DELETE /api/reminders?index=N      - remove the reminder shown at that position
namespace reminder {

class ReminderWebApi {
public:
    static void Register(WebServer& server);
};

}  // namespace reminder

#endif  // REMINDER_WEB_API_H
