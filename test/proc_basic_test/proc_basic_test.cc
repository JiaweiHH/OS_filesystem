#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../babyfs.h"
#include "../test_information.h"

/**
 * 仅仅是为了测试 proc 的基础功能
 * 不用做文件系统测试
 */

void test_self_simple_test() {
  std::ofstream ostrm("/proc/babyfs/1.txt");
  std::string arg = "-r -n 1 -l -i";
  ostrm << arg;
  ostrm.close();

  std::ifstream istrm("/proc/babyfs/1.txt");
  struct alloc_information info;

  istrm.read(reinterpret_cast<char *>(&info), sizeof info);
  std::cout << info.__rsv_goal_size << ", " << info.__rsv_alloc_hit << ", "
            << info.__rsv_start << ", " << info.__rsv_end << ", "
            << info.__last_alloc_logical_block << "\n";
  struct inode_information inode_info;
  istrm.read(reinterpret_cast<char *>(&inode_info), sizeof inode_info);
  std::cout << inode_info.__i_blocks[0] << ", " << inode_info.__i_subdir_num << "\n";
}

void test_set_goal() {
  std::ofstream ostrm("/proc/babyfs/1.txt");
  std::string arg = "-w -n 1 -s g 1111 ";
  ostrm << arg;
  // ostrm.close();

  arg = "-r -n 1 -l -i";
  ostrm << arg;
  ostrm.close();

  std::ifstream istrm("/proc/babyfs/1.txt");
  struct alloc_information info;

  istrm.read(reinterpret_cast<char *>(&info), sizeof info);
  std::cout << info.__rsv_goal_size << ", " << info.__rsv_alloc_hit << ", "
            << info.__rsv_start << ", " << info.__rsv_end << ", "
            << info.__last_alloc_logical_block << "\n";
  struct inode_information inode_info;
  istrm.read(reinterpret_cast<char *>(&inode_info), sizeof inode_info);
  std::cout << inode_info.__i_blocks[0] << ", " << inode_info.__i_subdir_num << "\n";

}

int main(int argc, char *argv[]) {
  // test_self_simple_test();
  // test_set_goal()
  return 0;
}
