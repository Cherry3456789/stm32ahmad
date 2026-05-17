
/**
  ******************************************************************************
  * @file    app_x-cube-ai.c
  * @author  X-CUBE-AI C code generator
  * @brief   AI program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

 /*
  * Description
  *   v1.0 - Minimum template to show how to use the Embedded Client API
  *          model. Only one input and one output is supported. All
  *          memory resources are allocated statically (AI_NETWORK_XX, defines
  *          are used).
  *          Re-target of the printf function is out-of-scope.
  *   v2.0 - add multiple IO and/or multiple heap support
  *
  *   For more information, see the embeded documentation:
  *
  *       [1] %X_CUBE_AI_DIR%/Documentation/index.html
  *
  *   X_CUBE_AI_DIR indicates the location where the X-CUBE-AI pack is installed
  *   typical : C:\Users\[user_name]\STM32Cube\Repository\STMicroelectronics\X-CUBE-AI\7.1.0
  */

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

#if defined ( __ICCARM__ )
#elif defined ( __CC_ARM ) || ( __GNUC__ )
#endif

/* System headers */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "app_x-cube-ai.h"
#include "main.h"
#include "ai_datatypes_defines.h"
#include "network.h"
#include "network_data.h"

/* USER CODE BEGIN includes */
#include "usb_cdc_handler.hpp"
/* USER CODE END includes */

/* IO buffers ----------------------------------------------------------------*/

#if !defined(AI_NETWORK_INPUTS_IN_ACTIVATIONS)
AI_ALIGNED(4) ai_i8 data_in_1[AI_NETWORK_IN_1_SIZE_BYTES];
ai_i8* data_ins[AI_NETWORK_IN_NUM] = {
data_in_1
};
#else
ai_i8* data_ins[AI_NETWORK_IN_NUM] = {
NULL
};
#endif

#if !defined(AI_NETWORK_OUTPUTS_IN_ACTIVATIONS)
AI_ALIGNED(4) ai_i8 data_out_1[AI_NETWORK_OUT_1_SIZE_BYTES];
ai_i8* data_outs[AI_NETWORKI_OUT_NUM] = {
data_out_1
};
#else
ai_i8* data_outs[AI_NETWORK_OUT_NUM] = {
NULL
};
#endif

/* Activations buffers -------------------------------------------------------*/

AI_ALIGNED(32)
static uint8_t pool0[AI_NETWORK_DATA_ACTIVATION_1_SIZE];

ai_handle data_activations0[] = {pool0};

/* AI objects ----------------------------------------------------------------*/

static ai_handle  network = AI_HANDLE_NULL;

static ai_buffer* ai_input;
static ai_buffer* ai_output;

static void ai_log_err(const ai_error err, const char *fct)
{
  /* USER CODE BEGIN log */
  if (fct)
    printf("TEMPLATE - Error (%s) - type=0x%02x code=0x%02x\r\n", fct,
        err.type, err.code);
  else
    printf("TEMPLATE - Error - type=0x%02x code=0x%02x\r\n", err.type, err.code);
  CDC_Printf("Error in inference");
  do {} while (1);
  /* USER CODE END log */
}

static int ai_boostrap(ai_handle *act_addr)
{
  ai_error err;

  /* Create and initialize an instance of the model */
  err = ai_network_create_and_init(&network, act_addr, NULL);
  if (err.type != AI_ERROR_NONE) {
    ai_log_err(err, "ai__create_and_init");
    return -1;
  }

  ai_input = ai_network_inputs_get(network, NULL);
  ai_output = ai_network_outputs_get(network, NULL);

#if defined(AI_NETWORK_INPUTS_IN_ACTIVATIONS)
  /*  In the case where "--allocate-inputs" option is used, memory buffer can be
   *  used from the activations buffer. This is not mandatory.
   */
  for (int idx=0; idx < AI_NETWORK_IN_NUM; idx++) {
	data_ins[idx] = ai_input[idx].data;
  }
#else
  for (int idx=0; idx < AI_NETWORK_IN_NUM; idx++) {
	  ai_input[idx].data = data_ins[idx];
  }
#endif

#if defined(AI_NETWORK_OUTPUTS_IN_ACTIVATIONS)
  /*  In the case where "--allocate-outputs" option is used, memory buffer can be
   *  used from the activations buffer. This is no mandatory.
   */
  for (int idx=0; idx < AI_NETWORK_OUT_NUM; idx++) {
	data_outs[idx] = ai_output[idx].data;
  }
#else
  for (int idx=0; idx < AI_NETWORK_OUT_NUM; idx++) {
	ai_output[idx].data = data_outs[idx];
  }
#endif

  return 0;
}

