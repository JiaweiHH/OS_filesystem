#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mpage.h>

#include "babyfs.h"

struct inode_operations baby_dir_inode_operations;
struct inode_operations baby_file_inode_operations;
struct inode_operations baby_symlink_inode_operations;

void baby_free_blocks(struct inode *inode, unsigned long block,
                      unsigned long count);

// 根据文件类型初始化文件inode的操作集合
void file_type_special_operation(struct inode *inode, umode_t mode) {
  switch (mode & S_IFMT) {
    default:  // 创建除了目录和普通文件之外的其他文件
      // init_special_inode(inode, mode, dev);
      break;
    case S_IFREG:  // 普通文件
      inode->i_op = &baby_file_inode_operations;
      inode->i_fop = &baby_file_operations;
      inode->i_mapping->a_ops = &baby_aops;

      // 普通文件的磁盘块分配使用预留窗口加速
      baby_init_block_alloc_info(inode);
      break;
    case S_IFDIR:  // 目录文件
      inode->i_op = &baby_dir_inode_operations;
      inode->i_fop = &baby_dir_operations;
      inode->i_mapping->a_ops = &baby_aops;
      break;
    case S_IFLNK:  // 符号链接文件
      // TODO 支持快速符号链接
      inode->i_op = &baby_symlink_inode_operations;
      inode_nohighmem(inode);
      inode->i_mapping->a_ops = &baby_aops;
      break;
  }
}

