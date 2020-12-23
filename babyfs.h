#include <linux/types.h>

/*
 * babyfs partition layout
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode bitmap |  block INODE_BIT_MAP_BLOCK_BASE
 * +---------------+
 * |  inode table  |  block INODE_TABLE_BLOCK_BASE
 * +---------------+
 * | block bitmap  |  block DATA_BIT_MAP_BLOCK_BASE
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 */

#define BLOCK_SIZE 1024     // 一个块的字节数
#define INODE_SIZE 128      // 一个 inode 结构体的大小
#define ROOT_INODE_NO 0     // 根目录的 inode 编号
#define SUPER_BLOCK 1       // 超级块的块号
#define INODE_BLOCKS_NUM 1024  // inode 占用块数
#define INODE_BIT_MAP_BLOCK_BASE            (SUPER_BLOCK + 1)           // inode 位图起始块号
#define INODE_NUM_PER_BLOCK                 (BLOCK_SIZE / INODE_SIZE)   // 每个块可以存放的 inode 数量
#define INODE_NUM_COUNTS                    (INODE_BLOCKS_NUM * INODE_NUM_PER_BLOCK)  // inode 的总数量
#define INODE_TABLE_BLOCK_BASE              (INODE_BIT_MAP_BLOCK_BASE + INODE_NUM_COUNTS  / (BLOCK_SIZE << 3))  // inode 表起始块号
#define DATA_BIT_MAP_BLOCK_BASE             (INODE_TABLE_BLOCK_BASE + INODE_BLOCKS_NUM)  // 数据位图起始块号

#define FILENAME_MAX_LEN 250  // 文件名最大长度，为了目录项对齐到 256B

/*
 * 数据块索引
 */
#define	DIRECT_BLOCK			12
#define	PRIMARY_BLOCK			(DIRECT_BLOCK + 1)
#define	SECONDRTY_BLOCK			(PRIMARY_BLOCK + 1)
#define	THIRD_BLOCKS			(SECONDRTY_BLOCK + 1)
#define N_BLOCKS                THIRD_BLOCKS

// 磁盘超级块
struct baby_super_block{
    __le16 magic;       /* 魔数 */
    __le32 nr_blocks;   /* blocks 总数 */
    __le32 nr_inodes;   /* inode 总数 */
    __le32 nr_istore_blocks;    /* inode 表起始块号 */
    __le32 nr_ifree_blocks;     /* inode 位图起始块号 */
    __le32 nr_bfree_blocks;     /* data block 位图起始块号 */
    __le32 nr_free_inodes;      /* 剩余空闲 inode 数量 */
    __le32 nr_free_blocks;      /* 剩余空闲 data block 数量 */
};

// 磁盘索引节点
struct baby_inode {
    __le16 i_mode;      /* 文件类型和访问权限 */
    __le16 i_uid;       /* inode 所属用户编号 */
    __le16 i_gid;       /* inode 所属用户组编号 */
    __le32 i_size;      /* inode 对应文件大小 */
    __le32 i_ctime;     /* i_ctime */
    __le32 i_atime;     /* i_atime */
    __le32 i_mtime;     /* i_mtime */
    __le32 i_blocks;    /* 文件块数 */
    __le16 i_nlink;     /* 硬链接计数 */
    __le32 i_blocks[N_BLOCKS];     /* 索引数组 */
    __u8 _padding[(INODE_SIZE - (2*3 + 4*6 +2))];   /* inode 结构体扩展到 128B */
};

/*
 * 目录项 1 个 block 可以存放 4个
 * 250 + 4 + 2 = 256B
 */
struct dir_record {
    __le32 inode_no;
    char[FILENAME_MAX_LEN] name;
    __u8 name_len;
    __u8 file_type;
};