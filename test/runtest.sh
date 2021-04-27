#!/bin/bash

. ./niceecho.sh

# 这里只是调用所有指定测试用例的入口
# 实际测试的方法由用户自定义给出，可以是sh、c，甚至是插装模块

# 将要执行的测试集
testsuits=( "basic" "usec" )
dir=`pwd`

for testsuit in ${testsuits[@]}
do
  cd "$dir/$testsuit"

  echoinfo "\n------->>>>>>> testsuit __${testsuit}__ start ------->>>>>>\n"
  
  bash ./main.sh
  if [ $? != 0 ]; then
    echoerr "<<<<<<<------- testsuit __${testsuit}__ failed! <<<<<<-----"
    exit -1
  fi
  
  echosucc "<<<<<<<------- testsuit __${testsuit}__ success! <<<<<<----"

  cd $dir
done

echosucc "Congratulations!!! every thing is ok." 