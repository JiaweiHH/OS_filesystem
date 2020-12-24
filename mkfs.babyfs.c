#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "babyfs.h"
// #include <unistd.h>

static int fd;
static int nr_dstore_blocks;  // 保存数据块起始块号
// static int count = 0;

static void write_superblock(u_int64_t file_size) {
  u_int32_t total_blocks = file_size / BABYFS_BLOCK_SIZE;

  // 保证每次偏移量移动一个 block_size
  char *block = malloc(BABYFS_BLOCK_SIZE);
  memset(block, 0, BABYFS_BLOCK_SIZE);
  struct baby_super_block *super_block = (struct baby_super_block *)block;

  // 填充数据
  super_block->magic = 0x1234;                     // 魔数
  super_block->nr_inodes = BABYFS_INODE_BLOCKS_NUM;  // inode 块数
  super_block->nr_istore_blocks =
      BABYFS_INODE_TABLE_BLOCK_BASE;  // inode 表起始块号
  super_block->nr_ifree_blocks =
      BABYFS_INODE_BIT_MAP_BLOCK_BASE;  // inode 位图起始块号
  super_block->nr_bfree_blocks =
      BABYFS_DATA_BIT_MAP_BLOCK_BASE;  // 数据块位图起始块号
  super_block->nr_dstore_blocks =
      (total_blocks - BABYFS_DATA_BIT_MAP_BLOCK_BASE) /
          (BABYFS_BLOCK_SIZE << 3) +
      BABYFS_DATA_BIT_MAP_BLOCK_BASE;  // 数据块起始块号。简单起见，但是这样计算有一点误差
  nr_dstore_blocks = super_block->nr_dstore_blocks;
  // printf("nr_dstore_blocks: %d\n", nr_dstore_blocks);  //输出看一下
  super_block->nr_blocks =
      total_blocks - super_block->nr_dstore_blocks;       // 数据块总块数
  super_block->nr_free_inodes = BABYFS_INODE_NUM_COUNTS;  // inode 剩余空闲数量
  super_block->nr_free_blocks =
      super_block->nr_blocks;  // data block 剩余空闲数量

  int ret = write(fd, block, BABYFS_BLOCK_SIZE);
  if (ret != BABYFS_BLOCK_SIZE) {
    perror("超级块写入出错!\n");
    return;
  }

  free(block);
  printf("super block 格式化完成!\n");
  // count++;
}

static void write_inode_table() {
  // 保证偏移量移动
  char *block = malloc(BABYFS_BLOCK_SIZE);
  memset(block, 0, BABYFS_BLOCK_SIZE);
  struct baby_inode *root_inode = (struct baby_inode *)block;

  root_inode->i_size = BABYFS_BLOCK_SIZE;  // 根目录是一个目录文件
  // root_inode->i_ctime = root_inode->i_atime = root_inode->i_mtime =
  // CURRENT_TIME; 放到 fill_super 里面做
  root_inode->i_blocknum = 1;  // inode 对应文件占用的块数
  root_inode->i_nlink = 1;     // 硬链接计数
  root_inode->i_mode = 0755 | S_IFDIR;
  // 写第一块 inode_table，里面包含了第一个 inode 和其他空的 inode
  int ret = write(fd, block, BABYFS_BLOCK_SIZE);
  // count++;
  if (ret != BABYFS_BLOCK_SIZE) {
    perror("inode_table: 0, 写出错!\n");
    return;
  }

  // 写剩余的空的 inode_table block
  memset(block, 0, BABYFS_BLOCK_SIZE);  // 清空前面写 root_inode 的数据
  for (int i = 1; i < BABYFS_INODE_BLOCKS_NUM; ++i) {
    // count++;
    if (ret = write(fd, block, BABYFS_BLOCK_SIZE) != BABYFS_BLOCK_SIZE) {
      fprintf(stderr, "inode_table: %d, 写出错!\n", i);
      return;
    }
  }

  free(block);
  printf("inode table 格式化完成!\n");
}

static void write_inode_bitmap() {
  // 分配一个块大小的内存，往这里面写东西再写到磁盘上
  char *block = malloc(BABYFS_BLOCK_SIZE);
  u_int64_t *inode_bitmap = (u_int64_t *)block;
  // 设置所有的 bit 为 1
  memset(inode_bitmap, 0xff, BABYFS_BLOCK_SIZE);
  // 设置第一个 inode（根目录） 为 0，表示已被占用
  *inode_bitmap = 0xfffffffffffffffe;
  int ret = write(fd, inode_bitmap, BABYFS_BLOCK_SIZE);
  // count++;
  if (ret != BABYFS_BLOCK_SIZE) {
    perror("inode_bitmap: 0 写出错\n");
    return;
  }

  // 如果还有剩下的 inode bitmap 块，继续写
  *inode_bitmap = 0xffffffffffffffff;
  for (int i = 1;
       i < BABYFS_INODE_TABLE_BLOCK_BASE - BABYFS_INODE_BIT_MAP_BLOCK_BASE;
       ++i) {
    // count++;
    ret = write(fd, inode_bitmap,
                BABYFS_BLOCK_SIZE);  // 文件指针每次写完会自动往下一个块大小
    if (ret != BABYFS_BLOCK_SIZE) {
      fprintf(stderr, "inode_bitmap: %d 写出错\n", i);
      return;
    }
  }

  free(block);
  printf("inode bitmap 格式化完成!\n");
}

