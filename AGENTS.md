# AGENTS.md

面向在本仓库工作的 AI / 自动化编码代理的开发指南：项目是什么、代码如何组织、各模块内部怎么运转、构建方式、以及改动时必须遵守的约定。

## 项目简介

**ESP32-CH374-MnK-Adapter**：键盘 + 鼠标 → Xbox 360 手柄（XInput）转换器。

- 主控为 **ESP32-S3**（Xtensa 双核），已在 Waveshare ESP32 S3 Zero + 自制 CH374T 转接板上验证（PCB 工程见 `LCEDA project/`）。
- 用 **三颗 CH374T**：两颗作 **USB Host** 分别读键盘/鼠标，一颗作 **USB Device** 把整机模拟成 Xbox 360 手柄（VID `0x045E` / PID `0x028E`）。
- 双核分工：**Core 1** 跑 Host 任务轮询键鼠，**Core 0** 跑 Device 任务上报手柄报文，两核通过 `input_bridge` 的自旋锁共享原始输入。
- 标准 ESP-IDF 工程（`idf.py`），目标芯片 `esp32s3`。

> 本项目的键鼠→手柄换算算法源自 AG32 版（`~/Documents/AG32/MnK Adapter`），但**已分叉**：ESP32 版删除了摇杆响应曲线 / 后坐力补偿 / 抖动，鼠标改为**固定倍率线性映射**（腰射/开镜两档）；EMA 平滑、方圆映射、舔包模式保留。AG32 版仍是旧算法，两边不再一致。

## 目录结构

```
├── CMakeLists.txt          # IDF 工程入口（project(CH374)）
├── sdkconfig               # 当前构建配置（gitignore，由 set-target/menuconfig 生成）
├── main/
│   ├── CMakeLists.txt      # 组件注册：host.c main.c input_bridge.c device.c
│   ├── main.c              # app_main：bridge_init → usb_device_init → usb_host_init
│   ├── host.c              # USB Host（SPI2_HOST, Core 1）：热插拔状态机 + 枚举 + 数据端点轮询（键盘 EP1 / 鼠标 EP2）
│   ├── device.c            # USB Device（SPI3_HOST, Core 0）：CH374 设备模式 + EP0 控制 + EP2 上报 + 换算
│   ├── input_bridge.c/.h   # 跨核共享 raw_input_state_t + 自旋锁
│   ├── host.h              # 遗留的 gamepad_state_t / g_gamepad（真实数据通路是 input_bridge）
│   └── XInputPad.h         # XInput 20 字节报文结构体 ReportDataXinput
└── LCEDA project/          # 立创 EDA 的 CH374T 转接板 PCB 工程（*.epro，二进制）
```

## 端到端数据流

```
键盘/鼠标 (USB)
 └─ CH374T Host ×2 (SPI2_HOST, Core 1)        host.c
     └─ 热插拔状态机 → 枚举 → 数据端点 IN 轮询（键盘 EP1 / 鼠标 EP2）→ 解析报文
         └─ bridge_push_keyboard / bridge_push_mouse
             └─ input_bridge（portMUX 自旋锁，累加 dx/dy/wheel）
                 └─ bridge_get_snapshot（Core 0 取快照并清零 dx/dy、衰减 wheel）
                     └─ build_xinput_report（换算）          device.c
                         └─ CH374T Device (SPI3_HOST) EP2 IN → 主机
```

- Host 侧把鼠标 dx/dy/wheel **累加**进 bridge；Device 侧每次取快照后清零 dx/dy、wheel 每帧 ±1 衰减回 0。
- 两个任务都 `xTaskCreatePinnedToCore(..., configMAX_PRIORITIES-1, ...)` 固定到各自核，并各自 `esp_task_wdt_add` + 循环内 `esp_task_wdt_reset`。

## 引脚映射

| 总线 | 用途 | 引脚 |
|---|---|---|
| SPI3_HOST | Device 侧 CH374T | SCLK=5, MOSI=4, MISO=3, CS=6, INT=2 |
| SPI2_HOST | Host 侧共享 | SCLK=10, MOSI=11, MISO=12 |
| SPI2_HOST | 键盘 CS / INT | CS=7, INT=8 |
| SPI2_HOST | 鼠标 CS / INT | CS=9, INT=13 |

