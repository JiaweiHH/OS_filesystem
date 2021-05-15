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
sudo dmesg -C

# mkimg 格式化功能

# ************************ 数据类型 ************************

gcc -o t_type.o t_type.c
./t_type.o
check_result $? "数据类型测试" "succ"

if [ !`cat /proc/filesystems | grep babyfs` ]; then
  echoerr "filesystem has not installed!"
  exit -1
fi

# ************************ 文件新建 ************************

# 进行下列测试前需要新建并挂载一个 50MB 大小的 babyfs 文件系统
mount_fs_with_img 1 50
gcc -o t_file_create t_file_create.c
./t_file_create
check_result $? "文件新建测试" "succ"

# ************************ 文件读写 ************************

# 进行下列测试前需要新建并挂载一个 2GB 大小的 babyfs 文件系统
mount_fs_with_img 1 2048

# 文件大小分别为 10KB 256KB 50MB 500MB
# 分别可以测试直接索引，1、2、3级索引
filelist=( "10.K" "256.K" "50.M" "500.M" )
for file in ${filelist[@]}
do
  cnt=`echo ${file} | cut -d '.' -f 1`
  unit=`echo ${file} | cut -d '.' -f 2`
  filename="__f_${cnt}${unit}B"
  unit="1${unit}"

  # 先在系统的文件系统中新建一个文件，计算其md5值
  dd if=/dev/zero of=./${filename} bs=$unit count=$cnt 2>> ${logfile}
  realmd5=`md5sum ${filename} | cut -d ' ' -f 1`
  # 再在babyfs中新建一个同样大小的文件，计算其md5值
  dd if=/dev/zero of=./imgdir/${filename} bs=$unit count=$cnt 2>> ${logfile}
  check_result $? "${filename} 在文件系统中写文件成功"
  # 通过比较md5值，判断babyfs的文件存储是否正确
  curmd5=`md5sum ./imgdir/${filename} | cut -d ' ' -f 1`
  if [ "${realmd5}" != "${curmd5}" ]; then
    echoerr "${filename} 第一次读文件时，发生错误"
    exit -1
  fi

  # 重新挂载文件系统，可测试经过磁盘同步之后，文件的存储是否发生错误
  mount_fs_with_img 0
  check_result $? "重新挂载文件系统成功"
  curmd5=`md5sum ./imgdir/${filename} | cut -d ' ' -f 1`
  if [ "${realmd5}" != "${curmd5}" ]; then
    echoerr "${filename} 重新挂载文件系统后，读文件，发生错误"
    exit -1
  fi

  echoinfo "该级文件索引的使用正确\n"
done

rm -f __f_*
echosucc "文件读写测试 成功"

# TODO 文件夹新建、删除
# TODO 文件与文件夹混合操作
# TODO 超大文件操作


finish_test