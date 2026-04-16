#include "host.h"
#include <stdio.h>
#include <string.h>
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "input_bridge.h"
#include "esp_task_wdt.h"

// ==========================================
// 硬件引脚定义
// ==========================================
#define CH374_SCLK_PIN  10
#define CH374_MOSI_PIN  11  
#define CH374_MISO_PIN  12  

#define KB_CS_PIN       7  
#define KB_INT_PIN      8

#define MOUSE_CS_PIN    9
#define MOUSE_INT_PIN   13

#define REG_HUB_SETUP   0x02
#define REG_SYS_INFO    0x04
#define REG_SYS_CTRL    0x05
#define REG_USB_SETUP   0x06
#define REG_INTER_EN    0x07
#define REG_USB_ADDR    0x08
#define REG_INTER_FLAG  0x09
#define REG_USB_STATUS  0x0A
#define REG_USB_LENGTH  0x0B
#define REG_USB_H_TOKEN 0x0D
#define REG_USB_H_CTRL  0x0E
#define RAM_HOST_TRAN   0x40
#define RAM_HOST_RECV   0xC0

static const char *TAG = "HOST";

volatile gamepad_state_t g_gamepad = {0}; 
portMUX_TYPE g_gamepad_spinlock = portMUX_INITIALIZER_UNLOCKED; 

static uint8_t *dma_tx_buf = NULL;
static uint8_t *dma_rx_buf = NULL;

static uint8_t kbd_data[8] = {0};
static uint8_t mouse_data[8] = {0};

// ==========================================
// 热插拔状态机定义
// ==========================================
typedef enum {
    STATE_DISCONNECTED = 0, // 物理断开
    STATE_WAIT_DEBOUNCE,    // 刚插入，等待 50ms 触点防抖
    STATE_BUS_RESETTING,    // 正在拉低总线进行复位 (持续 20ms)
    STATE_WAIT_STABLE,      // 复位完成，等待 10ms 稳定
    STATE_ENUMERATING,      // 正在下发 USB 枚举包
    STATE_ONLINE            // 正常在线轮询工作
} usb_state_t;

typedef struct {
    const char* name;
    spi_device_handle_t spi;
    gpio_num_t int_pin;
    
    // 热插拔相关属性
    usb_state_t state;
    uint64_t state_timer_us; // 记录状态切换时的时间戳
    
    bool ep1_in_toggle;
    bool is_low_speed;
    bool is_keyboard; 
} ch374_dev_t;

static ch374_dev_t dev_kb;
static ch374_dev_t dev_mouse;

static inline void ch374_write_reg(ch374_dev_t *dev, uint8_t addr, uint8_t data) {
    dma_tx_buf[0] = addr;
    dma_tx_buf[1] = 0x80;
    dma_tx_buf[2] = data;
    spi_transaction_t t = { .length = 24, .tx_buffer = dma_tx_buf };
    spi_device_polling_transmit(dev->spi, &t);
}

static inline uint8_t ch374_read_reg(ch374_dev_t *dev, uint8_t addr) {
    dma_tx_buf[0] = addr;
    dma_tx_buf[1] = 0xC0;
    dma_tx_buf[2] = 0x00; // Dummy byte to clock out the MISO data
    spi_transaction_t t = { .length = 24, .tx_buffer = dma_tx_buf, .rx_buffer = dma_rx_buf };
    spi_device_polling_transmit(dev->spi, &t);
    return dma_rx_buf[2]; // CH374 outputs data on the 3rd byte
}

static inline void ch374_write_multi(ch374_dev_t *dev, uint8_t addr, const uint8_t *data, size_t len) {
    dma_tx_buf[0] = addr; dma_tx_buf[1] = 0x80; memcpy(&dma_tx_buf[2], data, len);
    spi_transaction_t t = { .length = (len + 2) * 8, .tx_buffer = dma_tx_buf };
    spi_device_polling_transmit(dev->spi, &t);
}

static inline void ch374_read_multi(ch374_dev_t *dev, uint8_t addr, uint8_t *data, size_t len) {
    dma_tx_buf[0] = addr; dma_tx_buf[1] = 0xC0;
    spi_transaction_t t = { .length = (len + 2) * 8, .tx_buffer = dma_tx_buf, .rx_buffer = dma_rx_buf };
    spi_device_polling_transmit(dev->spi, &t);
    memcpy(data, &dma_rx_buf[2], len);
}

static uint8_t ch374_exec_transaction(ch374_dev_t *dev, uint8_t pid, bool toggle) {
    ch374_write_reg(dev, REG_USB_H_TOKEN, pid);
    ch374_write_reg(dev, REG_USB_H_CTRL, toggle ? 0x88 : 0x08);

    int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < 50 * 1000) { 
        if (gpio_get_level(dev->int_pin) == 0) {
            uint8_t flag = ch374_read_reg(dev, REG_INTER_FLAG);
            if (flag & 0x01) { 
                uint8_t status = ch374_read_reg(dev, REG_USB_STATUS);
                ch374_write_reg(dev, REG_INTER_FLAG, 0x1F); 
                return status;
            }
            ch374_write_reg(dev, REG_INTER_FLAG, 0x1F);
            return 0xFF;
        }
    }
    return 0xFF; // 超时
}

