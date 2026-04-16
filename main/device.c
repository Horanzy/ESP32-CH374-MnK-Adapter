#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "input_bridge.h"
#include "XInputPad.h"
#include "esp_task_wdt.h"

// ==========================================
// 硬件引脚定义 (独占 SPI 总线)
// ==========================================
#define DEVICE_INT_PIN  2
#define DEVICE_MISO_PIN 3
#define DEVICE_MOSI_PIN 4
#define DEVICE_SCLK_PIN 5
#define DEVICE_CS_PIN   6

// CH374 寄存器定义
#define REG_SYS_INFO    0x04
#define REG_SYS_CTRL    0x05
#define REG_USB_SETUP   0x06
#define REG_INTER_EN    0x07
#define REG_USB_ADDR    0x08
#define REG_INTER_FLAG  0x09
#define REG_USB_STATUS  0x0A
#define REG_USB_LENGTH  0x0B
#define REG_USB_ENDP0   0x0C
#define REG_USB_ENDP1   0x0D
#define REG_USB_ENDP2   0x0E

#define RAM_ENDP0_TRAN  0x20
#define RAM_ENDP0_RECV  0x28
#define RAM_ENDP2_TRAN  0x40
#define RAM_ENDP2_RECV  0xC0

static const char *TAG = "DEVICE";

static spi_device_handle_t spi_dev;
static uint8_t *dma_tx_buf;
static uint8_t *dma_rx_buf;

// 状态机与端点同步变量
static volatile bool is_configured = false;
static volatile bool ep2_tx_in_flight = false;
static uint8_t ep0_recv_tog = 0;
static uint8_t ep0_tran_tog = 0;
static uint8_t ep2_recv_tog = 0;
static uint8_t ep2_tran_tog = 0;
static uint8_t pending_address = 0;

static const uint8_t *ep0_tx_ptr = NULL;
static uint16_t ep0_tx_len = 0;

// ==============================================================================
// 描述符定义 (端点地址硬改为 EP2: 0x82 和 0x02)
// ==============================================================================
static const uint8_t xinput_device_descriptor[] = {
    0x12, 0x01, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0x08,
    0x5E, 0x04, 0x8E, 0x02, 0x72, 0x05, 0x00, 0x00,
    0x00, 0x01
};

static const uint8_t xinput_config_descriptor[] = {
    // Configuration Descriptor:
    0x09, 0x02, 0x30, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,
    // Interface Descriptor:
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0x00,
    // Unknown Descriptor (XInput):
    0x10, 0x21, 0x10, 0x01, 0x01, 0x24, 0x82, 0x14, 0x03, 0x00, 0x03, 0x13, 0x02, 0x00, 0x03, 0x00, 
    // Endpoint Descriptor (IN 0x82):
    0x07, 0x05, 0x82, 0x03, 0x20, 0x00, 0x01,
    // Endpoint Descriptor (OUT 0x02):
    0x07, 0x05, 0x02, 0x03, 0x20, 0x00, 0x08
};

// ==========================================
// XInput 映射宏与查找表
// ==========================================
#define XINPUT_MOUSE_TO_STICK_SCALE 48
#define XINPUT_MOUSE_TO_STICK_SCALE_X5 180

#define XINPUT_STICK_MAX  32767
#define XINPUT_STICK_MIN -32767
#define XINPUT_STICK_DIAG  23170
#define XINPUT_STICK_DIAG_N -23170

#define XBOX_BUTTON_UP     (1 << 0)
#define XBOX_BUTTON_DOWN   (1 << 1)
#define XBOX_BUTTON_LEFT   (1 << 2)
#define XBOX_BUTTON_RIGHT  (1 << 3)
#define XBOX_BUTTON_START  (1 << 4)
#define XBOX_BUTTON_BACK   (1 << 5)
#define XBOX_BUTTON_L3     (1 << 6)
#define XBOX_BUTTON_R3     (1 << 7)
#define XBOX_BUTTON_LB     (1 << 8)
#define XBOX_BUTTON_RB     (1 << 9)
#define XBOX_BUTTON_GUIDE  (1 << 10)
#define XBOX_BUTTON_A      (1 << 12)
#define XBOX_BUTTON_B      (1 << 13)
#define XBOX_BUTTON_X      (1 << 14)
#define XBOX_BUTTON_Y      (1 << 15)

