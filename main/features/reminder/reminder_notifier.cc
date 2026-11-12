#include "reminder_notifier.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "device_state.h"
#include "display/display.h"
#include "features/radio/esp32_radio.h"
#include "features/reminder/reminder_store.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>
#include <string>
#include <vector>

#define TAG "Reminder"

namespace {

constexpr int kCheckIntervalMs = 10000;
constexpr int kRadioResumeDelayMs = 9000;
constexpr int kNotifierStackSize = 6144;

TaskHandle_t task_handle = nullptr;

// The subtitle line is cleared by the clock on every tick, so the reminder is
// pushed onto the assistant line, which the clock leaves alone.
void Announce(const std::string& text, const std::string& station, bool resume_radio) {
    Application::GetInstance().Schedule([text, resume_radio]() {
        auto& app = Application::GetInstance();
        if (resume_radio) {
            if (auto radio = app.GetRadio(); radio != nullptr) {
                radio->Stop();
            }
        }
        if (auto display = Board::GetInstance().GetDisplay(); display != nullptr) {
            display->SetChatMessage("assistant", text.c_str());
        }
        app.PlaySound(Lang::Sounds::OGG_TIMER);
        ESP_LOGI(TAG, "Reminder: %s", text.c_str());
    });

    if (resume_radio && !station.empty()) {
        vTaskDelay(pdMS_TO_TICKS(kRadioResumeDelayMs));
        Application::GetInstance().Schedule([station]() {
            if (auto radio = Application::GetInstance().GetRadio(); radio != nullptr) {
                radio->PlayStation(station);
            }
        });
    }
}

void NotifierTask(void*) {
    while (true) {
        // A reminder must not talk over a conversation or a reply being played,
        // so a due one waits for the next pass instead of being dropped.
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            std::vector<reminder::Item> due;
            if (reminder::TakeDue(time(nullptr), due)) {
                Board::GetInstance().WakeUp();
                for (const auto& item : due) {
                    std::string station;
                    bool resume_radio = false;
                    if (auto radio = Application::GetInstance().GetRadio();
                        radio != nullptr && radio->IsPlaying()) {
                        station = radio->GetCurrentStation();
                        resume_radio = !station.empty();
                    }
                    Announce("Напоминание: " + item.text, station, resume_radio);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kCheckIntervalMs));
    }
}

}  // namespace

namespace reminder {

void StartNotifier() {
    if (task_handle != nullptr) {
        return;
    }
    const BaseType_t created = xTaskCreate(
        [](void*) {
            NotifierTask(nullptr);
            vTaskDelete(nullptr);
        },
        "reminder_notify", kNotifierStackSize, nullptr, 3, &task_handle);
    if (created != pdPASS) {
        task_handle = nullptr;
        ESP_LOGE(TAG, "Failed to start the reminder task");
        return;
    }
    ESP_LOGI(TAG, "Reminder task started");
}

}  // namespace reminder
