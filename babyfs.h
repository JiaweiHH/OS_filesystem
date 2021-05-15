#ifndef __BABYFS_H__
#define __BABYFS_H__

/* 代码区 */
#include <linux/types.h>

#ifdef __KERNEL__
#include <linux/writeback.h>
#endif

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
#define BABYFS_SUPER_BLOCK 0          // 超级块的块号
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
   (BABYFS_INODE_NUM_COUNTS + BABYFS_BIT_PRE_BLOCK - 1) / BABYFS_BIT_PRE_BLOCK)  // inode 表起始块号
#define BABYFS_DATA_BIT_MAP_BLOCK_BASE \
  (BABYFS_INODE_TABLE_BLOCK_BASE + BABYFS_INODE_BLOCKS_NUM)  // 数据位图起始块号

#define BABYFS_FILENAME_MAX_LEN 250  // 文件名最大长度，为了目录项对齐到 256B
#define BABYFS_DIR_RECORD_SIZE 256  // 目录项大小

#define BABYFS_CURRENT_TIME (current_kernel_time())  // 当前系统时间
#define BABYFS_FILE_TYPE_DIR 1
#define BABYFS_FILE_TYPE_FILE 2

extern unsigned long NR_DSTORE_BLOCKS;  // 保存数据块起始块号

/*
 * 数据块索引
 */
#define BABYFS_DIRECT_BLOCK 12
#define BABYFS_PRIMARY_BLOCK BABYFS_DIRECT_BLOCK
#define BABYFS_SECONDRTY_BLOCK (BABYFS_PRIMARY_BLOCK + 1)
#define BABYFS_THIRD_BLOCKS (BABYFS_SECONDRTY_BLOCK + 1)
#define BABYFS_N_BLOCKS (BABYFS_THIRD_BLOCKS + 1)
#define BABYFS_PER_INDEX_SIZE 4  // 每个索引数据的大小
#define BABYFS_PER_BLOCK_INDEX_NUM \
  BABYFS_BLOCK_SIZE / BABYFS_PER_INDEX_SIZE  // 每个数据块可以存放的索引数据数量

// 磁盘超级块
struct baby_super_block {
  __le16 magic;            /* 魔数 */
  __le32 nr_blocks;        /* blocks 总数 */
  __le32 nr_inodes;        /* inode 总数 */
  __le32 nr_istore_blocks; /* inode 表起始块号 */
  __le32 nr_dstore_blocks; /* 数据块起始块号 */
  __le32 nr_ifree_blocks;  /* inode 位图起始块号 */
  __le32 nr_bfree_blocks;  /* data block 位图起始块号 */
  __le32 nr_free_inodes;   /* 剩余空闲 inode 数量 */
  __le32 nr_free_blocks;   /* 剩余空闲 data block 数量 */
  __le32 last_bitmap_bits; /* 最后一块block bitmap含有的有效bit位数 */
};

/* 
 * 磁盘索引节点
 * 字节对齐：1. 结构体的大小等于其最大成员的整数倍 2.结构体成员的首地址相对于结构体首地址的偏移量是其类型大小的整数倍
 * 将大的数据放在前面可以避免字节对齐填充问题
 */
struct baby_inode {
  __le64 i_size;                    /* inode 对应文件大小 */
  __le32 i_ctime;                   /* i_ctime */
  __le32 i_atime;                   /* i_atime */
  __le32 i_mtime;                   /* i_mtime */
  __le32 i_blocknum;                /* 文件块数 */
  __le32 i_blocks[BABYFS_N_BLOCKS]; /* 索引数组 */
  __le16 i_mode;                    /* 文件类型和访问权限 */
  __le16 i_uid;                     /* inode 所属用户编号 */
  __le16 i_gid;                     /* inode 所属用户组编号 */
  __le16 i_nlink;                   /* 硬链接计数 */
  __le16 i_subdir_num;              /* 子目录项数量 */
  __u8 _padding[(BABYFS_INODE_SIZE - (4 + 2 * 3 + 4 * 5 + 2 * 2 + 4 * BABYFS_N_BLOCKS))]; /* inode 结构体扩展到 128B */
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

#ifdef __KERNEL__

#define rsv_start rsv_window._rsv_start
#define rsv_end rsv_window._rsv_end

/* data type for filesystem-wide blocks number */
typedef long long baby_fsblk_t;

#define BABY_DEFAULT_RESERVE_BLOCKS     8
/*max window size: 1024(direct blocks) + 3([t,d]indirect blocks) */
#define BABY_MAX_RESERVE_BLOCKS         1027
#define BABY_RESERVE_WINDOW_NOT_ALLOCATED 0
struct baby_reserve_window {
  baby_fsblk_t _rsv_start; /* 第一个预留的字节 */ 
  baby_fsblk_t _rsv_end;	/* 最后一个预留的字节，或为0 */ 
};

struct baby_reserve_window_node {
  struct rb_node rsv_node; // 预分配窗口的红黑树
  __u32 rsv_goal_size; // 预分配的块数量
  __u32 rsv_alloc_hit; // 跟踪预分配的命中数，即多少次分配是在预留窗口中进行的
  struct baby_reserve_window rsv_window;
};

struct baby_block_alloc_info { // 用于跟踪文件的磁盘块分配信息
  /* information about reservation window */
  // 采用预分配策略，预分配有关信息
  struct baby_reserve_window_node rsv_window_node;
  baby_fsblk_t last_alloc_logical_block; // 上一次分配的逻辑块号
  baby_fsblk_t last_alloc_physical_block; // 上一次分配的物理块号
};

struct baby_sb_info {
  struct baby_super_block *s_babysb;
  struct buffer_head *s_sbh;
  __le32 nr_free_blocks;
  __le32 nr_free_inodes;
  __le32 nr_blocks; // 数据块数量
  __le16 nr_bitmap; // bitmap 数量
  __le32 last_bitmap_bits; // 最后一块block bitmap含有的有效bit位数
  
