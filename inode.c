#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/mm.h>

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
      inode->i_fop = &simple_dir_operations;
      break;
    case S_IFLNK:  // 符号链接文件
      // inode->i_op = &page_symlink_inode_operations;
      // inode_nohighmem(inode);
      break;
  }
}

// 将磁盘中的 inode 读到内存，并新建与之关联的 vfs inode
struct inode *baby_iget(struct super_block *sb, unsigned long ino) {
  struct baby_inode *raw_inode;
  struct inode *vfs_inode;
  struct buffer_head *bh = NULL;
  long ret = -EIO;
  uid_t i_uid;
  gid_t i_gid;

  vfs_inode = iget_locked(sb, ino);  // 分配一个加锁的 vfs inode
  raw_inode = baby_get_raw_inode(sb, ino, &bh);  // 读磁盘的 inode
  if (IS_ERR(raw_inode)) {
    ret = PTR_ERR(raw_inode);
    goto bad_inode;
  }

  // 用读取的 raw inode初始化 vfs inode
  vfs_inode->i_private = raw_inode;
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
  // 初始化操作集合
  init_inode_operations(vfs_inode, vfs_inode->i_mode);

  brelse(bh);
  unlock_new_inode(vfs_inode);
  return vfs_inode;

bad_inode:
  iget_failed(vfs_inode);
  return ERR_PTR(ret);
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