// 由索引结点编号返回 inode 的磁盘块
// 因为一个块可以存储多个 raw inode，ino 可以指定块内偏移
struct baby_inode *baby_get_raw_inode(struct super_block *sb, ino_t ino,
                                      struct buffer_head **bh) {
  unsigned long inode_block_no =
      BABYFS_INODE_TABLE_BLOCK_BASE +
      ino / BABYFS_INODE_NUM_PER_BLOCK; // raw inode 所在磁盘块
  unsigned long offset =
      ino % BABYFS_INODE_NUM_PER_BLOCK; // 该 inode 是块内的第几个
  struct buffer_head *inode_block;
  if (!(inode_block = sb_bread(sb, inode_block_no))) {
    printk("baby_get_raw_inode: unable to read inode block - inode_no=%lu, "
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
  if (!vfs_inode)
    return ERR_PTR(-ENOMEM);
  if (!(vfs_inode->i_state & I_NEW)) // inode cache 中的 inode 可直接使用
    return vfs_inode;
  bbi = BABY_I(vfs_inode);

  raw_inode = baby_get_raw_inode(sb, ino, &bh); // 读磁盘的 inode
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
  vfs_inode->i_size = le64_to_cpu(raw_inode->i_size);
  vfs_inode->i_atime.tv_sec = (signed)le32_to_cpu(raw_inode->i_atime);
  vfs_inode->i_ctime.tv_sec = (signed)le32_to_cpu(raw_inode->i_ctime);
  vfs_inode->i_mtime.tv_sec = (signed)le32_to_cpu(raw_inode->i_mtime);
  vfs_inode->i_atime.tv_nsec = vfs_inode->i_mtime.tv_nsec =
      vfs_inode->i_ctime.tv_nsec = 0;
  vfs_inode->i_blocks = le32_to_cpu(raw_inode->i_blocknum);
  bbi->i_subdir_num = le16_to_cpu(raw_inode->i_subdir_num);
  bbi->i_block_alloc_info = NULL;
  for (i = 0; i < BABYFS_N_BLOCKS; i++) { // 拷贝数据块索引数组
    bbi->i_blocks[i] = raw_inode->i_blocks[i];
  }
  vfs_inode->i_private = bbi;

  // 根据文件类型执行特定操作
  file_type_special_operation(vfs_inode, vfs_inode->i_mode);

  brelse(bh);
  unlock_new_inode(vfs_inode);
  return vfs_inode;

bad_inode:
  iget_failed(vfs_inode);
  return ERR_PTR(ret);
}

/* address space ops */

typedef struct {
  __le32 *p;              // buffer_head 中的 **相对地址**
  __le32 key;             // p 中存储的值，也就是块号
  struct buffer_head *bh; // 索引块所在的缓冲区
} Indirect;

// 设置 Indirect 结构体
static inline void add_chain(Indirect *p, struct buffer_head *bh, __le32 *v) {
  p->key = *(p->p = v);
  p->bh = bh;
}

// 确保数据没有变更
static inline int verify_chain(Indirect *from, Indirect *to) {
  while (from <= to && from->key == *from->p)
    from++;
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
  // printk("p->key: %ld\n", p->key);
  if (!p->key)
    goto no_block;
  // offset 从上到下保存了每一级索引的地址，只需要按顺序读取就可以了
  while (--depth) {
    bh = sb_bread(sb, le32_to_cpu(p->key));
    if (!bh)
      goto failure;
    if (!verify_chain(chain, p))
      goto changed;
    add_chain(++p, bh, (__le32 *)bh->b_data + *++offsets);
    if (!p->key)
      goto no_block;
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
  int ptrs = BABYFS_BLOCK_SIZE / sizeof(__u32); // 每一块可以存放的间接地址数量
  int ptr_bits = 8;                             // ptrs 的对数
  const long direct_blocks = BABYFS_DIRECT_BLOCK, indirect_blocks = ptrs,
             double_blocks =
                 1 << (ptr_bits * 2); // 直接块、一次间接块、二次间接块的数量
  int n = 0, final = 0;
  if (i_block < 0) {
    printk(KERN_ERR "baby_block_to_path, i_block < 0");
  } else if (i_block < direct_blocks) {
    offsets[n++] = i_block;
    final = direct_blocks;
  } else if ((i_block -= direct_blocks) < indirect_blocks) {
    offsets[n++] = BABYFS_PRIMARY_BLOCK;
    offsets[n++] = i_block;
    final = ptrs;
  } else if ((i_block -= indirect_blocks) < double_blocks) {
    offsets[n++] = BABYFS_SECONDRTY_BLOCK; // 从二次间接开始
    offsets[n++] = i_block >> ptr_bits;    // i_block / ptrs
    offsets[n++] = i_block & (ptrs - 1);   // i_block % ptrs
    final = ptrs;
  } else if (((i_block -= double_blocks) >> (ptr_bits * 2)) < ptrs) {
    offsets[n++] = BABYFS_THIRD_BLOCKS;       // 从三次间接开始
    offsets[n++] = i_block >> (ptr_bits * 2); // i_block / double_blocks
    offsets[n++] = (i_block >> ptr_bits) &
                   (ptrs - 1); // (i_block % double_blocks) / indirect_blocks
    offsets[n++] = i_block & (ptrs - 1); // i_blocks % ptrs
    final = ptrs;
  }
  // boundary为最后一级索引中从要取的块到最后一块的距离
  if (boundary)
    *boundary = final - 1 - (i_block & (ptrs - 1));
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
    if (*p)
      return le32_to_cpu(*p);
  }
  // 没有找到，就找当前的间接块
  if (ind->bh)
    return ind->bh->b_blocknr;
  // 再没有的话就返回数据块的起始块号，表示从第一个数据块开始找
  return NR_DSTORE_BLOCKS;
}

/*
 * @block: 逻辑块号
 * @partial: 指向 chain 数组中出现问题（就是没有找到可用的物理块）的那个元素
 * 返回目标物理块号，在 goal 附近寻找可用块
 */
static inline int baby_find_goal(struct inode *inode, sector_t block,
                                 Indirect *partial) {
  struct baby_inode_info *inode_info = BABY_I(inode);
  struct baby_block_alloc_info *block_i = inode_info->i_block_alloc_info;

  // i_next_alloc_block 表示下一次要分配的逻辑块号，i_next_alloc_goal
  // 表示下一次可以分配的物理块号
  if (block_i && (block == block_i->last_alloc_logical_block + 1) &&
      (block_i->last_alloc_physical_block != 0)) {
    return block_i->last_alloc_physical_block + 1;
  }
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
  if (indirect_blk > 0) { // partial 表示的不是最后一级间接块
    count += min(blocks_to_boundary + 1, blks_need);
    return count;
  }

  // partial 表示的是最后一级间接块，寻找连续的块
  count++;
  while (count < blks_need && count <= blocks_to_boundary &&
         le32_to_cpu(*(partial[0].p + count)) ==
             0) { // partial[0] 就是 partial 自身
    count++;
  }
  return count;
}

static int baby_alloc_blocks(struct inode *inode, unsigned long goal,
                             int indirect_blks, int blks,
                             unsigned long new_blocks[4], int *err) {
  // 目标分配块数，只保证间接块分配完全并且至少分配一块直接块
  int target = indirect_blks + blks;
  unsigned long count = 0;
  unsigned long next_goal = goal;
  unsigned long current_block = 0;
  unsigned int index = 0; // new_blocks 数组索引
  int ret = 0, i = 0;
  while (1) {
    count = target;
    current_block = baby_new_blocks(inode, next_goal, &count, err);
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
    if (count > 0) {
      break;
    }
  }
  new_blocks[index] = current_block; // 直接块起始块号
  ret = count;                       // 直接块的数量
  *err = 0;
  return ret;

failed_out:
  printk(KERN_ERR "baby_alloc_blocks failed_out\n");
  for (i = 0; i < index; ++i) { // 依次释放所有已分配的磁盘块
    baby_free_blocks(inode, new_blocks[i], 1);
  }
  if (index)
    mark_inode_dirty(inode);
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
  unsigned long new_blocks[4] = {0};
  unsigned long current_block;
  int err = 0, i = 0;
  unsigned long num =
      baby_alloc_blocks(inode, goal, indirect_blks, *blks, new_blocks, &err);
  if (err)
    return err;

  partial[0].key = cpu_to_le32(new_blocks[0]); // partial 下一级索引对应的块号
  int n = 0;
  struct buffer_head *bh = NULL;
  for (n = 1; n <= indirect_blks; ++n) {
    bh = sb_getblk(inode->i_sb,
                   new_blocks[n - 1]); // 读取 partial 的下一级索引块
    if (unlikely(!bh)) {
      err = -ENOMEM;
      goto failed;
    }
    partial[n].bh = bh; // 设置下一级索引的 buffer_head
    lock_buffer(bh);
    memset(bh->b_data, 0, BABYFS_BLOCK_SIZE);
    partial[n].p =
        (__le32 *)bh->b_data + offsets[n]; // 下一级索引在 bh 块内偏移
    partial[n].key = cpu_to_le32(new_blocks[n]); // 设置下一级索引块号
    *(partial[n].p) = partial[n].key; // 设置间接块中的第一个直接块块号

    // partial[indirect_blks] 表示的是一级间接块，因此下面要设置直接块的信息
    if (n == indirect_blks) {
      current_block = new_blocks[n]; // 当前直接块的块号
      int i = 0;
      for (i = 1; i < num; ++i) { // 直接块是连续的，填充直接块块号
        *(partial[n].p + i) = cpu_to_le32(++current_block);
      }
    }
    set_buffer_uptodate(bh);
    unlock_buffer(bh);
    mark_buffer_dirty_inode(bh, inode);
    if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
      sync_dirty_buffer(bh);
  }
  *blks = num; // blks 表示分配的直接块的数量
  return err;

failed:
  printk(KERN_ERR "baby_alloc_branch failed");
  for (i = 1; i < n; ++i) {
    bforget(partial[i].bh);
  }
  for (i = 0; i < indirect_blks; ++i) { // 释放索引块
    baby_free_blocks(inode, new_blocks[i], 1);
  }
  baby_free_blocks(inode, new_blocks[i], num); // 释放数据块
  return err;
}

/*
 * @num: 间接块数量
 * @blks: 直接块数量
 */
static void baby_splice_branch(struct inode *inode, unsigned long block,
                               Indirect *partial, int num, int blks) {
  unsigned long current_block;
  // 那个迷失的 partial 中迷失的那个位置的数据，它后面的索引都已经在
  // alloc_branch 中更改完成了
  *partial->p = partial->key;
  // 如果一开始迷失的就是一级索引
  if (!num && blks > 1) {
    current_block =
        le32_to_cpu(partial->key) + 1; // partial->p 在上边已经设置了
    int i;
    for (i = 1; i < blks; ++i) {
      *(partial->p + i) = cpu_to_le32(current_block++);
    }
  }
  
  struct baby_inode_info *inode_info = BABY_I(inode);
  struct baby_block_alloc_info *block_i = inode_info->i_block_alloc_info;
  block_i->last_alloc_logical_block = block + blks - 1;
  block_i->last_alloc_physical_block = le32_to_cpu(partial[num].key) + blks - 1;
  if (partial->bh)
    mark_buffer_dirty_inode(partial->bh, inode);
  inode->i_ctime = current_time(inode);
  mark_inode_dirty(inode);
}

static int baby_get_blocks(struct inode *inode, sector_t block,
                           unsigned long maxblocks, struct buffer_head *bh,
                           int create) {
  int err = -EIO;
  int offset[4] = {99}; // 存放 block 的索引信息
  Indirect chain[4];    // 读取索引信息，存放数据
  Indirect *partial;
  struct super_block *sb = inode->i_sb;
  struct baby_sb_info *sb_info = BABY_SB(sb);
  int blocks_to_boundary =
      0; // boundary 为最后一级间接块中从要取的块到最后一块的距离
  // 获取索引深度，直接索引是 0
  int depth = baby_block_to_path(inode, block, offset, &blocks_to_boundary);
  if (!depth)
    return err;

  // 读取索引信息，返回 NULL 表示找到了所有的。partial 不为 NULL 说明 partial
  // 的下一级没有分配数据块
  partial = baby_get_branch(inode, depth, offset, chain, &err);
  if (!partial) {
    // printk("partial == NULL\n");
    goto got_it;
  }

  if (!create || err == -EIO)
    goto clean_up;

  /* 开始分配数据块，如果 find_goal 返回
   * 0，就让它等于数据块起始位置，这样可以避免在分配的时候 if-else 判断 */
  unsigned long temp = baby_find_goal(inode, block, partial);
  if (!temp)
    temp = NR_DSTORE_BLOCKS;
  unsigned long goal =
      (temp - NR_DSTORE_BLOCKS) % (sb_info->nr_blocks) + NR_DSTORE_BLOCKS;
  unsigned long indirect_blk =
      chain + depth - partial - 1; // 计算需要分配的间接块的数量
  unsigned long count = baby_blks_to_allocate(partial, indirect_blk, maxblocks,
                                              blocks_to_boundary);
  err = baby_alloc_branch(inode, indirect_blk, &count, goal,
                          offset + (partial - chain), partial);
  if (err)
    goto clean_up;
  // 收尾工作，此时的 count 表示直接块的数量
  baby_splice_branch(inode, block, partial, indirect_blk, count);

got_it:
  map_bh(bh, inode->i_sb, le32_to_cpu(chain[depth - 1].key));
  partial = chain + depth - 1;
clean_up:
  // printk("baby_get_blocks: phy_block no: %ld, logic_block no: %ld\n",
  //        chain[depth - 1].key, block);
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

int __baby_write_inode(struct inode *inode, int do_sync) {
  struct super_block *sb = inode->i_sb;
  struct baby_inode_info *bbi = BABY_I(inode);
  struct baby_inode *raw_inode;
  struct buffer_head *bh;
  int i;
  int ret = 0;

  // 读取 vfs_inode 对应的磁盘 inode
  raw_inode = baby_get_raw_inode(sb, inode->i_ino, &bh); // 读磁盘的 inode
  if (IS_ERR(raw_inode)) {
    return PTR_ERR(raw_inode);
  }

  // 用 vfs_inode 的数据设置磁盘 inode
  raw_inode->i_mode = cpu_to_le16(inode->i_mode);
  raw_inode->i_uid = cpu_to_le16(i_uid_read(inode));
  raw_inode->i_gid = cpu_to_le16(i_gid_read(inode));
  raw_inode->i_size = cpu_to_le64(inode->i_size);
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
  if (do_sync == WB_SYNC_ALL) { // 支持同步写
    sync_dirty_buffer(bh);
    if (buffer_req(bh) && !buffer_uptodate(bh))
      ret = -EIO;
  }
  brelse(bh);

  return ret;
}

// 将一个 inode 写回到磁盘上，(baby_inode_info, vfs_inode)->raw_inode
int baby_write_inode(struct inode *inode, struct writeback_control *wbc) {
  return __baby_write_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
}

// 创建一个新的 raw inode，并返回其对应的 vfs inode
struct inode *baby_new_inode(struct inode *dir, umode_t mode,
                             const struct qstr *qstr) {
  struct inode *inode;
  struct baby_inode_info *bbi;
  struct super_block *sb = dir->i_sb;
  struct baby_sb_info *sb_info;
  struct buffer_head *bh_bitmap;
  int i_no;
  int err;

  inode = new_inode(sb); // 获取一个 vfs 索引节点
  if (!inode)
    return ERR_PTR(-ENOMEM);
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

  baby_set_bit(i_no, bh_bitmap->b_data); // 占用这一位
  mark_buffer_dirty(bh_bitmap);
  brelse(bh_bitmap);

  // 设置 inode 的属性
  inode_init_owner(inode, dir, mode);
  inode->i_ino = i_no;
  inode->i_blocks = 0;
  inode->i_size = 0;
  inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
  bbi->i_subdir_num = 0;
  bbi->i_block_alloc_info = NULL;
  // bbi->i_blocks[0] = i_no + NR_DSTORE_BLOCKS; // 新 inode 的第一个数据块号
  memset(bbi->i_blocks, 0, sizeof(bbi->i_blocks)); // 初始化索引数组
  // 将新申请的 vfs inode 添加到inode cache 的 hash 表中，
  // 并设置 inode 的 i_state 状态
  if (insert_inode_locked(inode) < 0) {
    printk(KERN_ERR "baby_new_inode: inode number already in use - inode = %u",
           i_no);
    err = -EIO;
    goto fail;
  }
  // 根据文件类型执行特定操作
  file_type_special_operation(inode, inode->i_mode);

  mark_inode_dirty(inode);

  // printk("baby_new_inode: alloc new inode ino: %d\n", i_no);
  sb_info = BABY_SB(sb);
  sb_info->nr_free_inodes--;
  return inode;

fail:
  make_bad_inode(inode);
  iput(inode);
  return ERR_PTR(err);
}

static inline int parentdir_add_inode(struct dentry *dentry,
                                      struct inode *inode) {
  int err = baby_add_link(dentry, inode); // 在父目录中添加一个目录项
  if (!err) {
    d_instantiate_new(dentry, inode); // 将 inode 与 dentry 相关联
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

  // 因为子目录的“..”指向父目录，故需要将父目录的硬链接计数加一
  inode_inc_link_count(dir);
  inode = baby_new_inode(dir, S_IFDIR | mode, &dentry->d_name);
  if (IS_ERR(inode)) {
    printk(KERN_ERR "baby_mkdir: get new inode failed!\n");
    ret = PTR_ERR(inode);
    goto out;
  }
  inode_inc_link_count(inode);       // 新增的子目录引用计数为 2
  ret = baby_make_empty(inode, dir); // 增加 . 和 .. 目录项
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

link_fail: // 释放子目录的 inode
  inode_dec_link_count(inode);
  inode_dec_link_count(inode);
  unlock_new_inode(inode);
  iput(inode);
out:
  inode_dec_link_count(inode);
  return ret;
}

/**
 * @brief 创建一个软（符号）链接文件
 *
 * @param dir 链接文件所在目录
 * @param dentry 链接文件的目录项
 * @param symname 链接文件存储的源文件路径
 * @return int
 */
static int baby_symlink(struct inode *dir, struct dentry *dentry,
                        const char *symname) {
  int err = -ENAMETOOLONG;
  int l = strlen(symname) + 1; /*源文件路径长度*/
  struct inode *inode;

  if (l > BABYFS_BLOCK_SIZE) // 源文件路径长度不能大于一个磁盘块大小
    goto out;

  inode = baby_new_inode(dir, S_IFLNK | S_IRWXUGO, &dentry->d_name);
  err = PTR_ERR(inode);
  if (IS_ERR(inode)) {
    printk(KERN_ERR "baby_symlink: get new inode failed!\n");
    goto out;
  }
  err = page_symlink(inode, symname, l); // 将文件内容初始化为符号链接路径
  if (err) {
    printk(KERN_ERR "baby_symlink: page_symlink failed!\n");
    goto out_fail;
  }

  mark_inode_dirty(inode);
  err = parentdir_add_inode(dentry, inode);

out:
  return err;
out_fail:
  inode_dec_link_count(inode);
  unlock_new_inode(inode);
  iput(inode);
  goto out;
}

/**
 * @brief 在父目录 dir 下建立 dentry 到 old_dentry->inode 的硬链接
 *
 * @param old_dentry 现存的目录项
 * @param dir 新目录项的父目录
 * @param dentry 新目录项
 * @return int
 */
static int baby_link(struct dentry *old_dentry, struct inode *dir,
                     struct dentry *dentry) {
  struct inode *inode = d_inode(old_dentry); // 获得目标文件的inode
  int err;

  inode->i_ctime = current_time(inode);
  inode_inc_link_count(inode); // 目标inode的硬链接计数+1
  ihold(inode);                // 索引节点的引用计数+1

  err = baby_add_link(dentry, inode); // 父目录dentry->parent中新增目录项dentry
  if (!err) {
    d_instantiate(dentry, inode);
    return 0;
  }
  printk(KERN_ERR "baby_link: baby_add_link failed!\n");
  inode_dec_link_count(inode);
  iput(inode);
  return err;
}

// vfs删除文件时调用unlink，删除目录时调用rmdir，这两个函数都只做了目录项的删除，而没有删除文件内容
// 从父目录dir中删除目录项dentry
static int baby_unlink(struct inode *dir, struct dentry *dentry) {
  struct inode *inode = d_inode(dentry);
  struct dir_record *de;
  struct page *page;
  int err;

  de = baby_find_entry(dir, &dentry->d_name, &page); // 查找待删除的目录项
  if (!de) {
    err = -ENOENT;
    goto out;
  }

  err = baby_delete_entry(de, page); // 从目录项存储的 page 中删除目标目录项
  if (err)
    goto out;

  /*修改时间和减少这个被删除目录项 inode 的引用计数*/
  inode->i_ctime = dir->i_ctime;
  inode_dec_link_count(inode);
  err = 0;
out:
  return err;
}

/*删除目录，dir 是删除的目录的父目录，dentry 是待删除的目录的 dentry*/
static int baby_rmdir(struct inode *dir, struct dentry *dentry) {
  struct inode *inode = d_inode(dentry); // 待删除文件 inode
  int err = -ENOTEMPTY;

  if (baby_empty_dir(
          inode)) { // 检查待删除的目录是否为空，只有空的目录才能被删除
    err = baby_unlink(dir, dentry); // 从父目录中删除自身的目录项
    if (!err) { /*把目标文件大小置为0，减少引用计数*/
      inode->i_size = 0;
      inode_dec_link_count(inode);
      inode_dec_link_count(dir); // 子目录的“..”指向父目录
    }
  }
  return err;
}

/*
 * 根据父目录和文件名查找 inode，关联目录项；需要从磁盘文件系统根据 ino 读取
 * inode 信息
 */
struct dentry *baby_lookup(struct inode *dir, struct dentry *dentry,
                           unsigned int flags) {
  struct inode *inode;
  unsigned int ino;
  if (dentry->d_name.len > BABYFS_FILENAME_MAX_LEN)
    return ERR_PTR(-ENAMETOOLONG);
  // 从父 inode 中根据文件名查找 ino
  ino = baby_inode_by_name(dir, &dentry->d_name);
  inode = NULL;
  if (ino) {
    inode = baby_iget(dir->i_sb, ino);
    if (inode == ERR_PTR(-ESTALE))
      return ERR_PTR(-EIO);
  }
  return d_splice_alias(inode, dentry);
}

/*
 * @old_dir: 待移动文件的父目录 inode
 * @old_dentry: 旧的目录项，待移动的文件
 * @new_dir: 目的地的目录 inode
 * @new_dentry: 新的目录项，目的地文件
 */
static int baby_rename(struct inode *old_dir, struct dentry *old_dentry,
                       struct inode *new_dir, struct dentry *new_dentry,
                       unsigned int flags) {
  struct inode *old_inode = d_inode(old_dentry);
  struct inode *new_inode = d_inode(new_dentry);
  int err;
  // 获取待移动文件的磁盘目录项
  struct page *old_page = NULL;
  struct dir_record *old_de =
      baby_find_entry(old_dir, &old_dentry->d_name, &old_page);
  if (!old_de) {
    err = -ENOENT;
    goto out;
  }
  // 判断待移动的是不是一个目录文件，并获取 ".." 磁盘数据目录项
  struct page *dir_page = NULL;
  struct dir_record *dotdot_de = NULL;
  if (S_ISDIR(old_inode->i_mode)) {
    err = -EIO;
    dotdot_de = baby_dotdot(old_inode, &dir_page);
    if (!dotdot_de)
      goto out_old;
  }
  struct dir_record *new_de = NULL;
  if (new_inode) { // 目的地文件存在，覆盖它
    struct page *new_page = NULL;
    // 获取待覆盖的目录项
    new_de = baby_find_entry(new_dir, &new_dentry->d_name, &new_page);
    if (!new_de)
      goto out_dir;
    // 设置 new_de 的 inode_no 和 type 都和 old_inode 相同
    baby_set_link(new_dir, new_de, new_page, old_inode, 1);
    new_inode->i_ctime = current_time(new_inode);
    // 由于 ".." 存在，因此待覆盖目录中的 "." 指向的不再是 new_inode
    if (dotdot_de)
      drop_nlink(new_inode);
    // 待覆盖目录项的 inode_no 不再是 new_inode
    inode_dec_link_count(new_inode);
  } else { // 目的地文件不存在，只是简单的重命名
    err = baby_add_link(new_dentry, old_inode);
    if (err)
      goto out_dir;
    // 原文件的 ".." 目录项存在，因此会变成指向目的地的父目录
    if (dotdot_de)
      inode_inc_link_count(new_dir);
  }

  old_inode->i_ctime = current_time(old_inode);
  mark_inode_dirty(old_inode);
  // 从父目录中删除原来的目录项
  baby_delete_entry(old_de, old_page);

  // 如果是目录文件，需要将 ".." 指向新的父目录
  if (dotdot_de) {
    if (old_dir != new_dir)
      baby_set_link(old_inode, dotdot_de, dir_page, new_dir, 0);
    else {
      kunmap(dir_page);
      put_page(dir_page);
    }
    inode_dec_link_count(old_dir); // 对于旧目录来说少了一个 ".." 的链接
  }
  return 0;

out_dir:
  if (dotdot_de) {
    kunmap(dir_page);
    put_page(dir_page);
  }
out_old:
  kunmap(old_page);
  put_page(old_page);
out:
  return err;
}

// 从 block 块开始连续释放 count 块

/**
 * @block 物理块号，磁盘块距离第一个磁盘块的距离
 * 释放连续的磁盘块，[block, block+count)
 */
void baby_free_blocks(struct inode *inode, unsigned long block,
                      unsigned long count) {
  struct buffer_head *bitmap_bh = NULL; // 用来读取 bitmap 块
  struct super_block *sb = inode->i_sb;
  struct baby_sb_info *bbi = BABY_SB(sb);
  unsigned long nr_need_free = count;
  unsigned long i, bitmap_no, nr_del_bit, clear_bit_no;

  // 待释放 block 对应 bit 所在位图的物理磁盘块号
  bitmap_no = block / BABYFS_BIT_PRE_BLOCK + BABYFS_DATA_BIT_MAP_BLOCK_BASE;
  // 待释放 block 对应 bit 在其位图磁盘块中的偏移
  // data_bitmap记录磁盘块距离第一个数据块的距离，所以block要减去第一个数据块的偏移
  clear_bit_no = (block - NR_DSTORE_BLOCKS) % BABYFS_BIT_PRE_BLOCK;
  // 在一个位图中，待清除的bit个数，第一个块的起始位不为0
  nr_del_bit = min(count, BABYFS_BIT_PRE_BLOCK - clear_bit_no);

  // printk("block: %ld, bitmap_no: %ld, clear_bit_no: %ld, nr_del_bit: %d\n",
  //        block, bitmap_no, clear_bit_no, nr_del_bit);

  while (count > 0) { // 操作bitmap_no指示的位图
    bitmap_bh = sb_bread(sb, bitmap_no);
    int i_no =
        baby_find_first_zero_bit(bitmap_bh->b_data, BABYFS_BIT_PRE_BLOCK);
    // printk("baby_free_blocks：first_zero_bit: %d\n", i_no);
    for (i = 0; i < nr_del_bit;
         i++) { // 清空bitmap_no位图块内指定的bit，[clear_bit_no,
                // clear_bit_no+nr)
      baby_clear_bit(clear_bit_no + i,
                     bitmap_bh->b_data); // 清除该 bitmap 中的相对位置的 bit
    }
    i_no = baby_find_first_zero_bit(bitmap_bh->b_data, BABYFS_BIT_PRE_BLOCK);
    // printk("baby_free_blocks：first_zero_bit: %d\n", i_no);

    mark_buffer_dirty(bitmap_bh);
    brelse(bitmap_bh);

    count -= nr_del_bit; // 还剩多少bit要清除
    clear_bit_no = 0; // 跨位图的情况，除第一个位图外都从第一个bit开始清除
    bitmap_no++; // 操作下一个位图
    nr_del_bit =
        min(count, BABYFS_BIT_PRE_BLOCK); // 下一个位图中，要清除的bit个数
  }
  bbi->nr_free_blocks += nr_need_free; // 维护系统中剩余的可用数据块个数
}

/*
 * 释放直接块，连续的释放
 * @p: 索引数组开始的位置
 * @q: 索引数组结束的位置
 */
static inline baby_free_data(struct inode *inode, __le32 *p, __le32 *q) {
  unsigned long block_to_free = 0, count = 0, nr;
  struct baby_sb_info *sb_info = BABY_SB(inode->i_sb);
  for (; p < q; ++p) {
    nr = le32_to_cpu(*p);
    if (nr) {
      *p = 0;
      if (count == 0)
        goto free_this;
      else if (block_to_free ==
               nr - count) // 连续的块先记录着，然后再一次性 free
        count++;
      else {
        baby_free_blocks(inode, block_to_free, count);
        mark_inode_dirty(inode);
      free_this:
        block_to_free = nr;
        count = 1;
      }
    }
  }
  if (count > 0) {
    baby_free_blocks(inode, block_to_free, count);
    mark_inode_dirty(inode);
  }
}

/*
 * 释放间接块
 * @p: 开始释放的地址
 * @q: 截止释放的地址
 * @depth: 需要释放的数量
 */
static void baby_free_branches(struct inode *inode, __le32 *p, __le32 *q,
                               int depth) {
  struct buffer_head *bh;
  struct baby_sb_info *sb_info = BABY_SB(inode->i_sb);
  unsigned long nr;
  unsigned long address_num_per_block =
      BABYFS_BLOCK_SIZE / sizeof(__u32); // 每个磁盘块中可以表示的 块号的数量
  if (depth--) {
    for (; p < q; ++p) {
      nr = le32_to_cpu(*p); // 下一级索引块的物理块号
      if (!nr) // 为 0 说明当前层级索引数组中的该索引项未被使用，遍历下一个
        continue;
      *p = 0;
      bh = sb_bread(inode->i_sb, nr); // 读取下一级索引块
      if (!bh) {
        printk(KERN_ERR
               "baby_free_branches, read failure, inode = %ld, block = %ld\n",
               inode->i_ino, nr);
        continue;
      }
      baby_free_branches(inode, (__le32 *)bh->b_data,
                         (__le32 *)bh->b_data + address_num_per_block,
                         depth); // 递归释放下一级索引块中的所有索引项
      // 抛弃bh的所有待同步信息，并释放bh，因为释放数据不再需要关心数据同步
      bforget(bh);
      baby_free_blocks(inode, nr, 1); // 释放该索引项指示的下一级索引磁盘块
      mark_inode_dirty(inode);
    }
  } else {
    // 此时 depth == 0，说明是直接块，调用释放直接块的函数
    baby_free_data(inode, p, q);
  }
}

static void __baby_truncate_blocks(struct inode *inode, loff_t offset) {
  int offsets[4];
  __le32 nr = 0;
  struct baby_inode_info *inode_info = BABY_I(inode);
  __le32 *i_blocks = inode_info->i_blocks;
  // 获取开始截断的块号，这个函数应该还会在别的地方用到，单纯的释放 inode
  // 的话，iblock 就是 0 不需要计算
  unsigned long block_bit = 10;
  long iblock = (offset + BABYFS_BLOCK_SIZE - 1) >> block_bit;
  // printk("iblock: %ld\n", iblock);
  // 获取 iblock 的磁盘块信息
  int n = baby_block_to_path(inode, iblock, offsets, NULL);
  if (n == 0)
    return;
  if (n == 1) {
    // 释放直接块
    baby_free_data(inode, i_blocks + offsets[0],
                   i_blocks + BABYFS_DIRECT_BLOCK);
    goto do_indirects;
  }
  // TODO 非 inode 释放块的场景下的额外工作; 此时一开始返回的 n 可能会大于 1

// 从 offsets[0] 对应的间接块开始，往下释放更高级的间接块
do_indirects:
  switch (offsets[0]) {
  default: // 如果offsets[0]是直接块，则释放所有间接索引
    nr = i_blocks[BABYFS_PRIMARY_BLOCK];
    // printk("BABYFS_PRIMARY_BLOCK: %d\n", nr);
    if (nr) {
      i_blocks[BABYFS_PRIMARY_BLOCK] = 0; // 先重置i_blocks对应位
      mark_inode_dirty(inode);
      baby_free_branches(inode, &nr, &nr + 1, 1); // 释放索引
    }
  case BABYFS_PRIMARY_BLOCK: // 如果offsets[0]是一级索引，则释放二级和三级索引
    nr = i_blocks[BABYFS_SECONDRTY_BLOCK];
    // printk("BABYFS_SECONDRTY_BLOCK: %d\n", nr);
    if (nr) {
      i_blocks[BABYFS_SECONDRTY_BLOCK] = 0;
      mark_inode_dirty(inode);
      baby_free_branches(inode, &nr, &nr + 1, 2);
    }
  case BABYFS_SECONDRTY_BLOCK: // 如果offsets[0]是二级索引，则释放三级索引
    nr = i_blocks[BABYFS_THIRD_BLOCKS];
    // printk("BABYFS_THIRD_BLOCKS: %d\n", nr);
    if (nr) {
      i_blocks[BABYFS_THIRD_BLOCKS] = 0;
      mark_inode_dirty(inode);
      baby_free_branches(inode, &nr, &nr + 1, 3);
    }
  case BABYFS_THIRD_BLOCKS:;
  }

	baby_discard_reservation(inode);
}

static void baby_truncate_blocks(struct inode *inode, loff_t offset) {
  // 只有这些文件可以释放磁盘块
  if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
        S_ISLNK(inode->i_mode)))
    return;
  __baby_truncate_blocks(inode, offset);
}

/*
 * 删除索引节点，设置inode bitmap
 * 此时索引节点对象已经从散列表中删除，指向这个索引节点的最后一个硬链接
 * 已经从适当的目录中删除，文件的长度截为0，已回收它的所有数据块
 */
void baby_free_inode(struct inode *inode) {
  struct buffer_head *bitmap_bh;

  // TODO 当前 inode 分配位图只占了一个磁盘块，要支持多块的话这里要改成循环
  bitmap_bh = sb_bread(inode->i_sb, BABYFS_INODE_BIT_MAP_BLOCK_BASE);
  baby_clear_bit(inode->i_ino, bitmap_bh->b_data);
  mark_buffer_dirty(bitmap_bh);
  brelse(bitmap_bh);
}

/**
 * 调用 iput() 时，如果 i_nlink 为零，调用该函数执行 inode 相关磁盘块和 page
 * 的释放
 */
void baby_evict_inode(struct inode *inode) {
  struct baby_block_alloc_info *rsv;
  int want_delete = 0;
  struct baby_inode_info *inode_info = BABY_I(inode);
  struct baby_sb_info *sb_info = BABY_SB(inode->i_sb);

  // 删除 inode 的占用的 pages
  truncate_inode_pages_final(&inode->i_data);
  // iput可能会用在分配new inode失败时，此时inode未分配数据块，不用真的删除
  // 分配失败用make_bad_inode标记坏页，用is_bad_inode判断
  if (!is_bad_inode(inode) && !inode->i_nlink)
    want_delete = 1;
  if (want_delete) {
    sb_start_intwrite(inode->i_sb);
    inode_info->i_dtime = get_seconds();
    mark_inode_dirty(inode);
    __baby_write_inode(inode, inode_needs_sync(inode));
    inode->i_size = 0;
    // if (inode->i_blocks)
    baby_truncate_blocks(inode, 0); // 释放 inode 占用的磁盘块
  }
  // 要被删除的文件不需要再同步数据到磁盘了，清空待IO队列
  invalidate_inode_buffers(inode);
  clear_inode(inode);

  /*释放预留窗口中的块，释放预分配相关数据结构*/
	baby_discard_reservation(inode);
	rsv = inode_info->i_block_alloc_info;
	inode_info->i_block_alloc_info = NULL;
	if (unlikely(rsv))
		kfree(rsv);
  
  if (want_delete) {
    baby_free_inode(inode); // 释放 inode
    sb_info->nr_free_inodes++;
    sb_end_intwrite(inode->i_sb);
  }
}

struct inode_operations baby_dir_inode_operations = {
    // 目录文件inode的操作
    .lookup = baby_lookup,   //
    .create = baby_create,   // 新建文件
    .mkdir = baby_mkdir,     // 新建目录
    .rmdir = baby_rmdir,     // 删除目录
    .symlink = baby_symlink, // 新建软链接
    .link = baby_link,       // 新建硬链接
    .unlink = baby_unlink,   // 删除文件\硬链接
    .rename = baby_rename,   .getattr = simple_getattr,
};

struct inode_operations baby_file_inode_operations = {
    // 普通文件inode的操作
    .getattr = simple_getattr,
    .setattr = simple_setattr,
};

struct inode_operations baby_symlink_inode_operations = {
    // 符号链接文件inode的操作
    .get_link = page_get_link,
};

const struct address_space_operations baby_aops = {
    .readpage = baby_readpage,
    .writepage = baby_writepage,
    .writepages = baby_writepages,
    .write_end = baby_write_end,
    .write_begin = baby_write_begin,
};