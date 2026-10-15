# ============================================================================
#  app/Makefile —— sms_pdu 主机端单元测试（MinGW gcc / GNU make）
#
#  用法（在仓库根目录，即 app 的上一级）:
#      mingw32-make -f app/Makefile            # 生成向量 + 编译 + 运行
#      mingw32-make -f app/Makefile test       # 同上
#      mingw32-make -f app/Makefile clean
#
#  也可以直接: mingw32-make -C app test
#
#  注意: 本 Makefile 只用于 PC 上的主机端测试，不参与 OpenCPU 固件构建。
#
#  关于 SHELL: Windows 上如果 PATH 里存在 MSYS/Git 的 sh.exe，
#  GNU make 会优先用它执行配方，而 sh.exe 在某些受限制环境下无法启动
#  （"couldn't create signal pipe"）。因此这里固定用 cmd.exe 执行配方，
#  配方里也只使用 cmd 认识的命令。
# ============================================================================

SHELL := cmd.exe
.SHELLFLAGS := /c

# ---- 工具链 ----------------------------------------------------------------
CC      ?= gcc
PYTHON  ?= python

# ---- 目录 ------------------------------------------------------------------
# 支持两种调用方式: `make -f app/Makefile`（根目录）或 `make -C app`（app 内）
ifeq ($(wildcard app/inc/sms_pdu.h),)
  APP_DIR := .
else
  APP_DIR := app
endif

INC_DIR   := $(APP_DIR)/inc
SRC_DIR   := $(APP_DIR)/src
TEST_DIR  := $(APP_DIR)/test
BUILD_DIR := $(APP_DIR)/build

# ---- 编译选项（题目要求） --------------------------------------------------
CFLAGS  := -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I $(INC_DIR)
LDFLAGS := -fsanitize=address,undefined

# Windows 上 MinGW gcc 默认生成 .exe
EXEEXT  ?= .exe

TARGET  := $(BUILD_DIR)/test_sms_pdu$(EXEEXT)
HEADER  := $(TEST_DIR)/sms_pdu_vectors.h
GEN     := $(TEST_DIR)/gen_pdus.py

# ============================================================================
.PHONY: all test gen clean run

all: test

## 生成 C 向量头文件（独立 Python 编码器）
gen: $(HEADER)

$(HEADER): $(GEN)
	@echo [GEN]  $@
	@$(PYTHON) $(GEN) --emit-c-header $@

## 编译 + 运行
test: $(TARGET)
	@echo [RUN]  $(TARGET)
	@$(TARGET)

$(TARGET): $(TEST_DIR)/test_sms_pdu.c $(SRC_DIR)/sms_pdu.c $(HEADER)
	@if not exist "$(BUILD_DIR)" mkdir "$(BUILD_DIR)"
	@echo [CC]   $@
	$(CC) $(CFLAGS) $(TEST_DIR)/test_sms_pdu.c $(SRC_DIR)/sms_pdu.c -o $@ $(LDFLAGS)

## 只运行（不重新编译）
run: $(TARGET)
	@$(TARGET)

clean:
	@echo [CLEAN]
	@if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"
	@if exist "$(HEADER)" del /q "$(HEADER)"
