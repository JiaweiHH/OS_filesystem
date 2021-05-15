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
