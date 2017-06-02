#include "commands.h"
#include "hal.h"
#include "math.h"
#include "defines.h"
#include "angle.h"
#include "stm32f3xx_hal.h"
#include "common.h"

extern CRC_HandleTypeDef hcrc;

HAL_COMP(ls);

//process data from LS 
HAL_PIN(d_cmd);
HAL_PIN(q_cmd);
HAL_PIN(pos);
HAL_PIN(vel);
HAL_PIN(en);

// config data from LS
HAL_PIN(mode);
HAL_PIN(r);
HAL_PIN(l);
HAL_PIN(psi);
HAL_PIN(cur_p);
HAL_PIN(cur_i);
HAL_PIN(cur_ff);
HAL_PIN(cur_ind);
HAL_PIN(max_y);
HAL_PIN(max_cur);

// process data to LS
HAL_PIN(dc_volt);
HAL_PIN(d_fb);
HAL_PIN(q_fb);

// state data to LS
HAL_PIN(hv_temp);
HAL_PIN(mot_temp);
HAL_PIN(core_temp);
HAL_PIN(fault);
HAL_PIN(y);
HAL_PIN(u_fb);
HAL_PIN(v_fb);
HAL_PIN(w_fb);

// misc
HAL_PIN(pwm_volt);
HAL_PIN(crc_error);
HAL_PIN(crc_ok);
HAL_PIN(timeout);
HAL_PIN(dma_pos);
HAL_PIN(idle);

struct ls_ctx_t{
   uint32_t dma_pos;
   uint32_t timeout;
   uint32_t tx_addr;
   uint8_t send;
};

//TODO: move to ctx
volatile packet_to_hv_t packet_to_hv;
volatile packet_from_hv_t packet_from_hv;

f3_config_data_t config;
f3_state_data_t state;

static void nrt_init(volatile void * ctx_ptr, volatile hal_pin_inst_t * pin_ptr){
   // struct ls_ctx_t * ctx = (struct ls_ctx_t *)ctx_ptr;
   // struct ls_pin_ctx_t * pins = (struct ls_pin_ctx_t *)pin_ptr;
   
   GPIO_InitTypeDef GPIO_InitStruct;
   
   /* Peripheral clock enable */
   __HAL_RCC_USART3_CLK_ENABLE();
   
   UART_HandleTypeDef huart3;
   huart3.Instance = USART3;
   huart3.Init.BaudRate = DATABAUD;
   huart3.Init.WordLength = UART_WORDLENGTH_8B;
   huart3.Init.StopBits = UART_STOPBITS_1;
   huart3.Init.Parity = UART_PARITY_NONE;
   huart3.Init.Mode = UART_MODE_TX_RX;
   huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
   huart3.Init.OverSampling = UART_OVERSAMPLING_8;
   huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
   huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
   HAL_UART_Init(&huart3);
   USART3->CR3 |= USART_CR3_DMAT | USART_CR3_DMAR;
   
   /**USART3 GPIO Configuration    
   PB10     ------> USART3_TX
   PB11     ------> USART3_RX 
   */
   GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11;
   GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
   GPIO_InitStruct.Pull = GPIO_PULLUP;
   GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
   GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
   HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
   
   __HAL_RCC_DMA1_CLK_ENABLE();
   
   //TX DMA
   DMA1_Channel2->CCR &= (uint16_t)(~DMA_CCR_EN);
   DMA1_Channel2->CPAR = (uint32_t)&(USART3->TDR);
   DMA1_Channel2->CMAR = (uint32_t)&packet_from_hv;
   DMA1_Channel2->CNDTR = sizeof(packet_from_hv_t);
   DMA1_Channel2->CCR = DMA_CCR_MINC | DMA_CCR_DIR;// | DMA_CCR_PL_0 | DMA_CCR_PL_1
   DMA1->IFCR = DMA_IFCR_CTCIF2 | DMA_IFCR_CHTIF2 | DMA_IFCR_CGIF2;
   
   //RX DMA
   DMA1_Channel3->CCR &= (uint16_t)(~DMA_CCR_EN);
   DMA1_Channel3->CPAR = (uint32_t)&(USART3->RDR);
   DMA1_Channel3->CMAR = (uint32_t)&packet_to_hv;
   DMA1_Channel3->CNDTR = sizeof(packet_to_hv_t);
   DMA1_Channel3->CCR = DMA_CCR_MINC;// | DMA_CCR_PL_0 | DMA_CCR_PL_1
   DMA1->IFCR = DMA_IFCR_CTCIF3 | DMA_IFCR_CHTIF3 | DMA_IFCR_CGIF3;
   DMA1_Channel3->CCR |= DMA_CCR_EN;
   
   config.pins.mode = 0.0;
   config.pins.r = 0.0;
   config.pins.l = 0.0;
   config.pins.psi = 0.0;
   config.pins.cur_p = 0.0;
   config.pins.cur_i = 0.0;
   config.pins.cur_ff = 0.0;
   config.pins.cur_ind = 0.0;
   config.pins.max_y = 0.0;
   config.pins.max_cur = 0.0;
   
   USART3->RTOR = 16; // 16 bits timeout
   USART3->CR2 |= USART_CR2_RTOEN; // timeout en
   USART3->ICR |= USART_ICR_RTOCF; // timeout clear flag
}

