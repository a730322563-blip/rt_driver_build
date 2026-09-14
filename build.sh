#!/system/bin/sh
# Termux 编译脚本 —— 放到 rt_src 目录里执行
# 用法: sh build.sh /path/to/kernel_src

KERNEL_SRC="$1"
if [ -z "$KERNEL_SRC" ]; then
    echo "用法: sh build.sh <内核源码路径>"
    echo "示例: sh build.sh ~/kernel_src"
    exit 1
fi

# 生成 Makefile（独立模块编译）
cat > Makefile <<'EOF'
obj-m += comm.o memory.o process.o hide_process.o entry.o pte_track.o hw_breakpoint.o

# entry.c 是主入口，其他头文件被它 include，实际只编译 entry.o
# 但内核模块每个 .c 都会单独编译，这里只有 entry.c 是 .c 文件
# 其他都是 .h，所以实际只需要：
obj-m := rt_driver.o
rt_driver-objs := entry.o

ccflags-y += -I$(src)
EOF

# 实际上 entry.c include 了所有 .h，只需要编译 entry.o
cat > Makefile <<'EOF'
obj-m := rt_driver.o
rt_driver-objs := entry.o
ccflags-y += -I$(src)
EOF

export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-android-

make -C "$KERNEL_SRC" M=$(pwd) modules 2>&1 | tee build.log

if [ -f rt_driver.ko ]; then
    echo ""
    echo "=== 编译成功 ==="
    ls -lh rt_driver.ko
    echo ""
    echo "推送到手机:"
    echo "  cp rt_driver.ko /sdcard/Download/"
else
    echo ""
    echo "=== 编译失败，看 build.log ==="
fi
