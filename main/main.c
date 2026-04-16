#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "host.h"
#include "input_bridge.h"

static const char *TAG = "MAIN";

extern void usb_device_init(void);

// ============================================================================
// 系统主入口
// ============================================================================
void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Starting KeyMouse to Gamepad Converter...");

    // 1. 初始化键鼠数据桥接缓冲 (必须最先初始化，防止空指针)
    bridge_init();
    ESP_LOGI(TAG, "Input bridge initialized.");

    // 2. 初始化 CH374T 设备端 (模拟 Xbox 手柄，独占 SPI3_HOST)
    usb_device_init();
    ESP_LOGI(TAG, "USB Device (Gamepad) initialized.");

    // 3. 初始化 CH374T 主机端 (读取键鼠，独占 SPI2_HOST)
    usb_host_init();
    ESP_LOGI(TAG, "USB Host (Keyboard/Mouse) initialized.");
}