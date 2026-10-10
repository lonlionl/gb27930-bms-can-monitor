#include "bsp_usart.h"

/* NVIC_Configuration 在 bsp_usart.h 里是公开声明的，因此这里不能加 static，
   否则 GCC 会报 static declaration follows non-static declaration。
   Keil/ARMCC 对这种不一致比较宽容，但在 PC 上做语法检查时会被抓出来。 */
void NVIC_Configuration(void)
{
  NVIC_InitTypeDef NVIC_InitStructure;
  
  /* 嵌套向量中断控制器组选择 */
	/* 提示 NVIC_PriorityGroupConfig() 在整个工程只需要调用一次来配置优先级分组*/
  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
  
  /* 配置USART为中断源 */
  NVIC_InitStructure.NVIC_IRQChannel = DEBUG_USART_IRQ;
  /* 抢断优先级*/
  NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
  /* 子优先级 */
  NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
  /* 使能中断 */
  NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
  /* 初始化配置NVIC */
  NVIC_Init(&NVIC_InitStructure);
}

void USART_Config(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
	USART_InitTypeDef USART_InitStructure;

	// 打开串口GPIO的时钟
	DEBUG_USART_GPIO_APBxClkCmd(DEBUG_USART_GPIO_CLK, ENABLE);
	
	// 打开串口外设的时钟
	DEBUG_USART_APBxClkCmd(DEBUG_USART_CLK, ENABLE);

	// 将USART Tx的GPIO配置为推挽复用模式
	GPIO_InitStructure.GPIO_Pin = DEBUG_USART_TX_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(DEBUG_USART_TX_GPIO_PORT, &GPIO_InitStructure);

  // 将USART Rx的GPIO配置为浮空输入模式
	GPIO_InitStructure.GPIO_Pin = DEBUG_USART_RX_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
	GPIO_Init(DEBUG_USART_RX_GPIO_PORT, &GPIO_InitStructure);
	
	// 配置串口的工作参数
	// 配置波特率
	USART_InitStructure.USART_BaudRate = DEBUG_USART_BAUDRATE;
	// 配置 针数据字长
	USART_InitStructure.USART_WordLength = USART_WordLength_8b;
	// 配置停止位
	USART_InitStructure.USART_StopBits = USART_StopBits_1;
	// 配置校验位
	USART_InitStructure.USART_Parity = USART_Parity_No ;
	// 配置硬件流控制
	USART_InitStructure.USART_HardwareFlowControl = 
	USART_HardwareFlowControl_None;
	// 配置工作模式，收发一起
	USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
	// 完成串口的初始化配置
	USART_Init(DEBUG_USARTx, &USART_InitStructure);
	
	// 串口中断优先级配置
	NVIC_Configuration();
	
	// 使能串口接收中断
	USART_ITConfig(DEBUG_USARTx, USART_IT_RXNE, ENABLE);	
	
	// 使能串口
	USART_Cmd(DEBUG_USARTx, ENABLE);	    
}

/*发送一个字节*/
void Usart_SendByte(USART_TypeDef* pUSARTx,uint8_t data)
{
	USART_SendData(pUSARTx, data);
	while(USART_GetFlagStatus(pUSARTx, USART_FLAG_TXE)==RESET);
}

/*发送两个字节*/
void Usart_SendHalfWord(USART_TypeDef* pUSARTx,uint16_t data)
{
	uint8_t data_h,data_l;
	
	data_h=data>>8;//取出高八位
	data_l=data&0xff;//取出低八位
	
	//发送高八位
	USART_SendData(pUSARTx, data_h);
	while(USART_GetFlagStatus(pUSARTx, USART_FLAG_TXE)==RESET);
	
	//发送低八位
	USART_SendData(pUSARTx, data_l);
	while(USART_GetFlagStatus(pUSARTx, USART_FLAG_TXE)==RESET);
}

/*发送一个8位数组*/
void Usart_SendArray(USART_TypeDef* pUSARTx,uint8_t *array,uint16_t num)//8位一共最多256个
{
	uint8_t i=0;
	
	for(i=0;i<num;i++)//循环发送数组中的每一个成员
	{
	Usart_SendByte(pUSARTx, array[i]);//注意是Usart_SendByte，不是USART_SendData
	}
	while(USART_GetFlagStatus(pUSARTx, USART_FLAG_TC)==RESET);//TC是全部发送完的标志位
}

/*发送字符串*/
void Usart_SendStr(USART_TypeDef* pUSARTx,char *str)//char类型指针
{
	unsigned int i=0;
	
	do
	{
	Usart_SendByte(pUSARTx, *(str+i));
	i++;
	}while(*(str+i)!='\0');//当发送的字符串不为结束位时一直发送
	
	while(USART_GetFlagStatus(pUSARTx, USART_FLAG_TC)==RESET)
	{};
}

/*重定向c库函数printf到串口，重定向后可使用printf函数*/
int fputc(int ch, FILE *f)
{
		/* 发送一个字节数据到串口 */
		USART_SendData(DEBUG_USARTx, (uint8_t) ch);
		
		/* 等待发送完毕 */
		while (USART_GetFlagStatus(DEBUG_USARTx, USART_FLAG_TXE) == RESET);		
	
		return (ch);
}