static bool ch374_control_transfer(ch374_dev_t *dev, const uint8_t *setup_pkt) {
    ch374_write_multi(dev, RAM_HOST_TRAN, setup_pkt, 8);
    ch374_write_reg(dev, REG_USB_SETUP, dev->is_low_speed ? 0xE0 : 0xC0);
    ch374_write_reg(dev, REG_USB_LENGTH, 8);

    uint8_t setup_stat = ch374_exec_transaction(dev, 0xD0, false);
    if ((setup_stat & 0x0F) != 0x02) return false; 

    uint32_t retries = 0;
    while (retries < 100) {
        uint8_t stat = ch374_exec_transaction(dev, 0x90, true);
        uint8_t pid = stat & 0x0F;
        
        if (pid == 0x0B || pid == 0x03) return true; 
        
        if (pid == 0x0A) {
            esp_rom_delay_us(50); 
            retries++; continue;
        }
        return false;
    }
    return false;
}

static void init_ch374_chip_hardware(ch374_dev_t *dev) {
    ESP_LOGI(TAG, "Booting CH374 chip for %s...", dev->name);
    ch374_write_reg(dev, REG_SYS_CTRL, 0x4C);
    vTaskDelay(pdMS_TO_TICKS(50));
    ch374_write_reg(dev, REG_SYS_CTRL, 0x44); 
    vTaskDelay(pdMS_TO_TICKS(10));
    ch374_write_reg(dev, REG_USB_SETUP, 0xC0);
    ch374_write_reg(dev, REG_HUB_SETUP, 0x00);
    ch374_write_reg(dev, REG_INTER_EN, 0xF1);

    // Verify SPI communication
    uint8_t sys_info = ch374_read_reg(dev, REG_SYS_INFO);
    if ((sys_info & 0x03) != 0x01) {
        ESP_LOGE(TAG, "[%s] SPI Comm Failed! Read: 0x%02X (Expected bits 1:0 to be 01)", dev->name, sys_info);
        // Do not proceed if this fails
    } else {
        ESP_LOGI(TAG, "[%s] SPI Comm OK!", dev->name);
    }
    
    // 初始化时强制设为断开状态
    dev->state = STATE_DISCONNECTED;
}

