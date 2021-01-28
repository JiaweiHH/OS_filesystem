#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mpage.h>

#include "babyfs.h"

struct inode_operations baby_dir_inode_operations;
struct inode_operations baby_file_inode_operations;

void init_inode_operations(struct inode *inode, umode_t mode) {
  switch (mode & S_IFMT) {
    default:  // 创建除了目录和普通文件之外的其他文件
      // init_special_inode(inode, mode, dev);
      break;
    case S_IFREG:  // 普通文件
      inode->i_op = &baby_file_inode_operations;
      inode->i_fop = &baby_file_operations;
      inode->i_mapping->a_ops = &baby_aops;
      break;
    case S_IFDIR:  // 目录文件
      inode->i_op = &baby_dir_inode_operations;
      inode->i_fop = &baby_dir_operations;
      inode->i_mapping->a_ops = &baby_aops;
      break;
    case S_IFLNK:  // 符号链接文件
      // inode->i_op = &page_symlink_inode_operations;
      // inode_nohighmem(inode);
      break;
  }
}

// 由索引结点编号返回 inode 的磁盘块
// 因为一个块可以存储多个 raw inode，ino 可以指定块内偏移
struct baby_inode *baby_get_raw_inode(struct super_block *sb, ino_t ino,
                                      struct buffer_head **bh) {
  unsigned long inode_block_no =
      BABYFS_INODE_TABLE_BLOCK_BASE +
      ino / BABYFS_INODE_NUM_PER_BLOCK;  // raw inode 所在磁盘块
  unsigned long offset =
      ino % BABYFS_INODE_NUM_PER_BLOCK;  // 该 inode 是块内的第几个
  struct buffer_head *inode_block;
  if (!(inode_block = sb_bread(sb, inode_block_no))) {
    printk(
        "baby_get_raw_inode: unable to read inode block - inode_no=%lu, "
        "block_no=%lu",
        ino, inode_block_no);
    return ERR_PTR(-EIO);
  }
  *bh = inode_block;

  return ((struct baby_inode *)inode_block->b_data) + offset;
}

// 将磁盘中的 inode 读到内存，并新建与之关联的 vfs inode
struct inode *baby_iget(struct super_block *sb, unsigned long ino) {
  struct baby_inode *raw_inode;
  struct inode *vfs_inode;
  struct baby_inode_info *bbi;
  struct buffer_head *bh = NULL;
  long ret = -EIO;
  uid_t i_uid;
  gid_t i_gid;
  int i;

  // 获取 ino 标志的 inode，若在 inode cache 中直接返回，否则分配一个加锁的 vfs
  // inode
  vfs_inode = iget_locked(sb, ino);
  if (!vfs_inode) return ERR_PTR(-ENOMEM);
  if (!(vfs_inode->i_state & I_NEW))  // inode cache 中的 inode 可直接使用
    return vfs_inode;
  bbi = BABY_I(vfs_inode);

  raw_inode = baby_get_raw_inode(sb, ino, &bh);  // 读磁盘的 inode
  if (IS_ERR(raw_inode)) {
    ret = PTR_ERR(raw_inode);
    goto bad_inode;
  }

  // 用读取的 raw inode初始化 vfs inode
  vfs_inode->i_mode = le16_to_cpu(raw_inode->i_mode);
  i_uid = (uid_t)le16_to_cpu(raw_inode->i_uid);
  i_gid = (gid_t)le16_to_cpu(raw_inode->i_gid);
  i_uid_write(vfs_inode, i_uid);
  i_gid_write(vfs_inode, i_gid);
  set_nlink(vfs_inode, le16_to_cpu(raw_inode->i_nlink));
  vfs_inode->i_size = le32_to_cpu(raw_inode->i_size);
  vfs_inode->i_atime.tv_sec = (signed)le32_to_cpu(raw_inode->i_atime);
  vfs_inode->i_ctime.tv_sec = (signed)le32_to_cpu(raw_inode->i_ctime);
  vfs_inode->i_mtime.tv_sec = (signed)le32_to_cpu(raw_inode->i_mtime);
  vfs_inode->i_atime.tv_nsec = vfs_inode->i_mtime.tv_nsec =
      vfs_inode->i_ctime.tv_nsec = 0;
  vfs_inode->i_blocks = le32_to_cpu(raw_inode->i_blocknum);
  bbi->i_subdir_num = le16_to_cpu(raw_inode->i_subdir_num);
  for (i = 0; i < BABYFS_N_BLOCKS; i++) {  // 拷贝数据块索引数组
    bbi->i_blocks[i] = raw_inode->i_blocks[i];
  }
  vfs_inode->i_private = bbi;

