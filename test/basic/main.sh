#! /bin/bash

# 测试结束时进行一些清理操作
finish_test() {
  sudo umount ./imgdir
  rmdir imgdir
  rm -f fs.img
}

# 检查测试用例的结果是否合法
check_result()
{
  if [ $1 != 0 ]; then
    echo "testcase" $2 "failed!"
    # finish_test
    exit $1
  fi
}

sudo dmesg -C

# mkimg 格式化功能

# 数据类型
gcc -o t_type.o t_type.c
./t_type.o
check_result $? "数据类型"

# 进行下列测试前需要新建并挂载一个 50M 大小的 babyfs 文件系统
dd if=/dev/zero of=fs.img bs=1M count=50
../../mkfs.babyfs ./fs.img
if [ ! -d "imgdir" ]; then
  mkdir imgdir
fi
# 以sync方式挂载文件系统，可以保证对bitmap的操作同步执行，
sudo mount -t babyfs -o loop -o sync ./fs.img ./imgdir
if [ $? != 0 ]; then
  echo -e "\nmount failed!\n"
  exit -1
fi
echo -e "\nmount success!\n"
stat -f ./imgdir
echo -e "\n"

# 文件新建、删除、读写
gcc -o t_file.o t_file.c
./t_file.o
check_result $? "文件新建、删除、读写"

# 文件夹新建、删除
# 文件与文件夹混合操作

# 超大文件操作


finish_test