static void process_usb_device_statemachine(ch374_dev_t *dev, uint64_t current_time_us) {
    switch (dev->state) {
        
        case STATE_DISCONNECTED:
            // 物理检测：如果 BIT3 (0x08) 为 1，说明设备插上了！
            if (ch374_read_reg(dev, REG_HUB_SETUP) & 0x08) {
                dev->state = STATE_WAIT_DEBOUNCE;
                dev->state_timer_us = current_time_us; // 记录插入时刻
                ESP_LOGI(TAG, "[%s] Plugin detected, debouncing...", dev->name);
            }
            break;

        case STATE_WAIT_DEBOUNCE:
            // 非阻塞等待 200 毫秒
            if (current_time_us - dev->state_timer_us > 200 * 1000) {
                if (ch374_read_reg(dev, REG_HUB_SETUP) & 0x08) {
                    // 依然在，说明防抖通过，开始拉低总线复位
                    dev->is_low_speed = !(ch374_read_reg(dev, REG_INTER_FLAG) & 0x80);
                    ch374_write_reg(dev, REG_HUB_SETUP, dev->is_low_speed ? 0x06 : 0x02);
                    dev->state = STATE_BUS_RESETTING;
                    dev->state_timer_us = current_time_us;
                } else {
                    dev->state = STATE_DISCONNECTED;
                }
            }
            break;

        case STATE_BUS_RESETTING:
            // 非阻塞等待复位持续 20 毫秒
            if (current_time_us - dev->state_timer_us > 20 * 1000) {
                ch374_write_reg(dev, REG_HUB_SETUP, dev->is_low_speed ? 0x04 : 0x00);
                dev->state = STATE_WAIT_STABLE;
                dev->state_timer_us = current_time_us;
            }
            break;

        case STATE_WAIT_STABLE:
            // 非阻塞等待总线稳定 10 毫秒
            if (current_time_us - dev->state_timer_us > 10 * 1000) {
                // 使能端口
                ch374_write_reg(dev, REG_HUB_SETUP, ch374_read_reg(dev, REG_HUB_SETUP) | 0x01);
                dev->state = STATE_ENUMERATING;
                dev->state_timer_us = current_time_us;
            }
            break;

        case STATE_ENUMERATING:
            // 端口使能后再等 50 毫秒，器件完全就绪后，下发枚举包
            if (current_time_us - dev->state_timer_us > 50 * 1000) {
                ESP_LOGI(TAG, "[%s] Starting Enumeration...", dev->name);

                // =====================================
                // 每次重新枚举前，强制主机寻址地址复位为 0
                // =====================================
                ch374_write_reg(dev, REG_USB_ADDR, 0x00);
                
                // 下发配置包。
                const uint8_t set_addr_pkt[8]  = {0x00, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
                const uint8_t set_conf_pkt[8]  = {0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
                const uint8_t set_boot_proto_pkt[8] = {0x21, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

                if (ch374_control_transfer(dev, set_addr_pkt)) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    ch374_write_reg(dev, REG_USB_ADDR, 0x01); 
                    if (ch374_control_transfer(dev, set_conf_pkt)) {
                        if (dev->is_keyboard) {
                            ch374_control_transfer(dev, set_boot_proto_pkt);
                            ESP_LOGI(TAG, "[%s] switching to boot!", dev->name);
                        }
                        
                        // 枚举成功！
                        dev->ep1_in_toggle = false;
                        dev->state = STATE_ONLINE;
                        ESP_LOGI(TAG, "[%s] ONLINE and Ready!", dev->name);
                        break;
                    }
                }
                // 如果跑到这里说明枚举失败了（可能是劣质外设），退回断开状态重新来
                ESP_LOGE(TAG, "[%s] Enumeration Failed. Retrying...", dev->name);
                dev->state = STATE_DISCONNECTED;
            }
            break;

        case STATE_ONLINE:
            {
                uint8_t stat = ch374_exec_transaction(dev, 0x91, dev->ep1_in_toggle);
                uint8_t pid = stat & 0x0F; // 提取实际的 PID
                
                // 修复：依据 PID 是否为 0x03(DATA0) 或 0x0B(DATA1) 判断成功
                if (pid == 0x03 || pid == 0x0B) {
                    if (dev->is_keyboard) {
                        ch374_read_multi(dev, RAM_HOST_RECV, kbd_data, 8);
                        uint8_t modifier  = kbd_data[0];
                        uint8_t *keycodes = &kbd_data[2];
                        bridge_push_keyboard(keycodes, modifier);
                    } else {
                        if(ch374_read_reg(dev, REG_USB_LENGTH) == 13) {
                            ch374_read_multi(dev, RAM_HOST_RECV, mouse_data, 7);
                            uint16_t buttons = (uint16_t)(mouse_data[0] | (mouse_data[1] << 8));
                            int16_t dx = (int16_t)(mouse_data[2] | (mouse_data[3] << 8));
                            int16_t dy = (int16_t)(mouse_data[4] | (mouse_data[5] << 8));
                            int8_t wheel = (int8_t)mouse_data[6];
                            bridge_push_mouse(buttons, dx, dy, wheel);
                        }
                    }
                    dev->ep1_in_toggle = !dev->ep1_in_toggle;
                } else {
                    if (pid != 0x0A) { // 不是正常的 NAK
                        if (!(ch374_read_reg(dev, REG_HUB_SETUP) & 0x08)) {
                            ESP_LOGE(TAG, "[%s] Device Unplugged!", dev->name);
                            dev->state = STATE_DISCONNECTED;
                        }
                    }
                }
            }
            break;
    }
}

static void main_loop(void *pvParameters) {
    esp_task_wdt_add(NULL);
    dma_tx_buf = heap_caps_malloc(64, MALLOC_CAP_DMA);
    dma_rx_buf = heap_caps_malloc(64, MALLOC_CAP_DMA);

    // 1. 初始化两块 CH374T 芯片的寄存器（无需等待设备插入即可完成）
    init_ch374_chip_hardware(&dev_kb);
    init_ch374_chip_hardware(&dev_mouse);

    const uint64_t POLL_INTERVAL_US = 1000; 
    uint64_t next_poll_time = esp_timer_get_time();

    while (1) {
        next_poll_time += POLL_INTERVAL_US;
        uint64_t current_time = esp_timer_get_time();

        // 2. 状态机非阻塞驱动
        process_usb_device_statemachine(&dev_kb, current_time);
        process_usb_device_statemachine(&dev_mouse, current_time);

        // 3. 补齐剩余的空闲时间
        esp_task_wdt_reset();
        while (esp_timer_get_time() < next_poll_time) {
            esp_rom_delay_us(1);
        }
    }
}

void usb_host_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << KB_INT_PIN) | (1ULL << MOUSE_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE 
    };
    gpio_config(&io_conf);

    spi_bus_config_t buscfg = {
        .sclk_io_num = CH374_SCLK_PIN, .mosi_io_num = CH374_MOSI_PIN,
        .miso_io_num = CH374_MISO_PIN, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 64 
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t kb_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000, .mode = 0,                          
        .spics_io_num = KB_CS_PIN, .queue_size = 3,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &kb_cfg, &dev_kb.spi));
    dev_kb.name = "Keyboard";
    dev_kb.int_pin = KB_INT_PIN;
    dev_kb.is_keyboard = true; 

    spi_device_interface_config_t mouse_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000, .mode = 0,                          
        .spics_io_num = MOUSE_CS_PIN, .queue_size = 3,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &mouse_cfg, &dev_mouse.spi));
    dev_mouse.name = "Mouse";
    dev_mouse.int_pin = MOUSE_INT_PIN;
    dev_mouse.is_keyboard = false; 

    xTaskCreatePinnedToCore(main_loop, "host", 32767, NULL, configMAX_PRIORITIES - 1, NULL, 1);
} 