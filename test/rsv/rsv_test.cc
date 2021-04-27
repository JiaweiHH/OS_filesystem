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
 * 测试了创建两个文件的时候文件块的关系 以及 rsv 的基本信息
 */

int32_t rsv_test_write(const uint32_t &ino, const std::string &arg) {
  std::string file_name = "/proc/babyfs/" + std::to_string(ino) + ".txt";
  std::ofstream ostrm(file_name);
  if (!ostrm.is_open())
    return -1;
  ostrm << arg;
  ostrm.close();
  return 0;
}

int32_t rsv_test_read(const uint32_t &ino, struct alloc_information &info) {
  std::string filename = "/proc/babyfs/" + std::to_string(ino) + ".txt";
  std::ifstream istrm(filename);
  if (!istrm.is_open())
    return -1;
  istrm.read(reinterpret_cast<char *>(&info), sizeof info);
  istrm.close();
  return 0;
}

int32_t create_two_4blocks_file() {
  std::cout << "test create two file to see the block is right\n";
  std::vector<struct alloc_information> vec_alloc_info = {alloc_information{},
                                                          alloc_information{}};
  std::vector<std::string> vec_filename;

  /* 创建两个文件，大小都是 4 个 block */
  constexpr uint32_t count = 4;
  constexpr uint32_t sz = count * BABYFS_BLOCK_SIZE;
  for (int i = 1; i <= 2; ++i) {
    std::string filename = "../../imgdir/test_file_" + std::to_string(i);
    vec_filename.push_back(filename);
    std::ofstream ostrm(filename);
    ostrm.fill(' ');
    ostrm.width(sz);
    ostrm << " ";
    ostrm.close();
  }

  /* 获取每个文件的 first_block 和 alloc_info 信息 */
  for (int i = 1; i <= 2; ++i) {
    // 先写入参数
    std::string argv = "-r -n " + std::to_string(i) + " -l";
    int32_t ret = rsv_test_write(i, argv);
    if (ret) {
      std::cerr << "write argv [ " << argv << " ] err.\n";
      goto err;
    }
    // 获取数据
    ret = rsv_test_read(i, vec_alloc_info[i - 1]);
    if (ret) {
      std::cerr << "open read file err.\n";
      goto err;
    }
    vec_alloc_info[i - 1].__block_num = count;
  }

  std::for_each(vec_filename.begin(), vec_filename.end(),
                [](const std::string &fname) { std::remove(fname.c_str()); });

  /* 检查是否符合预期 */
  if (!rsv_check_self(vec_alloc_info[0].__last_alloc_logical_block,
                  vec_alloc_info[0].__rsv_start,
                  vec_alloc_info[0].__block_num) ||
      !rsv_check_self(vec_alloc_info[1].__last_alloc_logical_block,
                  vec_alloc_info[1].__rsv_start,
                  vec_alloc_info[1].__block_num) ||
      vec_alloc_info[0].__rsv_end + 1 != vec_alloc_info[1].__rsv_start) {
    for (auto &info : vec_alloc_info) {
      std::cerr << info.__rsv_goal_size << ", " << info.__rsv_alloc_hit << ", "
                << info.__rsv_start << ", " << info.__rsv_end << ", "
                << info.__last_alloc_logical_block << "\n";
    }
    goto err;
  }
  std::cout << "create two file test sucessful\n";
  return 0;
err:
  std::cout << "create two file test failed\n";
  return -1;
}

int main(int argc, char *argv[]) {
  if (create_two_4blocks_file() == -1)
    return -1;
  return 0;
}
