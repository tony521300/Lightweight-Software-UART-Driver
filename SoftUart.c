#include "SoftUart.h"

// ========================================
//           软件 UART 初始化
// ========================================
void SoftUART_Init(void)
{
    P1CON &= ~(1 << 4);  // 设置为输入
    P1PH &= ~(1 << 4);   // 禁用上拉

    P1CON |= (1 << 5);  // 设置为输出（强推挽）

    UART_TX_HIGH();  // TX 线空闲电平为高
}

/*================================  发送数据 ========================================*/

unsigned char su_tx_buf[SOFTUART_BUF_SIZE];
unsigned char su_tx_head = 0;
unsigned char su_tx_tail = 0;

unsigned char su_tx_busy   = 0;
unsigned char su_tx_byte   = 0;
unsigned char su_tx_bitcnt = 0;

// ==========================
// 入队发送 1 字节（非阻塞）
// ==========================
void SoftUART_SendByte(unsigned char dat)
{
    unsigned char next = (su_tx_head + 1) % SOFTUART_BUF_SIZE;
    // 队列满，阻塞等待
    while (next == su_tx_tail)
    {
    }

    su_tx_buf[su_tx_head] = dat;
    su_tx_head            = next;
}
// ==========================
// 入队发送 buffer
// ==========================
void SoftUART_SendBuffer(unsigned char* buf)
{
    unsigned char i;
    unsigned char check_sum = 0;
    /* 计算校验 */
    for (i = 1; i < SOFTUART_BUF_SIZE - 2; i++)
    {
        check_sum ^= buf[i];
    }
    /* 异或 校验位 */
    buf[SOFTUART_BUF_SIZE - 2] = check_sum;
    buf[0]                     = IRON_SEND_FRAME_HEAD;  // 帧头
    buf[SOFTUART_BUF_SIZE - 1] = IRON_SEND_FRAME_END;   // 帧尾
    // 发送 buffer 中的每个字节
    for (i = 0; i < SOFTUART_BUF_SIZE; i++)
    {
        SoftUART_SendByte(buf[i]);
    }
}

// unsigned char SoftUART_GetStatus(void)
// {
//     return su_tx_busy;
// }

// ==========================
// Timer1 ISR 每 100us 调用，发送 1 bit  9600
// ==========================
void SoftUART_Tick(void)
{
    if (!su_tx_busy)
    {
        // 队列有字节，开始发送
        if (su_tx_head != su_tx_tail)
        {
            su_tx_byte   = su_tx_buf[su_tx_tail];
            su_tx_tail   = (su_tx_tail + 1) % SOFTUART_BUF_SIZE;
            su_tx_bitcnt = 9;  // start + 8bit + stop
            su_tx_busy   = 1;

            UART_TX_LOW();  // start bit
            return;
        }
        else
        {
            return;  // 队列空，不发送
        }
    }

    // 正在发送
    if (su_tx_bitcnt > 1)
    {
        // 数据位 8bit
        if (su_tx_byte & 0x01)
            UART_TX_HIGH();
        else
            UART_TX_LOW();
        su_tx_byte >>= 1;
    }
    else
    {
        // 停止位
        UART_TX_HIGH();
    }

    su_tx_bitcnt--;
    if (su_tx_bitcnt == 0)
    {
        su_tx_busy = 0;
    }
}

/*================================  接收数据 ========================================*/

unsigned char UART_RX_READ(void)
{
    return RX_PIN;  // 1 = high, 0 = low
}

volatile unsigned char su_rx_buf[SOFTUART_RX_LEN];

volatile unsigned char su_rx_busy   = 0;
volatile unsigned char su_rx_bitcnt = 0;
volatile unsigned char su_rx_bitidx = 0;
volatile unsigned char su_rx_byte   = 0;
volatile unsigned char su_rx_sample = 0;

volatile unsigned char su_rx_index = 0;
volatile unsigned char su_rx_done  = 0;

#define SAMPLE_NUM 1

void SoftUART_Tick_RX(void)
{
    if (su_tx_busy) return;

    /* ---------- 空闲态：直接检测 start ---------- */
    if (!su_rx_busy)
    {
        if (UART_RX_READ() == 0)
        {
            su_rx_busy   = 1;
            su_rx_bitcnt = 9;  // 8 data + stop
            su_rx_bitidx = 0;
            su_rx_byte   = 0;
            su_rx_sample = SAMPLE_NUM;  // ★ 你现在验证过最稳的
        }
        return;
    }

    if (su_rx_sample)
    {
        su_rx_sample--;
        return;
    }

    su_rx_sample = SAMPLE_NUM;

    /* ---------- 数据位 ---------- */
    if (su_rx_bitcnt > 1)
    {
        if (UART_RX_READ())
            su_rx_byte |= (1 << su_rx_bitidx);

        su_rx_bitidx++;
    }
    /* ---------- 停止位 ---------- */
    else
    {
        if (UART_RX_READ())  // stop bit OK
        {
            /* ===== 帧头同步 ===== */
            if (su_rx_index == 0)
            {
                if (su_rx_byte == IRON_REC_FRAME_HEAD)
                {
                    su_rx_buf[su_rx_index++] = su_rx_byte;
                }
                // 非 0x77：直接丢弃
            }
            else
            {
                su_rx_buf[su_rx_index++] = su_rx_byte;
                if (su_rx_index >= SOFTUART_RX_LEN)
                {
                    if (su_rx_byte == IRON_REC_FRAME_END)
                    {
                        su_rx_done = 1;
                    }
                    su_rx_index = 0;
                }
            }
        }
        else
        {
            // stop bit 错，重置帧
            su_rx_index = 0;
        }
    }

    su_rx_bitcnt--;

    if (su_rx_bitcnt == 0)
        su_rx_busy = 0;
}

void Soft_Uart_Process(void)
{
    static unsigned char time_counter = 0;

    time_counter++;
    if (time_counter >= 2)
    {
        time_counter = 0;
        SoftUART_Tick();
    }
    SoftUART_Tick_RX();
}

/* ==========================
 * 主循环读取接口
 * ========================== */
unsigned char SoftUART_ReadData(unsigned char* buf)
{
    unsigned char i;
    unsigned char check = 0;

    if (!su_rx_done) return 0;

    /* 重新计算 */
    for (i = 1; i < SOFTUART_RX_LEN - 2; i++)
    {
        check ^= su_rx_buf[i];
    }

    /* 异或校验 */
    if (check != su_rx_buf[SOFTUART_RX_LEN - 2])
    {
        su_rx_index = 0;
        su_rx_done  = 0;
        return 0;
    }

    for (i = 0; i < SOFTUART_RX_LEN; i++)
    {
        buf[i] = su_rx_buf[i];
    }
    su_rx_index = 0;
    su_rx_done  = 0;
    return 1;
}
