#include <linux/types.h>

/*
 * babyfs partition layout
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode bitmap |  block BABYFS_INODE_BIT_MAP_BLOCK_BASE
 * +---------------+
 * |  inode table  |  block BABYFS_INODE_TABLE_BLOCK_BASE
 * +---------------+
 * | block bitmap  |  block BABYFS_DATA_BIT_MAP_BLOCK_BASE
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 */

#define BABYFS_BLOCK_SIZE 1024        // 一个块的字节数
#define BABYFS_INODE_SIZE 128         // 一个 inode 结构体的大小
#define BABYFS_ROOT_INODE_NO 0        // 根目录的 inode 编号
#define BABYFS_SUPER_BLOCK 1          // 超级块的块号
#define BABYFS_INODE_BLOCKS_NUM 1024  // inode 占用块数
#define BABYFS_INODE_BIT_MAP_BLOCK_BASE \
  (BABYFS_SUPER_BLOCK + 1)  // inode 位图起始块号
#define BABYFS_INODE_NUM_PER_BLOCK \
  (BABYFS_BLOCK_SIZE / BABYFS_INODE_SIZE)  // 每个块可以存放的 inode 数量
#define BABYFS_INODE_NUM_COUNTS \
  (BABYFS_INODE_BLOCKS_NUM * BABYFS_INODE_NUM_PER_BLOCK)  // inode 的总数量
#define BABYFS_BIT_PRE_BLOCK \
  (BABYFS_BLOCK_SIZE << 3)  // 每个 block 可以存放的位数
#define BABYFS_INODE_TABLE_BLOCK_BASE \
  (BABYFS_INODE_BIT_MAP_BLOCK_BASE +  \
   BABYFS_INODE_NUM_COUNTS / BABYFS_BIT_PRE_BLOCK)  // inode 表起始块号
#define BABYFS_DATA_BIT_MAP_BLOCK_BASE \
  (BABYFS_INODE_TABLE_BLOCK_BASE + BABYFS_INODE_BLOCKS_NUM)  // 数据位图起始块号

#define BABYFS_FILENAME_MAX_LEN 250  // 文件名最大长度，为了目录项对齐到 256B

#define BABYFS_CURRENT_TIME (current_kernel_time())  // 当前系统时间
#define BABYFS_FILE_TYPE_DIR 1
#define BABYFS_FILE_TYPE_FILE 2

/*
 * 数据块索引
 */
#define BABYFS_DIRECT_BLOCK 12
#define BABYFS_PRIMARY_BLOCK (BABYFS_DIRECT_BLOCK + 1)
#define BABYFS_SECONDRTY_BLOCK (BABYFS_PRIMARY_BLOCK + 1)
#define BABYFS_THIRD_BLOCKS (BABYFS_SECONDRTY_BLOCK + 1)
#define BABYFS_N_BLOCKS BABYFS_THIRD_BLOCKS

// 磁盘超级块
struct baby_super_block {
  __le16 magic;            /* 魔数 */
  __le32 nr_blocks;        /* blocks 总数 */
  __le32 nr_inodes;        /* inode 总数 */
  __le32 nr_istore_blocks; /* inode 表起始块号 */
  __le16 nr_dstore_blocks; /* 数据块起始块号 */
  __le32 nr_ifree_blocks;  /* inode 位图起始块号 */
  __le32 nr_bfree_blocks;  /* data block 位图起始块号 */
  __le32 nr_free_inodes;   /* 剩余空闲 inode 数量 */
  __le32 nr_free_blocks;   /* 剩余空闲 data block 数量 */
};

// 磁盘索引节点
struct baby_inode {
  __le16 i_mode;                    /* 文件类型和访问权限 */
  __le16 i_uid;                     /* inode 所属用户编号 */
  __le16 i_gid;                     /* inode 所属用户组编号 */
  __le32 i_size;                    /* inode 对应文件大小 */
  __le32 i_ctime;                   /* i_ctime */
  __le32 i_atime;                   /* i_atime */
  __le32 i_mtime;                   /* i_mtime */
  __le32 i_blocknum;                /* 文件块数 */
  __le16 i_nlink;                   /* 硬链接计数 */
  __le32 i_blocks[BABYFS_N_BLOCKS]; /* 索引数组 */
  __u8 _padding[(BABYFS_INODE_SIZE -
                 (2 * 3 + 4 * 6 + 2))]; /* inode 结构体扩展到 128B */
};

/*
 * 目录项 1 个 block 可以存放 4个
 * 250 + 4 + 2 = 256B
 */
struct dir_record {
  __le32 inode_no;
  char name[BABYFS_FILENAME_MAX_LEN];
  __u8 name_len;
  __u8 file_type;
};