  // 初始化操作集合
  init_inode_operations(vfs_inode, vfs_inode->i_mode);

  brelse(bh);
  unlock_new_inode(vfs_inode);
  return vfs_inode;

bad_inode:
  iget_failed(vfs_inode);
  return ERR_PTR(ret);
}

/* address space ops */

typedef struct {
  __le32 *p;               // buffer_head 中的 **相对地址**
  __le32 key;              // p 中存储的值，也就是块号
  struct buffer_head *bh;  // 索引块所在的缓冲区
} Indirect;

// 设置 Indirect 结构体
static inline void add_chain(Indirect *p, struct buffer_head *bh, __le32 *v) {
  p->key = *(p->p = v);
  p->bh = bh;
}

// 确保数据没有变更
static inline int verify_chain(Indirect *from, Indirect *to) {
  while (from <= to && from->key == *from->p) from++;
  return (from > to);
}

/*
 * 填充 chain 数组
 * @depth  : 间接索引的深度
 * @offsets: 间接索引的数组，里面保存了每一级索引的地址
 * @chain  : 存储间接索引数据
 */
static Indirect *baby_get_branch(struct inode *inode, int depth, int *offsets,
                                 Indirect chain[4], int *err) {
  struct super_block *sb = inode->i_sb;
  Indirect *p = chain;
  struct buffer_head *bh;

  *err = 0;
  // 先把最大的那个索引数据填充进去
  add_chain(chain, NULL, BABY_I(inode)->i_blocks + *offsets);
  // printk("BABY_I(inode)->i_blocks[0]: %d", BABY_I(inode)->i_blocks[0]);
  if (!p->key) goto no_block;
  // offset 从上到下保存了每一级索引的地址，只需要按顺序读取就可以了
  while (--depth) {
    bh = sb_bread(sb, le32_to_cpu(p->key));
    if (!bh) goto failure;
    if (!verify_chain(chain, p)) goto changed;
    add_chain(++p, bh, (__le32 *)bh->b_data + *++offsets);
    if (!p->key) goto no_block;
  }
  return NULL;

changed:
  brelse(bh);
  *err = -EAGAIN;
  goto no_block;
failure:
  *err = -EIO;
no_block:
  return p;
}

