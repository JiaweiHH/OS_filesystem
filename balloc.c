#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "babyfs.h"

/*
 * 预留窗口操作函数，操作功能包括：
 * dump, find, add, remove, is_empty, find_next_reservable_window, etc.
 * 利用红黑树实现文件系统相关的预留窗口机制
 */

/**
 * 发送rsv相关错误时打印错误信息
 */
static void __rsv_window_dump(struct rb_root *root, int verbose,
                              const char *fn) {
  struct rb_node *n;
  struct baby_reserve_window_node *rsv, *prev;
  int bad;

restart:
  n = rb_first(root);
  bad = 0;
  prev = NULL;

  printk("Block Allocation Reservation Windows Map (%s):\n", fn);
  while (n) {
    rsv = rb_entry(n, struct baby_reserve_window_node, rsv_node);
    if (verbose)
      printk("reservation window 0x%p "
             "start: %lu, end: %lu\n",
             rsv, rsv->rsv_start, rsv->rsv_end);
    if (rsv->rsv_start && rsv->rsv_start >= rsv->rsv_end) {
      printk("Bad reservation %p (start >= end)\n", rsv);
      bad = 1;
    }
    if (prev && prev->rsv_end >= rsv->rsv_start) {
      printk("Bad reservation %p (prev->end >= start)\n", rsv);
      bad = 1;
    }
    if (bad) {
      if (!verbose) {
        printk("Restarting reservation walk in verbose mode\n");
        verbose = 1;
        goto restart;
      }
    }
    n = rb_next(n);
    prev = rsv;
  }
  printk("Window map complete.\n");
  BUG_ON(bad);
}
#define rsv_window_dump(root, verbose)                                         \
  __rsv_window_dump((root), (verbose), __func__)

/**
 * 测试goal是否在预留窗口内
 */
static int goal_in_my_reservation(struct baby_reserve_window *rsv,
                                  unsigned int bitmap_no,
                                  unsigned int bitmap_offset) {
  baby_fsblk_t real_goal;

  if (bitmap_offset == -1)
    return 1; // 不使用goal的情况恒返回1

  real_goal = bitmap_no * BABYFS_BIT_PRE_BLOCK + bitmap_offset;
  return rsv->_rsv_start > real_goal || rsv->_rsv_end < real_goal ? 0 : 1;
}

/**
 * 判断预留窗口是否为空
 */
static inline int rsv_is_empty(struct baby_reserve_window *rsv) {
  /* a valid reservation end block could not be 0 */
  return (rsv->_rsv_end == BABY_RESERVE_WINDOW_NOT_ALLOCATED);
}

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

void baby_init_block_alloc_info(struct inode *inode) {
  struct baby_inode_info *bbi = BABY_I(inode);
  struct baby_block_alloc_info *block_i;
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
  bbi->i_block_alloc_info = block_i;
}

/**
 * @inode:		inode
 *
 * 在下列情况时丢弃当前inode的预留窗口：
 * 	baby_evict_inode(): inode 被释放时
 * 	baby_truncate_blocks(): 文件的数据发送重大改变时
 */
