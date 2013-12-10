/* Michael Pratt <michael@pratt.im>
 * Vastly simplified the code from https://github.com/jeremyherbert/stm32-templates/ */

#include <stm32f4_discovery.h>
#include <stm32f4xx_conf.h>
#include "../../sin.h"
#include "printf.h"
#include "scanf.h"
#include "stlinky.h"
#include "stm32_ub_dac_dma.h"
#include "setup.h"
#include <math.h>

void Delay(volatile uint32_t nCount);

#define pi 3.14159265
#define ABS(a)	   (((a) < 0) ? -(a) : (a))
#define CLAMP(x, low, high)  (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))
#define MIN(a, b)  (((a) < (b)) ? (a) : (b))
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#define DEG(a) ((a)*pi/180.0)
#define RAD(a) ((a)*180.0/pi)

#define pole_count 4
#define maxdiff 2*pi/pole_count/4
#define res_offset 0.64577
#define pwm_scale 0.8
#define sin_res 1024
#define min_cur = 0.1
#define max_cur = 1

#define NO 0
#define YES 1

#define offseta 0.0 * 2.0 * pi / 3.0
#define offsetb 1.0 * 2.0 * pi / 3.0
#define offsetc 2.0 * 2.0 * pi / 3.0

volatile float mag_pos;
volatile float mot_pos;
volatile float res_pos;
volatile float res_pos_tmp;

volatile int res1_tmp;
volatile int res2_tmp;

volatile int res1_pos;
volatile int res2_pos;
volatile int res1_neg;
volatile int res2_neg;
volatile int res_avg;

volatile int followe;

float p;
float p_i;

float freq;
float step;

volatile float current_scale;

volatile int dacpos;

float sine(float x){
    while(x < -pi){
        x += 2.0 * pi;
    }
    while(x > pi){
        x -= 2.0 * pi;
    }
    return(sint[(int)(x * sin_res / 2.0 / pi) + sin_res / 2]);
}

float minus(float a, float b){
	if(ABS(a - b) < pi){
		return(a - b);
	}
	else if(a > b){
		return(a - b - 2.0 * pi);
	}
	else{
		return(a - b + 2.0 * pi);
	}
}


void output_pwm(){
    TIM4->CCR1 = (sine(mag_pos + offseta) * pwm_scale * current_scale + 1.0) * mag_res / 2.0;
    TIM4->CCR2 = (sine(mag_pos + offsetb) * pwm_scale * current_scale + 1.0) * mag_res / 2.0;
    TIM4->CCR3 = (sine(mag_pos + offsetc) * pwm_scale * current_scale + 1.0) * mag_res / 2.0;
}

void pid(){
    //mag_pos = mag_pos + step;
//ist: res_pos
//soll: mot_pos
//ctr: mag_pos
    
//0 error: mag_pos = (res_pos + res_offset) * pole_count
//error = mot_pos - res_pos;
//mag_pos += p * error
    
    float error = 0;
    float ctr_mag = 0;
    float ctr_cur = 0;
    
    mag_pos = (minus(res_pos, res_offset) * pole_count);
    mot_pos = DEG((float)UB_ENCODER_TIM3_ReadPos()/2000.0*360.0);
    error = minus(mot_pos, res_pos);
    if(ABS(error) > DEG(90)){//schleppfehler
        followe = YES;
    }
    
    ctr_mag = p * error;
    ctr_cur = ABS(p_i * error);
    
    mag_pos += CLAMP(ctr_mag, -maxdiff, maxdiff);
    current_scale = CLAMP(ABS(ctr_cur), 0.1, 1);
    
    if(ABS(error) < DEG(5)){//deadband
        //error = 0;
        current_scale = 0;
    }
    
    if(mag_pos < -pi){
        mag_pos += 2.0 * pi;
    }
    if(mag_pos > pi){
        mag_pos -= 2.0 * pi;
    }
}

void TIM2_IRQHandler(void){//PWM int handler
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
    //GPIO_SetBits(GPIOD,GPIO_Pin_11);
    pid();
    output_pwm();
    //GPIO_ResetBits(GPIOD,GPIO_Pin_11);
}

