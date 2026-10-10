# ============================================================================
#  GB/T 27930-2015  BMS CAN 监控主终端  ——  Makefile
#
#  用法:
#     make                 编译（本机 gcc，用于 Ubuntu 虚拟机调试）
#     make ARCH=arm        交叉编译（arm-linux-gnueabihf-gcc，用于 I.MX6ULL）
#     make clean           清理
#     make run             编译后直接以默认参数运行
#     make db-info         查看数据库表结构与记录条数
#     make export          导出 CSV 到 ./export/
#
#  依赖: libsqlite3-dev (arm 版需 libsqlite3-dev:armhf 或交叉编译的 sqlite3)
#        安装: sudo apt-get install -y build-essential libsqlite3-dev
# ============================================================================

# ------------------------------ 编译器选择 ----------------------------------
# 说明：sqlite3 的头文件与库路径默认为系统路径（libsqlite3-dev 安装到 /usr）。
#       若在安装了 sqlite3 运行库但没有开发包的机器上编译，或做交叉编译，
#       可以通过下面两个变量指定自定义 sysroot，例如：
#         make SQLITE_CFLAGS=-I/opt/sysroot/usr/include \
#              SQLITE_LDFLAGS=-L/opt/sysroot/usr/lib
SQLITE_CFLAGS  ?=
SQLITE_LDFLAGS ?=

ifeq ($(ARCH),arm)
    CROSS_COMPILE ?= arm-linux-gnueabihf-
    CC            := $(CROSS_COMPILE)gcc
    STRIP         := $(CROSS_COMPILE)strip
    EXTRA_CFLAGS  := -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard
else
    CC            ?= gcc
    STRIP         ?= strip
    EXTRA_CFLAGS  :=
endif

TARGET      := can_monitor
SELFTEST    := selftest
UITEST      := uitest
BUILD_DIR   := build

# ------------------------------ 编译选项 ------------------------------------
# -std=gnu11     : 使用 C11 标准（_Atomic / stdatomic.h 需要）
# -D_GNU_SOURCE  : 打开 SO_TIMESTAMPNS、getopt_long、localtime_r 等扩展
# -pthread       : 多线程
CFLAGS      := -std=gnu11 -D_GNU_SOURCE -pthread \
               -Wall -Wextra -Wno-unused-parameter \
               -O2 -g -ffunction-sections -fdata-sections \
               $(EXTRA_CFLAGS) $(SQLITE_CFLAGS)

LDFLAGS     := -pthread -Wl,--gc-sections
LDLIBS      := $(SQLITE_LDFLAGS) -lsqlite3 -lpthread -lrt -lm

# ------------------------------ 源文件 --------------------------------------
# 说明：fbdev.c / gui.c / uitouch.c / gui_app.c 是本地 HMI（framebuffer + 触摸），
#       不依赖任何图形库；只有 gui_app.c 依赖 SQLite（读历史曲线）。
SRCS        := main.c \
               can_layer.c \
               ring_buffer.c \
               isotp.c \
               gb27930.c \
               storage.c \
               session.c \
               ui.c \
               fbdev.c \
               gui.c \
               uitouch.c \
               gui_app.c

OBJS        := $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))
DEPS        := $(OBJS:.o=.d)

# 自检程序：除 main.c 之外的全部模块 + selftest.c
SRCS_ST     := selftest.c can_layer.c ring_buffer.c isotp.c gb27930.c storage.c ui.c \
               fbdev.c gui.c uitouch.c
OBJS_ST     := $(addprefix $(BUILD_DIR)/st_,$(SRCS_ST:.c=.o))
DEPS_ST     := $(OBJS_ST:.o=.d)

# UI 离线渲染自检：纯内存 framebuffer，不需要屏幕、不需要 CAN、不需要数据库
SRCS_UT     := uitest.c fbdev.c gui.c
OBJS_UT     := $(addprefix $(BUILD_DIR)/ut_,$(SRCS_UT:.c=.o))
DEPS_UT     := $(OBJS_UT:.o=.d)

# ============================================================================
#  构建规则
# ============================================================================

.PHONY: all clean run db-info export install help selftest uitest

all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  LD      $@"
	@$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@$(STRIP) --strip-unneeded $@ 2>/dev/null || true
	@echo ""
	@echo "  编译完成: $(TARGET)"
	@ls -lh $(TARGET) | awk '{print "  文件大小: " $$5}'
	@echo "  运行: sudo ./$(TARGET) -i can0 -b 250000"
	@echo "  自检: make selftest && ./$(SELFTEST)"

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# 自检目标使用独立的对象目录前缀 st_，避免与主程序共用 .o 造成重复符号
$(BUILD_DIR)/st_%.o: %.c | $(BUILD_DIR)
	@echo "  CC      $< (selftest)"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# UI 自检目标使用前缀 ut_
$(BUILD_DIR)/ut_%.o: %.c | $(BUILD_DIR)
	@echo "  CC      $< (uitest)"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(SELFTEST): $(OBJS_ST)
	@echo "  LD      $@"
	@$(CC) $(OBJS_ST) -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "  编译完成: $(SELFTEST)（离线自检，无需 CAN 硬件）"

