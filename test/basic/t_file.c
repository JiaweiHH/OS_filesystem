#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
// 包含errno所需要的头文件
#include <errno.h>
// 包含strerror所需要的头文件
#include <string.h>

#include "../../babyfs.h"

#define data_bitmap_block_start 1026
#define data_block_start 1033
#define dirname "./imgdir"

int test_file_create(int testimg);

int main() {
  printf("testcase file\n");

  int testimg; // 二进制文件

  // 以二进制可读形式打开文件系统挂载的文件
  if ((testimg = open("./fs.img", O_RDONLY)) == -1) {
    printf("open testimg failed!"
           "errMessage : %s\n",
           strerror(errno));
    goto failed;
  }

  if (test_file_create(testimg) == -1) {
    goto close_failed;
  }

  close(testimg);
  return 0;

close_failed:
  close(testimg);

failed:
  return -1;
}

/**
 * 新建文件
 * 1. 新建一个文件
 * 2. 新建32个文件
 * 3. 删除第10个，再新建一个文件
 */
int test_file_create(int testimg) {
  volatile int tmpfile;
  char filename[128] = {0};

  /**
   * 1. 新建一个文件
   */ 
  if ((tmpfile = open("./imgdir/f0", O_RDWR | O_CREAT | O_SYNC,
                      S_IRWXU | S_IRWXG | S_IRWXO)) == -1) {
    printf("create tmpfile(./imgdir/f0) failed!"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  }
  close(tmpfile);

  // 读取inode bitmap，查看bitmap的设置是否正确
  char first = 0;
  lseek(testimg, BABYFS_INODE_BIT_MAP_BLOCK_BASE * BABYFS_BLOCK_SIZE, SEEK_SET);
  if (read(testimg, &first, sizeof(char)) != sizeof(char)) {
    printf("read testimg inode bitmap failed!"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  } else {
    // 新建1个文件，包括根inode在内，占用inode bitmap开头的2位，故inode bitmap的第一个字节应该是0x03
    if (first != 0x3) {
      printf("create failed!\n");
      return -1;
    }
  }
  printf("create one file success\n");


  /**
   * 2. 新建32个文件
   */
  char strbuff[10] = {0};
  for (int i = 1; i < 31; i++) { // 1 + 1 + 30 = 32 = 4*8
    sprintf(strbuff, "%d", i); // 将整数i转为字符串，linux下没有itoa
    sprintf(filename, "%s/f%s", dirname, strbuff); // 拼接文件名
    if ((tmpfile = open(filename, O_RDWR | O_CREAT | O_SYNC,
                        S_IRWXU | S_IRWXG | S_IRWXO)) == -1) {
      printf("create tmpfile(%s) failed!"
             "errMessage : %s\n", filename,
             strerror(errno));
      return -1;
    }

    close(tmpfile);
  }

  // 读取inode bitmap，查看bitmap的设置是否正确
  char bitmap_first40[5] = {0};
  lseek(testimg, BABYFS_INODE_BIT_MAP_BLOCK_BASE * BABYFS_BLOCK_SIZE, SEEK_SET);
  if (read(testimg, bitmap_first40, 5) != 5) {
    printf("read testimg inode bitmap failed!"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  } else {
    // 共占用inode bitmap开头的32位，故inode bitmap的前5个字节应该是 0xff ff ff ff 00
    // 0xffffffff 转换为int类型为 -1
    if ((*(int *)bitmap_first40 ^ 0xffffffff) != 0) {
      printf("create 32 file failed, set byte %#x \n", *(int *)bitmap_first40);
      return -1;
    }
    if (*(bitmap_first40 + 4) != 0) {
      printf("create 32 file failed, other byte\n");
      return -1;
    }
  }
  printf("create 32 file success\n");

  
  /**
   * 3. 删除第3个，再新建一个文件
   */ 
  if (remove("./imgdir/f3") != 0) {
    printf("remove file f3 failed!\n"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  }
  lseek(testimg, BABYFS_INODE_BIT_MAP_BLOCK_BASE * BABYFS_BLOCK_SIZE, SEEK_SET);
  if (read(testimg, bitmap_first40, 5) != 5) {
    printf("read testimg inode bitmap failed!"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  } else {
    // 删除第3个 ff ff ff ff => ff ff ff ef
    if ((*(int *)bitmap_first40 ^ 0xffffffef) != 0) {
      printf("remove file f3 failed!, %#x\n", *(int *)bitmap_first40);
      return -1;
    }
  }

  if (remove("./imgdir/f7") != 0) {
    printf("remove file f7 failed!\n"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  }
  lseek(testimg, BABYFS_INODE_BIT_MAP_BLOCK_BASE * BABYFS_BLOCK_SIZE, SEEK_SET);
  if (read(testimg, bitmap_first40, 5) != 5) {
    printf("read testimg inode bitmap failed!"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  } else {
    // 删除第7个 ff ff ff ef => ff ff fe ef 
    if ((*(int *)bitmap_first40 ^ 0xfffffeef ) != 0) {
      printf("remove file f7 failed!, %#x\n", *(int *)bitmap_first40);
      return -1;
    }
  }

  // 删除之后再创建
  if ((tmpfile = open("./imgdir/f31", O_RDWR | O_CREAT | O_SYNC,
                      S_IRWXU | S_IRWXG | S_IRWXO)) == -1) {
    printf("remove, create tmpfile(./imgdir/f31) failed!"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  }
  close(tmpfile);

  // 读取inode bitmap，查看bitmap的设置是否正确
  lseek(testimg, BABYFS_INODE_BIT_MAP_BLOCK_BASE * BABYFS_BLOCK_SIZE, SEEK_SET);
  if (read(testimg, &bitmap_first40, 5) != 5) {
    printf("read testimg inode bitmap failed!"
           "errMessage : %s\n",
           strerror(errno));
    return -1;
  } else {
    // 新建之后，ff ff fe ef => ff ff fe ff
    if ((*(int *)bitmap_first40 ^ 0xfffffeff) != 0) {
      printf("remove, add f31 failed!, %#x\n", *(int *)bitmap_first40);
      return -1;
    }
  }
  
  printf("create file(f31) after remove(f3,f7) success\n");
}
