#include <stdio.h>
#include "../../babyfs.h"
#include "../niceprint.h"

int main() {
  // 磁盘 inode 结构体的大小固定
  if (sizeof(struct baby_inode) != BABYFS_INODE_SIZE) {
    bberr("struct baby_inode size( %ld ) is wrong!\n", sizeof(struct baby_inode));
    goto failed;
  }
  
  // 目录项结构体的大小固定
  if (sizeof(struct dir_record) != BABYFS_DIR_RECORD_SIZE) {
    bberr("struct dir_record size( %ld ) is wrong!\n", sizeof(struct dir_record));
    goto failed;
  }

  return 0;

failed:
  return -1;
}