#ifndef _INFORMATION_H
#define _INFORMATION_H

#include "../babyfs.h"

/**
 * proc_write 协议:
 * -r 表示后续的参数现暂存着
 * -a 获取 sb_info
 * -n [ino] 需要的 inode 编号
 * -i 获取 inode_info
 * -l 获取 alloc_info
 *
 * -w -n [ino] -d [simple data name] data_value
 * [simple data name]: -g, goal; ... ;
 *
 * proc_read 协议：
 * 返回数据格式：根据参数的顺序，写入相应的结构体
 */

bool rsv_check_self(unsigned long long __last_alloc_logical_block,
                unsigned long long __rsv_start,
                unsigned long long __block_num) {
  return __last_alloc_logical_block - __block_num + 1 == __rsv_start;
}

struct alloc_information {
  unsigned int __rsv_goal_size;
  unsigned int __rsv_alloc_hit;
  unsigned long long __rsv_start;
  unsigned long long __rsv_end;
  unsigned long long __last_alloc_logical_block;
  unsigned long long __block_num;
};

struct inode_information {
  unsigned int __i_blocks[BABYFS_N_BLOCKS];
  unsigned short __i_subdir_num;
};

struct sb_information {
  unsigned int __nr_free_blocks;
  unsigned int __nr_free_inodes;
  unsigned int __nr_blocks;
  unsigned int __last_bitmap_bits;
};

#endif