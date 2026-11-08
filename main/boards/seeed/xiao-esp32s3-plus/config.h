#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Seeed XIAO ESP32-S3 Plus voice assistant: same wiring as bread-compact-wifi.
#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// 如果使用 Duplex I2S 模式，请注释下面一行
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_7
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_8
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_9
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_11
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_12
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_13

#else

#define AUDIO_I2S_GPIO_WS GPIO_NUM_8
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_7
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_9
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_11

#endif


#define BUILTIN_LED_GPIO        GPIO_NUM_2
// #define BUILTIN_LED_GPIO        GPIO_NUM_8
// The user LED is off while idle. Uncomment to keep GpioLed's default dim idle glow instead.
#define LED_IDLE_GLOW
#define BOOT_BUTTON_GPIO        GPIO_NUM_0

// // Push-to-talk pad is read by the ESP32-S3 capacitive touch peripheral, not as a GPIO input.
// // On ESP32-S3 touch channel N maps 1:1 to GPIO N for channels 1..14, so GPIO1 == channel 1.
// #define TOUCH_BUTTON_GPIO       GPIO_NUM_1
// #define TOUCH_BUTTON_CHANNEL    1
// // Active threshold relative to the untouched baseline measured at boot (usable range ~1%..20%).
// // The pad is wired to the speaker grille, so it also picks up a nearby hand: the ratio has to be
// // high enough that hovering does not trigger. The boot log prints the baseline, and every
// // press/release logs the measured delta as a percentage of it - use those numbers to tune this.
// #define TOUCH_BUTTON_THRESHOLD_RATIO    0.05f
// // Used when the baseline cannot be measured at boot.
// #define TOUCH_BUTTON_THRESHOLD_FALLBACK 2000

#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_42
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_10
#define VOLUME_SELECT_BUTTON_GPIO GPIO_NUM_41

#define DISPLAY_SDA_PIN GPIO_NUM_6
#define DISPLAY_SCL_PIN GPIO_NUM_5
#define DISPLAY_WIDTH   128

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT  64
#define SH1106
#else
#error "OLED display type is not selected"
#endif

#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false


// A MCP Test: Control a lamp
//#define LAMP_GPIO GPIO_NUM_18

#endif // _BOARD_CONFIG_H_
