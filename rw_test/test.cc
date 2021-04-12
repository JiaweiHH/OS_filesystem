#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

// g++ -Wall -std=c++11 -pthread test.cc -o test
// ./test [directory name] [file nums] [final size]

std::vector<std::string> v_file;
bool is_quit = false;
constexpr uint32_t max_size = 100 * 1024 * 1024;
uint64_t final_size;
std::atomic<uint64_t> now_size;

// 随机数产生
std::random_device rd;
std::mt19937 gen(rd());
std::uniform_int_distribution<> distrib(1, max_size);

void work_remove() {
  uint32_t pos;

  while (1) {
    uint32_t random_variable = distrib(gen);

    if (random_variable % 3 == 0) {
      uint64_t size = 0;
      std::vector<std::string> rm_file{};
      std::copy(v_file.begin() + pos, v_file.end(),
                std::inserter(rm_file, rm_file.begin()));
      if(rm_file.size() > 0) {
        for (auto &file : rm_file) {
          std::remove(file.c_str());
          size += std::stoul(file.substr(file.find_last_of('_') + 1));
        }
        std::cout << "rm " << rm_file.size() << "\n";
        pos += rm_file.size();
      }
      if(size) {
        now_size -= size;
        // std::cout << now_size << '\n';
      }
      // v_file.clear();
    } else if(random_variable % 3 == 1) {
      pos = v_file.size();
    }

    std::this_thread::sleep_for(std::chrono::microseconds(5000));

    if (is_quit)
      break;
  }
}

void work_create(std::string dictory, uint32_t num) {
  uint32_t size;
  while (num-- && now_size < final_size) {
    size = distrib(gen);
    std::string filename = dictory + "/" + std::string("this_is_a_test_file_") +
                           std::to_string(size);
    std::ofstream of(filename);
    of.fill(' ');
    of.width(size);
    of << " ";
    v_file.push_back(filename);
    now_size += size;
    // std::cout << now_size << '\n';
    std::cout << "num: " << num << "\n";
  }
  is_quit = true;
}

void get_file_size(std::string s_size) {
  std::string measure = s_size.substr(s_size.length() - 2);
  uint16_t per_size = std::stoul(s_size.substr(0, s_size.length() - 2));
  if(measure == std::string("GB"))
    final_size = per_size * 1024 * 1024 * 1024;
  else if(measure == std::string("MB"))
    final_size = per_size * 1024 * 1024;
  else
    final_size = per_size * 1024;
}

int main(int argc, char *argv[]) {
  std::string dictory = argv[1];
  uint32_t num = std::atoi(argv[2]);
  get_file_size(argv[3]);
  std::cout << final_size << '\n';

  std::thread t_create(work_create, dictory, num);
  std::thread t_remove(work_remove);

  t_remove.join();
  t_create.join();
}
