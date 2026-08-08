# Makefile for live_wallpaper
# 用法: make [ARCH=64|32] [DEBUG=1] [STATIC=1] [all|clean|run]

# 设置代码页为UTF-8
$(shell chcp 65001 >nul)

# 默认架构
ARCH ?= 64

# 是否静态链接（默认开启，因为通常发布需要）
STATIC ?= 1

# 编译器选择
ifeq ($(ARCH),32)
    CXX = i686-w64-mingw32-g++
    RC  = i686-w64-mingw32-windres
else
    CXX = g++
    RC  = windres
endif

# 编译标志
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN
ifeq ($(DEBUG),1)
    CXXFLAGS += -g -D_DEBUG
endif

# 链接标志（基础）
LDFLAGS  = -mwindows -municode
# 如果需要静态链接，添加 -static
ifeq ($(STATIC),1)
    LDFLAGS += -static
endif

LDLIBS   = -lgdi32 -luser32 -lshell32 -lcomdlg32 -lshlwapi

# 目标文件
TARGET = live_wallpaper.exe
SRC    = main.cpp
RC_SRC = resources.rc
RC_OBJ = resources.o

# 默认目标
all: $(TARGET)

# 链接
$(TARGET): $(SRC) $(RC_OBJ)
	@echo 链接生成 $@ ...
	@$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# 编译资源
$(RC_OBJ): $(RC_SRC) resources.hpp
	@echo 编译资源文件 $< ...
	@$(RC) $< -O coff -o $@

# 清理
clean:
	@echo 清理文件...
	@if exist $(TARGET) del $(TARGET)
	@if exist $(RC_OBJ) del $(RC_OBJ)

# 运行
run: $(TARGET)
	@echo 运行 $(TARGET)
	@$(TARGET)

# 帮助
help:
	@echo 可用目标: all clean run
	@echo 变量: ARCH=32/64 (默认64), DEBUG=1 (启用调试), STATIC=1/0 (默认1, 静态链接)

.PHONY: all clean run help