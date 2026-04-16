#ifndef HOST_H
#define HOST_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

// ==========================================
// 跨核共享内存结构体 (Core 1 写入，Core 0 读取)
// ==========================================
typedef struct {
    int8_t left_x;   // 左摇杆 X轴
    int8_t left_y;   // 左摇杆 Y轴
    int8_t right_x;  // 右摇杆 X轴
    int8_t right_y;  // 右摇杆 Y轴
    uint16_t buttons;// 按键状态位 (例如 BIT0=A, BIT1=B...)
} gamepad_state_t;

// ==========================================
// 全局变量声明 (实现在 host.c 中)
// ==========================================
extern volatile gamepad_state_t g_gamepad; 
extern portMUX_TYPE g_gamepad_spinlock; 

// ==========================================
// 外部 API
// ==========================================
/**
 * @brief 初始化 USB 硬件环境，并启动 Core 1 的极速轮询任务
 */
void usb_host_init(void);

#endif // HOST_H