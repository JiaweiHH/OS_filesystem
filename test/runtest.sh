#!/bin/bash

dir=`pwd`
logfile="${dir}/test.log"
# 将已有的日志文件删除
if [ -f $logfile ]; then
  rm -f ${logfile}
  # 记录本次测试的开始时间
  echo "TEST TIME: `date`" >> ${logfile}
  # 记录本次测试的版本信息，模块的hash值可作为唯一的版本信息
  echo "TEST VERSION: `md5sum ../babyfs.ko`" >> ${logfile}
  # 
  echo "PLATFORM: `uname -a`" >> ${logfile}
  echo "USER: `whoami`" >> ${logfile}
fi

. ./niceecho.sh

# 这里只是调用所有指定测试用例的入口
# 实际测试的方法由用户自定义给出，可以是sh、c，甚至是插装模块

# 将要执行的测试集，其中example是一个示例测试
testsuits=( "basic" "example" )

for testsuit in ${testsuits[@]}
do
  cd "$dir/$testsuit"

  echoinfo "\n------->>>>>>> testsuit __${testsuit}__ start ------->>>>>>\n"
  
  bash ./main.sh ${logfile}
  if [ $? != 0 ]; then
    echoerr "<<<<<<<------- testsuit __${testsuit}__ failed! <<<<<<-----"
    exit -1
  fi
  
  echosucc "<<<<<<<------- testsuit __${testsuit}__ success! <<<<<<----"

  cd $dir
done

echosucc "Congratulations!!! every thing is ok." 