static int baby_block_to_path(struct inode *inode, long i_block, int offsets[4],
                              int *boundary) {
  int ptrs = BABYFS_BLOCK_SIZE / sizeof(__u32);  // 每一块可以存放的间接地址数量
  int ptr_bits = 8;                              // ptrs 的对数
  const long direct_blocks = BABYFS_DIRECT_BLOCK, indirect_blocks = ptrs,
             double_blocks =
                 1 << (ptr_bits * 2);  // 直接块、一次间接块、二次间接块的数量
  int n = 0, final = 0;
  if (i_block < 0) {
    printk("baby_block_to_path, i_block < 0");
  } else if (i_block < direct_blocks) {
    offsets[n++] = i_block;
    final = direct_blocks;
  } else if ((i_block -= direct_blocks) < indirect_blocks) {
    offsets[n++] = BABYFS_PRIMARY_BLOCK;
    offsets[n++] = i_block;
    final = ptrs;
  } else if ((i_block -= indirect_blocks) < double_blocks) {
    offsets[n++] = BABYFS_SECONDRTY_BLOCK;  // 从二次间接开始
    offsets[n++] = i_block >> ptr_bits;     // i_block / ptrs
    offsets[n++] = i_block & (ptrs - 1);    // i_block % ptrs
    final = ptrs;
  } else if (((i_block -= double_blocks) >> (ptr_bits * 2)) < ptrs) {
    offsets[n++] = BABYFS_THIRD_BLOCKS;        // 从三次间接开始
    offsets[n++] = i_block >> (ptr_bits * 2);  // i_block / double_blocks
    offsets[n++] = (i_block >> ptr_bits) &
                   (ptrs - 1);  // (i_block % double_blocks) / indirect_blocks
    offsets[n++] = i_block & (ptrs - 1);  // i_blocks % ptrs
    final = ptrs;
  }
  // boundary为最后一级索引中从要取的块到最后一块的距离
  if (boundary) *boundary = final - 1 - (i_block & (ptrs - 1));
  return n;
}

// *(ind->p) 表示的数据无效，就近找一个可用的数据块
static unsigned long baby_find_near(struct inode *inode, Indirect *ind) {
  struct baby_inode_info *inode_info = BABY_I(inode);
  // 首先把start指向当前所在的映射块的起点，如果没有，则指向ext2_inode_info中的块数组
  __le32 *start = ind->bh ? (__le32 *)ind->bh->b_data : inode_info->i_blocks;
  __le32 *p;
  /*
   * 1. chain[0] 的数据没有分配，则向左遍历 i_blocks
   * 2. chain[1] ~ chain[3] 的数据块找不到，在 ind->bh 对应的 block
   * 中向左寻找可用块
   */
  for (p = ind->p - 1; p >= start; --p) {
    if (*p) return le32_to_cpu(*p);
  }
  // 没有找到，就找当前的间接块
  if (ind->bh) return ind->bh->b_blocknr;
  // 再没有的话就返回 0，表示接下来不使用 goal
  return 0;
}

/*
 * @block: 逻辑块号
 * @partial: 指向 chain 数组中出现问题（就是没有找到可用的物理块）的那个元素
 * 返回目标物理块号，在 goal 附近寻找可用块
 */
static inline int baby_find_goal(struct inode *inode, sector_t block,
                                 Indirect *partial) {
  struct baby_inode_info *inode_info = BABY_I(inode);
  // i_next_alloc_block 表示下一次要分配的逻辑块号，i_next_alloc_goal
  // 表示下一次可以分配的物理块号
  if (block == inode_info->i_next_alloc_block && inode_info->i_next_alloc_goal)
    return inode_info->i_next_alloc_goal;
  // 说明没有指定下一次要分配的物理块，此时从 partial 附近就近找一个块号
  return baby_find_near(inode, partial);
}

/*
 * 计算实际需要分配的直接块数量
 * @partial: partial 的下一级没有分配数据块
 */
static int baby_blks_to_allocate(Indirect *partial, int indirect_blk,
                                 unsigned long blks_need,
                                 int blocks_to_boundary) {
  unsigned long count = 0;
  if (indirect_blk > 0) {  // partial 表示的不是最后一级间接块
    count += min(blocks_to_boundary + 1, blks_need);
    return count;
  }

  // partial 表示的是最后一级间接块，寻找连续的块
  count++;
  while (count < blks_need && count <= blocks_to_boundary &&
         le32_to_cpu(*(partial[0].p + count)) ==
             0) {  // partial[0] 就是 partial 自身
    count++;
  }
  return count;
}

static unsigned long baby_find_next_usable_block(int start,
                                                 struct buffer_head *bitmap_bh,
                                                 int end) {
  unsigned long here, next;
  if (start > 0) {
  }
}

