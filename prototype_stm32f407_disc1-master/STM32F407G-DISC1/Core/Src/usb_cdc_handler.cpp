#include "usb_cdc_handler.hpp"

#include "stm32f4xx_hal.h"
#include "usbd_cdc_if.h"
#include "usb_device.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>


extern USBD_HandleTypeDef hUsbDeviceFS;

namespace {

static uint8_t   s_rx_buf[CDC_RX_BUF_SIZE];
static volatile uint16_t  s_rx_head = 0;
static volatile bool s_frame_ready  = false;

static volatile uint32_t  s_exp_offset = 0;
static volatile uint16_t  s_exp_len    = 0;

static char      s_cmd_buf[CDC_CMD_MAX_LEN];
static uint8_t   s_cmd_len = 0;

}

volatile bool jumpRequested = false;

void CDC_Proto_Init(void)
{
    memset(s_rx_buf, 0, sizeof(s_rx_buf));
    s_rx_head     = 0;
    s_frame_ready = false;
    s_exp_offset  = 0;
    s_exp_len     = 0;
    s_cmd_len     = 0;
    memset(s_cmd_buf, 0, sizeof(s_cmd_buf));
}


CDC_Status_t CDC_SendDebugString(const char *msg)
{
    if (msg == nullptr) return CDC_ERR_ARG;

    uint16_t len = static_cast<uint16_t>(strlen(msg));

    USBD_StatusTypeDef rc =
       (USBD_StatusTypeDef) CDC_Transmit_FS(reinterpret_cast<uint8_t *>(const_cast<char *>(msg)), len);

    if (rc == USBD_OK && (len == 0 || msg[len - 1] != '\n')) {
        static const uint8_t nl[] = {'\n'};
        HAL_Delay(1);
        rc = (USBD_StatusTypeDef) CDC_Transmit_FS(const_cast<uint8_t *>(nl), 1);
    }

    HAL_Delay(2);
    return (rc == USBD_OK) ? CDC_OK : CDC_ERR_TX;
}


