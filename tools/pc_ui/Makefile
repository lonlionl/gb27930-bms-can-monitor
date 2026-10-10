# ============================================================================
#  tools/pc_ui/Makefile —— 在 PC 上编译并渲染 STM32 界面（仅开发期自检用）
#
#  目的:
#      STM32 的界面没法在 PC 上真跑，但可以把它「原样」编译出来，
#      把绘制指令落在一块内存里的 480x320 虚拟屏上，再导成字符画和 PPM 图，
#      用来检查：元素有没有重叠、文字有没有画出屏、布局是否合理。
#
#  关键编译选项:
#      -finput-charset=GBK    源码是 Keil 用的 GBK 编码
#      -fexec-charset=UTF-8   编译时把字符串字面量转成 UTF-8，
#                             这样就能直接查 font16.h 里的 Unicode 字模表
#
#  用法（在仓库根目录执行）:
#      make -f tools/pc_ui/Makefile
# ============================================================================

CC      := gcc
# ui_app.c 是 Keil 工程里的 GBK 源码，按 GBK 读入再转成 UTF-8 执行字符集；
# 本目录下的工具文件（pc_ui.c 等）是 UTF-8，因此单独去掉 -finput-charset=GBK。
CHARSET := -finput-charset=GBK -fexec-charset=UTF-8
CFLAGS  := -std=gnu99 -Wall -Wextra -Wno-unused-parameter -O1 -g \
           -Itools/pc_ui -IUser/Ui
CFLAGS_GBK  := $(CFLAGS) $(CHARSET)
CFLAGS_UTF8 := $(CFLAGS) $(filter-out -finput-charset=GBK,$(CHARSET))

OBJS    := build/pcui_pc_ui.o build/pcui_ui_app.o
BIN     := build/pc_ui

.PHONY: all run clean

all: $(BIN)
	@echo ""
	@echo "  运行: make -f tools/pc_ui/Makefile run"

$(BIN): $(OBJS)
	@echo "  LD      $@"
	@$(CC) $(OBJS) -o $@ -lm

build/pcui_%.o: tools/pc_ui/%.c | build
	@echo "  CC      $<"
	@$(CC) $(CFLAGS_UTF8) -c $< -o $@

build/pcui_ui_app.o: User/Ui/ui_app.c | build
	@echo "  CC      $<  (Keil 的 GBK 源码，按 GBK 读入)"
	@$(CC) $(CFLAGS_GBK) -c $< -o $@

build:
	@mkdir -p build

run: $(BIN)
	@./$(BIN)

clean:
	@rm -f build/pcui_*.o $(BIN)
	@echo "已清理 PC 界面自检产物"