unsigned long baby_new_blocks(struct inode *inode, unsigned long goal,
                              unsigned long *count, int *err) {
  printk(KERN_INFO "baby_new_blocks------");
  unsigned long ret_block;
  struct super_block *sb = inode->i_sb;
  unsigned long per_block_num = BABYFS_BLOCK_SIZE
                                << 3;  // 每一块可以表示的数据块个数
  unsigned long block_bitmap_bno = 0;
  struct buffer_head *bitmap_bh = NULL;
  unsigned long first = 0;
  unsigned long next = 0;
  unsigned long bitmap_delta = 0;

  // 首先在 goal 附近开始向后寻找可用的块
  if (goal > 0) {
    // data_bitmap 起始搜寻块
    block_bitmap_bno = goal / per_block_num + BABYFS_DATA_BIT_MAP_BLOCK_BASE;
    goal %= per_block_num;
    // 一直搜索直到存储数据的数据块为止
    while (block_bitmap_bno < NR_DSTORE_BLOCKS) {
      bitmap_bh = sb_bread(sb, block_bitmap_bno);
      // TODO 还可以进一步优化。
      // 找到第一个可用的，和第一个不可用的，之间的差值就是可用的数据块数量
      first = baby_find_next_zero_bit(bitmap_bh->b_data, per_block_num - goal,
                                      goal);
      next = baby_find_next_bit((unsigned long *)bitmap_bh->b_data,
                                per_block_num - first, first) %
             (per_block_num + 1);
      if (first) goto got_it;
      block_bitmap_bno++;
      brelse(bitmap_bh);
    }
  } else {
    block_bitmap_bno = BABYFS_DATA_BIT_MAP_BLOCK_BASE;
    while (block_bitmap_bno < NR_DSTORE_BLOCKS) {
      bitmap_bh = sb_bread(sb, block_bitmap_bno);
      first = baby_find_first_zero_bit(bitmap_bh->b_data, per_block_num);
      next = baby_find_next_bit((unsigned long *)bitmap_bh->b_data,
                                per_block_num - first, first) %
             (per_block_num + 1);
      if (first) goto got_it;
      block_bitmap_bno++;
      brelse(bitmap_bh);
    }
  }
got_it:
  if (first) {
    *count = min(*count, next - first);  // 没有达到预期分配的数量
    int i;
    for (i = 0; i < *count; ++i) {
      baby_set_bit(first + i, bitmap_bh->b_data);
    }
    mark_buffer_dirty(bitmap_bh);
    brelse(bitmap_bh);
    printk("baby_new_blocks, first: %d, block_bitmap_bno: %d", first,
           block_bitmap_bno);
    bitmap_delta = block_bitmap_bno - BABYFS_DATA_BIT_MAP_BLOCK_BASE;
    return first + bitmap_delta * per_block_num + NR_DSTORE_BLOCKS;
  }
  return 0;
}

static int baby_alloc_blocks(struct inode *inode, unsigned long goal,
                             int indirect_blks, int blks,
                             unsigned long new_blocks[4], int *err) {
  // 目标分配块数，只保证间接块分配完全并且至少分配一块直接块
  int target = indirect_blks + blks;
  unsigned long count = 0;
  unsigned long current_block = 0;
  unsigned int index = 0;  // new_blocks 数组索引
  int ret = 0, i = 0;
  while (1) {
    count = target;
    current_block = baby_new_blocks(inode, goal, &count, err);
    if (*err) {
      goto failed_out;
    }
    target -= count;
    while (index < indirect_blks && count) {
      new_blocks[index++] = current_block++;
      count--;
    }
    /*
     * count >  0 表示间接块已经全部分配了，并且分配了一定数量的直接块
     * count = 0 表示简介块分配完，但是直接块还没有分配一块
     */
    if (count > 0) break;
  }
  new_blocks[index] = current_block;  // 直接块起始块号
  printk("baby_alloc_blocks end, new_blocks[%d]: %d", index, new_blocks[index]);
  ret = count;  // 直接块的数量
  return ret;

failed_out:
  printk(KERN_ERR "baby_alloc_blocks failed_out");
  for (i = 0; i < index; ++i) {
    // TODO free_block
  }
  if (index) mark_inode_dirty(inode);
  return ret;
}

