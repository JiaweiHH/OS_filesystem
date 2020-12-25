#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "babyfs.h"

static void test_block_write_and_read() {
  int fd = open("./a", O_RDWR);
  // char *block = malloc(1024);
  // u_int64_t *ifree = (u_int64_t *)block;
  // memset(ifree, 0xff, 1024);
  // ifree[0] = 0xfffffffffffffffe;
  // int ret = write(fd, ifree, 1024);
  // if(ret != 1024)
  //   printf("error\n");
  char *block;
  read(fd, block, 1024);
  u_int64_t *ifree = (u_int64_t *)block;
  printf("0x%llx\n", *ifree);
  ifree++;
  printf("0x%llx\n", *ifree);
  // printf("0x%llx\n", ifree[0]);
  // printf("0x%llx\n", ifree[1]);
  // printf("0x%llx\n", ifree[2]);
}

static void test_mkfs() {
  int fd = open("./test.img", O_RDWR);
  // lseek(fd, 1032 * BABYFS_BLOCK_SIZE, SEEK_SET);
  // char *block = malloc(BABYFS_BLOCK_SIZE);
  // read(fd, block, BABYFS_BLOCK_SIZE);
  // struct dir_record *record = (struct dir_record *)block;
  // printf("name: %s\n", record->name);
  // record++;
  // printf("name: %s\n", record->name);
  char *buf = malloc(BABYFS_BLOCK_SIZE);
  lseek(fd, 2 * BABYFS_BLOCK_SIZE, SEEK_SET);
  read(fd, buf, BABYFS_BLOCK_SIZE);
  // u_int64_t *data_bitmap = (u_int64_t *)buf;
  struct baby_inode *inode = (struct baby_inode *)buf;
  printf("i_blocknum: %d\n", inode->i_blocknum);
  printf("i_blocks: %d\n", *(inode->i_blocks));
}

static void printf_message() {
  printf("inode 位图起始块号: %d\ninode 表起始块号: %d\n数据位图起始块号: %d\n",
         BABYFS_INODE_BIT_MAP_BLOCK_BASE, BABYFS_INODE_TABLE_BLOCK_BASE,
         BABYFS_DATA_BIT_MAP_BLOCK_BASE);
}

int main() {
  printf_message();
  test_mkfs();
  return 0;
}