static void rt_start(volatile void * ctx_ptr, volatile hal_pin_inst_t * pin_ptr){
   struct ls_ctx_t * ctx = (struct ls_ctx_t *)ctx_ptr;
   struct ls_pin_ctx_t * pins = (struct ls_pin_ctx_t *)pin_ptr;
   ctx->timeout = 0;
   ctx->dma_pos = 0;
   ctx->tx_addr = 0;
   ctx->send = 0;
   PIN(crc_error) = 0.0;
   PIN(crc_ok) = 0.0;
   PIN(timeout) = 0.0;
   PIN(idle) = 0.0;
}

static void rt_func(float period, volatile void * ctx_ptr, volatile hal_pin_inst_t * pin_ptr){
   struct ls_ctx_t * ctx = (struct ls_ctx_t *)ctx_ptr;
   struct ls_pin_ctx_t * pins = (struct ls_pin_ctx_t *)pin_ptr;
   
   uint32_t dma_pos = sizeof(packet_to_hv) - DMA1_Channel3->CNDTR;
   
   
   if(dma_pos == sizeof(packet_to_hv)){
      uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *) &packet_to_hv, sizeof(packet_to_hv) / 4 - 1);
      if(crc == packet_to_hv.crc){
         PIN(en) = packet_to_hv.flags.enable;
         PIN(d_cmd) = packet_to_hv.d_cmd;
         PIN(q_cmd) = packet_to_hv.q_cmd;
         PIN(pos) = packet_to_hv.pos;
         PIN(vel) = packet_to_hv.vel;
         uint8_t a = packet_to_hv.addr;
         a = CLAMP(a, 0, sizeof(config) / 4);
         config.data[a] = packet_to_hv.value; // TODO: first enable after complete update
         
         PIN(mode) = config.pins.mode;
         PIN(r) = config.pins.r;
         PIN(l) = config.pins.l;
         PIN(psi) = config.pins.psi;
         PIN(cur_p) = config.pins.cur_p;
         PIN(cur_i) = config.pins.cur_i;
         PIN(cur_ff) = config.pins.cur_ff;
         PIN(cur_ind) = config.pins.cur_ind;
         PIN(max_y) = config.pins.max_y;
         PIN(max_cur) = config.pins.max_cur;
         ctx->timeout = 0;
         PIN(crc_ok)++;
         ctx->send = 1;
      }
      else{
         PIN(crc_error)++;
      }
   }


   
   if(USART3->ISR & USART_ISR_RTOF){ // idle line
      USART3->ICR |= USART_ICR_RTOCF; // timeout clear flag
      GPIOA->BSRR |= GPIO_PIN_10;

      PIN(idle)++;
      if(dma_pos != sizeof(packet_to_hv_t)){
         PIN(dma_pos) = dma_pos;
      }
      
      // reset rx DMA
      DMA1_Channel3->CCR &= (uint16_t)(~DMA_CCR_EN);
      DMA1_Channel3->CNDTR = sizeof(packet_to_hv);
      DMA1_Channel3->CCR |= DMA_CCR_EN;
      dma_pos = 0;
      GPIOA->BSRR |= GPIO_PIN_10 << 16;

      //ctx->send = 1;
   }
   if(ctx->send && dma_pos != 0){
      ctx->send = 0;
      //packet_to_hv.d_cmd = -99.0;
      state.pins.u_fb = PIN(u_fb);
      state.pins.v_fb = PIN(v_fb);
      state.pins.w_fb = PIN(w_fb);
      state.pins.hv_temp = PIN(hv_temp);
      state.pins.mot_temp = PIN(mot_temp);
      state.pins.core_temp = PIN(core_temp);
      state.pins.fault = PIN(fault);
      state.pins.y = PIN(y);
      
      // fill tx struct
      packet_from_hv.dc_volt = PIN(dc_volt);
      packet_from_hv.pwm_volt = PIN(pwm_volt);
      packet_from_hv.d_fb =  PIN(d_fb);
      packet_from_hv.q_fb =  PIN(q_fb);
      packet_from_hv.addr = ctx->tx_addr;
      packet_from_hv.value = state.data[ctx->tx_addr++];
      ctx->tx_addr %= sizeof(state) / 4;
      packet_from_hv.crc = HAL_CRC_Calculate(&hcrc, (uint32_t *) &packet_from_hv, sizeof(packet_from_hv) / 4 - 1);
      
      // start tx DMA
      DMA1_Channel2->CCR &= (uint16_t)(~DMA_CCR_EN);
      DMA1_Channel2->CNDTR = sizeof(packet_from_hv);
      DMA1_Channel2->CCR |= DMA_CCR_EN;
      //ctx->send = 0;
   }
   
   if(ctx->timeout > 5){//disable driver
      PIN(en) = 0.0;
      PIN(timeout)++;
   }
   ctx->timeout++;

   //if(dma_pos == ctx->dma_pos){ // pause
   // reset rx DMA
   //   DMA1_Channel3->CCR &= (uint16_t)(~DMA_CCR_EN);
   //   DMA1_Channel3->CNDTR = sizeof(packet_to_hv);
   //   DMA1_Channel3->CCR |= DMA_CCR_EN;
   //   dma_pos = 0;
   
   //if(ctx->send){
   // read state
   //  state.pins.u_fb = PIN(u_fb);
   //  state.pins.v_fb = PIN(v_fb);
   //  state.pins.w_fb = PIN(w_fb);
   //  state.pins.hv_temp = PIN(hv_temp);
   //  state.pins.mot_temp = PIN(mot_temp);
   //  state.pins.core_temp = PIN(core_temp);
   //  state.pins.fault = PIN(fault);
   //  state.pins.y = PIN(y);
   //  
   //  // fill tx struct
   //  packet_from_hv.dc_volt = PIN(dc_volt);
   //  packet_from_hv.pwm_volt = PIN(pwm_volt);
   //  packet_from_hv.d_fb =  PIN(d_fb);
   //  packet_from_hv.q_fb =  PIN(q_fb);
   //  packet_from_hv.addr = ctx->tx_addr;
   //  packet_from_hv.value = state.data[ctx->tx_addr++];
   //  ctx->tx_addr %= sizeof(state) / 4;
   //  packet_from_hv.crc = HAL_CRC_Calculate(&hcrc, (uint32_t *) &packet_from_hv, sizeof(packet_from_hv) / 4 - 1);
   //     
   //  // start tx DMA
   //  DMA1_Channel2->CCR &= (uint16_t)(~DMA_CCR_EN);
   //  DMA1_Channel2->CNDTR = sizeof(packet_from_hv);
   //  DMA1_Channel2->CCR |= DMA_CCR_EN;
   
   //ctx->send = 0;
   //   }
   
   // }
   
   
   // else if(dma_pos == sizeof(packet_to_hv)){ // check crc
   //    uint32_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *) &packet_to_hv, sizeof(packet_to_hv) / 4 - 1);
   //    if(crc == packet_to_hv.crc){
   //       PIN(en) = packet_to_hv.flags.enable;
   //       PIN(d_cmd) = packet_to_hv.d_cmd;
   //       PIN(q_cmd) = packet_to_hv.q_cmd;
   //       PIN(pos) = packet_to_hv.pos;
   //       PIN(vel) = packet_to_hv.vel;
   //       uint8_t a = packet_to_hv.addr;
   //       a = CLAMP(a, 0, sizeof(config) / 4);
   //       config.data[a] = packet_to_hv.value; // TODO: first enable after complete update
   //       
   //       PIN(mode) = config.pins.mode;
   //       PIN(r) = config.pins.r;
   //       PIN(l) = config.pins.l;
   //       PIN(psi) = config.pins.psi;
   //       PIN(cur_p) = config.pins.cur_p;
   //       PIN(cur_i) = config.pins.cur_i;
   //       PIN(cur_ff) = config.pins.cur_ff;
   //       PIN(cur_ind) = config.pins.cur_ind;
   //       PIN(max_y) = config.pins.max_y;
   //       PIN(max_cur) = config.pins.max_cur;
   //       
   //       ctx->timeout = 0; //reset timeout
   //       ctx->send = 1;
   //    }
   //    else{ // wrong crc
   //       PIN(crc_error)++;
   //       PIN(en) = 0;
   //    }
   // }
   ctx->dma_pos = dma_pos;
   
   // TODO: sin = 0.5
   if(config.pins.mode == 0){// 90°
      PIN(pwm_volt) = PIN(dc_volt) / M_SQRT2 * 0.95;
   }else if(config.pins.mode == 1){// 120°
      PIN(pwm_volt) = PIN(dc_volt) / M_SQRT3 * 0.95;
   }else if(config.pins.mode == 2){// 180°
      PIN(pwm_volt) = PIN(dc_volt) * 0.95;
   }else{
      PIN(pwm_volt) = 0.0;
   }
   
   
   
   
}

hal_comp_t ls_comp_struct = {
   .name = "ls",
   .nrt = 0,
   .rt = rt_func,
   .frt = 0,
   .nrt_init = nrt_init,
   .rt_start = rt_start,
   .frt_start = 0,
   .rt_stop = 0,
   .frt_stop = 0,
   .ctx_size = sizeof(struct ls_ctx_t),
   .pin_count = sizeof(struct ls_pin_ctx_t) / sizeof(struct hal_pin_inst_t),
};