void TIM7_IRQHandler(void){//DAC int handler
    TIM_ClearITPendingBit(TIM7, TIM_IT_Update);
    dacpos++;//DMA fragen?
    if(dacpos >= 32){
        dacpos = 0;
    }

    if((dacpos >= 9-4 && dacpos <= 9+4) || (dacpos >= 25-4 && dacpos <= 25+4)){// phase shift 2
        GPIO_SetBits(GPIOD,GPIO_Pin_11);
        ADC_SoftwareStartConv(ADC1);
        ADC_SoftwareStartConv(ADC2);
        ADC_SoftwareStartConv(ADC3);
    }
    if(dacpos == 9 + 4 + 1){
        res_pos_tmp = atan2f(res1_pos, res2_pos);
        res1_pos = 0;
        res2_pos = 0;
        //pid();
        //output_pwm();
    }
    if(dacpos == 25 + 4 + 1){

        res_pos = (res_pos_tmp + atan2f(res1_neg, res2_neg)) / 2.0;
        //res_pos = atan2f(res1_neg, res2_neg);
        res1_neg = 0;
        res2_neg = 0;
        //pid();
        //output_pwm();
    }
}

void ADC_IRQHandler(void)
{
    int t1, t2, t3;
    while(!ADC_GetFlagStatus(ADC2, ADC_FLAG_EOC));
    while(!ADC_GetFlagStatus(ADC3, ADC_FLAG_EOC));
    ADC_ClearITPendingBit(ADC1, ADC_IT_EOC);
    t1 = ADC_GetConversionValue(ADC1);
    t2 = ADC_GetConversionValue(ADC2);
    t3 = ADC_GetConversionValue(ADC3);
    
    t1 -= t3;
    t2 -= t3;
    
    if(t1 < 820 && t1 > -820 && t2 < 820 && t2 > -820){
        if(dacpos >= 9-4 && dacpos <= 9+4){
            res1_pos += t1;
            res2_pos += t2;
        }
        else if(dacpos >= 25-4 && dacpos <= 25+4){
            res1_neg -= t1;
            res2_neg -= t2;
        }
    }
    GPIO_ResetBits(GPIOD,GPIO_Pin_11);
}

int main(void)
{
    dacpos = 31;
    setup();
    mag_pos = 0;
    mot_pos = 0;
    followe = NO;
    current_scale = 1;
    p = 20;
    p_i = 1.0 / (maxdiff);
    freq = 0;
    step = freq / 10000 * pi * 2.0;
    /* TIM4 enable counter */
    res_pos = -10;
    while (res_pos == -10){
    }
    mot_pos = res_pos;
    TIM_Cmd(TIM4, ENABLE);//PWM
    TIM_Cmd(TIM2, ENABLE);//int
    GPIO_SetBits(GPIOD,GPIO_Pin_15);//enable
    
    
    int buffer_pos = 0;
    int i = 0;
    int scan = 0;
    char buf[STLINKY_BSIZE];
    char c;
    float f;
    
    while(1)  // Do not exit
    {
        if(stlinky_todo(&g_stlinky_term) == 0){
            //printf_("soll = %f, ist = %f, error = %f, ctr = %f,current_scale=%f\n", RAD(mot_pos), RAD(res_pos), RAD((mot_pos - res_pos)), RAD(mag_pos),current_scale);
            //printf_("res_pos_pos = %f, res_neg_pos = %f\n", res_pos_tmp, res_pos);
            printf_("%i\n",UB_ENCODER_TIM3_ReadPos());
            Delay(10000);
        }
        if(stlinky_avail(&g_stlinky_term) != 0){
            stlinky_rx(&g_stlinky_term, buf, STLINKY_BSIZE);
            i = 0;
            scan = 0;
            while(buffer_pos < SCANF_BSIZE - 1 && i < STLINKY_BSIZE && buf[i] != '\0'){
                scanf_buffer[buffer_pos] = buf[i];
                if(buf[i] == '\n'){
                    scan = 1;
                }
                i++;
                buffer_pos++;
            }
            scanf_buffer[buffer_pos] = '\0';
            if(scan){
                c = ' ';
                f = 0;
                scanf_("%c = %f \n", &c, &f);
                buffer_pos = 0;
                if(c == 'p'){
                    mag_pos += 10 * pi / 180.0;
                }
                if(c == 'm'){
                    mag_pos -= 10 * pi / 180.0;
                }
            }
            
            //scanf_("%c = %f\n", &c, &f);

        }
        
        //GPIO_ResetBits(GPIOD,GPIO_Pin_11);
/* 
        if(stlinky_todo(&g_stlinky_term) == 0){
            //stlinky_tx(&g_stlinky_term, buf, sizeof(buf));
            printf_("step %f : %i\n", step, i++);
        }
*/


        /*if(followe){
            GPIO_ResetBits(GPIOD,GPIO_Pin_15);//disable
            TIM4->CCR1 = 0;
            TIM4->CCR2 = 0;
            TIM4->CCR3 = 0;
            TIM_Cmd(TIM4, DISABLE);//PWM
            TIM_Cmd(TIM2, DISABLE);//int
        }*/
    }
}

void Delay(volatile uint32_t nCount) {
    //float one;
    while(nCount--)
    {
        //one = (float) nCount;
    }
}