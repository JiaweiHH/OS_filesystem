#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>

#include "babyfs.h"

struct super_operations babyfs_super_opts;

struct baby_sb_info *BABY_SB(struct super_block *sb) {
  return sb->s_fs_info;
}

void init_inode_operations(struct inode *inode, umode_t mode) {
  switch (mode & S_IFMT) {
    default: // 创建除了目录和普通文件之外的其他文件
      // init_special_inode(inode, mode, dev);
      break;
    case S_IFREG: // 普通文件
      // inode->i_op = &myfs_file_inode_ops;
      // inode->i_fop = &myfs_file_operations;
      break;
    case S_IFDIR: // 目录文件
      inode->i_op = &simple_dir_inode_operations;
      inode->i_fop = &simple_dir_operations; 
      break;
    case S_IFLNK: // 符号链接文件
      // inode->i_op = &page_symlink_inode_operations;
      // inode_nohighmem(inode);
      break;
  }
}

// 由索引结点编号返回 inode 的磁盘块
// 因为一个块可以存储多个 raw inode，ino 可以指定块内偏移
static struct baby_inode *baby_get_raw_inode(struct super_block *sb, ino_t ino,
                                      struct buffer_head **bh) {
  unsigned long inode_block_no =
      BABYFS_INODE_TABLE_BLOCK_BASE +
      ino / BABYFS_INODE_NUM_PER_BLOCK; // raw inode 所在磁盘块
  unsigned long offset = ino % BABYFS_INODE_NUM_PER_BLOCK; // 该 inode 是块内的第几个
  struct buffer_head *inode_block;
  printk("baby_get_raw_inode, %lu, inode_block_no: %lu, offset: %lu\n",ino, inode_block_no, offset);
  if (!(inode_block = sb_bread(sb, inode_block_no))) {
    printk(
        "baby_get_raw_inode: unable to read inode block - inode_no=%lu, "
        "block_no=%lu",
        ino, inode_block_no);
    return NULL;
  }
  *bh = inode_block;
  printk("inode_block_no: %lu, offset: %lu\n", inode_block_no, offset);
  struct baby_inode *binode = ((struct baby_inode *)inode_block->b_data) + offset;
  printk("i_nlink:%u ,imode: %u\n", le16_to_cpu(binode->i_nlink), le16_to_cpu(binode->i_mode));

  return ((struct baby_inode *)inode_block->b_data) + offset;
}

// 创建一个新的 raw inode，并返回其对应的 vfs inode
// struct inode *baby_new_inode(){};


// 将磁盘中的 inode 读到内存，并新建与之关联的 vfs inode
struct inode *baby_iget(struct super_block *sb, unsigned long ino) {
  struct baby_inode *raw_inode;
  struct inode *vfs_inode;
  struct buffer_head *bh = NULL;
  long ret = -EIO;
  uid_t i_uid;
	gid_t i_gid;

  vfs_inode = iget_locked(sb, ino); // 分配一个加锁的 vfs inode
  raw_inode = baby_get_raw_inode(sb, ino, &bh); // 读磁盘的 inode
  if (IS_ERR(raw_inode)) {
    ret = PTR_ERR(raw_inode);
    goto bad_inode;
  }

  // 用读取的 raw inode初始化 vfs inode
  vfs_inode->i_private = raw_inode;
  vfs_inode->i_mode = le16_to_cpu(raw_inode->i_mode);
  printk("i_mode:%u %u\n", vfs_inode->i_mode, le16_to_cpu(raw_inode->i_mode));
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

static int babyfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct buffer_head *bh;
  struct baby_super_block *baby_sb;
  struct baby_sb_info *baby_sb_info;
  struct inode *root_vfs_inode;
  long ret = -ENOMEM;

