#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mpage.h>

#include "babyfs.h"

void init_inode_operations(struct inode *inode, umode_t mode) {
  switch (mode & S_IFMT) {
    default:  // 创建除了目录和普通文件之外的其他文件
      // init_special_inode(inode, mode, dev);
      break;
    case S_IFREG:  // 普通文件
      // inode->i_op = &myfs_file_inode_ops;
      // inode->i_fop = &myfs_file_operations;
      break;
    case S_IFDIR:  // 目录文件
      inode->i_op = &simple_dir_inode_operations;
      inode->i_fop = &baby_dir_operations;
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
    return NULL;
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

  vfs_inode = iget_locked(sb, ino);  // 分配一个加锁的 vfs inode
  if (!vfs_inode) return ERR_PTR(-ENOMEM);
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
  __le32 *p;               // buffer_head 中的相对地址
  __le32 key;              // p 中存储的值
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

static int baby_block_to_path(struct inode *inode, long i_block,
                              int offsets[4]) {
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
  } else if ((i_block -= direct_blocks) < indirect_blocks) {
    offsets[n++] = BABYFS_PRIMARY_BLOCK;
    offsets[n++] = i_block;
  } else if ((i_block -= indirect_blocks) < double_blocks) {
    offsets[n++] = BABYFS_SECONDRTY_BLOCK;  // 从二次间接开始
    offsets[n++] = i_block >> ptr_bits;     // i_block / ptrs
    offsets[n++] = i_block & (ptrs - 1);    // i_block % ptrs
  } else if (((i_block -= double_blocks) >> (ptr_bits * 2)) < ptrs) {
    offsets[n++] = BABYFS_THIRD_BLOCKS;        // 从三次间接开始
    offsets[n++] = i_block >> (ptr_bits * 2);  // i_block / double_blocks
    offsets[n++] = (i_block >> ptr_bits) &
                   (ptrs - 1);  // (i_block % double_blocks) / indirect_blocks
    offsets[n++] = i_block & (ptrs - 1);  // i_blocks % ptrs
  }
  return n;
}

static int baby_get_blocks(struct inode *inode, sector_t block,
                           unsigned long maxblocks, struct buffer_head *bh,
                           int create) {
  int err = -EIO;
  int offset[4];      // 存放 block 的索引信息
  Indirect chain[4];  // 读取索引信息，存放数据
  Indirect *partial;
  // 获取索引深度，直接索引是 0
  int depth = baby_block_to_path(inode, block, offset);
  if (!depth) return err;
  // 读取索引信息，返回 NULL 表示找到了所有的
  partial = baby_get_branch(inode, depth, offset, chain, &err);
  // TODO 没找到的时候需要怎么做
  if (!partial) {
    map_bh(bh, inode->i_sb,
           chain[depth - 1].key);  // chain 的最后一个元素存储的是最后的块号
    partial = chain + depth - 1;
  }
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

const struct address_space_operations baby_aops = {
    .readpage = baby_readpage,
};
