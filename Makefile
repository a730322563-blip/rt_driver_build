# rt_driver 内核模块 Makefile
# 用法: make -C <内核源码路径> M=$(pwd) modules
#
# entry.c include 了 comm.h / memory.h / process.h / hide_process.h
#          / pte_track.h / hw_breakpoint.h
# 所以只需要编译 entry.o，链接成 rt_driver.ko

obj-m := rt_driver.o
rt_driver-objs := entry.o

ccflags-y += -I$(src) -Wno-error
