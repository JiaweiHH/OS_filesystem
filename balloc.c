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
			      struct baby_reserve_window_node *rsv)
{
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
static inline int rsv_is_empty(struct ext2_reserve_window *rsv)
{
	/* a valid reservation end block could not be 0 */
	return (rsv->_rsv_end == BABY_RESERVE_WINDOW_NOT_ALLOCATED);
}

void baby_init_block_alloc_info(struct inode * inode) {
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
    block_i->last_alloc_logical_block = 0; // 上一次分配的逻辑块号
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

/**
 * 尽最大努力分配连续的磁盘块，仅在一个bitmap管理的数据块中分配
 * 
 * @param goal 建议分配的物理磁盘块号，引用类型，
 * @param count 要求分配的磁盘块数，引用类型，返回实际分配的磁盘块数
 * @return 第一个分配的磁盘块号
 */ 
unsigned long baby_new_blocks(struct inode *inode, unsigned long *goal,
                              unsigned long *count, int *err) {
  struct super_block *sb = inode->i_sb;
  // 最后一个bitmap中包含的有效bit数量
  unsigned long last_bitmap_bits = BABY_SB(sb)->last_bitmap_bits;
  unsigned long target_bit; // 找到的空闲bit位在其bitmap中的偏移
  int start = 0;
  unsigned long first_available_bit; // 第一次分配的是第几个数据块
  int bitmap_bits;
  int real_count = *count;
  unsigned long real_goal = *goal - NR_DSTORE_BLOCKS; // 从第几个数据块开始找
  struct buffer_head *bitmap_bh = NULL;

  unsigned long bitmap_bno = real_goal / BABYFS_BIT_PRE_BLOCK; // 当前遍历到的是第几个bitmap，起点bitmap号由goal确定
  start = real_goal % BABYFS_BIT_PRE_BLOCK; // 在bitmap中从哪一位开始查找空位，第一次由goal确定
  while (1) { // 向后遍历 bitmap block
    // 最后一块bitmap的有效位数特殊
    bitmap_bits = (bitmap_bno + BABYFS_DATA_BIT_MAP_BLOCK_BASE) == NR_DSTORE_BLOCKS - 1 ? last_bitmap_bits : BABYFS_BIT_PRE_BLOCK;
    // 读取目标bitmap block
    bitmap_bh = sb_bread(sb, bitmap_bno + BABYFS_DATA_BIT_MAP_BLOCK_BASE);

    // 在[start, size)中寻找第一个可用的bitmap，返回绝对位置而不是与start的偏移，没找到就返回size
    target_bit = baby_find_next_zero_bit(bitmap_bh->b_data, bitmap_bits, start);
    
    if (target_bit < bitmap_bits) { // 该bitmap存在空闲位
      first_available_bit = bitmap_bno * BABYFS_BIT_PRE_BLOCK + target_bit; // 记录第一个分配的是第几个数据块
      // 在bitmap block内从第一个可用的依次向后占用，遇到第一个已占用的就退出
      while (target_bit < bitmap_bits && !baby_set_bit(target_bit, bitmap_bh->b_data) && --real_count) {
        target_bit++;
      }
      break; // 连续分配过一次就退出
    }

    // 下一个bitmapblock，取bitmap总数量的模保证可以跳转到第0个bitmap block
    bitmap_bno = (bitmap_bno + 1) % (NR_DSTORE_BLOCKS - BABYFS_DATA_BIT_MAP_BLOCK_BASE);  
    start = 0;  // 从第二块开始，都从第一位开始寻找空bit
  }

  mark_buffer_dirty(bitmap_bh);
  brelse(bitmap_bh);

  *count = *count - real_count; // 更新分配磁盘块数量
  *goal = (bitmap_bno * BABYFS_BIT_PRE_BLOCK + target_bit + 1) % BABY_SB(sb)->nr_blocks + NR_DSTORE_BLOCKS; // 更新下一次分配起点

  // printk("baby_new_blocks: first_available_bit %lu; count %d; goal %lu\n", first_available_bit, *count, *goal);
  return first_available_bit + NR_DSTORE_BLOCKS;
}