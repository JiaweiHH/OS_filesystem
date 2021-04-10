#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

// g++ -Wall -std=c++11 -pthread test.cc -o test
// ./test [directory name] [file nums]

std::vector<std::string> v_file;
bool is_quit = false;
constexpr uint32_t max_size = 100 * 1024 * 1024;

// 随机数产生
std::random_device rd;
std::mt19937 gen(rd());
std::uniform_int_distribution<> distrib(1, max_size);

void work_remove() {
  uint32_t pos;

  while (1) {
    uint32_t random_variable = distrib(gen);

    if (random_variable % 3 == 0) {
      std::vector<std::string> rm_file{};
      std::copy(v_file.begin() + pos, v_file.end(),
                std::inserter(rm_file, rm_file.begin()));
      if(rm_file.size() > 0) {
        for (auto &file : rm_file)
          std::remove(file.c_str());
        std::cout << "rm " << rm_file.size() << "\n";
        pos += rm_file.size();
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
  while (num--) {
    size = distrib(gen);
    std::string filename = dictory + "/" + std::string("this_is_a_test_file_") +
                           std::to_string(size);
    std::ofstream of(filename);
    of.fill(' ');
    of.width(size);
    of << " ";
    v_file.push_back(filename);
    std::cout << "num: " << num << "\n";
  }
  is_quit = true;
}

int main(int argc, char *argv[]) {
  std::string dictory = argv[1];
  uint32_t num = std::atoi(argv[2]);

  std::thread t_create(work_create, dictory, num);
  std::thread t_remove(work_remove);

  t_remove.join();
  t_create.join();
}