  if(!(baby_sb_info = kzalloc(sizeof(*baby_sb_info), GFP_KERNEL))) {
    printk(KERN_ERR "babyfs_fill_super: kalloc baby_sb_info failed!\n");
    goto failed;
  }
  printk("kzalloc\n");
  sb->s_blocksize = BABYFS_BLOCK_SIZE; // sb_bread 读取的逻辑块大小
  // 仅在初始化的时候读取超级块，后面只需要操作内存中的超级块对象，并在恰当的时候同步到磁盘
  bh = sb_bread(sb, 0);
  printk("sb bread 0!,%lu\n", bh->b_size);
  bh = sb_bread(sb, 1);
  printk("sb bread 1!\n");
  bh = sb_bread(sb, 2);
  struct baby_inode *baby_inode = (struct baby_inode *)bh->b_data;
  printk("i_nlink: %u\n", baby_inode->i_nlink);
  if(!(bh = sb_bread(sb, BABYFS_SUPER_BLOCK))) {
    printk(KERN_ERR "babyfs_fill_super: canot read super block\n");
    goto failed;
  }
  printk("sb_bread super block\n");
  baby_sb = (struct baby_super_block *)bh->b_data;

  // 初始化超级块
  sb->s_magic = baby_sb->magic; // 魔幻数
  printk("magic=%u\n", baby_sb->magic);
  sb->s_op = &babyfs_super_opts; // 操作集合
  baby_sb_info->s_babysb = baby_sb;
  baby_sb_info->s_sbh = bh;
  sb->s_fs_info = baby_sb_info; // superblock 的私有域存放磁盘上的结构体

  // 获取磁盘存储的 inode 结构体
  root_vfs_inode = baby_iget(sb, BABYFS_ROOT_INODE_NO);
  if (IS_ERR(root_vfs_inode)) {
		ret = PTR_ERR(root_vfs_inode);
	  goto failed_mount;
	}
  printk("baby_iget\n");
  // 创建根目录
  sb->s_root = d_make_root(root_vfs_inode);
  if (!sb->s_root) {
    printk(KERN_ERR "babyfs_fill_super: create root dentry failed\n");
    ret = -ENOMEM;
    goto failed_mount;
  }
  printk("success! root_vfs_inode->i_mode:%u\n", root_vfs_inode->i_mode);
  return 0;

failed_mount:
  brelse(bh);
failed:  
  return ret;
}

static struct dentry *babyfs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    // 在块设备上挂载文件系统
    struct dentry *dentry = mount_bdev(fs_type, flags, dev_name, data, babyfs_fill_super);
    if(!dentry)
        printk(KERN_ERR "babyfs_mount: mounted error\n");
    return dentry;
}

static void babyfs_put_super(struct super_block *sb) {
  struct baby_sb_info *baby_sb_info = BABY_SB(sb);
  if (baby_sb_info == NULL) {
    printk("null!\n");

    return;
  }
  brelse(baby_sb_info->s_sbh);
  sb->s_fs_info = NULL;
  kfree(baby_sb_info);
}

struct super_operations babyfs_super_opts = { // 自定义 super_block 操作集合
  .statfs       = simple_statfs,        // 给出文件系统的统计信息，例如使用和未使用的数据块的数目，或者文件名的最大长度
  .put_super    = babyfs_put_super,     // 删除超级块实例的方法
};

static struct file_system_type baby_fs_type = { // 文件系统类型
  .owner        = THIS_MODULE,
  .name         = "babyfs",
  .mount        = babyfs_mount,
  .kill_sb      = kill_block_super, // VFS 提供的销毁方法
  .fs_flags     = FS_REQUIRES_DEV,  // 给定文件系统的每个实例都使用底层块设备
};

static int __init init_babyfs(void)
{
	printk("init babyfs\n");
  // 模块初始化时将文件系统类型注册到系统中
  return register_filesystem(&baby_fs_type);
}

static void __exit exit_babyfs(void)
{
	printk("unloading fs...\n");
  unregister_filesystem(&baby_fs_type);
}

module_init(init_babyfs);
module_exit(exit_babyfs);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("my little baby filesystem");
MODULE_VERSION("Ver 0.1.0");