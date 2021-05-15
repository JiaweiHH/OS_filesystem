#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "babyfs.h"

void __dump_myrsv(struct baby_reserve_window_node *my_rsv, const char *fn, int line) {
  printk("%s[%d] dump_myrsv: [%lu, %lu] %d/%d \n", fn, line,
         my_rsv->rsv_start, my_rsv->rsv_end, my_rsv->rsv_alloc_hit,
         my_rsv->rsv_goal_size);
}
#define dump_myrsv(rsv) __dump_myrsv(rsv, __func__, __LINE__)

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
#define rsv_window_dump(root, verbose)  \
  __rsv_window_dump((root), (verbose), __func__)

/**
 * 测试goal是否在预留窗口内
 */
static int goal_in_my_reservation(struct baby_reserve_window *rsv,
                                  baby_fsblk_t goal) {
  if (goal == -1)
    return 1; // 不使用goal的情况恒返回1

  return rsv->_rsv_start > goal || rsv->_rsv_end < goal ? 0 : 1;
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
  struct baby_sb_info *sb_info = BABY_SB(sb);
  struct rb_root *rsv_root = &sb_info->s_rsv_window_root;
  struct rb_node *n = rsv_root->rb_node;
#ifdef RSV_DEBUG
  printk("find_next_reservable_window: start_block %ld end_block %ld\n", start_block, end_block);
#endif
  if(!rsv)
    return -1;
  while (1) {
    // 从当前 rsv 之外开始找
    if (cur <= rsv->rsv_end)
      cur = rsv->rsv_end + 1;
    if (cur > end_block)
      goto try_prev;
      // return -1;

    prev = rsv;
    next = rb_next(&rsv->rsv_node);
    rsv = rb_entry(next, struct baby_reserve_window_node, rsv_node);
    if (!next) {
      if(cur + size <= end_block)
        goto find;
      goto try_prev;
      // break;
    }
      

    // 找到了一个足够容纳 size 大小的空间
    if (cur + size <= rsv->rsv_start)
      goto find;
  }

try_prev:
  // 从 [0, 0] 开始找
#ifdef RSV_DEBUG
  printk("find_next_reservable_window: try_prev\n");
#endif
  rsv = rb_entry(n, struct baby_reserve_window_node, rsv_node);
  cur = 1;  // 跳过第 0 块

  if(rsv == search_head)  // 第一个预留窗口
    goto find;

  while(rsv != search_head) {
    if(cur <= rsv->rsv_end)
      cur = rsv->rsv_end + 1;
  #ifdef RSV_DEBUG
    printk("cur: %ld rsv.start: %ld rsv.end: %ld\n", cur, rsv->rsv_start, rsv->rsv_end);
  #endif
    prev = rsv;
    next = rb_next(&rsv->rsv_node);
    rsv = rb_entry(next, struct baby_reserve_window_node, rsv_node);
    if(cur + size <= rsv->rsv_start)
      goto find;
  }
  return -1;

find:
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

static int bitmap_search_next_usable_block(unsigned int start, unsigned int end,
                                           struct buffer_head *bh) {
  int next;
  next = baby_find_next_zero_bit(bh->b_data, end, start);
  if (next >= end)
    return -1;
  return next;
}

/**
 * 重新初始化预留窗口
 */
static int alloc_new_reservation(struct baby_reserve_window_node *my_rsv,
                                 baby_fsblk_t goal, struct super_block *sb,
                                 struct buffer_head **bh) {
  struct baby_reserve_window_node *search_head = NULL;
  struct baby_sb_info *sb_info = BABY_SB(sb);
  struct baby_super_block *b_sb = sb_info->s_babysb;
  struct rb_root *rsv_root = &sb_info->s_rsv_window_root;

  // 确定 rsv 起始搜索位置，要么是 goal，要么是 bitmap 第一个
  unsigned long start_block = goal > 0 ? goal : 0,
                end_block = sb_info->nr_blocks - 1;

  unsigned int size = my_rsv->rsv_goal_size;

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
    #ifdef RSV_DEBUG
      printk("alloc_new_reservation: extend rsv size to %u\n", size);
    #endif
    }
  }

  // 查询是否有窗口包含了 goal
  // 没有的话返回 goal 之前的一个窗口
  search_head = search_reserve_window(rsv_root, start_block);
#ifdef RSV_DEBUG
  printk("alloc_new_reservation: search_reserve_window done start_block %lu, search_head:\n", start_block);
  dump_myrsv(search_head);
#endif
  int bitmap_no_1 = -1, bitmap_no_2;
  int first_free_block;
  int ret;
