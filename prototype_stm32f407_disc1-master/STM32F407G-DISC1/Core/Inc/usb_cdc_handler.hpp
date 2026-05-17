#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CDC_PROTO_START_BYTE    ((uint8_t)0xAAU)

#define CDC_PROTO_HDR_LEN       7U

#define CDC_MAX_CHUNK_SIZE      512U

#define CDC_RX_BUF_SIZE         (CDC_PROTO_HDR_LEN + CDC_MAX_CHUNK_SIZE)

#define CDC_PULL_TIMEOUT_MS     5000U

#define CDC_CMD_MAX_LEN         64U

#define CDC_MASTER_MODE

typedef enum {
	CDC_FINISH = 1,
    CDC_OK      =  0,
    CDC_ERR_TX  = -1,
    CDC_ERR_TMO = -2,
    CDC_ERR_ARG = -3,
	CDC_ERR_DATA = -4,
	CDC_ERR_HANDSHAKE = -5,
} CDC_Status_t;


void CDC_Proto_Init(void);

CDC_Status_t CDC_SendDebugString(const char *msg);

CDC_Status_t CDC_Printf(const char *fmt, ...);


#ifdef CDC_MASTER_MODE
CDC_Status_t CDC_PullChunk(uint32_t offset, uint16_t request_len, uint8_t  *out_buf, uint16_t *received);
CDC_Status_t CDC_SendHandshake();
#elif defined(CDC_SLAVE_MODE)
CDC_Status_t CDC_WaitForHandshake();
CDC_Status_t CDC_WaitChunk(uint8_t *out_buf, uint16_t out_buf_len, uint16_t *received, uint32_t timeout);
#endif

void CDC_Proto_RxCallback(const uint8_t *buf, uint32_t len);


void CDC_Proto_CommandCallback(const char *cmd);

__attribute__((noreturn))
void CDC_JumpToBootloader(void);

extern volatile bool jumpRequested;

#ifdef __cplusplus
}
#endif