CDC_Status_t CDC_Printf(const char *fmt, ...)
{
    static char scratch[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(scratch, sizeof(scratch), fmt, args);
    va_end(args);
    return CDC_SendDebugString(scratch);
}


#if defined ( CDC_MASTER_MODE )

CDC_Status_t CDC_SendHandshake()
{
    uint8_t handshake[CDC_PROTO_HDR_LEN] = {0};
    handshake[0] = CDC_PROTO_START_BYTE;

    if (CDC_Transmit_FS(handshake, CDC_PROTO_HDR_LEN) != USBD_OK) {
        CDC_SendDebugString("[CDCProto] TX failed in CDC_SendHandshake");
        return CDC_ERR_TX;
    }

    const uint32_t t_start = HAL_GetTick();
    while (!s_frame_ready) {
        if ((HAL_GetTick() - t_start) >= 2000) {
            s_rx_head = 0;
            CDC_Printf("[CDCProto] Timeout waiting for handshake");
            return CDC_ERR_TMO;
        }
        HAL_Delay(1);
    }
    if (s_exp_len == 0 && s_exp_offset == 0) {
    	 CDC_Printf("[CDCProto] Handshaking success");
    	 return CDC_OK;

    }
    else  {
    	 CDC_Printf("[CDCProto] Handshaking error");
    	 return CDC_ERR_HANDSHAKE;
    }
}


CDC_Status_t CDC_PullChunk(
    uint32_t  offset,
    uint16_t  request_len,
    uint8_t  *out_buf,
    uint16_t *received)
{
    if (out_buf == nullptr || received == nullptr)   return CDC_ERR_ARG;
    if (request_len == 0)                           return CDC_ERR_ARG;
    if (request_len > CDC_MAX_CHUNK_SIZE)           request_len = CDC_MAX_CHUNK_SIZE;

    s_exp_offset  = offset;
    s_exp_len     = request_len;
    s_rx_head     = 0;
    s_frame_ready = false;

    uint8_t req[CDC_PROTO_HDR_LEN];
    req[0] = CDC_PROTO_START_BYTE;
    req[1] = static_cast<uint8_t>( offset        & 0xFFU);
    req[2] = static_cast<uint8_t>((offset >>  8) & 0xFFU);
    req[3] = static_cast<uint8_t>((offset >> 16) & 0xFFU);
    req[4] = static_cast<uint8_t>((offset >> 24) & 0xFFU);
    req[5] = static_cast<uint8_t>( request_len        & 0xFFU);
    req[6] = static_cast<uint8_t>((request_len >>  8) & 0xFFU);

    if (CDC_Transmit_FS(req, CDC_PROTO_HDR_LEN) != USBD_OK) {
        CDC_SendDebugString("[CDCProto] TX failed in CDC_PullChunk");
        return CDC_ERR_TX;
    }


    const uint32_t t_start = HAL_GetTick();
    while (!s_frame_ready) {
        if ((HAL_GetTick() - t_start) >= CDC_PULL_TIMEOUT_MS) {
            s_rx_head = 0;
            CDC_Printf("[CDCProto] Timeout waiting for chunk @0x%08lX len=%u",
                       (unsigned long)offset, (unsigned)request_len);
            return CDC_ERR_TMO;
        }
        HAL_Delay(1);
    }


    uint16_t actual = s_exp_len;
    memcpy(out_buf, s_rx_buf + CDC_PROTO_HDR_LEN, actual);
    *received     = actual;
    s_rx_head     = 0;
    s_frame_ready = false;

    return CDC_OK;
}

#elif defined ( CDC_SLAVE_MODE )
CDC_Status_t CDC_WaitForHandshake()
{
    const uint32_t t_start = HAL_GetTick();
     while (!s_frame_ready) {
        if ((HAL_GetTick() - t_start) >= 2000) {
             s_rx_head = 0;
             CDC_Printf("[CDCProto] Timeout waiting for handshake");
             return CDC_ERR_TMO;
        }
        HAL_Delay(1);
     }
     if (s_exp_len == 0 && s_exp_offset == 0) {
    	 s_rx_head = 0;
    	 s_frame_ready = false;
         uint8_t handshake[CDC_PROTO_HDR_LEN] = {0};
         handshake[0] = CDC_PROTO_START_BYTE;
         if (CDC_Transmit_FS(handshake, CDC_PROTO_HDR_LEN) != USBD_OK) {
             CDC_SendDebugString("[CDCProto] TX failed in CDC_WaitForHandshake");
             return CDC_ERR_TX;
         }
        HAL_Delay(10);
        CDC_Printf("[CDCProto] Handshake Successful");
        	return CDC_OK;
        }
        else  {
        	 CDC_Printf("[CDCProto] Handshake Error");
        	 return CDC_ERR_HANDSHAKE;
        }
}

CDC_Status_t CDC_WaitChunk(uint8_t  *out_buf, uint16_t out_buf_len,  uint16_t *received, uint32_t timeout) {
    if (out_buf == nullptr || received == nullptr)   return CDC_ERR_ARG;
    if (out_buf_len == 0)                           return CDC_ERR_ARG;
    if (*received >= out_buf_len)	return CDC_FINISH;
    const uint32_t t_start = HAL_GetTick();
    while (!s_frame_ready) {
        if ((HAL_GetTick() - t_start) >= timeout) {
            s_rx_head = 0;
            CDC_Printf("[CDCProto] Timeout waiting for a chunk");
            return CDC_ERR_TMO;
        }
        HAL_Delay(1);
    }

    if ((s_exp_offset + s_exp_len) > out_buf_len) {
        s_rx_head = 0;
        CDC_Printf("[CDCProto] Offset + length > data buffer length");
    	return CDC_ERR_DATA;
    }

    memcpy(&out_buf[s_exp_offset], s_rx_buf + CDC_PROTO_HDR_LEN, s_exp_len);
    *received += s_exp_len;
    s_rx_head     = 0;
    s_frame_ready = false;

    uint8_t ack[CDC_PROTO_HDR_LEN];
    ack[0] = CDC_PROTO_START_BYTE;
    ack[1] = static_cast<uint8_t>( s_exp_offset        & 0xFFU);
    ack[2] = static_cast<uint8_t>((s_exp_offset >>  8) & 0xFFU);
    ack[3] = static_cast<uint8_t>((s_exp_offset >> 16) & 0xFFU);
    ack[4] = static_cast<uint8_t>((s_exp_offset >> 24) & 0xFFU);
    ack[5] = static_cast<uint8_t>( s_exp_len        & 0xFFU);
    ack[6] = static_cast<uint8_t>((s_exp_len >>  8) & 0xFFU);

    if (CDC_Transmit_FS(ack, CDC_PROTO_HDR_LEN) != USBD_OK) {
        CDC_SendDebugString("[CDCProto] TX failed in CDC_WaitChunk");
        return CDC_ERR_TX;
    }
    return CDC_OK;
}
#endif

void CDC_Proto_RxCallback(const uint8_t *buf, uint32_t len)
{
    if (buf == nullptr || len == 0) return;

    for (uint32_t i = 0; i < len; ++i) {
        const uint8_t b = buf[i];

        if (!s_frame_ready && (s_rx_head > 0 || b == CDC_PROTO_START_BYTE)) {

            if (s_rx_head < sizeof(s_rx_buf)) {
                s_rx_buf[s_rx_head++] = b;
            }

            if (s_rx_head >= CDC_PROTO_HDR_LEN) {
                if (s_rx_buf[0] != CDC_PROTO_START_BYTE) {
                    s_rx_head = 0;
                    continue;
                }

                const uint32_t resp_off =
                    static_cast<uint32_t>(s_rx_buf[1])
                  | static_cast<uint32_t>(s_rx_buf[2]) << 8
                  | static_cast<uint32_t>(s_rx_buf[3]) << 16
                  | static_cast<uint32_t>(s_rx_buf[4]) << 24;

                const uint16_t resp_len =
                    static_cast<uint16_t>(s_rx_buf[5])
                  | static_cast<uint16_t>(s_rx_buf[6]) << 8;
#if defined ( CDC_MASTER_MODE )
                if (resp_off == s_exp_offset && resp_len == s_exp_len) {
                    const uint16_t total_needed = CDC_PROTO_HDR_LEN + resp_len;
                    if (s_rx_head >= total_needed) {
                        s_frame_ready = true;
                    }
                } else {

                    s_rx_head = 0;
                }
#elif defined ( CDC_SLAVE_MODE )
                const uint16_t total_needed = CDC_PROTO_HDR_LEN + resp_len;
                if (s_rx_head >= total_needed) {
                	s_exp_offset = resp_off;
                	s_exp_len = resp_len;
                    s_frame_ready = true;
                }
#endif
            }
            continue;
        }


        if (b == '\n' || b == '\r') {
            if (s_cmd_len > 0) {
                s_cmd_buf[s_cmd_len] = '\0';
                s_cmd_len = 0;
                CDC_Proto_CommandCallback(s_cmd_buf);
            }
        } else {
            if (s_cmd_len < CDC_CMD_MAX_LEN - 1) {
                s_cmd_buf[s_cmd_len++] = static_cast<char>(b);
            }
        }
    }
}


#define SYSMEM_ADDRESS 0x1FFF0000
typedef void (*pFunction)(void);

void CDC_JumpToBootloader(void)
{
    uint32_t JumpAddress = *(__IO uint32_t*)(SYSMEM_ADDRESS + 4);
    pFunction Jump       = (pFunction)JumpAddress;

    HAL_RCC_DeInit();
    HAL_DeInit();

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();


    __set_MSP(*(__IO uint32_t*)SYSMEM_ADDRESS);
    Jump();

    while(1);
}


__attribute__((weak))
void CDC_Proto_CommandCallback(const char *cmd)
{
    if (cmd == nullptr) return;

    if (strncmp(cmd, "CMD:JUMP_BOOTLOADER", 19) == 0) {
    	jumpRequested = true;
    }
}