static void write_datablock_bitmap() {
  // 写第一块 bitmap
  char *block = malloc(BABYFS_BLOCK_SIZE);
  u_int64_t *data_bitmap = (u_int64_t *)block;
  memset(data_bitmap, 0xff, BABYFS_BLOCK_SIZE);
  // 标记 root_inode 使用的第一块数据块
  *data_bitmap = 0xfffffffffffffffe;
  int ret = write(fd, block, BABYFS_BLOCK_SIZE);
  // count++;
  // printf("write_datablock_bitmap: %d\n", count);
  if (ret != BABYFS_BLOCK_SIZE) {
    perror("data_block_bitmap: 0, 写出错!\n");
    return;
  }

  // 写剩余的 bitmap
  *data_bitmap = 0xffffffffffffffff;
  for (int i = 1; i < nr_dstore_blocks - BABYFS_DATA_BIT_MAP_BLOCK_BASE; ++i) {
    // count++;
    if (ret = write(fd, block, BABYFS_BLOCK_SIZE) != BABYFS_BLOCK_SIZE) {
      fprintf(stderr, "data_block_bitmap: %d, 写出错!\n", i);
      return;
    }
  }

  free(block);
  printf("data block bitmap 格式化完成!\n");
}

static void write_first_datablock() {
  // 分配一个 block_size 大小的内存
  char *block = malloc(BABYFS_BLOCK_SIZE);
  struct dir_record *d_record = (struct dir_record *)block;
  memset(d_record->name, 0, sizeof(d_record->name));  // 清空 name 字段
  // 添加 “.” 目录项
  memcpy(d_record->name, ".", 1);
  d_record->inode_no =
      BABYFS_ROOT_INODE_NO;  // inode 编号为 0，这样可以通过 ino +
                             // BABYFS_INODE_TABLE_BLOCK_BASE 找到 inode block
  d_record->name_len = 1;
  d_record->file_type = BABYFS_FILE_TYPE_DIR;

  // 添加 “..” 目录项
  d_record++;
  memset(d_record->name, 0, sizeof(d_record->name));	//清空 name 字段
  memcpy(d_record->name, "..", 2);
  d_record->inode_no =
      BABYFS_ROOT_INODE_NO;  // inode 编号为 0，这样可以通过 ino +
                             // BABYFS_INODE_TABLE_BLOCK_BASE 找到 inode block
  d_record->name_len = 2;
  d_record->file_type = BABYFS_FILE_TYPE_DIR;

  // 写入目录项
  int ret = write(fd, block, BABYFS_BLOCK_SIZE);
  if (ret != BABYFS_BLOCK_SIZE) {
    perror(". 目录项写入出错!\n");
    return;
  }

  // 更新 root_inode->i_blocks
  lseek(fd, BABYFS_INODE_BIT_MAP_BLOCK_BASE * BABYFS_BLOCK_SIZE,
        SEEK_SET);  // 从头开始移动偏移量
  char *inode_block = malloc(BABYFS_BLOCK_SIZE);
  if (ret = read(fd, inode_block, BABYFS_BLOCK_SIZE) != BABYFS_BLOCK_SIZE) {
    perror("读取 inode 数据出错!\n");
    return;
  }
  struct baby_inode *inode = (struct baby_inode *)inode_block;
  inode->i_blocks[0] = nr_dstore_blocks;

  free(inode_block);
  free(block);
  printf("根目录目录项写入成功!\n");
}

int main(int argc, char **argv) {
  if (argc != 2) {
    perror("没有设备文件\n");
    return EXIT_FAILURE;
  }

  // 打开设备文件
  fd = open(argv[1], O_RDWR);
  if (fd == -1) {
    perror("打开文件出错\n");
    return EXIT_FAILURE;
  }

  // 获取文件大小
  struct stat statbuf;
  stat(argv[1], &statbuf);
  u_int64_t file_size = statbuf.st_size;

  // 按顺序写各个分区部分
  write_superblock(file_size);  // 超级块部分
  write_inode_bitmap();         // inode 位图
  write_inode_table();          // indoe 表
  write_datablock_bitmap();     // 数据位图
  write_first_datablock();      // 主要是写根目录的目录项

  printf("格式化完成!\n");
  // printf("已初始化的块数: %d\n", count);

  close(fd);
  return EXIT_SUCCESS;
}