两条 SPI 总线物理隔离（Host=SPI2、Device=SPI3），两核可并发访问互不干扰。INT 引脚均为开漏，需内部上拉（代码已配置）。

## host.c 详解（USB Host，Core 1）

- 每颗 CH374T 用一个 `ch374_dev_t` 描述（`dev_kb` / `dev_mouse`），含 SPI 句柄、INT 引脚、热插拔状态、toggle、低速标志、`is_keyboard`。
- **寄存器辅助函数**（带 `dev` 参数）：`ch374_write_reg / read_reg / write_multi / read_multi`，帧格式 `[addr][0x80 写 / 0xC0 读][data...]`，SPI 10MHz，`spi_device_polling_transmit`。
- `ch374_exec_transaction(pid, toggle)`：下发 TOKEN + H_CTRL，轮询 INT# 读 `USB_STATUS`，50ms 超时返回 `0xFF`。
- `ch374_control_transfer(setup)`：SETUP 阶段 + IN 状态阶段，NAK 重试最多 100 次。
- **热插拔状态机** `process_usb_device_statemachine`（非阻塞，基于 `esp_timer_get_time`）：
  ```
  DISCONNECTED（HUB_SETUP[3] 检测插入）→ WAIT_DEBOUNCE(200ms)
   → BUS_RESETTING(20ms，测速 is_low_speed) → WAIT_STABLE(10ms，使能端口)
   → ENUMERATING（Set_Address→Set_Configuration→[键盘]Set_Protocol(boot)）
   → ONLINE（数据端点 IN 轮询：键盘 EP1 / 鼠标 EP2，按 DATA0/1 PID 判定成功，读 RAM_HOST_RECV 解析）
  ```
  枚举失败或非 NAK 异常且设备拔出 → 回 `DISCONNECTED` 重来。
- **设备相关**：鼠标为 **罗技 G304（经 C53F 接收器）专用**——数据在 **EP2**（令牌 0x92），固定 Report ID 0x02 共 9 字节：`[0]=0x02 [1..2]=按键位图 u16 [3..4]=X int16 [5..6]=Y int16 [7]=滚轮 int8 [8]=水平滚轮(忽略)`，按首字节 0x02 过滤同端点上的多媒体报告；全 16 位位移，非 boot 协议（对鼠标不下发 Set_Protocol）。键盘读 8 字节（modifier + 6 keycode）。换设备需重新确认端点号、报告格式与长度。
- 主循环 `POLL_INTERVAL_US = 1000`（1ms），每轮驱动两个状态机后补齐空闲时间。

## device.c 详解（USB Device，Core 0）

- CH374T 设备模式：软复位 → `SYS_CTRL=0x40`（使能 USB 引脚 OE 极性）→ `USB_SETUP=0x03`（设备模式 + 内部上拉）→ `INTER_EN=0x07`（总线复位/挂起/传输完成中断）。
- **描述符**：`xinput_device_descriptor` / `xinput_config_descriptor`（XInput，端点硬编码为 IN `0x82` / OUT `0x02`）。
- **EP0 控制状态机** `handle_ep0_setup` / `handle_usb_transfer`：
  - Get Descriptor（设备/配置）、Set Address（IN ACK 后生效）、Set Configuration（配置 EP2）、XInput 厂商握手（`0xC1/0x01`，按 wValue 回 8/20 字节）；其余 STALL。
  - 处理 ZLP（`zlp_pending`）、DATA0/1 toggle（`ep0_*_tog`）。
- **EP2 IN 上报**：`is_configured && !ep2_tx_in_flight` 时取快照 → `build_xinput_report` → 写 `RAM_ENDP2_TRAN` + 长度 → EP2 响应切 ACK 等主机取走；发完切回 NAK 防重传风暴。OUT（主机震动）仅翻转 toggle。
- 主循环 `esp_rom_delay_us(1)` 让出 CPU 但保持微秒级中断响应。

### 换算流水线（`build_xinput_report`，曲线/压枪/抖动已删除）

