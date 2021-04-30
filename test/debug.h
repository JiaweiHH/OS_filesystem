#ifndef _OUTPUT_REPLACE_H
#define _OUTPUT_REPLACE_H

#ifdef __cplusplus
#include <fstream>
#include <iostream>
#include <fstream>
#include <string>
#else
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#endif  // __cplusplus

#ifdef __cplusplus

namespace baby_test {

class log_out {
public:
  log_out() : __ostrm("test.log", std::ios::app) {}
  log_out(const std::string &log_file) : __log_file(log_file), __ostrm(log_file, std::ios::app) {}
  template <typename type> log_out& operator << (const type &t) {
    __ostrm << t;
    return *this;
  }

  void set_log_name(const std::string &name) {
    __log_file = name;
    if(__ostrm.is_open())
      __ostrm.close();
    __ostrm.open(__log_file, std::ios::app);
  }

private:
  std::ofstream __ostrm;
  std::string __log_file;
};

} // namespace baby_test

#else
FILE *fp;
char __log_name[20] = {0};
void set_log_name(char *name) {
  memset(__log_name, 0, sizeof(log_name));
  strcpy(__log_name, name);
  fp = fopen(__log_name, "aw+");
}
void c_log(const char *fmt, ...) {
  
  va_list args;

  /* 使用 fmt 之后的参数初始化 args */
  va_start(args, fmt);
  /* 打印数据 */
  vfprintf(fp, fmt, args);
  /* 清除 args */
  va_end(args);
}

#endif  //__cplusplus

#endif  //_OUTPUT_REPLACE_H