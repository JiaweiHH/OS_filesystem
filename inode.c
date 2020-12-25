#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/mm.h>

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
      // inode->i_fop = &myfs_file_operations;
      break;
    case S_IFDIR:  // 目录文件
      inode->i_op = &baby_dir_inode_operations;
      inode->i_fop = &simple_dir_operations;
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

  vfs_inode = iget_locked(sb, ino);  // 分配一个加锁的 vfs inode
  if (!vfs_inode)
		return ERR_PTR(-ENOMEM);
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
  for (i = 0; i < BABYFS_N_BLOCKS; i++) { // 拷贝数据块索引数组
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

// 将一个 inode 写会到磁盘上，(baby_inode_info, vfs_inode)->raw_inode
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
  if (wbc->sync_mode == WB_SYNC_ALL) { // 支持同步写
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh))
			ret = -EIO;
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

  inode = new_inode(sb); // 获取一个 vfs 索引节点
  if (!inode)
    return ERR_PTR(-ENOMEM);
  bbi = BABY_I(inode);

  // 读 inode 分配位图
  // TODO 当前 inode 分配位图只占了一个磁盘块，要支持多块的话这里要改成循环
  bh_bitmap = sb_bread(sb, BABYFS_INODE_BIT_MAP_BLOCK_BASE);
  // 寻找第一个空闲的位
  i_no = baby_find_first_zero_bit(bh_bitmap->b_data, BABYFS_BIT_PRE_BLOCK);
  printk("baby_find_first_zero_bit get i_no: %d\n", i_no);
  if (i_no >= BABYFS_BIT_PRE_BLOCK) {
    brelse(bh_bitmap);
    return NULL;
  }
  
  baby_clear_bit(i_no, bh_bitmap->b_data); // 占用这一位
  mark_buffer_dirty(bh_bitmap);
	brelse(bh_bitmap);

  // 设置 inode 的属性
  inode_init_owner(inode, dir, mode);
  inode->i_ino = i_no;
	inode->i_blocks = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
  bbi->i_subdir_num = 0;

  // 初始化操作集合
  init_inode_operations(inode, inode->i_mode);

	mark_inode_dirty(inode);

  printk("baby_new_inode: alloc new inode ino");
	return inode;
}

static inline int parentdir_add_inode(struct dentry *dentry, struct inode *inode)
{
	int err = baby_add_link(dentry, inode); // 在父目录中添加一个目录项
	if (!err) {
		d_instantiate_new(dentry, inode); // 将 inode 与 dentry 相关联
		return 0;
	}
	inode_dec_link_count(inode);
	unlock_new_inode(inode);
	iput(inode);
	return err;
}

// 创建普通文件
static int baby_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
  struct inode *inode = baby_new_inode(dir, mode, &dentry->d_name);
  
  if (IS_ERR(inode)) {
    printk(KERN_ERR "baby_create: get new inode failed!\n");
    return PTR_ERR(inode);
  }

  mark_inode_dirty(inode);
	return parentdir_add_inode(dentry, inode);
}

// 创建目录
static int baby_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) {
  int ret = 0;
  struct inode *inode = baby_new_inode(dir, S_IFDIR | mode, &dentry->d_name);
  
  if (IS_ERR(inode)) {
    printk(KERN_ERR "baby_mkdir: get new inode failed!\n");
    return PTR_ERR(inode);
  }

  mark_inode_dirty(inode);
	ret = parentdir_add_inode(dentry, inode);
	if (!ret) inode_dec_link_count(dir); // 父目录的引用计数+1

  return ret;
}

struct dentry *baby_lookup(struct inode *parent, struct dentry *child, unsigned int flags) {
    return NULL;
}

struct inode_operations baby_dir_inode_operations = {
  .lookup         = baby_lookup,  // 
  .create         = baby_create,  // 新建文件
  .mkdir          = baby_mkdir,   // 新建目录
};

struct inode_operations baby_file_inode_operations = {
  .getattr        = simple_getattr,
  .setattr        = simple_setattr,
};