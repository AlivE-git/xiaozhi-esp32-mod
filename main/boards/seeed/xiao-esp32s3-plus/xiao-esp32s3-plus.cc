#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "led/gpio_led.h"
#include "power_save_timer.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/touch_sens.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "XiaoEsp32s3PlusBoard"

// static_assert(TOUCH_BUTTON_CHANNEL == static_cast<int>(TOUCH_BUTTON_GPIO),
//               "On ESP32-S3 touch channel N is wired to GPIO N, keep TOUCH_BUTTON_CHANNEL in sync");

// GpioLed leaves the LED dimly lit while idle and runs the LEDC breathing fade in the
// listening states. This board keeps the user LED dark when idle; define LED_IDLE_GLOW in
// config.h to fall back to the shared dim-idle behavior.
class XiaoEsp32s3PlusLed : public GpioLed {
public:
    XiaoEsp32s3PlusLed(gpio_num_t gpio, int output_invert) : GpioLed(gpio, output_invert) {}

    void OnStateChanged() override {
#ifndef LED_IDLE_GLOW
        if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            TurnOff();
            return;
        }
#endif
        GpioLed::OnStateChanged();
    }
};

class XiaoEsp32s3PlusBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    PowerSaveTimer* power_save_timer_ = nullptr;

    // Local interactions (volume, chat toggle) do not change the device state, so the
    // Application::SetPowerSaveLevel(PERFORMANCE) hook never fires for them.
    void WakeScreen() {
        if (power_save_timer_ != nullptr) {
            power_save_timer_->WakeUp();
        }
    }

    // // Bits posted from the touch ISR to touch_task_.
    // static constexpr uint32_t kTouchPressed = 1u << 0;
    // static constexpr uint32_t kTouchReleased = 1u << 1;

    // touch_sensor_handle_t touch_sensor_ = nullptr;
    // touch_channel_handle_t touch_channel_ = nullptr;
    // TaskHandle_t touch_task_ = nullptr;
    // uint32_t touch_baseline_ = 0;

    // // Touch callbacks run in the touch driver interrupt handler: only ISR-safe calls belong here.
    // static bool OnTouchActive(touch_sensor_handle_t sens_handle, const touch_active_event_data_t* event,
    //                           void* user_ctx) {
    //     (void)sens_handle;
    //     auto* self = static_cast<XiaoEsp32s3PlusBoard*>(user_ctx);
    //     if (self == nullptr || self->touch_task_ == nullptr || event->chan_id != TOUCH_BUTTON_CHANNEL) {
    //         return false;
    //     }
    //     BaseType_t woken = pdFALSE;
    //     xTaskNotifyFromISR(self->touch_task_, kTouchPressed, eSetBits, &woken);
    //     return woken == pdTRUE;
    // }

    // static bool OnTouchInactive(touch_sensor_handle_t sens_handle, const touch_inactive_event_data_t* event,
    //                             void* user_ctx) {
    //     (void)sens_handle;
    //     auto* self = static_cast<XiaoEsp32s3PlusBoard*>(user_ctx);
    //     if (self == nullptr || self->touch_task_ == nullptr || event->chan_id != TOUCH_BUTTON_CHANNEL) {
    //         return false;
    //     }
    //     BaseType_t woken = pdFALSE;
    //     xTaskNotifyFromISR(self->touch_task_, kTouchReleased, eSetBits, &woken);
    //     return woken == pdTRUE;
    // }

    // // Logs how far the measured capacitance moved from the untouched baseline, so the active
    // // threshold can be picked from real numbers instead of guessed (see TOUCH_BUTTON_THRESHOLD_RATIO).
    // void LogTouchDelta(const char* what) {
    //     uint32_t smooth[TOUCH_SAMPLE_CFG_NUM] = {};
    //     if (touch_channel_ == nullptr ||
    //         touch_channel_read_data(touch_channel_, TOUCH_CHAN_DATA_TYPE_SMOOTH, smooth) != ESP_OK) {
    //         return;
    //     }
    //     int64_t delta = static_cast<int64_t>(smooth[0]) - static_cast<int64_t>(touch_baseline_);
    //     int64_t percent = touch_baseline_ == 0 ? 0 : delta * 100 / static_cast<int64_t>(touch_baseline_);
    //     ESP_LOGI(TAG, "Touch %s: smooth %u, baseline %u, delta %lld (%lld%%)", what,
    //              static_cast<unsigned>(smooth[0]), static_cast<unsigned>(touch_baseline_),
    //              static_cast<long long>(delta), static_cast<long long>(percent));
    // }

    // // StartListening()/StopListening() set event bits and are not ISR-safe, so the touch state
    // // change is applied here, outside the interrupt context.
    // static void TouchButtonTask(void* arg) {
    //     auto* self = static_cast<XiaoEsp32s3PlusBoard*>(arg);
    //     auto& app = Application::GetInstance();
    //     while (true) {
    //         uint32_t events = 0;
    //         if (xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY) != pdTRUE) {
    //             continue;
    //         }
    //         if (events & kTouchPressed) {
    //             self->LogTouchDelta("pressed");
    //             app.StartListening();
    //         }
    //         if (events & kTouchReleased) {
    //             self->LogTouchDelta("released");
    //             app.StopListening();
    //         }
    //     }
    // }

    // void InitializeTouchButton() {
    //     static touch_sensor_sample_config_t sample_config[TOUCH_SAMPLE_CFG_NUM] = {
    //         TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2)};
    //     touch_sensor_config_t controller_config =
    //         TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample_config);
    //     if (touch_sensor_new_controller(&controller_config, &touch_sensor_) != ESP_OK) {
    //         ESP_LOGW(TAG, "Touch controller init failed, push-to-talk pad disabled");
    //         touch_sensor_ = nullptr;
    //         return;
    //     }

    //     touch_channel_config_t channel_config = {};
    //     channel_config.active_thresh[0] = TOUCH_BUTTON_THRESHOLD_FALLBACK;
    //     channel_config.charge_speed = TOUCH_CHARGE_SPEED_7;
    //     channel_config.init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT;
    //     if (touch_sensor_new_channel(touch_sensor_, TOUCH_BUTTON_CHANNEL, &channel_config,
    //                                  &touch_channel_) != ESP_OK) {
    //         ESP_LOGW(TAG, "Touch channel %d init failed (must be GPIO1..GPIO14), push-to-talk pad disabled",
    //                  TOUCH_BUTTON_CHANNEL);
    //         touch_sensor_del_controller(touch_sensor_);
    //         touch_sensor_ = nullptr;
    //         return;
    //     }

    //     touch_chan_info_t channel_info = {};
    //     if (touch_sensor_get_channel_info(touch_channel_, &channel_info) == ESP_OK) {
    //         ESP_LOGI(TAG, "Touch PTT on channel %d (GPIO%d)", channel_info.chan_id, channel_info.chan_gpio);
    //     }

    //     touch_sensor_filter_config_t filter_config = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    //     if (touch_sensor_config_filter(touch_sensor_, &filter_config) != ESP_OK) {
    //         ESP_LOGW(TAG, "Touch filter config failed, touch may be triggered by noise");
    //     }

    //     // The first scans initialize the channel data; the untouched baseline then scales the threshold.
    //     uint32_t threshold = TOUCH_BUTTON_THRESHOLD_FALLBACK;
    //     esp_err_t err = touch_sensor_enable(touch_sensor_);
    //     if (err == ESP_OK) {
    //         for (int i = 0; i < 3 && err == ESP_OK; i++) {
    //             err = touch_sensor_trigger_oneshot_scanning(touch_sensor_, 2000);
    //         }
    //         touch_sensor_disable(touch_sensor_);
    //     }
    //     if (err == ESP_OK) {
    //         uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {};
    //         if (touch_channel_read_data(touch_channel_, TOUCH_CHAN_DATA_TYPE_BENCHMARK, benchmark) == ESP_OK &&
    //             benchmark[0] > 0) {
    //             touch_baseline_ = benchmark[0];
    //             threshold = static_cast<uint32_t>(benchmark[0] * TOUCH_BUTTON_THRESHOLD_RATIO);
    //             if (threshold == 0) {
    //                 threshold = 1;
    //             }
    //             ESP_LOGI(TAG, "Touch baseline %u, active threshold %u", static_cast<unsigned>(benchmark[0]),
    //                      static_cast<unsigned>(threshold));
    //         } else {
    //             ESP_LOGW(TAG, "Touch baseline unreadable, keeping fallback threshold %d",
    //                      TOUCH_BUTTON_THRESHOLD_FALLBACK);
    //         }
    //     } else {
    //         ESP_LOGW(TAG, "Touch calibration scan failed (%s), keeping fallback threshold %d",
    //                  esp_err_to_name(err), TOUCH_BUTTON_THRESHOLD_FALLBACK);
    //     }

    //     channel_config.active_thresh[0] = threshold;
    //     if (touch_sensor_reconfig_channel(touch_channel_, &channel_config) != ESP_OK) {
    //         ESP_LOGW(TAG, "Touch threshold update failed, keeping fallback threshold");
    //     }

    //     touch_event_callbacks_t callbacks = {};
    //     callbacks.on_active = OnTouchActive;
    //     callbacks.on_inactive = OnTouchInactive;
    //     if (touch_sensor_register_callbacks(touch_sensor_, &callbacks, this) != ESP_OK) {
    //         ESP_LOGW(TAG, "Touch callback registration failed, push-to-talk pad disabled");
    //         return;
    //     }

    //     if (xTaskCreate(TouchButtonTask, "touch_ptt", 3072, this, 3, &touch_task_) != pdPASS) {
    //         ESP_LOGW(TAG, "Touch task create failed, push-to-talk pad disabled");
    //         touch_task_ = nullptr;
    //         return;
    //     }

    //     if (touch_sensor_enable(touch_sensor_) != ESP_OK ||
    //         touch_sensor_start_continuous_scanning(touch_sensor_) != ESP_OK) {
    //         ESP_LOGW(TAG, "Touch scanning start failed, push-to-talk pad disabled");
    //     }
    // }

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeSsd1306Display() {
        // SSD1306 config
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .scl_speed_hz = 400 * 1000,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // Reset the display
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        // Set the display to on
        ESP_LOGI(TAG, "Turning display on");
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            WakeScreen();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        volume_up_button_.OnClick([this]() {
            WakeScreen();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            WakeScreen();
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            WakeScreen();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            WakeScreen();
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    void InitializePowerSaveTimer() {
        // cpu_max_freq = -1 leaves the CPU frequency, the microphone and wake word detection
        // untouched, so the device keeps listening for the wake word with the screen dark.
        power_save_timer_ = new PowerSaveTimer(-1, 30, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            if (display_ != nullptr) {
                display_->SetPowerSaveMode(true);
            }
            if (panel_ != nullptr) {
                // SSD1306 display-off command: pixels go dark while GDDRAM is retained.
                esp_lcd_panel_disp_on_off(panel_, false);
            }
        });
        power_save_timer_->OnExitSleepMode([this]() {
            // Draw the neutral face before switching the panel back on, so the sleepy frame
            // is never visible.
            if (display_ != nullptr) {
                display_->SetPowerSaveMode(false);
            }
            if (panel_ != nullptr) {
                esp_lcd_panel_disp_on_off(panel_, true);
            }
        });
        power_save_timer_->SetEnabled(true);
    }

    // 物联网初始化，逐步迁移到 MCP 协议
//    void InitializeTools() {
//        static LampController lamp(LAMP_GPIO);
//    }

public:
    XiaoEsp32s3PlusBoard() :
        // boot_button_(BOOT_BUTTON_GPIO),
        boot_button_(VOLUME_SELECT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializePowerSaveTimer();
        // InitializeTouchButton();
        //InitializeTools();
    }

    virtual Led* GetLed() override {
        // The onboard user LED is a plain LED on GPIO21 driven low-active, so it is dimmed by
        // the LEDC-backed driver (which also breathes while listening) instead of an addressable strip.
        // static XiaoEsp32s3PlusLed led(BUILTIN_LED_GPIO, 1);
        static XiaoEsp32s3PlusLed led(BUILTIN_LED_GPIO, 0);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    // Application raises PERFORMANCE when a conversation, notification or upgrade starts,
    // which is exactly when the screen has to be back on before anything is shown.
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::PERFORMANCE) {
            WakeScreen();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(XiaoEsp32s3PlusBoard);