static int ai_run(void)
{
  ai_i32 batch;

  batch = ai_network_run(network, ai_input, ai_output);
  if (batch != 1) {
    ai_log_err(ai_network_get_error(network),
        "ai__run");
    return -1;
  }

  return 0;
}

/* USER CODE BEGIN 2 */
bool end_of_inference = false;
#define MODEL_INPUT_SIZE  AI_NETWORK_IN_1_SIZE   // total bytes
static uint8_t data_buf[MODEL_INPUT_SIZE] = {0};
static uint32_t offset = 0;

int acquire_and_process_data(ai_i8* data[])
{
   uint16_t       received = 0;
#if defined ( CDC_MASTER_MODE )
   static const uint16_t MAX_CHUNK_SIZE   = 512U;
   static const uint32_t NUM_CHUNKS       = MODEL_INPUT_SIZE / MAX_CHUNK_SIZE  + ((float)MODEL_INPUT_SIZE / MAX_CHUNK_SIZE > 0 ? 1 : 0);
   static const uint32_t LAST_CHUNK_SIZE  = MODEL_INPUT_SIZE - MAX_CHUNK_SIZE * (MODEL_INPUT_SIZE / MAX_CHUNK_SIZE);
   for (uint32_t chunk = 0; chunk < NUM_CHUNKS; ++chunk) {
	   uint32_t buf_offset = chunk * MAX_CHUNK_SIZE;
	   uint16_t size = MAX_CHUNK_SIZE;
	   if (chunk >= (NUM_CHUNKS -1) && LAST_CHUNK_SIZE != 0) size = LAST_CHUNK_SIZE;
	   uint16_t       rec = 0;

       CDC_Status_t rc = CDC_PullChunk(offset, size, &data_buf[buf_offset], &rec);
       offset += size;
       received += rec;
       if (rc != CDC_OK) {
           CDC_Printf("[APP] Pull failed at chunk %lu (rc=%d)\r\n",
                      (unsigned long)chunk, (int)rc);
           return -1;
       }
       CDC_Printf("[APP] Chunk %lu/%lu OK (%u B)\r\n",
                  (unsigned long)(chunk + 1),
                  (unsigned long)NUM_CHUNKS,
                  (unsigned)received);
   }

#elif defined ( CDC_SLAVE_MODE )
   CDC_Status_t rc = CDC_OK;
   uint32_t chunck = 0;
   while (rc != CDC_FINISH) {
	   rc = CDC_WaitChunk(data_buf, (uint16_t)MODEL_INPUT_SIZE, &received, 5000);

       if (rc != CDC_OK && rc != CDC_FINISH)   return -1;
       CDC_Printf("[APP] Chunk Received (Total %u B)\r\n", (unsigned)received);
       chunck++;
       HAL_Delay(10);
   }

#endif

   float* in_data = (float*)(data[0]);
   for (int i = 0; i < MODEL_INPUT_SIZE; i++) {
       in_data[i] = (float)data_buf[i] / 255.0f;
   }
  return 0;
}

int post_process(ai_i8* data[])
{
	  float* predictions = (float*)data[0];
	  int best_digit = 0;
	  float max_score = -1.0f;

	  for (int i = 0; i < 10; i++) {
	    if (predictions[i] > max_score) {
	      max_score = predictions[i];
	      best_digit = i;
	    }
	  }
	  max_score *= 100;
      CDC_Printf("Detected Digit: %d, Score: %d\r\n", best_digit, (uint16_t)max_score);
  return 0;
}
/* USER CODE END 2 */

/* Entry points --------------------------------------------------------------*/

void MX_X_CUBE_AI_Init(void)
{
    /* USER CODE BEGIN 5 */
  printf("\r\nTEMPLATE - initialization\r\n");

  ai_boostrap(data_activations0);

  offset = 0;
    /* USER CODE END 5 */
}

void MX_X_CUBE_AI_Process(void)
{
    /* USER CODE BEGIN 6 */
  int res = -1;

  CDC_Printf("[APP] Starting inference loop\r\n");

  if (network) {

    do {
      /* 1 - acquire and pre-process input data */
      res = acquire_and_process_data(data_ins);
      /* 2 - process the data - call inference engine */
      if (res == 0)
        res = ai_run();
      /* 3- post-process the predictions */
      if (res == 0)
        res = post_process(data_outs);
    } while (res==0);
  }

  if (res) {
    //ai_error err = {AI_ERROR_INVALID_STATE, AI_ERROR_CODE_NETWORK};
    //ai_log_err(err, "Process has FAILED");
	  CDC_Printf("[APP] End of inference loop\r\n");
	  end_of_inference = true;
  }
    /* USER CODE END 6 */
}
#ifdef __cplusplus
}
#endif
