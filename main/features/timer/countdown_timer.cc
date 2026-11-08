#include "countdown_timer.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display/display.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <cstdio>

#define TAG "CountdownTimer"

namespace {

constexpr int64_t kMicrosecondsPerSecond = 1000000;

// Round up: 00:01 stays visible during the last second instead of 00:00.
int RemainingSecondsFrom(int64_t remaining_us) {
    if (remaining_us <= 0) {
        return 0;
    }
    return static_cast<int>((remaining_us + kMicrosecondsPerSecond - 1) / kMicrosecondsPerSecond);
}

std::string FormatSeconds(int seconds) {
    char buffer[16];
    if (seconds >= 3600) {
        snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", seconds / 3600, (seconds % 3600) / 60,
                 seconds % 60);
    } else {
        snprintf(buffer, sizeof(buffer), "%02d:%02d", seconds / 60, seconds % 60);
    }
    return buffer;
}

}  // namespace

bool CountdownTimer::Start(int duration_seconds) {
    if (duration_seconds <= 0 || duration_seconds > kMaxDurationSeconds) {
        ESP_LOGW(TAG, "Rejected timer duration: %d s", duration_seconds);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        end_time_us_ =
            esp_timer_get_time() + static_cast<int64_t>(duration_seconds) * kMicrosecondsPerSecond;
        running_ = true;
    }

    ESP_LOGI(TAG, "Timer started for %d s", duration_seconds);
    ShowRemaining(duration_seconds);
    return true;
}

bool CountdownTimer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return false;
        }
        running_ = false;
    }

    ESP_LOGI(TAG, "Timer stopped");
    // Hand the status line back to the clock. UpdateStatusBar() picks it up on the
    // next clock tick because it was told to refresh as soon as the timer ends.
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (auto display = Board::GetInstance().GetDisplay()) {
            display->SetStatus(Lang::Strings::STANDBY);
        }
    }
    return true;
}

bool CountdownTimer::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

int CountdownTimer::GetRemainingSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return 0;
    }
    return RemainingSecondsFrom(end_time_us_ - esp_timer_get_time());
}

std::string CountdownTimer::FormatRemaining() const {
    int remaining_seconds = GetRemainingSeconds();
    if (remaining_seconds <= 0) {
        return {};
    }
    return FormatSeconds(remaining_seconds);
}

void CountdownTimer::Tick() {
    int remaining_seconds = 0;
    bool expired = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }
        int64_t remaining_us = end_time_us_ - esp_timer_get_time();
        if (remaining_us <= 0) {
            running_ = false;
            expired = true;
        } else {
            remaining_seconds = RemainingSecondsFrom(remaining_us);
        }
    }

    if (expired) {
        ESP_LOGI(TAG, "Timer expired");
        ShowAlarm();
        return;
    }

    ShowRemaining(remaining_seconds);
}

void CountdownTimer::ShowRemaining(int remaining_seconds) {
    if (remaining_seconds <= 0) {
        return;
    }

    // Outside the idle state the status line belongs to the state text
    // ("Слушаю...", "Говорю..."), so the countdown is only drawn when idle.
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }
    auto text = FormatSeconds(remaining_seconds);
    display->SetStatus(text.c_str());
}

void CountdownTimer::ShowAlarm() {
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        // The subtitle line is left alone by the clock, so the alarm stays
        // readable while the status line returns to the current time.
        display->SetChatMessage("assistant", "Таймер сработал");
    }
    Application::GetInstance().PlaySound(Lang::Sounds::OGG_TIMER);
}
