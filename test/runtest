#!/bin/bash

# 这里只是调用所有指定测试用例的入口
# 实际测试的方法由用户自定义给出，可以是sh、c，甚至是插装模块

# 将要执行的测试集
testsuits=( "basic" "usec" )
dir=`pwd`

for testsuit in ${testsuits[@]}
do
  cd "$dir/$testsuit"
  
  bash ./main.sh
  if [ $? != 0 ]; then
    echo "testsuit ${testsuit} failed!"
    exit -1
  fi
  
  echo -e "\n----> testsuit * ${testsuit} * success! <----\n"

  cd $dir
done

echo -e "Congratulations!!! every thing is ok."