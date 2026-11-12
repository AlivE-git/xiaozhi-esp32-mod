#ifndef REMINDER_NOTIFIER_H
#define REMINDER_NOTIFIER_H

// Watches the reminders stored on the device and announces the due ones with a
// sound and a subtitle line. Runs without the network: only the clock matters.
namespace reminder {

// Starts the watching task. Safe to call more than once; a failed start is
// logged and retried by the next call.
void StartNotifier();

}  // namespace reminder

#endif  // REMINDER_NOTIFIER_H
