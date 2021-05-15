#! /bin/bash

# 日志文件的地址从外面传入，可以用来将某些信息输出到日志文件中
logfile=$1
# 使用辅助函数优化命令行输出，辅助函数会自动将输出写入到日志文件
. ../niceecho.sh
# 大多数测试集都需要挂载文件系统，这里提供了一些帮助函数
. ../imgtesttool.sh


# *********************************************************
# >>>>>>>>>>>>>>>>>>>>>>> 开始测试 <<<<<<<<<<<<<<<<<<<<<<<<*
# *********************************************************

# 测试前的准备，比如清空系统日志，检查文件系统是否安装

# ************************ 使用c语言的测试用例 ************************

# 编译、运行c程序，然后用帮助函数检查程序返回值是否正确
gcc -o usec usec.c
./usec
check_result $? "使用c语言的测试用例" "succ"

# ************************ 使用shell的测试用例 ************************
