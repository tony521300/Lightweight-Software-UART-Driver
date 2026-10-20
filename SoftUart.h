#ifndef __SOFT_UART_H__
#define __SOFT_UART_H__

// #include "H/Function_Init.H"

#define SOFTUART_BUF_SIZE 5
#define SOFTUART_RX_LEN 6
// ==== 配置 GPIO ====
// PB5 -> TX
// PB4 -> RX
#define RX_PIN P14

#define UART_TX_HIGH() P15 = 0
#define UART_TX_LOW() P15 = 1

// 发送 帧头/帧尾定义
#define IRON_SEND_FRAME_HEAD 0xCC
#define IRON_SEND_FRAME_END 0x77
// 接收 帧头/帧尾定义
#define IRON_REC_FRAME_HEAD 0xDD
#define IRON_REC_FRAME_END 0x88

// 初始化（TX=输出，RX=输入）
void SoftUART_Init(void);

void SoftUART_Tick(void);
// 发送 1 字节
void SoftUART_SendByte(unsigned char dat);

// 发送任意长度 buffer
void SoftUART_SendBuffer(unsigned char* buf);

void          SoftUART_Tick_RX(void);
void          Soft_Uart_Process(void);
unsigned char SoftUART_ReadData(unsigned char* buf);
unsigned char SoftUART_GetStatus(void);

#endif
