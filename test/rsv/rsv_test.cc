#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../babyfs.h"
#include "../debug.h"
#include "../niceprint.h"
#include "../test_information.h"

/**
 * 测试了创建两个文件的时候文件块的关系 以及 rsv 的基本信息
 */

baby_test::log_out log;
const std::string dir("../../imgdir/");

int32_t proc_write(const uint32_t &ino, const std::string &arg) {
  std::string file_name = "/proc/babyfs/" + std::to_string(ino) + ".txt";
  std::ofstream ostrm(file_name);
  if (!ostrm.is_open())
    return -1;
  ostrm << arg;
  ostrm.close();
  return 0;
}

int32_t proc_read(const uint32_t &ino, struct alloc_information &info) {
  std::string filename = "/proc/babyfs/" + std::to_string(ino) + ".txt";
  std::ifstream istrm(filename);
  if (!istrm.is_open())
    return -1;
  istrm.read(reinterpret_cast<char *>(&info), sizeof info);
  istrm.close();
  return 0;
}

int32_t set_and_get_goal(const uint32_t &ino, struct alloc_information &info) {
  log << "set_and_get_goal\n";
  /* 写入读取 alloc_info 参数*/
  std::string argv = "-r -n " + std::to_string(ino) + " -l";
  int8_t ret = proc_write(ino, argv);
  if (ret < 0) {
    std::cout << ERR << "write argv [" << argv << "] err.\n";
    log << "write argv [" << argv << "] err.\n";
    return -1;
  }
  log << "write argv [" << argv << "] successful~~~\n";

  /* 读取 alloc_info */
  ret = proc_read(ino, info);
  if (ret < 0) {
    std::cerr << ERR << "read alloc_info failed\n";
    log << ERR << "read alloc_info failed\n";
    return -1;
  }
  log << "read alloc_info successful~~~\n";
  return 0;
}

// 测试预留窗口的基本功能
int32_t create_two_4blocks_file() {
  // std::cout << "test create two file to see the block is right\n";
  log << "test create two file to see the block is right\n";
  std::vector<struct alloc_information> vec_alloc_info = {alloc_information{},
                                                          alloc_information{}};
  std::vector<std::string> vec_filename;

  /* 创建两个文件，大小都是 4 个 block */
  constexpr uint32_t count = 4;
  constexpr uint32_t sz = count * BABYFS_BLOCK_SIZE;
  for (int i = 1; i <= 2; ++i) {
    std::string filename = dir + "test_file_" + std::to_string(i);
    vec_filename.push_back(filename);
    std::ofstream ostrm(filename);
    ostrm.fill(' ');
    ostrm.width(sz);
    ostrm << " ";
    ostrm.close();
    log << "create " << filename << "\n";
  }
  // log << "========================\n";

  /* 获取每个文件的 first_block 和 alloc_info 信息 */
  log << "attemp to get alloc_info\n";
  for (int i = 1; i <= 2; ++i) {
    log << "file" << i << ":\n";
    // 先写入参数
    std::string argv = "-r -n " + std::to_string(i) + " -l";
    int32_t ret = proc_write(i, argv);
    log << "write argv to procfs: " << argv << "\n";
    if (ret) {
      std::cerr << ERR << "write argv [ " << argv << " ] err.\n";
      log << ERR << "write argv [ " << argv << " ] err.\n";
      goto err;
    }
    // 获取数据
    ret = proc_read(i, vec_alloc_info[i - 1]);
    if (ret) {
      std::cerr << ERR << "open read file err.\n";
      log << ERR << "open read file err.\n";
      goto err;
    }
    log << "read file " << i << " alloc_info successful~~~\n\n";
    vec_alloc_info[i - 1].__block_num = count;
  }
  // log << "========================\n";

  log << "remove all files\n";
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
      std::cerr << ERR << info.__rsv_goal_size << ", " << info.__rsv_alloc_hit
                << ", " << info.__rsv_start << ", " << info.__rsv_end << ", "
                << info.__last_alloc_logical_block << "\n";
    }
    goto err;
  }
  std::cout << SUCC << "create two file test sucessful~~~\n";
  log << "create_two_4blocks_file() test sucessful~~~\n";
  log << "========================\n";
  return 0;
