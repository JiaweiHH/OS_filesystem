#include <linux/buffer_head.h>
#include <linux/fs.h>

#include "babyfs.h"

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