  // 保护这个文件系统上预留窗口的锁
  spinlock_t s_rsv_window_lock;
	// 树根，文件系统下所有inode的预分配窗口被组织在这棵红黑树上
  struct rb_root s_rsv_window_root;
	struct baby_reserve_window_node s_rsv_window_head;
};

// 包含 vfs inode 的自定义 inode，存放对应于磁盘 inode 的额外信息
struct baby_inode_info {
  __le16 i_subdir_num;              /* 子目录项数量 */
  __le32 i_blocks[BABYFS_N_BLOCKS]; /* 索引数组 */
  struct inode vfs_inode;
  __u32 i_dtime;            /* 删除时间 */

  struct baby_block_alloc_info *i_block_alloc_info; // 每一个普通文件都有预留窗口，用来加速磁盘块分配
};

// 从 vfs inode 返回包含他的 baby_inode_info
static inline struct baby_inode_info *BABY_I(struct inode *inode) {
  return container_of(inode, struct baby_inode_info, vfs_inode);
}

/* dir.c */
extern int baby_add_link(struct dentry *dentry, struct inode *inode);
extern const struct file_operations baby_dir_operations;
extern unsigned int baby_inode_by_name(struct inode *dir,
                                       const struct qstr *child);
extern int baby_make_empty(struct inode *inode, struct inode *parent);
extern int baby_delete_entry(struct dir_record *de, struct page *page);
extern struct dir_record *baby_find_entry(struct inode *dir,
                                          const struct qstr *child, struct page **res_page);
extern struct page *baby_get_page(struct inode *dir, int n);
extern int baby_prepare_chunk(struct page *page, loff_t pos, unsigned len);
extern int baby_commit_chunk(struct page *page, loff_t pos, unsigned len);
extern void baby_set_de_type(struct dir_record *de, struct inode *inode);
extern inline void baby_put_page(struct page *page);
extern void baby_set_link(struct inode *dir, struct dir_record *de, struct page *page,
                   struct inode *inode, int update_times);
extern struct dir_record *baby_dotdot(struct inode *dir, struct page **p);
extern int baby_empty_dir (struct inode * inode);

/* inode.c */
extern struct inode *baby_iget(struct super_block *, unsigned long);
extern struct baby_inode *baby_get_raw_inode(struct super_block *, ino_t,
                                             struct buffer_head **);
extern void init_inode_operations(struct inode *, umode_t);
extern const struct address_space_operations baby_aops;
extern int baby_get_block(struct inode *inode, sector_t block,
                          struct buffer_head *bh, int create);
extern int baby_write_inode(struct inode *inode, struct writeback_control *wbc);
extern void baby_evict_inode(struct inode *inode);
extern unsigned long baby_count_free_inodes(struct super_block *sb);
extern unsigned long baby_count_free_blocks(struct super_block *sb);

/* file.c */
extern const struct file_operations baby_file_operations;

/* balloc.c */
extern unsigned long baby_new_blocks(struct inode *inode, unsigned long goal,
                                     unsigned long *count, int *err);
extern void baby_init_block_alloc_info(struct inode *inode);
extern void baby_discard_reservation(struct inode *inode);
extern void rsv_window_add(struct super_block *sb,
                           struct baby_reserve_window_node *rsv);

/* 获取超级块 */
static inline struct baby_sb_info *BABY_SB(struct super_block *sb) {
  return sb->s_fs_info;
}

// 小端序位图操作方法
// baby_find_next_zero_bit(void *map, unsigned long search_maxnum, unsigned long search_start)
#define baby_set_bit __test_and_set_bit_le      // set 1，并返回原值
#define baby_test_bit test_bit_le
#define baby_clear_bit	__test_and_clear_bit_le
#define baby_find_first_zero_bit find_first_zero_bit_le
#define baby_find_first_bit find_first_bit
#define baby_find_next_zero_bit find_next_zero_bit_le
#define baby_find_next_bit find_next_bit
#define baby_test_bit test_bit_le
#endif

#endif