static const uint32_t s_key_lut_btn[256] = {
    [0x14] = XBOX_BUTTON_LB,                    // Q
    [0x08] = XBOX_BUTTON_X,                     // E
    [0x15] = XBOX_BUTTON_X,                     // R
    [0x09] = XBOX_BUTTON_R3,                    // F
    [0x0A] = XBOX_BUTTON_RIGHT,                 // G
    [0x17] = XBOX_BUTTON_RIGHT,                 // T
    [0x10] = XBOX_BUTTON_BACK,                  // M
    [0x19] = XBOX_BUTTON_LEFT,                  // V
    [0x1D] = XBOX_BUTTON_LB | XBOX_BUTTON_RB,   // Z
    [0x1E] = XBOX_BUTTON_Y,                     // 1
    [0x1F] = XBOX_BUTTON_Y,                     // 2
    [0x20] = XBOX_BUTTON_Y,                     // 3
    [0x2C] = XBOX_BUTTON_A,                     // Space
    [0x2B] = XBOX_BUTTON_START,                 // Tab
    [0x29] = XBOX_BUTTON_START,                 // Esc
    [0x52] = XBOX_BUTTON_UP,                    // Up
    [0x51] = XBOX_BUTTON_DOWN,                  // Down
    [0x50] = XBOX_BUTTON_LEFT,                  // Left
    [0x4F] = XBOX_BUTTON_RIGHT,                 // Right
};

static const uint8_t s_key_lut_wasd[256] = {
    [0x1A] = 1u << 0,   // W
    [0x04] = 1u << 1,   // A
    [0x16] = 1u << 2,   // S
    [0x07] = 1u << 3,   // D
};

static const int16_t s_lx_lut[16] = {
    0, 0, XINPUT_STICK_MIN, XINPUT_STICK_DIAG_N, 0, 0, XINPUT_STICK_DIAG_N, XINPUT_STICK_MIN, 
    XINPUT_STICK_MAX, XINPUT_STICK_DIAG, 0, 0, XINPUT_STICK_DIAG, XINPUT_STICK_MAX, 0, 0
};

static const int16_t s_ly_lut[16] = {
    0, XINPUT_STICK_MAX, 0, XINPUT_STICK_DIAG, XINPUT_STICK_MIN, 0, XINPUT_STICK_DIAG_N, 0, 
    0, XINPUT_STICK_DIAG, 0, XINPUT_STICK_MAX, XINPUT_STICK_DIAG_N, 0, XINPUT_STICK_MIN, 0
};

