# SoftUart

> Software-based UART (bit-banged) for resource-constrained 8-bit MCUs / 家电控制 MCU
> 面向资源受限 8 位单片机 / 家电控制 MCU 的纯软件（位操作模拟）串口模块

[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-8051%20%7C%20%20generic%208-bit-blue.svg)](#)
[![Language](https://img.shields.io/badge/language-C--C99-lightgrey.svg)](#)

> ⚠️ **节拍与波特率速查 / Tick-to-baud cheat sheet** — 本模块 **1 个位 = 2 个节拍**，即 `节拍间隔 = 1 / (2 × 波特率)`。
> **9600 bps（位时间 104 μs）→ 定时器中断内每 52 次调用一次 `Soft_Uart_Process()`；4800 bps（位时间 208 μs）→ 每 104 次调用一次。**
> 详见 [5.3](#53-提供节拍) 节。 / See [Section 5.3](#53-provide-the-tick).

---

## 中文

### 1. 模块简介

`SoftUart` 是一个**不依赖硬件 UART 外设**的串口收发模块。它完全依靠一个周期性调用的“节拍函数”（Tick）在 GPIO 上模拟 UART 的时序，因此在只有少量 GPIO、没有 UART 外设或 UART 已被其它业务占用的芯片上也能提供串口能力。

核心设计：

| 特性         | 说明                                                   |
| ------------ | ------------------------------------------------------ |
| 全双工       | TX 与 RX 各用一根独立 GPIO，互不干扰                   |
| 非阻塞发送   | 发送数据先进环形队列，实际发送在节拍函数中完成         |
| 中断驱动接收 | 在节拍函数中采样 RX 引脚，识别起始位 / 数据位 / 停止位 |
| 帧同步       | 通过帧头 / 帧尾自动对齐字节流，校验失败自动丢弃        |
| 零动态内存   | 全部使用静态数组，无 `malloc`，适合 RAM 紧张的 MCU     |
| 无第三方依赖 | 除自身头文件外无任何依赖                               |

### 2. 特性

- **纯 GPIO 实现**：不需要 UART 外设，不占用定时器以外的任何硬件资源。
- **中断安全**：发送缓冲区通过 `head` / `tail` 双索引实现，可在中断与主循环之间安全传递数据。
- **XOR 校验 + 帧头帧尾**：发送端自动追加校验位与帧尾，接收端自动校验并给出整帧有效标志。
- **静默丢弃策略**：接收缓冲区以帧头 `0xDD` 开始同步，未对齐或校验失败的帧直接丢弃，不阻塞、不返回错误码。
- **可移植**：所有 GPIO 操作被收敛到 `SoftUart.h` 中的两个宏，移植到其它 MCU 只需修改宏定义。

### 3. 硬件资源需求

| 资源                            | 用途       | 说明                                             |
| ------------------------------- | ---------- | ------------------------------------------------ |
| 1 路 GPIO（推挽输出）           | TX         | 空闲为高电平                                     |
| 1 路 GPIO（输入，建议关闭上拉） | RX         | 空闲为高电平                                     |
| 1 个周期性节拍源                | 时基       | 由定时器中断 / 主循环分频提供，建议 50～100 μs   |
| 约 11 字节 RAM                  | 收发缓冲区 | `SOFTUART_BUF_SIZE + SOFTUART_RX_LEN + 状态变量` |

> ⚠️ 本模块为**单线半双工式时序复用设计**：`SoftUART_Tick_RX()` 内部会检查 `su_tx_busy`，发送过程中直接跳过接收采样。也就是说本模块**不支持全双工同时收发**，TX 与 RX 在硬件上应为两根独立引脚，但在时间上互斥。若业务要求真正全双工，请去掉该互斥判断，并确认总线上不存在自己回环导致的自干扰。

### 4. 文件结构

```
SoftUart/
├── SoftUart.c    # 模块实现
├── SoftUart.h    # 对外接口、GPIO 宏、帧格式常量
└── README.md     # 本文档
```

### 5. 快速开始

#### 5.1 配置引脚（必做）

所有平台相关操作都集中在 `SoftUart.h`，移植时**只需修改这一处**：

```c
/* ==== 配置 GPIO ====
 * PB5 -> TX
 * PB4 -> RX
 */
#define RX_PIN P14

/* 注意：本平台 TX 为低有效，逻辑高 = 引脚输出 0 */
#define UART_TX_HIGH() P15 = 0
#define UART_TX_LOW()  P15 = 1
```

同时调整帧长度：

```c
#define SOFTUART_BUF_SIZE 5   /* 发送帧长度（字节） */
#define SOFTUART_RX_LEN  6    /* 接收帧长度（字节） */
```

#### 5.2 初始化

```c
#include "SoftUart.h"

void System_Init(void)
{
    SoftUART_Init();   /* 配置 TX/RX 方向与初始电平 */
}
```

`SoftUART_Init()` 会把 RX 配为输入并关闭内部上拉，把 TX 配为推挽输出，并输出现位（空闲高电平）。

#### 5.3 提供节拍

**这是本模块唯一且最重要的集成点。** 在固定周期的定时器中断或主循环分频中调用 `Soft_Uart_Process()`：

```c
/* Timer1 中断，每 52 个计数进入一次（见下方注意事项） */
void Timer1_ISR(void) interrupt 1
{
    static unsigned char div = 0;

    if (++div >= UART_TICK_DIV)   /* UART_TICK_DIV = 52 → 9600 bps */
    {
        div = 0;
        Soft_Uart_Process();
    }
}
```

节拍周期决定波特率。**本模块每个位需要 2 个节拍**（发送侧由 `Soft_Uart_Process()` 内的 2 分频实现，接收侧由 `SAMPLE_NUM = 1` 实现），因此：

```
节拍间隔 = 1 / (2 × 波特率)
```

⚠️ **注意事项：节拍间隔必须凑整到定时器计数**

波特率对应的理想节拍间隔往往不是整数，直接取整会引入偏差。工程上通常固定一个定时器中断周期，再在中断里做整数分频：

| 目标波特率 | 理想位时间 | 理想节拍间隔 | 定时器中断周期 | **中断内分频**        | 实际波特率           |
| ---------- | ---------- | ------------ | -------------- | --------------------- | -------------------- |
| 9600 bps   | 104 μs     | 52 μs        | 52 μs          | **每 52 次调用一次**  | ≈ 9615 bps（+0.16%） |
| 4800 bps   | 208 μs     | 104 μs       | 104 μs         | **每 104 次调用一次** | ≈ 4808 bps（+0.16%） |

即：**9600 bps 时定时器每 52 个中断周期调用一次 `Soft_Uart_Process()`；4800 bps 时每 104 个中断周期调用一次。**

> 换算示例：若定时器中断周期为 1 μs，则 `UART_TICK_DIV = 52`；若为 4 μs，则 `UART_TICK_DIV = 13`（52 / 4）；若为 2.5 μs，则取 21（52 / 2.5 ≈ 21）。**尽量取整数商**，避免累积误差。
>
> 波特率误差需满足 **≤ 2%** 才能与常规 UART 稳定通信；9600 bps 下约 ±1.5%，4800 bps 下约 ±3.75%。上表两种配置误差均在 0.2% 以内，是安全的。

#### 5.4 发送数据

```c
unsigned char frame[SOFTUART_BUF_SIZE];

frame[1] = 0x01;   /* 命令 */
frame[2] = 0x02;   /* 数据 */
frame[3] = 0x03;   /* 数据 */
/* frame[0] / 帧尾 / 校验位由 SoftUART_SendBuffer 自动填写 */

SoftUART_SendBuffer(frame);
```

`SoftUART_SendBuffer()` 会自动完成：

1. 对 `buf[1] .. buf[N-3]` 计算 XOR 校验并写入 `buf[N-2]`；
2. 写入帧头 `buf[0] = 0xCC`；
3. 写入帧尾 `buf[N-1] = 0x77`；
4. 逐字节入队发送。

#### 5.5 接收数据

```c
unsigned char rx_frame[SOFTUART_RX_LEN];

void Main_Loop(void)
{
    if (SoftUART_ReadData(rx_frame))   /* 返回 1 表示收到完整有效帧 */
    {
        Handle_Command(rx_frame[1], rx_frame[2]);
    }
}
```

`SoftUART_ReadData()` 行为说明：

| 返回值 | 含义                                         |
| ------ | -------------------------------------------- |
| `0`    | 尚无完整帧，或 XOR 校验失败（帧被丢弃）      |
| `1`    | 收到一帧完整且校验通过的数据，已拷贝到 `buf` |

> ⚠️ 该接口**没有区分**“无数据”和“校验失败”，两者都返回 `0`。若业务需要统计通信错误率，请在模块内增加错误计数器。

### 6. 帧格式

#### 发送帧（`SOFTUART_BUF_SIZE = 5`）

| 字节 | 名称   | 值       |
| ---- | ------ | -------- |
| 0    | 帧头   | `0xCC`   |
| 1    | 数据 0 | 业务数据 |
| 2    | 数据 1 | 业务数据 |
| 3    | 数据 2 | 业务数据 |
| 4    | 帧尾   | `0x77`   |

> 当 `SOFTUART_BUF_SIZE = 5` 时，校验位与帧尾**占用了同一字节**，`buf[N-2]` 会立即被帧尾 `0x77` 覆盖，因此当前配置下校验位实际上无效。请将 `SOFTUART_BUF_SIZE` 设为 **≥ 6** 才能同时携带独立的校验位与帧尾。

#### 接收帧（`SOFTUART_RX_LEN = 6`）

| 字节 | 名称   | 值               |
| ---- | ------ | ---------------- |
| 0    | 帧头   | `0xDD`           |
| 1    | 数据 0 | 业务数据         |
| 2    | 数据 1 | 业务数据         |
| 3    | 数据 2 | 业务数据         |
| 4    | 校验   | XOR(`buf[1..3]`) |
| 5    | 帧尾   | `0x88`           |

### 7. API 参考

| 函数                                                  | 说明                                                                                             |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `void SoftUART_Init(void)`                            | 配置 TX 为推挽输出、RX 为输入并设置初始电平。**上电必须调用一次。**                              |
| `void Soft_Uart_Process(void)`                        | 模块主节拍。内部调用 `SoftUART_Tick()`（带 2 分频）与 `SoftUART_Tick_RX()`。**必须周期性调用。** |
| `void SoftUART_SendByte(unsigned char dat)`           | 单字节入队。队列满时阻塞等待空位。                                                               |
| `void SoftUART_SendBuffer(unsigned char* buf)`        | 填充帧头 / XOR 校验 / 帧尾后整帧入队发送。会**原地修改** `buf`。                                 |
| `unsigned char SoftUART_ReadData(unsigned char* buf)` | 若收到完整有效帧则拷贝到 `buf` 并返回 `1`，否则返回 `0`。                                        |

### 8. 已知问题与改进建议

1. **`SoftUART_GetStatus()` 已在头文件声明，但实现被注释掉。**
   链接时会报未定义符号。请补全实现：

   ```c
   unsigned char SoftUART_GetStatus(void)
   {
       return su_tx_busy;
   }
   ```

2. **`SOFTUART_BUF_SIZE = 5` 时发送校验位失效。**
   校验字节 `buf[3]` 会被帧尾 `buf[4]` 覆盖。建议设为 `6`，与接收帧结构保持一致。

3. **波特率由节拍分频决定，必须在真机上核对。**
   发送侧每个位消耗 2 个节拍（`Soft_Uart_Process()` 内的 `time_counter >= 2`），接收侧同样每个位消耗 2 个节拍（`SAMPLE_NUM = 1` 使其隔拍采样）。**两侧是对称的**，均满足 `节拍间隔 = 1 / (2 × 波特率)`。例如 9600 bps 对应 52 μs 节拍，4800 bps 对应 104 μs 节拍。详见 5.3 节《提供节拍》。

4. **发送队列阻塞且无超时。**
   `SoftUART_SendByte()` 在队列满时死等。`SOFTUART_BUF_SIZE = 5` 时有效容量仅 **4 字节**（环形队列需空出一个位置）。建议改为“满则丢弃并返回错误码”，或在中断中发送。

5. **`su_rx_index` / `su_rx_buf` 未做并发保护。**
   `SoftUART_Tick_RX()` 在中断中写、`SoftUART_ReadData()` 在主循环读写，建议在中断中仅置标志位，拷贝与校验全部放到主循环。

6. **接收没有超时与重同步机制。**
   若传输中途丢位，模块会一直等待帧尾。建议增加帧间隔超时（如超过 2 个帧长未收到数据则清空 `su_rx_index`）。

7. **注释与源码编码为 GBK。**
   若提交到 GitHub 建议统一转为 UTF-8，避免乱码。

### 9. 移植清单

- [ ] 修改 `SoftUart.h` 中 `RX_PIN` / `UART_TX_HIGH()` / `UART_TX_LOW()` / `P1CON` 相关初始化
- [ ] 确认 TX 逻辑极性（是否低有效）
- [ ] 按实际帧长设置 `SOFTUART_BUF_SIZE` / `SOFTUART_RX_LEN`
- [ ] 若与接收端协议不一致，修改 `IRON_SEND_FRAME_HEAD/END`、`IRON_REC_FRAME_HEAD/END`
- [ ] 确认节拍周期，计算并验证波特率
- [ ] 补全 `SoftUART_GetStatus()` 实现
- [ ] 在主循环或中断中加入 `Soft_Uart_Process()`
- [ ] 用逻辑分析仪验证帧头、校验、帧尾

### 10. License

MIT

---

## English

### 1. Overview

`SoftUart` is a **software (bit-banged) UART** implementation for MCUs that lack a hardware UART peripheral. Instead of relying on a dedicated serial controller, it emulates the UART timing entirely on GPIO pins by means of a periodically invoked tick function. This makes serial communication possible on chips with almost no I/O budget, or where the hardware UART is already occupied by another subsystem.

Key design points:

| Aspect              | Description                                                                              |
| ------------------- | ---------------------------------------------------------------------------------------- |
| Simplex-oriented    | TX and RX use two separate GPIOs, but they are time-multiplexed                          |
| Non-blocking TX     | Outgoing bytes are queued; the wire is driven from the tick function                     |
| Interrupt-driven RX | RX is sampled inside the tick, detecting start / data / stop bits                        |
| Frame sync          | Frame head and tail are used to realign the byte stream; bad frames are silently dropped |
| Zero dynamic memory | Static arrays only — no `malloc`, suitable for RAM-constrained MCUs                      |
| No dependencies     | Nothing beyond its own header file                                                       |

### 2. Features

- **Pure GPIO implementation** — no UART peripheral required.
- **Interrupt-safe** — the TX ring buffer uses dual `head` / `tail` indices so data can be handed over safely between ISR and main loop.
- **XOR checksum + framing** — the transmitter appends a checksum and a tail byte; the receiver validates and exposes a whole-frame valid flag.
- **Silent discard** — frames that are not head-aligned or fail checksum validation are dropped without blocking or returning an error code.
- **Portable** — all GPIO access is isolated into two macros in `SoftUart.h`; porting to another MCU only requires editing those macros.

### 3. Hardware Requirements

| Resource                                    | Purpose   | Notes                                                           |
| ------------------------------------------- | --------- | --------------------------------------------------------------- |
| 1 GPIO (push-pull output)                   | TX        | Idle level is high                                              |
| 1 GPIO (input, pull-up preferably disabled) | RX        | Idle level is high                                              |
| 1 periodic tick source                      | Time base | From a timer ISR or a divided main loop; ~50–100 µs recommended |
| ~11 bytes RAM                               | Buffers   | TX buffer + RX buffer + state variables                         |

> ⚠️ This module uses a **single-wire, time-multiplexed** arrangement: `SoftUART_Tick_RX()` returns early when `su_tx_busy` is set, so reception is skipped while transmitting. True simultaneous TX and RX is therefore **not supported**. TX and RX must still be two distinct GPIOs on the hardware side. Remove the mutex if full duplex is required, and verify that self-reception on the bus cannot occur.

### 4. Directory Layout

```
SoftUart/
├── SoftUart.c    # Implementation
├── SoftUart.h    # Public API, GPIO macros, frame constants
└── README.md     # This document
```

### 5. Getting Started

#### 5.1 Configure the pins (mandatory)

All platform-specific code is confined to `SoftUart.h`; porting only requires editing this section:

```c
/* ==== Configure GPIO ====
 * PB5 -> TX
 * PB4 -> RX
 */
#define RX_PIN P14

/* Note: TX is active-low on this platform; logic high = pin driven to 0 */
#define UART_TX_HIGH() P15 = 0
#define UART_TX_LOW()  P15 = 1
```

Also adjust the frame lengths:

```c
#define SOFTUART_BUF_SIZE 5   /* TX frame size in bytes */
#define SOFTUART_RX_LEN  6    /* RX frame size in bytes */
```

#### 5.2 Initialization

```c
#include "SoftUart.h"

void System_Init(void)
{
    SoftUART_Init();   /* Configure TX/RX directions and idle levels */
}
```

`SoftUART_Init()` configures RX as an input with its internal pull-up disabled, configures TX as a push-pull output, and drives the idle (high) level.

#### 5.3 Provide the tick

**This is the single most important integration point.** Call `Soft_Uart_Process()` from a fixed-period timer interrupt, or from a divided main loop:

```c
/* Timer1 ISR, entered once every 52 counts (see the note below) */
void Timer1_ISR(void) interrupt 1
{
    static unsigned char div = 0;

    if (++div >= UART_TICK_DIV)   /* UART_TICK_DIV = 52 → 9600 bps */
    {
        div = 0;
        Soft_Uart_Process();
    }
}
```

The tick period determines the baud rate. **This module needs 2 ticks per bit** — on the transmit side via the divide-by-2 inside `Soft_Uart_Process()`, and on the receive side via `SAMPLE_NUM = 1`. Therefore:

```
tick interval = 1 / (2 × baud rate)
```

⚠️ **Note: the tick interval must be rounded to an integer timer count**

The ideal tick interval for a given baud rate is rarely a whole number, and rounding it introduces error. In practice, fix one timer interrupt period and perform integer division inside the ISR:

| Target baud | Ideal bit time | Ideal tick interval | Timer ISR period | **Divider in ISR**            | Effective baud      |
| ----------- | -------------- | ------------------- | ---------------- | ----------------------------- | ------------------- |
| 9600 bps    | 104 µs         | 52 µs               | 1 µs             | **call once every 52 ticks**  | ≈ 9615 bps (+0.16%) |
| 4800 bps    | 208 µs         | 104 µs              | 1 µs             | **call once every 104 ticks** | ≈ 4808 bps (+0.16%) |

That is: **for 9600 bps, call `Soft_Uart_Process()` once every 52 timer interrupts; for 4800 bps, once every 104.**

> Conversion examples: if the timer ISR period is 1 µs, use `UART_TICK_DIV = 52`; if it is 4 µs, use `UART_TICK_DIV = 13` (52 / 4); if it is 2.5 µs, use 21 (52 / 2.5 ≈ 21). **Prefer exact integer quotients** to avoid accumulating error.
>
> The baud error must stay **within ±2%** for reliable communication with a conventional UART — about ±1.5% at 9600 bps and ±3.75% at 4800 bps. Both configurations above are within 0.2%, which is comfortably safe.

#### 5.4 Transmitting

```c
unsigned char frame[SOFTUART_BUF_SIZE];

frame[1] = 0x01;   /* Command */
frame[2] = 0x02;   /* Data */
frame[3] = 0x03;   /* Data */
/* frame[0], checksum and tail are filled in by SoftUART_SendBuffer() */

SoftUART_SendBuffer(frame);
```

`SoftUART_SendBuffer()` automatically:

1. computes the XOR checksum over `buf[1] .. buf[N-3]` and stores it in `buf[N-2]`;
2. writes the frame head `buf[0] = 0xCC`;
3. writes the frame tail `buf[N-1] = 0x77`;
4. queues every byte for transmission.

#### 5.5 Receiving

```c
unsigned char rx_frame[SOFTUART_RX_LEN];

void Main_Loop(void)
{
    if (SoftUART_ReadData(rx_frame))   /* returns 1 on a complete valid frame */
    {
        Handle_Command(rx_frame[1], rx_frame[2]);
    }
}
```

`SoftUART_ReadData()` behaviour:

| Return | Meaning                                                             |
| ------ | ------------------------------------------------------------------- |
| `0`    | No complete frame yet, or XOR checksum failed (frame discarded)     |
| `1`    | A complete, checksum-valid frame was received and copied into `buf` |

> ⚠️ This API does **not** distinguish "no data" from "checksum failure" — both return `0`. Add an error counter inside the module if the application needs link-quality statistics.

### 6. Frame Format

#### TX frame (`SOFTUART_BUF_SIZE = 5`)

| Byte | Name   | Value   |
| ---- | ------ | ------- |
| 0    | Head   | `0xCC`  |
| 1    | Data 0 | Payload |
| 2    | Data 1 | Payload |
| 3    | Data 2 | Payload |
| 4    | Tail   | `0x77`  |

> With `SOFTUART_BUF_SIZE = 5`, the checksum and the tail **share the same byte**: `buf[N-2]` is immediately overwritten by the tail `0x77`, so the checksum is effectively disabled. Set `SOFTUART_BUF_SIZE` to **≥ 6** to carry a distinct checksum byte and tail byte.

#### RX frame (`SOFTUART_RX_LEN = 6`)

| Byte | Name     | Value            |
| ---- | -------- | ---------------- |
| 0    | Head     | `0xDD`           |
| 1    | Data 0   | Payload          |
| 2    | Data 1   | Payload          |
| 3    | Data 2   | Payload          |
| 4    | Checksum | XOR(`buf[1..3]`) |
| 5    | Tail     | `0x88`           |

### 7. API Reference

| Function                                              | Description                                                                                                   |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| `void SoftUART_Init(void)`                            | Configures TX as push-pull output and RX as input, then sets idle levels. **Must be called once at startup.** |
| `void Soft_Uart_Process(void)`                        | Main module tick. Calls `SoftUART_Tick()` (÷2) and `SoftUART_Tick_RX()`. **Must be called periodically.**     |
| `void SoftUART_SendByte(unsigned char dat)`           | Enqueues one byte. Blocks when the queue is full.                                                             |
| `void SoftUART_SendBuffer(unsigned char* buf)`        | Fills head / XOR checksum / tail, then queues the whole frame. **Modifies `buf` in place.**                   |
| `unsigned char SoftUART_ReadData(unsigned char* buf)` | Copies a complete valid frame into `buf` and returns `1`, otherwise returns `0`.                              |

### 8. Known Issues and Improvement Ideas

1. **`SoftUART_GetStatus()` is declared in the header but its implementation is commented out.**
   This yields an undefined symbol at link time. Restore it:

   ```c
   unsigned char SoftUART_GetStatus(void)
   {
       return su_tx_busy;
   }
   ```

2. **The transmit checksum is disabled when `SOFTUART_BUF_SIZE = 5`.**
   The checksum byte `buf[3]` gets overwritten by the tail `buf[4]`. Use `6` to match the RX frame layout.

3. **The baud rate is set by the tick divider and must be verified on hardware.**
   Each bit on the transmit side consumes 2 ticks (the `time_counter >= 2` divider inside `Soft_Uart_Process()`), and each bit on the receive side likewise consumes 2 ticks (`SAMPLE_NUM = 1` makes it sample on alternate ticks). **The two directions are symmetric**, both obeying `tick interval = 1 / (2 × baud rate)`. For example, 9600 bps corresponds to a 52 µs tick and 4800 bps to a 104 µs tick. See Section 5.3 _Provide the tick_.

4. **The TX queue blocks without a timeout.**
   `SoftUART_SendByte()` spins forever when the queue is full. With `SOFTUART_BUF_SIZE = 5` the usable capacity is only **4 bytes** (a ring buffer must keep one slot free). Consider dropping the byte and returning an error code instead.

5. **`su_rx_index` / `su_rx_buf` lack concurrency protection.**
   `SoftUART_Tick_RX()` writes from the ISR while `SoftUART_ReadData()` reads and writes from the main loop. Set flags in the ISR and do all copying and checking in the main loop.

6. **No inter-frame timeout or resynchronisation on RX.**
   If a bit is lost mid-frame, the receiver waits indefinitely for the tail byte. Add an inter-byte timeout (e.g. clear `su_rx_index` when more than two frame lengths elapse).

7. **Source comments are encoded in GBK.**
   Convert to UTF-8 before publishing on GitHub to avoid mojibake.

### 9. Porting Checklist

- [ ] Update `RX_PIN`, `UART_TX_HIGH()`, `UART_TX_LOW()` and the `P1CON` setup in `SoftUart.h`
- [ ] Confirm the TX logic polarity (active-low vs. active-high)
- [ ] Set `SOFTUART_BUF_SIZE` / `SOFTUART_RX_LEN` to the real frame lengths
- [ ] Adjust `IRON_SEND_FRAME_HEAD/END` and `IRON_REC_FRAME_HEAD/END` if the peer protocol differs
- [ ] Confirm the tick period and verify the resulting baud rate
- [ ] Re-add the `SoftUART_GetStatus()` implementation
- [ ] Call `Soft_Uart_Process()` from the main loop or a timer ISR
- [ ] Validate head, checksum and tail with a logic analyzer

### 10. License

MIT
