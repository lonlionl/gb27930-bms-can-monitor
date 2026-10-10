#include "bsp_led.h"

void LED_GPIO_Config(void)
{
	//红色灯初始化
	GPIO_InitTypeDef		GPIO_R_InitStruct;
	//变量的声明不能出现在可执行语句之后
	GPIO_InitTypeDef		GPIO_G_InitStruct;
	
	RCC_APB2PeriphClockCmd(LED_R_GPIO_CLK, ENABLE);
	
	GPIO_R_InitStruct.GPIO_Pin=LED_R_GPIO_PIN;
	GPIO_R_InitStruct.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_R_InitStruct.GPIO_Mode=GPIO_Mode_Out_PP;
	
	GPIO_Init(LED_R_GPIO_PORT,&GPIO_R_InitStruct);
	
	//绿色灯初始化
	RCC_APB2PeriphClockCmd(LED_G_GPIO_CLK, ENABLE);
	
	GPIO_G_InitStruct.GPIO_Pin=LED_G_GPIO_PIN;
	GPIO_G_InitStruct.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_G_InitStruct.GPIO_Mode=GPIO_Mode_Out_PP;
	
	GPIO_Init(LED_G_GPIO_PORT,&GPIO_G_InitStruct);
}