static inline int16_t clamp_s16(int32_t v) {
  if (v > 32767) return 32767;
  if (v < -32768) return -32768;
  return (int16_t)v;
}
static inline uint32_t isqrt(uint32_t n) {
    uint32_t root = 0;
    uint32_t bit = 1UL << 30; // 假设是 32位 整数
    
    while (bit > n) bit >>= 2;
    
    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static inline void square_to_circle_int(int16_t x, int16_t y, int16_t *out_x, int16_t *out_y) {
    // 拦截原点，防止后续除以 0
    if (x == 0 && y == 0) {
        *out_x = 0; 
        *out_y = 0;
        return;
    }

    // 求绝对值 (转为 32位 防止 -32768 取绝对值溢出)
    int32_t ax = (x > 0) ? x : -(int32_t)x;
    int32_t ay = (y > 0) ? y : -(int32_t)y;
    int32_t max_side = (ax > ay) ? ax : ay;

    // 计算平方和 
    // 注意：一定要用 uint32_t！因为 (-32768)^2 + (-32768)^2 = 2,147,483,648
    // 这个值刚好比 signed int32_t 的最大值 (2,147,483,647) 大 1，用有符号会溢出。
    uint32_t xx = (uint32_t)ax * ax;
    uint32_t yy = (uint32_t)ay * ay;
    uint32_t mag = isqrt(xx + yy);

    // 核心转换：利用“先乘后除”替代浮点小数的 rescale
    // 原逻辑: x * (max_side / mag) -> 新逻辑: (x * max_side) / mag
    // 32768 * 32768 = 1,073,741,824，在 int32_t 范围内，不会溢出
    *out_x = (int16_t)(((int32_t)x * max_side) / (int32_t)mag);
    *out_y = (int16_t)(((int32_t)y * max_side) / (int32_t)mag);
}

static void build_xinput_report(raw_input_state_t const *in, ReportDataXinput *out) {
    out->rid   = 0x00;
    out->rsize = 0x14;
    uint8_t wasd = 0;
    if (in->kbd_modifier & ((1u << 0) | (1u << 4))) out->buttons |= XBOX_BUTTON_B;   // Ctrl
    if (in->kbd_modifier & ((1u << 2) | (1u << 3))) out->buttons |= XBOX_BUTTON_DOWN;// Alt
    if (in->kbd_modifier & ((1u << 1) | (1u << 5))) out->buttons |= XBOX_BUTTON_L3;  // Shift

    int16_t temp_x = 0, temp_y = 0, recoil_offset = 0;

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t kc = in->kbd_keycode[i];
        out->buttons |= s_key_lut_btn[kc];
        wasd |= s_key_lut_wasd[kc];
    }

    if(in->mouse_buttons & (1u << 4)) {
        out->r_x = s_lx_lut[wasd & 0x0F];
        out->r_y = s_ly_lut[wasd & 0x0F];
        if(in->mouse_wheel < 0) out->r_y = -32767;
        if(in->mouse_wheel > 0) out->r_y = 32767;
        square_to_circle_int(clamp_s16(in->mouse_dx * XINPUT_MOUSE_TO_STICK_SCALE_X5),
                         clamp_s16(-in->mouse_dy * XINPUT_MOUSE_TO_STICK_SCALE_X5),
                         &temp_x, &temp_y);
        out->l_x = temp_x;
        out->l_y = temp_y;
        if (in->mouse_buttons & (1u << 0)) out->buttons |= XBOX_BUTTON_A;
        if (in->mouse_buttons & (1u << 1)) out->buttons |= XBOX_BUTTON_X;
    } else {
        out->l_x = s_lx_lut[wasd & 0x0F];
        out->l_y = s_ly_lut[wasd & 0x0F];
        if (in->mouse_buttons & (1u << 0)) { out->rt = 255; recoil_offset = -200; } 
        if (in->mouse_buttons & (1u << 1)) { out->lt = 255; recoil_offset *= 4; } 
        if (in->mouse_buttons & (1u << 2)) out->buttons |= XBOX_BUTTON_RB;
        if (in->mouse_buttons & (1u << 3)) out->buttons |= XBOX_BUTTON_UP;
        if (in->mouse_wheel != 0)          out->buttons |= XBOX_BUTTON_Y;
        square_to_circle_int(clamp_s16(in->mouse_dx * XINPUT_MOUSE_TO_STICK_SCALE),
                         clamp_s16(-in->mouse_dy * XINPUT_MOUSE_TO_STICK_SCALE + recoil_offset),
                         &temp_x, &temp_y);
        out->r_x = temp_x;
        out->r_y = temp_y;
    }
}

// ==========================================
// SPI 底层通讯
// ==========================================
static inline void ch374_write_reg(uint8_t addr, uint8_t data) {
    dma_tx_buf[0] = addr; dma_tx_buf[1] = 0x80; dma_tx_buf[2] = data;
    spi_transaction_t t = { .length = 24, .tx_buffer = dma_tx_buf };
    spi_device_polling_transmit(spi_dev, &t);
}

