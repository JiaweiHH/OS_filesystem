#include <stdio.h>

// 通过printf设置Linux终端输出的颜色和显示
// https://www.cnblogs.com/clover-toeic/p/4031618.html
#define NONE    "\e[0m"
#define GRAY    "\e[1;37m"
#define RED     "\e[1;31m"
#define YELLOW  "\e[1;33m"
#define GREEN   "\e[1;32m"

// 在基础的输出信息前输出代码函数名和调用的代码行数，并做颜色高亮
#define __bbprintf(printfmt, fmt, args...) printf(printfmt "%s:%d => " fmt NONE, __func__, __LINE__, ##args)
#define __bbprintf_s(printfmt, fmt, args...) printf(printfmt fmt NONE, ##args)

#define bbprintf(fmt, args...) __bbprintf(NONE, fmt, ##args)
#define bbinfo(fmt, args...) __bbprintf_s(GRAY, fmt, ##args)
#define bbwarning(fmt, args...) __bbprintf(YELLOW "\n", fmt "\n", ##args)
#define bberr(fmt, args...) __bbprintf(RED "\n", fmt "\n", ##args)
#define bbsucc(fmt, args...) __bbprintf_s(GREEN, fmt, ##args)