static int baby_alloc_branch(struct inode *inode, int indirect_blks,
                             unsigned long *blks, unsigned long goal,
                             int *offsets, Indirect *partial) {
  /*
   * 存储分配得到的每一级索引 block 块号，new_blocks
   * 索引编号越小表示的数据块索引数组级别越高只有 4
   * 个元素的原因是：最多只有三级索引加上一个直接块，直接块只需要存储起始块号
   * 因为分配的 block 都是连续的，否则就分配一个数据块
   * baby_alloc_blocks 会返回分配的数据块的数量
   */
  unsigned long new_blocks[4];
  unsigned long current_block;
  int err = 0, i = 0;
  unsigned long num =
      baby_alloc_blocks(inode, goal, indirect_blks, *blks, new_blocks, &err);
  if (err) return err;

  partial[0].key = cpu_to_le32(new_blocks[0]);  // partial 下一级索引对应的块号
  printk("baby_alloc_branch, partial[0].key: %d", partial[0].key);
  int n = 0;
  struct buffer_head *bh = NULL;
  for (n = 1; n <= indirect_blks; ++n) {
    bh = sb_getblk(inode->i_sb,
                   new_blocks[n - 1]);  // 读取 partial 的下一级索引块
    if (unlikely(!bh)) {
      err = -ENOMEM;
      goto failed;
    }
    partial[n].bh = bh;  // 设置下一级索引的 buffer_head
    lock_buffer(bh);
    memset(bh->b_data, 0, BABYFS_BLOCK_SIZE);
    partial[n].p =
        (__le32 *)bh->b_data + offsets[n];  // 下一级索引在 bh 块内偏移
    partial[n].key = cpu_to_le32(new_blocks[n]);  // 设置下一级索引块号

    // partial[indirect_blks] 表示的是一级间接块，因此下面要设置直接块的信息
    if (n == indirect_blks) {
      current_block = new_blocks[n];  // 当前直接块的块号
      int i = 0;
      for (i = 1; i < num; ++i) {  // 直接块是连续的，填充直接块块号
        *(partial[n].p + i) = cpu_to_le32(++current_block);
      }
    }
    set_buffer_uptodate(bh);
    unlock_buffer(bh);
    mark_buffer_dirty_inode(bh, inode);
    if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode)) sync_dirty_buffer(bh);
  }
  *blks = num;  // blks 表示分配的直接块的数量
  return err;

failed:
  printk(KERN_ERR "baby_alloc_branch failed");
  for (i = 1; i < n; ++i) {
    bforget(partial[i].bh);
  }
  for (i = 0; i < indirect_blks; ++i) {
    // TODO free blocks
  }
  return err;
}

static void baby_splice_branch(struct inode *inode, unsigned long block,
                               Indirect *partial, int num, int blks) {
  unsigned long current_block;
  // 那个迷失的 partial 中迷失的那个位置的数据，它后面的索引都已经在
  // alloc_branch 中更改完成了
  *partial->p = partial->key;
  // 如果一开始迷失的就是一级索引
  if (!num && blks > 1) {
    current_block =
        le32_to_cpu(partial->key) + 1;  // partial->p 在上边已经设置了
    int i;
    for (i = 1; i < blks; ++i) {
      *(partial->p + i) = cpu_to_le32(current_block++);
    }
  }
  // 更新 next_block
  struct baby_inode_info *inode_info = BABY_I(inode);
  inode_info->i_next_alloc_block = block + blks - 1;
  inode_info->i_next_alloc_goal = le32_to_cpu(partial[num].key + blks - 1);
  if (partial->bh) mark_buffer_dirty_inode(partial->bh, inode);
  inode->i_ctime = current_time(inode);
  mark_inode_dirty(inode);
}