static inline uint8_t ch374_read_reg(uint8_t addr) {
    dma_tx_buf[0] = addr; dma_tx_buf[1] = 0xC0; dma_tx_buf[2] = 0x00;
    spi_transaction_t t = { .length = 24, .tx_buffer = dma_tx_buf, .rx_buffer = dma_rx_buf };
    spi_device_polling_transmit(spi_dev, &t);
    return dma_rx_buf[2];
}

static inline void ch374_write_multi(uint8_t addr, const uint8_t *data, size_t len) {
    dma_tx_buf[0] = addr; dma_tx_buf[1] = 0x80; memcpy(&dma_tx_buf[2], data, len);
    spi_transaction_t t = { .length = (len + 2) * 8, .tx_buffer = dma_tx_buf };
    spi_device_polling_transmit(spi_dev, &t);
}

static inline void ch374_read_multi(uint8_t addr, uint8_t *data, size_t len) {
    dma_tx_buf[0] = addr; dma_tx_buf[1] = 0xC0;
    spi_transaction_t t = { .length = (len + 2) * 8, .tx_buffer = dma_tx_buf, .rx_buffer = dma_rx_buf };
    spi_device_polling_transmit(spi_dev, &t);
    memcpy(data, &dma_rx_buf[2], len);
}

// ==========================================
// 端点 0 状态机核心
// ==========================================
static bool zlp_pending = false; // 新增全局状态

static void send_ep0_data() {
    uint8_t send_len = ep0_tx_len > 8 ? 8 : ep0_tx_len;
    
    if (send_len > 0) {
        ch374_write_multi(RAM_ENDP0_TRAN, ep0_tx_ptr, send_len);
        ep0_tx_ptr += send_len;
        ep0_tx_len -= send_len;
    } else {
        if (zlp_pending) {
            send_len = 0; // 发送 0 长度的 ZLP
            zlp_pending = false;
        } else {
            // 修复：确保低 4 位（长度）为 0
            // 如果你想模仿 EP1/2 的 NAK 逻辑，在 EP0 上应确保不触发错误的长度。
            // 写入 0x00 表示发送长度为 0 的包（ACK 模式）或根据需要设置 D5-D4 响应位。
            ch374_write_reg(REG_USB_ENDP0, (ep0_recv_tog << 7) | (ep0_tran_tog << 6) | 0x00); 
            return;
        }
    }

    // 设置寄存器：低 4 位使用 send_len，确保长度准确
    uint8_t reg = (ep0_recv_tog << 7) | (ep0_tran_tog << 6) | (0x00 << 4) | send_len;
    ch374_write_reg(REG_USB_ENDP0, reg);

    ep0_tran_tog ^= 1; 
}