$(UITEST): $(OBJS_UT)
	@echo "  LD      $@"
	@$(CC) $(OBJS_UT) -o $@ $(LDFLAGS) -lm
	@echo "  编译完成: $(UITEST)（UI 离线渲染自检，无需屏幕）"

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

-include $(DEPS) $(DEPS_ST) $(DEPS_UT)

# ============================================================================
#  辅助目标
# ============================================================================

# 编译并以 GB/T 27930 标准参数运行
run: $(TARGET)
	sudo ./$(TARGET) -i can0 -b 250000 -d ./gb27930_data.db

# 纯抓包模式（不主动发报文），用于第一步联调验证
dump: $(TARGET)
	sudo ./$(TARGET) -i can0 --dump --no-ui -d ./dump_data.db

# 单板回环自测（无对端硬件时验证软件链路）
loopback: $(TARGET)
	sudo ./$(TARGET) -i can0 -l -d ./loopback_data.db

# 查看数据库内容
db-info:
	@echo "=== 数据库表结构 ==="
	@sqlite3 ./gb27930_data.db ".tables"
	@echo ""
	@echo "=== 记录条数 ==="
	@sqlite3 ./gb27930_data.db "SELECT 'can_raw', COUNT(*) FROM can_raw UNION ALL SELECT 'charge_data', COUNT(*) FROM charge_data;"
	@echo ""
	@echo "=== 最近 5 条解析数据 ==="
	@sqlite3 -header -column ./gb27930_data.db \
	    "SELECT ts_us, state, bcs_measure_voltage, bcs_measure_current, bcs_current_soc FROM charge_data ORDER BY id DESC LIMIT 5;"
	@echo ""
	@echo "=== 数据库文件 ==="
	@ls -lh ./gb27930_data.db* 2>/dev/null || true

# 导出 CSV
export: $(TARGET)
	@mkdir -p export
	@sqlite3 -header -csv ./gb27930_data.db "SELECT * FROM can_raw;"      > export/can_raw.csv
	@sqlite3 -header -csv ./gb27930_data.db "SELECT * FROM charge_data;"  > export/charge_data.csv
	@echo "已导出: export/can_raw.csv  export/charge_data.csv"
	@wc -l export/*.csv

install: $(TARGET)
	install -m 0755 $(TARGET) /usr/local/bin/$(TARGET)
	@echo "已安装到 /usr/local/bin/$(TARGET)"

help:
	@echo "可用目标:"
	@echo "  all       编译（默认）"
	@echo "  clean     清理编译产物"
	@echo "  run       以 GB/T 27930 标准参数运行"
	@echo "  dump      纯抓包模式"
	@echo "  loopback  回环自测"
	@echo "  db-info   查看数据库内容"
	@echo "  export    导出 CSV"
	@echo "  install   安装到 /usr/local/bin"
	@echo "  selftest-run  协议/存储离线自检"
	@echo "  uitest-run    界面离线渲染自检"
	@echo "  gui-test      界面整链路验证（虚拟屏+脚本化触摸，导出 PNG）"
	@echo "  png           把界面导出的 PPM 帧转成 PNG"
	@echo ""
	@echo "交叉编译: make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-"

clean:
	@rm -rf $(BUILD_DIR) $(TARGET) $(SELFTEST) $(UITEST) export *.ppm
	@echo "已清理"

# ============================================================================
#  环境自检（编译前可用 make check 确认依赖齐全）
# ============================================================================
.PHONY: check selftest-run uitest-run png gui-test
# 离线自检（无需 CAN 硬件，验证协议解析/多帧重组/状态机/存储）
selftest-run: $(SELFTEST)
	@./$(SELFTEST)

# UI 离线渲染自检（无需屏幕/触摸，渲染两屏并导出 PPM + 字符画）
uitest-run: $(UITEST)
	@./$(UITEST)

# 界面整链路验证（虚拟屏 + 脚本化触摸 + 真实 CAN 数据 + 导出 PNG）
gui-test: $(TARGET)
	@./run_gui_test.sh

# 把界面导出的 PPM 帧转成 PNG（便于目视检查 / 报告截图）
# 需要 Pillow: sudo apt-get install -y python3-pil
png:
	@if [ -x ./tools/ppm2png.py ] || [ -f ./tools/ppm2png.py ]; then \
	    python3 ./tools/ppm2png.py /tmp/gui_frames /tmp; \
	else \
	    echo "找不到 tools/ppm2png.py，请把仓库的 tools/ 目录一起拷过来"; \
	fi

check:
	@echo "=== 编译器 ==="; $(CC) --version | head -1
	@echo "=== SQLite 开发库 ==="; \
	    (pkg-config --modversion sqlite3 2>/dev/null || echo "未找到 pkg-config 信息"); \
	    ls /usr/include/sqlite3.h >/dev/null 2>&1 && echo "sqlite3.h 存在" || echo "缺少 sqlite3.h，请安装 libsqlite3-dev"
	@echo "=== CAN 工具 ==="; \
	    which candump cansend cangen ip 2>/dev/null || true
	@echo "=== CAN 内核模块 ==="; lsmod | grep -E 'can|flexcan|mcp251' || echo "(无已加载模块，请在开发板上检查)"
	@echo "=== CAN 接口 ==="; ip -details link show can0 2>/dev/null || echo "(本机无 can0，正常)"
