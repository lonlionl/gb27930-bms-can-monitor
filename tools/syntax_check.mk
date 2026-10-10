# ============================================================================
#  syntax_check.mk —— STM32 端源码的 PC 侧语法检查（不需要 ARM 工具链）
#
#  用途:
#      在 Ubuntu 虚拟机上用本机 gcc 对 STM32 源码做「只解析不生成代码」的
#      检查，能提前发现：类型不匹配、函数原型不符、变量未声明、
#      C89 里「声明必须放在块首」这类问题。
#
#  用法（在解包后的目录里）:
#      make -f tools/syntax_check.mk
# ============================================================================

CC      := gcc
STD     := -std=gnu90
WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable

INCS    := -ILibraries/CMSIS \
           -ILibraries/STM32F10x_StdPeriph_Driver/inc \
           -IUser \
           -IUser/Can \
           -IUser/Bms \
           -IUser/Led \
           -IUser/Usart \
           -IUser/Ui \
           -IUser/Lcd

DEFS    := -DUSE_STDPERIPH_DRIVER -DSTM32F10X_HD
CFLAGS  := $(STD) $(WARN) $(DEFS) $(INCS) -fsyntax-only

SRCS    := User/main.c \
           User/stm32f10x_it.c \
           User/Can/can.c \
           User/Bms/bms_protocol.c \
           User/Led/bsp_led.c \
           User/Usart/bsp_usart.c \
           User/Ui/ui_port.c \
           User/Ui/ui_app.c \
           User/Lcd/lcd.c \
           User/Lcd/ili93xx.c \
           User/Lcd/ctiic.c \
           User/Lcd/touch.c

.PHONY: all clean
all:
	@fail=0; \
	for f in $(SRCS); do \
	    printf "  CHECK   %s\n" $$f; \
	    if ! $(CC) $(CFLAGS) $$f; then fail=1; fi; \
	done; \
	if [ $$fail -eq 0 ]; then \
	    echo ""; echo "  == 全部源文件语法检查通过 =="; \
	else \
	    echo ""; echo "  !! 存在语法/类型错误，见上方输出 !!"; exit 1; \
	fi

clean:
	@echo "语法检查不产生中间文件"