void baby_discard_reservation(struct inode *inode) {
  struct baby_inode_info *ei = BABY_I(inode);
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

// 直接使用 ext2 的
void rsv_window_add(struct super_block *sb,
                    struct baby_reserve_window_node *rsv) {
  struct rb_root *root = &BABY_SB(sb)->s_rsv_window_root;
  struct rb_node *node = &rsv->rsv_node;
  unsigned long start = rsv->rsv_start;

  struct rb_node **p = &root->rb_node;
  struct rb_node *parent = NULL;
  struct baby_reserve_window_node *this;

  while (*p) {
    parent = *p;
    this = rb_entry(parent, struct baby_reserve_window_node, rsv_node);

    if (start < this->rsv_start)
      p = &(*p)->rb_left;
    else if (start > this->rsv_end)
      p = &(*p)->rb_right;
    else {
      rsv_window_dump(root, 1);
      BUG();
    }
  }

  rb_link_node(node, parent, p);
  rb_insert_color(node, root);
}

static int
find_next_reservable_window(struct baby_reserve_window_node *search_head,
                            struct baby_reserve_window_node *my_rsv,
                            struct super_block *sb, unsigned long start_block,
                            unsigned long end_block) {
  struct rb_node *next;
  unsigned short size = my_rsv->rsv_goal_size;
  unsigned long cur = start_block;
  struct baby_reserve_window_node *rsv = search_head, *prev = NULL;

  while (1) {
    // 从当前 rsv 之外开始找
    if (cur <= rsv->rsv_end)
      cur = rsv->rsv_end + 1;
    if (cur > end_block)
      return -1;

    prev = rsv;
    next = rb_next(&rsv->rsv_node);
    rsv = rb_entry(next, struct baby_reserve_window_node, rsv_node);
    if (!next)
      break;

    // 找到了一个足够容纳 size 大小的空间
    if (cur + size <= rsv->rsv_start)
      break;
  }

  // 下列条件成立，说明这次找到的和上一次使用的预留窗口不是相邻的关系
  // 是相邻的关系可以直接更改 my_rsv 的字段值
  // 不是相邻的关系需要先移除红黑树，因为直接修改会导致红黑树 不“有序”
  if ((prev != my_rsv) && (!rsv_is_empty(&my_rsv->rsv_window)))
    rsv_window_remove(sb, my_rsv);

  my_rsv->rsv_start = cur;
  my_rsv->rsv_end = cur + size - 1;
  my_rsv->rsv_alloc_hit = 0;
  if (prev != my_rsv)
    rsv_window_add(sb, my_rsv);

  return 0;
}

static struct baby_reserve_window_node *
search_reserve_window(struct rb_root *root, unsigned long goal) {
  struct rb_node *n = root->rb_node;
  struct baby_reserve_window_node *rsv;

  if (!n)
    return NULL;

  do {
    rsv = rb_entry(n, struct baby_reserve_window_node, rsv_node);

    if (goal < rsv->rsv_start)
      n = n->rb_left;
    else if (goal > rsv->rsv_end)
      n = n->rb_right;
    else
      return rsv;
  } while (n);
  /*
   * We've fallen off the end of the tree: the goal wasn't inside
   * any particular node.  OK, the previous node must be to one
   * side of the interval containing the goal.  If it's the RHS,
   * we need to back up one.
   */
  if (rsv->rsv_start > goal) {
    n = rb_prev(&rsv->rsv_node);
    rsv = rb_entry(n, struct baby_reserve_window_node, rsv_node);
  }
  return rsv;
}

static unsigned int bitmap_search_next_usable_block(unsigned int rsv_start_,
                                                    unsigned int end,
                                                    struct buffer_head *bh) {
  unsigned int next;
  next = baby_find_next_zero_bit(bh->b_data, end, rsv_start_);
  if (next >= end)
    return -1;
  return next;
}

/**
 * 重新初始化预留窗口
 */
static int alloc_new_reservation(struct baby_reserve_window_node *my_rsv,
                                 unsigned int bitmap_no,
                                 unsigned int bitmap_offset,
                                 struct super_block *sb,
                                 struct buffer_head **bh) {
  struct baby_reserve_window_node *search_head = NULL;
  struct baby_sb_info *sb_info = BABY_SB(sb);
  struct baby_super_block *b_sb = sb_info->s_babysb;
  struct rb_root *rsv_root = &sb_info->s_rsv_window_root;

  // 确定 rsv 起始搜索位置，要么是 goal，要么是 bitmap 第一个
  unsigned long start_block,
      map_first_block = sb_info->s_babysb->nr_dstore_blocks +
                        bitmap_no * BABYFS_BIT_PRE_BLOCK,
      end_block = sb_info->s_babysb->nr_dstore_blocks + sb_info->nr_blocks;
  if (bitmap_offset < 0)
    start_block = map_first_block;
  else
    start_block = map_first_block + bitmap_offset;

  unsigned short size = my_rsv->rsv_goal_size;

  /*
   * 在未能从上一个窗口分配的时候，控制流将进入这里
   * 如果之前命中率高，那么就提高窗口大小
   */
  if (!rsv_is_empty(&my_rsv->rsv_window)) {
    if (my_rsv->rsv_alloc_hit > (my_rsv->rsv_end - my_rsv->rsv_start + 1) / 2) {
      size = size * 2;
      if (size > BABY_MAX_RESERVE_BLOCKS)
        size = BABY_MAX_RESERVE_BLOCKS;
      my_rsv->rsv_goal_size = size;
    }
  }

  // 查询是否有窗口包含了 goal
  // 没有的话返回 goal 之前的一个窗口
  search_head = search_reserve_window(rsv_root, start_block);

  int bitmap_no_1 = -1, bitmap_no_2;
  unsigned int first_free_block;
  int ret;
retry:
  // 以 search_head 为起点，查询一个可以容纳 my_rsv
  // 并且不与其他预留窗口重叠的新的预留窗口
  ret = find_next_reservable_window(search_head, my_rsv, sb, start_block,
                                    end_block);

  // retry 失败，移除上一次 add 的 node
  if (ret == -1) {
    if (!rsv_is_empty(&my_rsv->rsv_window))
      rsv_window_remove(sb, my_rsv);
    return -1;
  }

  // 读取第一个 bitmap
  if (bitmap_no_1 != my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK) { // 非连续
    if (bitmap_no_1 != -1) // 第一次进入不需要释放
      brelse(bh[0]);
    bitmap_no_1 = my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK;
    bh[0] = sb_bread(sb, b_sb->nr_bfree_blocks + bitmap_no_1);
  }

  // 检查可能出现的第二个 bitmap
  bitmap_no_2 = my_rsv->rsv_end / BABYFS_BIT_PRE_BLOCK;
  if (bitmap_no_1 != bitmap_no_2)
    bh[1] = sb_bread(sb, b_sb->nr_bfree_blocks + bitmap_no_2);

  // 找到 bitmap 中的第一个 free_block
  first_free_block = bitmap_search_next_usable_block(
      my_rsv->rsv_start - bitmap_no_1 * BABYFS_BIT_PRE_BLOCK,
      BABYFS_BIT_PRE_BLOCK, bh[0]);
  if (first_free_block > 0) {
    // 更新 start_block
    start_block = first_free_block + bitmap_no_1 * BABYFS_BIT_PRE_BLOCK;
    // 判断 free block 是不是在 rsv 内
    if (start_block >= my_rsv->rsv_start && start_block <= my_rsv->rsv_end)
      return 0;
    else // bh[0]中有空闲的，从空闲位重新分配
      goto prepare_retry;
  }

  if (bitmap_no_1 != bitmap_no_2) { // 第一个bitmap没找到，且rsv跨bitmap
    bh[1] = sb_bread(sb, bitmap_no_2 + b_sb->nr_bfree_blocks);
    first_free_block =
        bitmap_search_next_usable_block(0, BABYFS_BIT_PRE_BLOCK, bh[1]);
    if (first_free_block > 0) {
      // 更新 start_block
      start_block = first_free_block + bitmap_no_2 * BABYFS_BIT_PRE_BLOCK;
      // 判断 free block 是不是在 rsv 内
      if (start_block >= my_rsv->rsv_start && start_block <= my_rsv->rsv_end)
        return 0;
      else { // bh[1]中有空闲的，释放第一个bh，保留第二个bh做下次分配
        brelse(bh[0]);
        bh[0] = bh[1];
        bh[1] = NULL;
        bitmap_no_1 = bitmap_no_2;
      }
    } else { // bh[0]和bh[1]中都不存在空闲位，释放全部bh
      brelse(bh[0]);
      brelse(bh[1]);
      bh[0] = bh[1] = NULL;
      start_block = (bitmap_no_2 + 1) * BABYFS_BIT_PRE_BLOCK;
      bitmap_no_1 = -1;
    }
  } else { // bh[0]中不存在空闲位 且 rsv不跨bitmap，释放第一个bh
    brelse(bh[0]);
    start_block = (bitmap_no_1 + 1) * BABYFS_BIT_PRE_BLOCK;
    bitmap_no_1 = -1;
  }

prepare_retry:

  search_head = my_rsv;
  goto retry;
}

/*
 * 尝试将预留窗口向后扩展大小为size的空间，最多扩展至红黑树中下一个预留窗口的上边界。
 * 这里扩展空间只会考虑和其他预留窗口的冲突。
 */
static void try_to_extend_reservation(struct baby_reserve_window_node *my_rsv,
                                      struct super_block *sb, int size) {
  struct baby_reserve_window_node *next_rsv = NULL;
  struct rb_node *next;
  spinlock_t *rsv_lock = &BABY_SB(sb)->s_rsv_window_lock;
  if (!spin_trylock(rsv_lock))
    return;

  next = rb_next(&my_rsv->rsv_node);
  if (!next)
    my_rsv->rsv_end += size;
  else {
    next_rsv = rb_entry(next, struct baby_reserve_window_node, rsv_node);
    if (next_rsv->rsv_start - my_rsv->rsv_end - 1 >= size)
      my_rsv->rsv_end += size;
    else
      my_rsv->rsv_end = next_rsv->rsv_start - 1;
  }
  spin_unlock(rsv_lock);
}

/**
 * 在一个指定范围内分配磁盘块，范围由窗口指定
 */
static baby_fsblk_t baby_try_to_allocate(struct super_block *sb,
                                         unsigned int bitmap_no,
                                         unsigned int bitmap_offset,
                                         unsigned long *count,
                                         struct baby_reserve_window *my_rsv,
                                         struct buffer_head **bh) {}

/**
 * @sb:			superblock
 * @group:		goal 所在的块组的块组号
 * @bitmap_bh:		goal 所在块组的位图
 * @grp_goal:		goal 在其所在块组中的相对块号
 * @count:		target number of blocks to allocate
 * @my_rsv:		reservation window
 *
 * 块分配的核心函数
 */
static baby_fsblk_t baby_try_to_allocate_with_rsv(
    struct super_block *sb, unsigned int bitmap_no, unsigned int bitmap_offset,
    struct baby_reserve_window_node *my_rsv, unsigned long *count) {
  struct baby_sb_info *bbi = BABY_SB(sb);
  struct baby_super_block *b_sb = bbi->s_babysb;
  baby_fsblk_t ret = 0;
  unsigned long num = *count;

  // bh 数组用来存放可能读取到的相邻两个 bitmap
  struct buffer_head *bh_array[2] = {0};

  if (my_rsv == NULL) { // (非普通文件)不使用预留窗口分配数据块
    return baby_try_to_allocate(sb, bitmap_no, bitmap_offset, count, NULL,
                                NULL);
  }

  /**
   * 根据预留窗口和goal分配磁盘块
   * 预留窗口会在下列情况发生重新初始化：
   * 1. 预分配窗户口还未初始化
   * 2.
   * 上一次实际块占用行为没有成功，即并发情况下预留窗口中的块被其他分配进程占用了，
   *    或者上次分配时没有用完的块被其他进程占用了
   * 3. goal不在预留窗口中
   *
   * 分配磁盘块在一个无限循环中，分为两步进行，首先查找一个预留窗口，再去实际占用预留窗口
   * 中的磁盘块，这样做将预分配数量和实际分配数量解耦，可以对预分配数量进行适当优化
   * 无限循环使得分配有多次尝试的机会，上一次块实际占用行为失败后返回ret<0，将会下一次尝试分
   * 配时重新初始化预留窗口；循环会在第一次占用成功时就退出
   */
  while (1) {
    // 预留窗口为空 或者 上一次实际块占用行为没有成功 或者 goal不在预留窗口中
    // 需要分配预留窗口
    if (rsv_is_empty(&my_rsv->rsv_window) || (ret < 0) ||
        !goal_in_my_reservation(&my_rsv->rsv_window, bitmap_no,
                                bitmap_offset)) {

      // 新分配的预留窗口大小至少等于本次分配需求的数据块个数
      if (my_rsv->rsv_goal_size < *count)
        my_rsv->rsv_goal_size = *count;

      // 重新分配预留窗口
      ret =
          alloc_new_reservation(my_rsv, bitmap_no, bitmap_offset, sb, bh_array);
      if (ret < 0) // 整个磁盘块都分配不出新的窗口
        break;     /* failed */

      // 分配新的窗口时会尽量使goal在窗口内，若不能办到，则设置goal为-1，表示真正分配时不使用goal
      if (!goal_in_my_reservation(&my_rsv->rsv_window, bitmap_no,
                                  bitmap_offset))
        bitmap_offset = -1;
    } else { // 文件有预留窗口且goal在预留窗口中，只有第一次循环才有可能进入该执行流
      // 计算grp_goal到窗口尾部共有几个空闲块，即可分配区域长度
      int curr = my_rsv->rsv_end -
                 (bitmap_no * BABYFS_BIT_PRE_BLOCK + bitmap_offset) + 1;
      // 若可分配长度比需要分配的数量小，则尝试扩大预留窗口到满足所需，也有可能不能扩大这么多
      if (curr < *count)
        try_to_extend_reservation(my_rsv, sb, *count - curr);

      // 读出当前预留窗口的bh
      bh_array[0] = sb_bread(sb, my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK +
                                     b_sb->nr_bfree_blocks);
      if (my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK !=
          my_rsv->rsv_end / BABYFS_BIT_PRE_BLOCK) {
        bh_array[1] = sb_bread(sb, my_rsv->rsv_end / BABYFS_BIT_PRE_BLOCK +
                                       b_sb->nr_bfree_blocks);
      }
    }

    // 若预留窗口超过了可分配范围，则报错
    if ((my_rsv->rsv_start > bbi->nr_blocks) || (my_rsv->rsv_end < 0)) {
      rsv_window_dump(&BABY_SB(sb)->s_rsv_window_root, 1);
      BUG();
    }
    // 将预留窗口中预分配的数据块，在其所属块组位图上对应的bit置1，即正式占用
    ret = baby_try_to_allocate(sb, bitmap_no, bitmap_offset, &num,
                               &my_rsv->rsv_window, bh_array);

    if (ret >= 0) { // 如果分配成功，统计预留窗口中已分配数量后退出循环
      my_rsv->rsv_alloc_hit += num; // 统计预留窗口中已分配数量
      *count = num;                 // 返回分配到的块数
      break;                        /* succeed */
    }
    num = *count;
  }
  return ret;
}

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
  struct baby_reserve_window_node *my_rsv = NULL;
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
  unsigned int goal_no = (goal - b_sb->nr_dstore_blocks) / BABYFS_BIT_PRE_BLOCK,
               bitmap_no = goal_no;

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
    *err = -ENOSPC;
    goto out;
  }

  // 获取 bitmap 偏移值
  bitmap_offset = (goal - b_sb->nr_dstore_blocks) % BABYFS_BIT_PRE_BLOCK;

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
  *err = 0;
  if (num < *count) {
    *count = num;
    mark_inode_dirty(inode);
  }
  return ret_block;

out:
  return 0;
}