err:
  std::cerr << ERR << "create_two_4blocks_file() test failed\n";
  log << "create_two_4blocks_file() test failed\n";
  log << "========================\n";
  return -1;
}

/**
 * 测试循环查找预留窗口
 * 创建文件并设置 goal 为文件系统的最后一块，然后继续追加大小
 */
int32_t loop_find_rsv(const uint32_t &last_block) {
  log << "test loop_find_rsv()\n";

  /* 创建一个文件 */
  const std::string file_name = dir + "loop_test_file";
  std::ofstream ostrm;
  constexpr int ino = 1;
  ostrm.open(file_name);
  log << "create file successful~~~\n";

  /* 设置 goal 为最后一块数据块 */
  std::string argv =
      "-w -n " + std::to_string(ino) + " -g " + std::to_string(last_block);
  int32_t ret = proc_write(ino, argv);
  if (ret < 0) {
    std::cout << ERR << "write argv [" << argv << "] err.\n";
    log << "write argv [" << argv << "] err.\n";
    std::remove(file_name.c_str());
    return -1;
  }
  log << "write argv [" << argv << "] successful~~~\n";

  /* 先获取 goal 看看是不是正确设置了 */
  struct alloc_information info;
  ret = set_and_get_goal(ino, info);
  if (ret < 0) {
    std::cerr << ERR << "set_and_get_goal err.\n";
    log << "set_and_get_goal err.\n";
    std::remove(file_name.c_str());
    return -1;
  }
  log << "goal " << info.__last_alloc_logical_block << "\n";

  /* 追加数据 */
  const uint8_t num = 5;
  ostrm.fill(' ');
  ostrm.width(BABYFS_BLOCK_SIZE * num); // 追加 5 块数据块
  ostrm << " ";
  ostrm.close();
  log << "append " << num << " blocks\n";

  /* 写入读取 alloc_info 参数*/
  argv = "-r -n " + std::to_string(ino) + " -l";
  ret = proc_write(ino, argv);
  if (ret < 0) {
    std::cerr << ERR << "write argv [" << argv << "] err.\n";
    log << "write argv [" << argv << "] err.\n";
    std::remove(file_name.c_str());
    return -1;
  }
  log << "write argv [" << argv << "] successful~~~\n";

  /* 读取 alloc_info */
  ret = proc_read(ino, info);
  if (ret < 0) {
    std::cerr << ERR << "read alloc_info failed\n";
    log << ERR << "read alloc_info failed\n";
    std::remove(file_name.c_str());
    return -1;
  }
  log << "read alloc_info successful~~~\n";
  log << info.__rsv_goal_size << ", " << info.__rsv_alloc_hit << ", "
      << info.__rsv_start << ", " << info.__rsv_end << ", "
      << info.__last_alloc_logical_block << "\n";
  if (info.__last_alloc_logical_block != num) {
    std::cerr << ERR << "loop_find_rsv() err.\n";
    log << "loop_find_rsv() err.\n";
    std::remove(file_name.c_str());
    return -1;
  }
  std::cout << SUCC << "loop_find_rsv() successful~~~\n";
  log << "loop_find_rsv() successful~~~\n";
  std::remove(file_name.c_str());
  return 0;
}

int main(int argc, char *argv[]) {
  if(argc < 1) {
    std::cerr << ERR << "please give correct argv\n";
    return -1;
  }
  if (create_two_4blocks_file() == -1)
    return -1;

  const uint32_t last_block = std::stoul(argv[1]);
  if (loop_find_rsv(last_block) == -1)
    return -1;
  return 0;
}