static int baby_get_blocks(struct inode *inode, sector_t block,
                           unsigned long maxblocks, struct buffer_head *bh,
                           int create) {
  int err = -EIO;
  int offset[4] = {99};  // 存放 block 的索引信息
  Indirect chain[4];     // 读取索引信息，存放数据
  Indirect *partial;
  int blocks_to_boundary =
      0;  // boundary 为最后一级间接块中从要取的块到最后一块的距离
  // 获取索引深度，直接索引是 0
  int depth = baby_block_to_path(inode, block, offset, &blocks_to_boundary);
  if (!depth) return err;

  printk("depth: %d", depth);
  printk("offset[0]: %d, offset[1]: %d, offset[2]: %d, offset[3]: %d",
         offset[0], offset[1], offset[2], offset[3]);

  // 读取索引信息，返回 NULL 表示找到了所有的。partial 不为 NULL 说明 partial
  // 的下一级没有分配数据块
  partial = baby_get_branch(inode, depth, offset, chain, &err);
  if (!partial) {
    printk(KERN_INFO "partial==NULL, chain[depth - 1].key: %d",
           chain[depth - 1].key);
    goto got_it;
  }
  printk(KERN_INFO "partial!=NULL, chain[depth - 1].key: %d",
         chain[depth - 1].key);

  if (!create || err == -EIO) goto clean_up;

  /* 开始分配数据块 */
  unsigned long goal = baby_find_goal(inode, block, partial);
  unsigned long indirect_blk =
      chain + depth - partial - 1;  // 计算需要分配的间接块的数量
  unsigned long count = baby_blks_to_allocate(partial, indirect_blk, maxblocks,
                                              blocks_to_boundary);
  err = baby_alloc_branch(inode, indirect_blk, &count, goal,
                          offset + (partial - chain), partial);
  if (err) goto clean_up;
  // 收尾工作，此时的 count 表示直接块的数量
  baby_splice_branch(inode, block, partial, indirect_blk, count);

got_it:
  map_bh(bh, inode->i_sb, le32_to_cpu(chain[depth - 1].key));
  partial = chain + depth - 1;
clean_up:
  printk("clean_up, chain[depth - 1]: %d", chain[depth - 1].key);
  while (partial > chain) {
    brelse(partial->bh);
    partial--;
  }
  return err;
}

int baby_get_block(struct inode *inode, sector_t block, struct buffer_head *bh,
                   int create) {
  unsigned maxblocks = bh->b_size / inode->i_sb->s_blocksize;
  int ret = baby_get_blocks(inode, block, maxblocks, bh, create);
  return ret;
}

static int baby_readpage(struct file *file, struct page *page) {
  printk(KERN_INFO "baby_readpage 调用了");
  return mpage_readpage(page, baby_get_block);
}

static int baby_writepage(struct page *page, struct writeback_control *wbc) {
  return block_write_full_page(page, baby_get_block, wbc);
}

static int baby_writepages(struct address_space *mapping,
                           struct writeback_control *wbc) {
  return mpage_writepages(mapping, wbc, baby_get_block);
}

static int baby_write_end(struct file *file, struct address_space *mapping,
                          loff_t pos, unsigned len, unsigned copied,
                          struct page *page, void *fsdata) {
  int ret;

  ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
  // TODO if (ret < 0)
  return ret;
}

static int baby_write_begin(struct file *file, struct address_space *mapping,
                            loff_t pos, unsigned len, unsigned flags,
                            struct page **pagep, void **fsdata) {
  int ret;

  ret = block_write_begin(mapping, pos, len, flags, pagep, baby_get_block);
  // TODO if (ret < len)
  return ret;
}

const struct address_space_operations baby_aops = {
    .readpage = baby_readpage,
    .writepage = baby_writepage,
    .writepages = baby_writepages,
    .write_end = baby_write_end,
    .write_begin = baby_write_begin,
};

