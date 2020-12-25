#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>
#include <linux/blkdev.h>

#include "babyfs.h"

struct super_operations babyfs_super_opts;

// 创建一个新的 raw inode，并返回其对应的 vfs inode
// struct inode *baby_new_inode(){};

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

  if(!sb_set_blocksize(sb, BABYFS_BLOCK_SIZE)) { // 设置 sb_bread 读取的逻辑块大小
    printk(KERN_ERR "sb_set_blocksize: failed! current blocksize: %lu\n", sb->s_blocksize);
  }

  // 仅在初始化的时候读取超级块，后面只需要操作内存中的超级块对象，并在恰当的时候同步到磁盘
  struct baby_inode *baby_inode = (struct baby_inode *)bh->b_data;
  if(!(bh = sb_bread(sb, BABYFS_SUPER_BLOCK))) {
    printk(KERN_ERR "babyfs_fill_super: canot read super block\n");
    goto failed;
  }
  baby_sb = (struct baby_super_block *)bh->b_data;

  // 初始化超级块
  sb->s_magic = baby_sb->magic; // 魔幻数
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
  // 创建根目录
  sb->s_root = d_make_root(root_vfs_inode);
  if (!sb->s_root) {
    printk(KERN_ERR "babyfs_fill_super: create root dentry failed\n");
    ret = -ENOMEM;
    goto failed_mount;
  }
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