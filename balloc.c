#include <linux/buffer_head.h>
#include <linux/fs.h>

#include "babyfs.h"

/**
 * rsv_window_remove() -- unlink a window from the reservation rb tree
 * @sb:			super block
 * @rsv:		reservation window to remove
 *
 * Mark the block reservation window as not allocated, and unlink it
 * from the filesystem reservation window rb tree. Must be called with
 * rsv_lock held.
 */
static void rsv_window_remove(struct super_block *sb,
                              struct baby_reserve_window_node *rsv) {
  rsv->rsv_start = BABY_RESERVE_WINDOW_NOT_ALLOCATED;
  rsv->rsv_end = BABY_RESERVE_WINDOW_NOT_ALLOCATED;
  rsv->rsv_alloc_hit = 0;
  rb_erase(&rsv->rsv_node, &BABY_SB(sb)->s_rsv_window_root);
}

/*
 * rsv_is_empty() -- Check if the reservation window is allocated.
 * @rsv:		given reservation window to check
 *
 * returns 1 if the end block is BABY_RESERVE_WINDOW_NOT_ALLOCATED.
 */
static inline int rsv_is_empty(struct ext2_reserve_window *rsv) {
  /* a valid reservation end block could not be 0 */
  return (rsv->_rsv_end == BABY_RESERVE_WINDOW_NOT_ALLOCATED);
}

void baby_init_block_alloc_info(struct inode *inode) {
  struct baby_inode_info *bbi = BABY_I(inode);
  struct baby2_block_alloc_info *block_i;
  struct super_block *sb = inode->i_sb;

  block_i = kmalloc(sizeof(*block_i), GFP_NOFS);
  if (block_i) {
    struct baby_reserve_window_node *rsv = &block_i->rsv_window_node;

    rsv->rsv_start = BABY_RESERVE_WINDOW_NOT_ALLOCATED;
    rsv->rsv_end = BABY_RESERVE_WINDOW_NOT_ALLOCATED; // 标识预留窗口为空
    rsv->rsv_goal_size = BABY_DEFAULT_RESERVE_BLOCKS; // 默认预留窗口大小为8
    rsv->rsv_alloc_hit = 0;
    block_i->last_alloc_logical_block = 0;  // 上一次分配的逻辑块号
    block_i->last_alloc_physical_block = 0; // 上一次分配的物理块号
  }
  ei->i_block_alloc_info = block_i;
}

/**
 * @inode:		inode
 *
 * 在下列情况时丢弃当前inode的预留窗口：
 * 	baby_evict_inode(): inode 被释放时
 * 	baby_truncate_blocks(): 文件的数据发送重大改变时
 */
void baby_discard_reservation(struct inode *inode) {
  struct baby_inode_info *ei = EXT2_I(inode);
  struct baby_block_alloc_info *block_i = ei->i_block_alloc_info;
  struct baby_reserve_window_node *rsv;
  spinlock_t *rsv_lock = &BABY_SB(inode->i_sb)->s_rsv_window_lock;

  if (!block_i)
    return;

  rsv = &block_i->rsv_window_node;
  if (!rsv_is_empty(&rsv->rsv_window)) {
    spin_lock(rsv_lock);
    if (!rsv_is_empty(&rsv->rsv_window))
      rsv_window_remove(inode->i_sb, rsv);
    spin_unlock(rsv_lock);
  }
}

static inline int rsv_is_empty(struct baby_reserve_window *rsv) {
  /* a valid reservation end block could not be 0 */
  return (rsv->_rsv_end == 0);
}

static unsigned long baby_try_to_allocate_with_rsv(
    struct super_block *sb, unsigned int bitmap_no,
    struct buffer_head *bitmap_bh, unsigned long bitmap_offset,
    struct baby_reserve_window_node *my_rsv, unsigned long *count) {}

/**
 * 尽最大努力分配连续的磁盘块，仅在一个bitmap管理的数据块中分配
 *
 * @param goal 建议分配的物理磁盘块号，引用类型，
 * @param count 要求分配的磁盘块数，引用类型，返回实际分配的磁盘块数
 * @return 第一个分配的磁盘块号
 */
unsigned long baby_new_blocks(struct inode *inode, unsigned long goal,
                              unsigned long *count, int *err) {
  struct super_block *sb = inode->i_sb;
  struct baby_inode_info *inode_info = BABY_I(inode);
  struct baby_block_alloc_info *my_rsv = NULL;
  unsigned long free_blocks, ret_block;

  struct baby_block_alloc_info *block_i = inode_info->i_block_alloc_info;
  if (block_i) {
    my_rsv = &block_i->rsv_window_node;
  }

  // 检查剩余块数量和需要的数量
  struct baby_sb_info *sb_info = BABY_SB(sb);
  if (sb_info->nr_free_blocks < *count) {
    *err = -ENOSPC;
    goto out;
  }

  // 检查 goal 是否超出
  struct baby_super_block *b_sb = sb_info->s_babysb;
  if (goal < b_sb->nr_dstore_blocks ||
      goal > b_sb->nr_dstore_blocks + b_sb->nr_blocks)
    goal = b_sb->nr_dstore_blocks;

  // 计算 bitmap 块号
  unsigned int goal_no = bitmap_no =
      (goal - b_sb->nr_dstore_blocks) / BABYFS_BIT_PRE_BLOCK;

  unsigned int bitmap_offset = 0;       // 块内偏移
  unsigned int alloc_no = 0;            // 分配得到的块号（物理）
  struct buffer_head *bitmap_bh = NULL; // 映射 bitmap block
  // bitmap 数量
  unsigned int bitmap_num = b_sb->nr_dstore_blocks - b_sb->nr_bfree_blocks;
  unsigned long num = *count;
  unsigned int bno = 0;
  unsigned int windowsz = my_rsv->rsv_goal_size; // 窗口大小
  free_blocks = sb_info->nr_free_blocks;         // 系统剩余空闲数量

retry_alloc:
  // 没有剩余空间分配预留窗口了
  if (my_rsv && (free_blocks < windowsz) && (free_blocks > 0) &&
      (rsv_is_empty(&my_rsv->rsv_window)))
    my_rsv = NULL;

  if (!free_blocks) {
    *errp = -ENOSPC;
    goto out;
  }

  // 获取 bitmap 偏移值
  bitmap_offset = (goal - b_sb->nr_dstore_blocks) % BABYFS_BIT_PRE_BLOCK;
  bitmap_bh = sb_bread(sb, bitmap_no + b_sb->nr_bfree_blocks);

  // 尝试分配
  alloc_no =
      baby_try_to_allocate_with_rsv(sb, bitmap_no, bitmap_offset, my_rsv, &num);
  if (alloc_no >= 0)
    goto allocated;

  // 上面的分配失败可能是因为，为了 my_rsv 分配而产生的问题
  // 不采用预留窗口分配，重新再来一次
  if (my_rsv) {
    my_rsv = NULL;
    bitmap_no = goal_no;
    goto retry_alloc;
  }
  *err = -ENOSPC;
  goto out;

allocated:
  ret_block = alloc_no;
  sb_info->nr_free_blocks -= num;
  if (sb->s_flags & SB_SYNCHRONOUS)
    sync_dirty_buffer(bitmap_bh);
  // brelse(bitmap_bh);
  *err = 0;
  if (num < *count) {
    *count = num;
    mark_inode_dirty(inode);
  }
  return ret_block;

out:
  // brelse(bitmap_bh);
  return 0;
}