retry:
  // 以 search_head 为起点，查询一个可以容纳 my_rsv
  // 并且不与其他预留窗口重叠的新的预留窗口
  ret = find_next_reservable_window(search_head, my_rsv, sb, start_block,
                                    end_block);
#ifdef RSV_DEBUG
  printk("alloc_new_reservation: find_next_reservable_window done, ret %d\n",
         ret);
#endif
  // retry 失败，移除上一次 add 的 node
  if (ret == -1) {
    if (!rsv_is_empty(&my_rsv->rsv_window))
      rsv_window_remove(sb, my_rsv);
    return -1;
  }
#ifdef RSV_DEBUG
  dump_myrsv(my_rsv);
#endif

  // 读取第一个 bitmap
  if (bitmap_no_1 != my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK) { // 非连续
    if (bitmap_no_1 != -1) // 第一次进入不需要释放
      brelse(bh[0]);
    bitmap_no_1 = my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK;
    bh[0] = sb_bread(sb, bitmap_no_1 + BABYFS_DATA_BIT_MAP_BLOCK_BASE);
#ifdef RSV_DEBUG
    printk("alloc_new_reservation: read bitmap_1 block NO.%d\n",
           BABYFS_DATA_BIT_MAP_BLOCK_BASE + bitmap_no_1);
#endif
  }

  // 检查可能出现的第二个 bitmap
  bitmap_no_2 = my_rsv->rsv_end / BABYFS_BIT_PRE_BLOCK;
  if (bitmap_no_1 != bitmap_no_2) {
    bh[1] = sb_bread(sb, bitmap_no_2 + BABYFS_DATA_BIT_MAP_BLOCK_BASE);
    #ifdef RSV_DEBUG
      printk("alloc_new_reservation: read bitmap_2 block NO.%d\n",
            bitmap_no_2 + BABYFS_DATA_BIT_MAP_BLOCK_BASE);
    #endif
  }

  // 找到 bitmap 中的第一个 free_block
  first_free_block = bitmap_search_next_usable_block(
      my_rsv->rsv_start - bitmap_no_1 * BABYFS_BIT_PRE_BLOCK,
      BABYFS_BIT_PRE_BLOCK, bh[0]);
  #ifdef RSV_DEBUG
    printk("alloc_new_reservation: in bh[0], first_free_block %d, start %d, end %d, bh %c\n",
          first_free_block,
          my_rsv->rsv_start - bitmap_no_1 * BABYFS_BIT_PRE_BLOCK,
          BABYFS_BIT_PRE_BLOCK, *((char *)(bh[0]->b_data) + (first_free_block > 0 ? first_free_block : 0) / 8));
  #endif
  if (first_free_block >= 0) {
    // 更新 start_block
    start_block = first_free_block + bitmap_no_1 * BABYFS_BIT_PRE_BLOCK;
    // 判断 free block 是不是在 rsv 内
    if (start_block >= my_rsv->rsv_start && start_block <= my_rsv->rsv_end) {
      #ifdef RSV_DEBUG
        printk("alloc_new_reservation: get a new rsv in bh[0], start_block %lu\n", start_block);
      #endif
      return 0;
    } else // bh[0]中有空闲的，从空闲位重新分配
      goto prepare_retry;
  }

  if (bitmap_no_1 != bitmap_no_2) { // 第一个bitmap没找到，且rsv跨bitmap
    first_free_block =
        bitmap_search_next_usable_block(0, BABYFS_BIT_PRE_BLOCK, bh[1]);
    #ifdef RSV_DEBUG
      printk("alloc_new_reservation: in bh[1], first_free_block %d, start 0, end %d, bh %c\n",
          first_free_block, BABYFS_BIT_PRE_BLOCK, 
          *((char *)(bh[0]->b_data) + (first_free_block > 0 ? first_free_block : 0) / 8));
    #endif
    if (first_free_block >= 0) {
      // 更新 start_block
      start_block = first_free_block + bitmap_no_2 * BABYFS_BIT_PRE_BLOCK;
      // 判断 free block 是不是在 rsv 内
      if (start_block >= my_rsv->rsv_start && start_block <= my_rsv->rsv_end) {
        #ifdef RSV_DEBUG
          printk("alloc_new_reservation: get a new rsv in bh[1], start_block %lu\n", start_block);
        #endif
        return 0;
      }
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
 * 占用bitmap中连续的磁盘块[start,end)
 *
 */
static baby_fsblk_t do_allocate(struct buffer_head *bh, unsigned long *count,
                                unsigned int start, unsigned int end, baby_fsblk_t goal,
                                unsigned short is_next) {
  unsigned long num = 0;
#ifdef RSV_DEBUG
  printk("do_allocate begin: start %ld, goal %ld\n", start, goal);
#endif
repeat:
  if (goal < 0) {
    // TODO 使用按字节查找加速起始块的查找过程
    goal = bitmap_search_next_usable_block(start, end, bh);
    if (goal < 0) {
      goto fail;
    }
  }
  // printk("do_allocate *count %lu start %u end %u goal %lld is_next %d\n",
  // *count, start, end, goal, is_next);

  start = goal;
#ifdef RSV_DEBUG
  printk("do_allocate: start %ld\n", start);
#endif
  // 返回1，说明占用失败，start位原先就是1，看下一位是不是空闲位
  if (baby_set_bit(start, bh->b_data)) {
    start++;
    goal++;
  #ifdef RSV_DEBUG
    printk("baby_set_bit failed: set bit %ld\n", goal - 1);
  #endif
    if (start >= end || is_next) {
      goto fail;
    }
    goto repeat;
  }
  num++;  // 已经占用一块了
  goal++; // 从已经占用的下一块开始
#ifdef RSV_DEBUG
  printk("baby_set_bit succeed: set bit %ld, test bit is %ld\n", goal - 1, baby_test_bit(goal - 1, bh->b_data));
#endif
  // 继续占用 goal 之后的位，直到到达边界或满足需求
  while (num < *count && goal < end && !baby_set_bit(goal, bh->b_data)) {
    num++;
    goal++;
  #ifdef RSV_DEBUG
    printk("baby_set_bit succeed: set bit %ld, test bit is %ld\n", goal - 1, baby_test_bit(goal - 1, bh->b_data));
  #endif
  }

  *count = num;
  mark_buffer_dirty(bh);
  // sync_dirty_buffer(bh);
  brelse(bh);
  return goal - num;

fail:
  *count = num;
  brelse(bh);
  return -1;
}

/**
 * 在一个指定范围内分配磁盘块，范围由窗口指定
 * @bh: my_rsv 所在的 bitmap，rsv 跨 bitmap 时，数组长度为2
 */
static baby_fsblk_t baby_try_to_allocate(struct super_block *sb,
                                         baby_fsblk_t goal,
                                         unsigned long *count,
                                         struct baby_reserve_window *my_rsv,
                                         struct buffer_head **bh) {
  int start, end, first;
  unsigned long num = *count, remain;
  unsigned short has_next = 0;
  int bitmap_offset = 0, bitmap_no = 0, bitmap_no_1, bitmap_no_2, ret_bitmap_no;
  if (goal > 0) {
    bitmap_offset = goal % BABYFS_BIT_PRE_BLOCK;
    bitmap_no = goal / BABYFS_BIT_PRE_BLOCK;
  }
#ifdef RSV_DEBUG
  printk("baby_try_to_allocate goal %lld count %lu\n", goal, *count);
#endif
  if (my_rsv) {
    bitmap_no_1 = my_rsv->_rsv_start / BABYFS_BIT_PRE_BLOCK;
    bitmap_no_2 = my_rsv->_rsv_end / BABYFS_BIT_PRE_BLOCK;
    start = my_rsv->_rsv_start % BABYFS_BIT_PRE_BLOCK;
    end = my_rsv->_rsv_end % BABYFS_BIT_PRE_BLOCK + 1;
    ret_bitmap_no = bitmap_no_1;

    if (bh && bh[0] && bh[1])
      has_next = 1;

    /* my_rsv 存在，goal 也存在并且在 rsv 内部 */
    if (my_rsv->_rsv_start <= goal && goal <= my_rsv->_rsv_end) {
      start = bitmap_offset;

      /* goal 在第二个 bitmap */
      if (bitmap_no > bitmap_no_1) {
        brelse(bh[0]);
        bh[0] = bh[1];
        ret_bitmap_no = bitmap_no_2;
        has_next = 0; // 在第二块的 [bitmap_offset,rsv_end]查找
      #ifdef RSV_DEBUG
        printk("baby_try_to_allocate: goal in second bitmap\n");
      #endif
      }
      /* goal 在第一个 bitmap 上，并且有两个 bitmap */
      else if (bitmap_no_1 != bitmap_no_2)
        end = BABYFS_BIT_PRE_BLOCK; // 在第一块的 [bitmap_offset,bitmap_end]
      /* else，goal 在第一个 bitmap 并只有一个 bitmap 的情况不需要额外修改，就是初始情况 */
    }
    /* goal 不在里面或者 goal=-1（其实就是 goal 不在里面） */
    else {
      goal = -1;
      end = BABYFS_BIT_PRE_BLOCK;
    }
  } 
  /* myrsv 不存在 */
  else {
    if (bitmap_offset > 0)
      start = bitmap_offset;
    else
      start = 0;
    end = (bitmap_no + BABYFS_DATA_BIT_MAP_BLOCK_BASE) == NR_DSTORE_BLOCKS - 1 ? BABY_SB(sb)->last_bitmap_bits : BABYFS_BIT_PRE_BLOCK;

    ret_bitmap_no = bitmap_no;
    bh[0] = sb_bread(sb, bitmap_no + BABYFS_DATA_BIT_MAP_BLOCK_BASE);
    if (!bh[0])
      goto fail;
  }
  #ifdef RSV_DEBUG
    printk("baby_try_to_allocate: num %d start %u end %u goal %lld\n", num, start,
          end, goal);
  #endif
  // 在第一个bitmap中分配
  baby_fsblk_t mod_goal = goal < 0 ? goal : goal % BABYFS_BIT_PRE_BLOCK;
  first = do_allocate(bh[0], &num, start, end, mod_goal, 0);
#ifdef RSV_DEBUG
  printk("baby_try_to_allocate: first %d, get %d, [%u, %u) goal %lld\n",
         first, num, start, end, goal);
#endif

  if (first < 0) {
  #ifdef RSV_DEBUG
    printk("baby_try_to_allocate: first < 0\n");  
  #endif
    if (!has_next) { // 在第一块中分配失败，且没有第二块
      goto fail;
    }
    /* 第一块失败，但是存在第二块的情况，继续往下执行 */
    ret_bitmap_no = bitmap_no_2;
  } else {
    // 在第一块里分配就满足要求了，跳转到成功返回
    if (!(num < *count && has_next && first + num == BABYFS_BIT_PRE_BLOCK))
      goto success;
  }

  // 分配到第一个bitmap末尾都没达到需求的block数量，尝试第二个bitmap
  remain = *count - num; // 剩余需求数量
#ifdef RSV_DEBUG
  printk("baby_try_to_allocate next, remain %lu\n", remain);
#endif
  int ret = do_allocate(bh[1], &remain, 0, my_rsv->_rsv_end % BABYFS_BIT_PRE_BLOCK + 1, 0, 1);
  if(ret < 0 && first < 0) // 第一和第二块都分配失败
    goto fail;
  num += remain;
  if(first < 0)
    first = ret;

success:
  *count = num;
#ifdef RSV_DEBUG
  printk("baby_try_to_allocate return %lld count %ld\n",
         first + BABYFS_BIT_PRE_BLOCK * ret_bitmap_no, *count);
#endif
  return first + BABYFS_BIT_PRE_BLOCK * ret_bitmap_no;

fail:
  return -1;
}

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
static baby_fsblk_t
baby_try_to_allocate_with_rsv(struct super_block *sb, baby_fsblk_t goal,
                              struct baby_reserve_window_node *my_rsv,
                              unsigned long *count) {
  struct baby_sb_info *bbi = BABY_SB(sb);
  baby_fsblk_t ret = 0;
  unsigned long num = *count;

  // bh 数组用来存放可能读取到的相邻两个 bitmap
  struct buffer_head *bh_array[2];
  bh_array[0] = NULL;
  bh_array[1] = NULL;
#ifdef RSV_DEBUG
  printk("baby_try_to_allocate_with_rsv goal %lld count %lu\n", goal, *count);
#endif
  if (my_rsv == NULL) { // (非普通文件)不使用预留窗口分配数据块
  #ifdef RSV_DEBUG
    printk("baby_try_to_allocate_with_rsv not use rsv\n");
  #endif
    return baby_try_to_allocate(sb, goal, count, NULL, bh_array);
  }
#ifdef RSV_DEBUG
  printk("baby_try_to_allocate_with_rsv use rsv\n");
#endif
#ifdef RSV_DEBUG
  dump_myrsv(my_rsv);
#endif

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
        !goal_in_my_reservation(&my_rsv->rsv_window, goal)) {

      // 新分配的预留窗口大小至少等于本次分配需求的数据块个数
      if (my_rsv->rsv_goal_size < *count)
        my_rsv->rsv_goal_size = *count;

      // 重新分配预留窗口
      ret = alloc_new_reservation(my_rsv, goal, sb, bh_array);
      if (ret < 0) // 整个磁盘块都分配不出新的窗口
        break;     /* failed */

      // 分配新的窗口时会尽量使goal在窗口内，若不能办到，则设置goal为-1，表示真正分配时不使用goal
      if (!goal_in_my_reservation(&my_rsv->rsv_window, goal))
        goal = -1;
    } else { // 文件有预留窗口且goal在预留窗口中，只有第一次循环才有可能进入该执行流
      // 计算goal到窗口尾部共有几个空闲块，即可分配区域长度
      int curr = my_rsv->rsv_end - goal + 1;
      // 若可分配长度比需要分配的数量小，则尝试扩大预留窗口到满足所需，也有可能不能扩大这么多
      if (curr < *count)
        try_to_extend_reservation(my_rsv, sb, *count - curr);

      // 读出当前预留窗口的bh
      bh_array[0] = sb_bread(sb, my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK +
                                     BABYFS_DATA_BIT_MAP_BLOCK_BASE);
      if (my_rsv->rsv_start / BABYFS_BIT_PRE_BLOCK !=
          my_rsv->rsv_end / BABYFS_BIT_PRE_BLOCK) {
        bh_array[1] = sb_bread(sb, my_rsv->rsv_end / BABYFS_BIT_PRE_BLOCK +
                                       BABYFS_DATA_BIT_MAP_BLOCK_BASE);
      }
    }

    // 若预留窗口超过了可分配范围，则报错
    if ((my_rsv->rsv_start > bbi->nr_blocks) || (my_rsv->rsv_end < 0)) {
      rsv_window_dump(&bbi->s_rsv_window_root, 1);
      BUG();
    }
    // 将预留窗口中预分配的数据块，在其所属块组位图上对应的bit置1，即正式占用
    ret = baby_try_to_allocate(sb, goal, &num, &my_rsv->rsv_window, bh_array);

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
  unsigned long free_blocks;
  baby_fsblk_t ret_block;
#ifdef RSV_DEBUG
  printk("-----------------------------\n");
  printk("baby_new_blocks: physical goal %lu\n", goal);
#endif
  struct baby_block_alloc_info *block_i = inode_info->i_block_alloc_info;
  if (block_i) {
    my_rsv = &block_i->rsv_window_node;
  }
  // 检查文件系统中剩余块数是否能满足需求数量
  struct baby_sb_info *sb_info = BABY_SB(sb);
  if (sb_info->nr_free_blocks < *count) {
    *err = -ENOSPC;
    goto out;
  }
  // 检查 goal 是否超出
  struct baby_super_block *b_sb = sb_info->s_babysb;
  if (goal < NR_DSTORE_BLOCKS || goal > NR_DSTORE_BLOCKS + b_sb->nr_blocks - 1)
    goal = NR_DSTORE_BLOCKS;

  goal -= NR_DSTORE_BLOCKS;
#ifdef RSV_DEBUG
  printk("baby_new_blocks: logical goal %lu\n", goal);
#endif
  // bitmap 数量
  unsigned long num = *count;
  unsigned int windowsz = 0; // 窗口大小
  if(my_rsv)  // 需要判断，否则会在目录文件为 null 的时候使用 my_rsv
    windowsz = my_rsv->rsv_goal_size;
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

  // 尝试分配
  ret_block = baby_try_to_allocate_with_rsv(sb, goal, my_rsv, &num);
#ifdef RSV_DEBUG
  printk("baby_new_blocks: ret_block %lld goal %lu num %lu\n", ret_block, goal,
         num);
#endif
  if (ret_block >= 0)
    goto allocated;

  // 上面的分配失败可能是因为，为了 my_rsv 分配而产生的问题
  // 不采用预留窗口分配，重新再来一次
  if (my_rsv) {
    my_rsv = NULL;
    goto retry_alloc;
  }
  *err = -ENOSPC;
  goto out;

allocated:
#ifdef RSV_DEBUG
  printk("-----------------------------\n");
#endif

  sb_info->nr_free_blocks -= num;
  *err = 0;
  if (num < *count) {
    *count = num;
    mark_inode_dirty(inode);
  }
  // printk("baby_new_blocks: NR_DSTORE_BLOCKS %ld, ret_block %ld, ret_block + NR_DSTORE_BLOCKS %ld\n", NR_DSTORE_BLOCKS, ret_block, ret_block + NR_DSTORE_BLOCKS);
  return ret_block + NR_DSTORE_BLOCKS;

out:
#ifdef RSV_DEBUG
  printk("-----------------------------\n");
#endif

  return 0;
}