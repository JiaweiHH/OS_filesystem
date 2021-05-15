#! /bin/bash

# 日志文件的地址从外面传入
logfile=$1
. ../niceecho.sh

# 测试结束时进行一些清理操作
finish_test() {
  sudo umount ./imgdir
  rmdir imgdir
  rm -f fs.img
}

# 检查测试用例/某个命令的结果是否合法
# $1 命令的结果；$2 需要输出的字符串；$3="succ" 使用成功输出
check_result()
{
  if [ $1 != 0 ]; then
    echoerr $2 "失败！"
    # finish_test
    exit $1
    else
    if [ "$3" != "succ" ]; then
      echoinfo $2
      else
      echosucc $2 "成功"
    fi
  fi
}

# $1 指定是否需要重新生成磁盘镜像，若 $1!=0，则生成大小为 $2 的新磁盘镜像
mount_fs_with_img() {
  if [ ! -d "imgdir" ]; then
    mkdir imgdir
  fi
  # 若之前挂载的文件系统还没卸载，则先卸载
  if [ `findmnt | grep test/basic/imgdir | cut -d " " -f1` ]; then
    sudo umount ./imgdir
    check_result $? "卸载已挂载的文件系统"
  fi

  # 生成新的磁盘镜像，并格式化
  if [ $1 != 0 ]; then
    dd if=/dev/zero of=fs.img bs=1M count=$2 2>> ${logfile}
    ../../mkfs.babyfs ./fs.img >> ${logfile}
  fi

  # 以sync方式挂载文件系统，可以保证对bitmap的操作同步执行
  sudo mount -t babyfs -o loop -o sync ./fs.img ./imgdir
  if [ $? != 0 ]; then
    echoerr "mount failed!"
    exit -1
  fi

  if [ $1 == 1 ]; then
    echoinfo "mount success. filesystem info:"
    stat -f ./imgdir | tee -a ${logfile}
    echo -e "\n"
  fi
}

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