// 将一个 inode 写回到磁盘上，(baby_inode_info, vfs_inode)->raw_inode
int baby_write_inode(struct inode *inode, struct writeback_control *wbc) {
  struct super_block *sb = inode->i_sb;
  struct baby_inode_info *bbi = BABY_I(inode);
  struct baby_inode *raw_inode;
  struct buffer_head *bh;
  int i;
  int ret = 0;

  // 读取 vfs_inode 对应的磁盘 inode
  raw_inode = baby_get_raw_inode(sb, inode->i_ino, &bh);  // 读磁盘的 inode
  if (IS_ERR(raw_inode)) {
    return PTR_ERR(raw_inode);
  }

  // 用 vfs_inode 的数据设置磁盘 inode
  raw_inode->i_mode = cpu_to_le16(inode->i_mode);
  raw_inode->i_uid = cpu_to_le16(i_uid_read(inode));
  raw_inode->i_gid = cpu_to_le16(i_gid_read(inode));
  raw_inode->i_size = cpu_to_le32(inode->i_size);
  raw_inode->i_atime = cpu_to_le32(inode->i_atime.tv_sec);
  raw_inode->i_ctime = cpu_to_le32(inode->i_ctime.tv_sec);
  raw_inode->i_mtime = cpu_to_le32(inode->i_mtime.tv_sec);
  raw_inode->i_blocknum = cpu_to_le32(inode->i_blocks);
  raw_inode->i_nlink = cpu_to_le16(inode->i_nlink);
  raw_inode->i_subdir_num = cpu_to_le16(bbi->i_subdir_num);
  for (i = 0; i < BABYFS_N_BLOCKS; i++) {
    raw_inode->i_blocks[i] = bbi->i_blocks[i];
  }

  mark_buffer_dirty(bh);
  if (wbc->sync_mode == WB_SYNC_ALL) {  // 支持同步写
    sync_dirty_buffer(bh);
    if (buffer_req(bh) && !buffer_uptodate(bh)) ret = -EIO;
  }
  brelse(bh);

  return ret;
}

// 创建一个新的 raw inode，并返回其对应的 vfs inode
struct inode *baby_new_inode(struct inode *dir, umode_t mode,
                             const struct qstr *qstr) {
  struct inode *inode;
  struct baby_inode_info *bbi;
  struct super_block *sb = dir->i_sb;
  struct buffer_head *bh_bitmap;
  int i_no;
  int err;

  inode = new_inode(sb);  // 获取一个 vfs 索引节点
  if (!inode) return ERR_PTR(-ENOMEM);
  bbi = BABY_I(inode);

  // 读 inode 分配位图
  // TODO 当前 inode 分配位图只占了一个磁盘块，要支持多块的话这里要改成循环
  bh_bitmap = sb_bread(sb, BABYFS_INODE_BIT_MAP_BLOCK_BASE);
  // 寻找第一个空闲的位
  i_no = baby_find_first_zero_bit(bh_bitmap->b_data, BABYFS_BIT_PRE_BLOCK);
  if (i_no >= BABYFS_BIT_PRE_BLOCK) {
    brelse(bh_bitmap);
    err = -ENOSPC;
    goto fail;
  }

  baby_set_bit(i_no, bh_bitmap->b_data);  // 占用这一位
  mark_buffer_dirty(bh_bitmap);
  brelse(bh_bitmap);

  // 设置 inode 的属性
  inode_init_owner(inode, dir, mode);
  inode->i_ino = i_no;
  inode->i_blocks = 0;
  inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
  bbi->i_subdir_num = 0;
  // bbi->i_blocks[0] = i_no + NR_DSTORE_BLOCKS; // 新 inode 的第一个数据块号
  memset(bbi->i_blocks, 0, sizeof(bbi->i_blocks));  // 初始化索引数组
  if (insert_inode_locked(inode) <
      0) {  // 将新申请的 vfs inode 添加到inode cache 的 hash 表中，并设置 inode
            // 的 i_state 状态
    printk(KERN_ERR "baby_new_inode: inode number already in use - inode = %lu",
           i_no);
    err = -EIO;
    goto fail;
  }
  // 初始化操作集合
  init_inode_operations(inode, inode->i_mode);

  mark_inode_dirty(inode);

  printk("baby_new_inode: alloc new inode ino: %d\n", i_no);
  return inode;

fail:
  make_bad_inode(inode);
  iput(inode);
  return ERR_PTR(err);
}

