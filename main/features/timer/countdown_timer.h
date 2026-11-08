#ifndef COUNTDOWN_TIMER_H
#define COUNTDOWN_TIMER_H

#include <cstdint>
#include <mutex>
#include <string>

// Countdown timer for the voice assistant. The remaining time is derived from a
// monotonic end timestamp, so the countdown keeps running through conversations,
// radio playback and display updates, and its value is shown in the status line
// while the device is idle.
class CountdownTimer {
public:
    // Upper bound accepted from MCP: one day.
    static constexpr int kMaxDurationSeconds = 24 * 60 * 60;

    CountdownTimer() = default;
    ~CountdownTimer() = default;

    // Starts a new countdown, replacing any running one. Returns false when the
    // duration is out of range.
    bool Start(int duration_seconds);

    // Cancels the countdown without ringing. Returns false when none was running.
    bool Stop();

    bool IsRunning() const;

    // Whole seconds left, rounded up so the last second is shown as 00:01.
    int GetRemainingSeconds() const;

    // "MM:SS", or "H:MM:SS" above one hour. Empty when the timer is not running.
    std::string FormatRemaining() const;

    // Advances the countdown and refreshes the display. Called once per second
    // from the main task (Application clock tick).
    void Tick();

private:
    void ShowRemaining(int remaining_seconds);
    void ShowAlarm();

    // Short "timer set" confirmation on the subtitle line. It lives in the same
    // line as the chat text, so it is removed after kStartMessageTicks ticks
    // instead of occupying the line for the whole countdown.
    void ShowStartMessage(int duration_seconds);
    void TickStartMessage();
    void HideStartMessage();

    static constexpr int kStartMessageTicks = 10;

    mutable std::mutex mutex_;
    bool running_ = false;
    int64_t end_time_us_ = 0;
    std::string start_message_;
    int start_message_ticks_ = 0;
};

#endif  // COUNTDOWN_TIMER_H