static void handle_ep0_setup() {
    uint8_t setup[8];
    ch374_read_multi(RAM_ENDP0_RECV, setup, 8);
    uint8_t reqType = setup[0];
    uint8_t req = setup[1];
    uint16_t wValue = setup[2] | (setup[3] << 8);
    uint16_t wLength = setup[6] | (setup[7] << 8);

    ep0_tx_len = 0;
    ep0_tx_ptr = NULL;
    bool stall = false;

    if (reqType == 0x80 && req == 0x06) { // Get Descriptor
        uint8_t descType = setup[3];
        if (descType == 1) { 
            ep0_tx_ptr = xinput_device_descriptor; 
            ep0_tx_len = sizeof(xinput_device_descriptor); 
        } else if (descType == 2) { 
            ep0_tx_ptr = xinput_config_descriptor; 
            ep0_tx_len = sizeof(xinput_config_descriptor); 
        } else {
            // 不支持的描述符（例如字符串描述符、设备限定符等），必须 STALL
            stall = true;
        }
    } else if (reqType == 0x00 && req == 0x05) { // Set Address
        pending_address = wValue;
    } else if (reqType == 0x00 && req == 0x09) { // Set Configuration
        is_configured = true;
        ep2_recv_tog = 0; ep2_tran_tog = 0;
        // 配置端点 2: RECV_RESP=ACK(00), TRAN_RESP=NAK(10) -> 所以写入 0x02
        ch374_write_reg(REG_USB_ENDP2, 0x02); 
    } else if (reqType == 0xC1 && req == 0x01) { // XInput 特有握手
        if (wValue == 0x0000) {
            static const uint8_t rsp1[8] = {0};
            ep0_tx_ptr = rsp1; ep0_tx_len = 8;
        } else if (wValue == 0x0100) {
            static const uint8_t rsp2[20] = {0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; 
            ep0_tx_ptr = rsp2; ep0_tx_len = 20;
        } else {
            stall = true;
        }
    } else {
        // 对于其他无法识别的控制请求，默认 STALL
        stall = true;
    }

    if (stall) {
        // STALL 端点 0 (TRAN_RESP = 1111b, RECV_RESP = 0011b)
        ch374_write_reg(REG_USB_ENDP0, (ep0_recv_tog << 7) | (ep0_tran_tog << 6) | 0x3F);
        return;
    }

    if (ep0_tx_len < wLength && (ep0_tx_len % 8 == 0) && ep0_tx_len > 0) {
        zlp_pending = true;
    } else {
        zlp_pending = false;
    }

    if (ep0_tx_len > wLength) ep0_tx_len = wLength;
    ep0_tran_tog = 1; // 重点：SETUP 后的首个 IN 包必须是 DATA1
    
    if (reqType & 0x80) { // Device to Host 
        send_ep0_data();
    } else { // Host to Device 
        // 挂起一个 0 长度应答，准备接收状态阶段的 IN ACK
        ch374_write_reg(REG_USB_ENDP0, (ep0_recv_tog << 7) | (ep0_tran_tog << 6) | 0x00);
    }
}

// ==========================================
// 核心事务分配器
// ==========================================
static void handle_usb_transfer(uint8_t status) {
    uint8_t ep = status & 0x03; // 解析端点号
    uint8_t pid = (status >> 2) & 0x03; // 解析事务 PID

    if (ep == 0x00) { // 端点 0
        if (pid == 0x03) { // SETUP 事务
            ep0_recv_tog = 1; 
            handle_ep0_setup();
        } else if (pid == 0x02) { // IN 事务
            if (pending_address) {
                // SET_ADDRESS 必须在 IN ACK 发送完成后再生效地址 
                ch374_write_reg(REG_USB_ADDR, pending_address);
                pending_address = 0;
            } else {
                send_ep0_data(); 
            }
        } else if (pid == 0x00) { // OUT 事务
            ep0_recv_tog ^= 1;
            ch374_write_reg(REG_USB_ENDP0, (ep0_recv_tog << 7) | (ep0_tran_tog << 6) | 0x00);
        }
    } else if (ep == 0x02) { // 端点 2
        if (pid == 0x02) { // IN 事务 (Gamepad Report 成功发送)
            ep2_tx_in_flight = false;
            ep2_tran_tog ^= 1; // 成功发送后翻转 Toggle
            
            // 重点：发完一包后立刻切回 NAK (0x02)，防止总线上的重传风暴
            ch374_write_reg(REG_USB_ENDP2, (ep2_recv_tog << 7) | (ep2_tran_tog << 6) | 0x02);
        } else if (pid == 0x00) { // OUT 事务 (主机下发的震动数据)
            ep2_recv_tog ^= 1;
            // 接收 OUT 数据后，依然保持 TRAN 端点为 NAK
            ch374_write_reg(REG_USB_ENDP2, (ep2_recv_tog << 7) | (ep2_tran_tog << 6) | 0x02); 
        }
    }
}

// ==========================================
// 游戏控制器主任务
// ==========================================
static void usb_device_task(void *pvParameters) {
    esp_task_wdt_add(NULL);

    dma_tx_buf = heap_caps_malloc(64, MALLOC_CAP_DMA);
    dma_rx_buf = heap_caps_malloc(64, MALLOC_CAP_DMA);

    // 1. 软件复位
    ch374_write_reg(REG_SYS_CTRL, 0x08); 
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // 2. 修复点：CH374T 必须手动强制置位 BIT_CTRL_OE_POLAR (Bit 6) 才能使能 USB 引脚！
    // 写入 0x40 (01000000b)。如果你的 V3 引脚输入的是 3.3V 外部电源，建议写入 0x44 (关闭内部 5V->3.3V 稳压器以省电)
    ch374_write_reg(REG_SYS_CTRL, 0x40); 
    vTaskDelay(pdMS_TO_TICKS(10));

    // 建议加一段 SPI 通讯校验，确保接线无误
    uint8_t sys_info = ch374_read_reg(REG_SYS_INFO);
    if ((sys_info & 0x03) != 0x01) {
        ESP_LOGE(TAG, "Device SPI Comm Failed! Read: 0x%02X", sys_info);
    } else {
        ESP_LOGI(TAG, "Device SPI Comm OK!");
    }
    
    // 3. 设备模式 (BIT_SETP_HOST_MODE=0)，启用内部上拉，启用传输
    ch374_write_reg(REG_USB_SETUP, 0x03); 
    
    // 4. 开启总线复位、总线挂起、传输完成中断输出
    ch374_write_reg(REG_INTER_EN, 0x07);

    while (1) {
        if (gpio_get_level(DEVICE_INT_PIN) == 0) {
            uint8_t flag = ch374_read_reg(REG_INTER_FLAG);
            
            if (flag & 0x02) { // BIT_IF_BUS_RESET: 总线复位
                is_configured = false;
                ep0_recv_tog = 0; ep0_tran_tog = 0;
                ep2_recv_tog = 0; ep2_tran_tog = 0;
                ep2_tx_in_flight = false;
                ch374_write_reg(REG_USB_ADDR, 0x00);
                ch374_write_reg(REG_INTER_FLAG, 0x1F);
                ESP_LOGI(TAG, "Bus Reset Received.");
            }
            
            if (flag & 0x01) { // BIT_IF_TRANSFER: 传输完成
                uint8_t status = ch374_read_reg(REG_USB_STATUS);
                handle_usb_transfer(status);
                ch374_write_reg(REG_INTER_FLAG, 0x1F);
            }
        }

        // XInput 数据压入与发送
        if (is_configured && !ep2_tx_in_flight) {
            raw_input_state_t in;
            bridge_get_snapshot(&in);
            
            ReportDataXinput report = {0};
            build_xinput_report(&in, &report); 

            ep2_tx_in_flight = true;
            ch374_write_multi(RAM_ENDP2_TRAN, (uint8_t*)&report, 20);
            ch374_write_reg(REG_USB_LENGTH, 20);
            
            // 填好数据后，将 EP2 响应模式改为 ACK (0x00)，等待主机取走数据
            ch374_write_reg(REG_USB_ENDP2, (ep2_recv_tog << 7) | (ep2_tran_tog << 6) | 0x00); 
        }

        // 放弃 CPU 执行权但不进行毫秒级死等，确保微秒级的中断响应
        esp_task_wdt_reset();
        esp_rom_delay_us(1); 
    }
}

void usb_device_init(void) {
    // 1. 配置 INT 中断输入引脚
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DEVICE_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // CH374 INT 是开漏输出，必须上拉
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE 
    };
    gpio_config(&io_conf);

    // 2. 初始化独立的 SPI 总线 (使用 SPI3_HOST，与 Host 端的 SPI2_HOST 物理隔离)
    spi_bus_config_t buscfg = {
        .sclk_io_num = DEVICE_SCLK_PIN,
        .mosi_io_num = DEVICE_MOSI_PIN,
        .miso_io_num = DEVICE_MISO_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 3. 挂载 CH374T 设备
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000, // 10MHz
        .mode = 0,                                  
        .spics_io_num = DEVICE_CS_PIN,
        .queue_size = 3,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev_cfg, &spi_dev));

    ESP_LOGI(TAG, "Device SPI Bus initialized on SPI3_HOST");

    // 4. 启动状态机轮询任务
    xTaskCreatePinnedToCore(usb_device_task, "xinput_dev", 32767, NULL, configMAX_PRIORITIES - 1, NULL, 0);
}