1. **修饰键**：Ctrl→B，Alt→DOWN，Shift→L3。
2. **按键 LUT**：`s_key_lut_btn[256]`（HID keycode → Xbox 按键位）、`s_key_lut_wasd[256]`（WASD → 4bit 方向位）。
3. **左摇杆（移动）**：`s_lx_lut/s_ly_lut[16]`，含对角线归一化（±23170 ≈ 32767/√2）。
4. **右摇杆（视角，正常模式）**：鼠标 dx/dy →
   - 固定倍率线性映射（带饱和）：`dx * XINPUT_MOUSE_TO_STICK_SCALE_HIP(40)`，开镜（右键按下 `is_ads`）切换为 `ADS(25)`，clamp ±32767。
   - EMA 平滑：定点 `<<8`，`EMA_SHIFT=1`。
   - `square_to_circle_int()`：方→圆映射（`isqrt` + 先乘后除，平方和用 `uint32_t` 防溢出）。
5. **舔包模式**（鼠标侧键 bit4 按住）：鼠标改映射到**左**摇杆（scale 180 + 独立 EMA），WASD 改到右摇杆，滚轮置满右摇杆 Y，左/右键→A/X。
6. **滚轮**：非 0 → Y 按键。

## 构建 / 烧录

标准 ESP-IDF（需先 `export.sh`/`export.bat` 激活环境）：

```bash
idf.py set-target esp32s3     # 首次或换目标时
idf.py build                  # 构建
idf.py -p <PORT> flash        # 烧录
idf.py -p <PORT> monitor      # 串口监视（Ctrl+] 退出）
```

- `sdkconfig` 被 gitignore，由 `set-target`/`menuconfig` 生成；`sdkconfig.old` 同样忽略。
- 组件源文件在 `main/CMakeLists.txt` 注册，新增 `.c` 需加进 `SRCS`。

## 开发约定与注意事项

- **注释语言为中文**，请保持一致。
- **两套 CH374 寄存器辅助函数是有意重复的**：`host.c`（带 `dev` 参数，SPI2/多设备）与 `device.c`（单设备，SPI3）各自一份，分属不同总线与核，不要强行合并。
- **真实数据通路是 `input_bridge`**；`host.h` 里的 `g_gamepad`/`gamepad_state_t` 是遗留声明，未被使用，改动勿依赖它。
- **跨核共享只能走 bridge** 的 `bridge_push_*` / `bridge_get_snapshot`（内部已用 `portMUX` 保护）；不要直接读写对方任务的状态。
- **换算算法与 AG32 的关系**：本项目已删除响应曲线 / 后坐力补偿 / 抖动（`update_mouse_curve` / `apply_dynamic_curve` 已不存在），鼠标→摇杆为固定倍率线性映射；EMA 平滑与舔包模式保留。AG32 版（`src/main.c`）仍是旧算法，两边不再要求同步。
- **设备相关格式**：host.c 鼠标解析写死为 **G304/C53F 接收器的 Report ID 0x02（9 字节，EP2，16 位位移）**，键盘读 8 字节 boot 报文（EP1）；换其他键鼠需重新确认端点号、报告格式与字段布局。
- **两个任务都喂任务级看门狗**：改动主循环时保留 `esp_task_wdt_reset()`，长时间阻塞会触发 WDT 复位。
- 文件使用 **CRLF 行尾**，编辑时保持一致，避免整文件行尾抖动。

## 常见改动速查

| 想做的事 | 改哪里 |
|---|---|
| 增删按键映射 | `device.c`：`s_key_lut_btn` / `s_key_lut_wasd`（HID keycode） |
| 调鼠标灵敏度 | `device.c`：`XINPUT_MOUSE_TO_STICK_SCALE_{HIP,ADS,LOOT}`（固定倍率，腰射/开镜/舔包三档） |
| 调平滑 | `device.c` `build_xinput_report`：`EMA_SHIFT`（正常与舔包各一处） |
| 改轮询周期 | `host.c`：`POLL_INTERVAL_US` |
| 改 SPI 频率 | `host.c` / `device.c` 的 `clock_speed_hz`（各 10MHz） |
| 改引脚 | `host.c`（Host 侧）/ `device.c`（Device 侧）顶部 `#define`，并核对 PCB |
| 换键鼠设备 | `host.c`：枚举参数 + `STATE_ONLINE` 里的端点令牌与报文解析（现为 G304 专用） |
| 改 USB 身份/报文 | `device.c`：`xinput_*_descriptor`、`XInputPad.h`（主机按 VID/PID 缓存驱动） |
| 改 EP0 控制行为 | `device.c`：`handle_ep0_setup` / `handle_usb_transfer` |
