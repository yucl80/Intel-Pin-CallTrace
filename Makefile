##############################################################
# calltrace Pin tool
#
# 依赖 Intel Pin 3.x 完整安装包（含 source/tools/Config）。
#
# 用法：
#   make PIN_ROOT=/path/to/pin-3.x TARGET=intel64
#
# 产物：obj-intel64/calltrace.so
##############################################################
PIN_ROOT ?= /opt/pin

CONFIG_ROOT := $(PIN_ROOT)/source/tools/Config
include $(CONFIG_ROOT)/makefile.config

# 注意：TOOL_ROOTS 必须定义在 makefile.default.rules 之前，
# 因为该规则文件用 := 简单展开，解析时就要看到工具列表。
TOOL_ROOTS := calltrace

include $(CONFIG_ROOT)/makefile.default.rules
