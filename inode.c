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
      // inode->i_fop = &baby_file_operations;
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

  // 获取 ino 标志的 inode，若在 inode cache 中直接返回，否则分配一个加锁的 vfs inode
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
  printk(KERN_INFO "baby_readpage 调用了");
  return mpage_readpage(page, baby_get_block);
}

static int baby_writepage(struct page *page, struct writeback_control *wbc) {
  return block_write_full_page(page, baby_get_block, wbc);
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

	ret = block_write_begin(mapping, pos, len, flags, pagep,
				baby_get_block);
  // TODO if (ret < len)
  return ret;
}

const struct address_space_operations baby_aops = {
    .readpage = baby_readpage,
    .writepage = baby_writepage,
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
  
  baby_set_bit(i_no, bh_bitmap->b_data); // 占用这一位
  mark_buffer_dirty(bh_bitmap);
  brelse(bh_bitmap);

  // 设置 inode 的属性
  inode_init_owner(inode, dir, mode);
  inode->i_ino = i_no;
  inode->i_blocks = 0;
  inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
  bbi->i_subdir_num = 0;
  if (insert_inode_locked(inode) < 0) { // 将新申请的 vfs inode 添加到inode cache 的 hash 表中，并设置 inode 的 i_state 状态
    printk(KERN_ERR "baby_new_inode: inode number already in use - inode = %lu", i_no);
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
  inode_inc_link_count(dir); // 父目录下添加子目录，需要将父目录的引用计数加一，因为子目录的“..”
  inode = baby_new_inode(dir, S_IFDIR | mode, &dentry->d_name);
  if (IS_ERR(inode)) {
    printk(KERN_ERR "baby_mkdir: get new inode failed!\n");
    ret = PTR_ERR(inode);
    goto out;
  }
  inode_inc_link_count(inode); // 新增的子目录引用计数为 2
  mark_inode_dirty(inode);
  
  ret = baby_add_link(dentry, inode);
  if (ret) {
    printk(KERN_ERR "baby_mkdir:baby_add_link failed!\n");
    goto link_fail;
  }

  d_instantiate_new(dentry, inode);
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

struct dentry *baby_lookup(struct inode *parent, struct dentry *child,
                           unsigned int flags) {
  return NULL;
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
