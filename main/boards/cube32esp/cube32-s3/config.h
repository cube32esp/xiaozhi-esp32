#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include "cube32_config.h"

// ESP32-S3 chip strapping pin used as a generic boot/user button.
// (cube32_config.h does not currently expose a BOOT_BUTTON pin — use GPIO0.)
#define BOOT_BUTTON_GPIO        GPIO_NUM_0

#define BUILTIN_LED_GPIO        GPIO_NUM_NC

#endif // _BOARD_CONFIG_H_
