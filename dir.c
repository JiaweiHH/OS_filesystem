#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

#include "babyfs.h"

// 在父目录中添加一个目录项
int baby_add_link(struct dentry *dentry, struct inode *inode) {
  return 0;
}

int baby_iterate(struct file *dir, struct dir_context *ctx) {
  struct inode *inode = file_inode(dir);
  // 检查文件是不是目录
  if (!S_ISDIR(inode->i_mode)){
    return -ENOTDIR;
  }
  // 获取超级块和磁盘存储的 inode 结构体
  struct super_block *sb = inode->i_sb;
  struct baby_inode_info *inode_info = BABY_I(inode);
  // 检查是不是超过最大数量了
  if (ctx->pos >= inode_info->i_subdir_num + 2){
    printk("ctx->pos 超出\n");
    return 0;
  }
  // 添加 "." 和 ".." 目录项
  if (!dir_emit_dots(dir, ctx))
    return 0;
  printk("ctx->pos: %d", ctx->pos);
  // 开始查找目录项
  __le32 *index_blocks = inode_info->i_blocks;
  // 遍历直接索引
  int i, j;
  for (i = 0; i < BABYFS_DIRECT_BLOCK; ++i, ++index_blocks) {
    if (*index_blocks == 0){
      continue;
    }
    // 读取数据块
    uint32_t block = le32_to_cpu(*index_blocks);
    struct buffer_head *bh = sb_bread(sb, block);
    // 查找目录项
    struct dir_record *record = (struct dir_record *)bh->b_data;
    for(j = 0; j < BABYFS_BLOCK_SIZE; j += BABYFS_DIR_RECORD_SIZE, ++record){
      if(record->name_len == 0){
        continue;
      }
      printk("name: %s", record->name);
      if (!dir_emit(ctx, record->name, record->name_len,
						le32_to_cpu(record->inode_no),
						DT_UNKNOWN)) {
          printk("dir_emit");
					brelse(bh);
					return 0;
				}
      ctx->pos++;printk("ctx->pos: %d", ctx->pos);
      if(ctx->pos >= inode_info->i_subdir_num + 2){
        brelse(bh);
        return 0;
      }
    }
    brelse(bh);
  }

  return 0;
}

static inline void baby_put_page(struct page *page) {
	kunmap(page);
	put_page(page);
}

// static struct page *baby_get_page(struct inode *dir, unsigned long n){
//   struct address_space *mapping = dir->i_mapping;
//   struct page *page = read_mapping_page(mapping, n, NULL);  // 去 page_cache 中找 page，没找到的话会调用 readpage 从磁盘读取
//   if(!page)
// }

// int baby_iterate1(struct file *dir, struct dir_context *ctx) {
//   struct inode *inode = file_inode(dir);
//   struct  super_block *sb = inode->i_sb;
//   loff_t pos = ctx->pos;  // ctx->pos 表示当前存放的目录项总 size
//   if(pos > inode->i_size)
//     return 0;
//   unsigned int offset = pos & ~PAGE_MASK;  // 计算当前的页内偏移量
//   unsigned int now_page = pos >> PAGE_SHIFT;  // 获取当前的 page num
//   unsigned int nr_pages = dir_pages(inode);   // 获取 inode 对应的文件占用的 page 数量

//   while(n != nr_pages){
//     struct page *page = 
//   }
// }

// 目录文件操作函数
const struct file_operations baby_dir_operations = {
    .read = generic_read_dir,
    .iterate_shared = baby_iterate,
};