static inline int parentdir_add_inode(struct dentry *dentry,
                                      struct inode *inode) {
  int err = baby_add_link(dentry, inode);  // 在父目录中添加一个目录项
  if (!err) {
    d_instantiate_new(dentry, inode);  // 将 inode 与 dentry 相关联
    return 0;
  }

  printk(KERN_ERR "parentdir_add_inode:baby_add_link failed!\n");
  inode_dec_link_count(inode);
  unlock_new_inode(inode);
  iput(inode);
  return err;
}

// 创建普通文件
static int baby_create(struct inode *dir, struct dentry *dentry, umode_t mode,
                       bool excl) {
  struct inode *inode = baby_new_inode(dir, mode, &dentry->d_name);

  if (IS_ERR(inode)) {
    printk(KERN_ERR "baby_create: get new inode failed!\n");
    return PTR_ERR(inode);
  }

  return parentdir_add_inode(dentry, inode);
}

// 创建目录
static int baby_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) {
  int ret = 0;
  struct inode *inode;
  inode_inc_link_count(
      dir);  // 父目录下添加子目录，需要将父目录的引用计数加一，因为子目录的“..”
  inode = baby_new_inode(dir, S_IFDIR | mode, &dentry->d_name);
  if (IS_ERR(inode)) {
    printk(KERN_ERR "baby_mkdir: get new inode failed!\n");
    ret = PTR_ERR(inode);
    goto out;
  }
  inode_inc_link_count(inode);        // 新增的子目录引用计数为 2
  ret = baby_make_empty(inode, dir);  // 增加 . 和 .. 目录项
  if (ret) {
    printk(KERN_ERR "baby_mkdir:add . and .. failed!\n");
    goto link_fail;
  }
  mark_inode_dirty(inode);

  ret = baby_add_link(dentry, inode);
  if (ret) {
    printk(KERN_ERR "baby_mkdir:baby_add_link failed!\n");
    goto link_fail;
  }

  d_instantiate_new(dentry, inode);
  printk("d_instance");
  return 0;

link_fail:  // 释放子目录的 inode
  inode_dec_link_count(inode);
  inode_dec_link_count(inode);
  unlock_new_inode(inode);
  iput(inode);
out:
  inode_dec_link_count(inode);
  return ret;
}

/*
 * 根据父目录和文件名查找 inode，关联目录项；需要从磁盘文件系统根据 ino 读取
 * inode 信息
 */
struct dentry *baby_lookup(struct inode *dir, struct dentry *dentry,
                           unsigned int flags) {
  // printk(KERN_INFO "lookup调用");
  struct inode *inode;
  unsigned int ino;
  if (dentry->d_name.len > BABYFS_FILENAME_MAX_LEN)
    return ERR_PTR(-ENAMETOOLONG);
  // 从父 inode 中根据文件名查找 ino
  ino = baby_inode_by_name(dir, &dentry->d_name);
  inode = NULL;
  if (ino) {
    inode = baby_iget(dir->i_sb, ino);
    if (inode == ERR_PTR(-ESTALE)) return ERR_PTR(-EIO);
  }
  return d_splice_alias(inode, dentry);
}

struct inode_operations baby_dir_inode_operations = {
    .lookup = baby_lookup,  //
    .create = baby_create,  // 新建文件
    .mkdir = baby_mkdir,    // 新建目录
    .getattr = simple_getattr,
};

struct inode_operations baby_file_inode_operations = {
    .getattr = simple_getattr,
    .setattr = simple_setattr,
};
