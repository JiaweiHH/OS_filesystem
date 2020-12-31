#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

#include "babyfs.h"

#define ADD_DIR 0
#define FIND_FIR 1

static struct work_extra_arg_fillctx {
  struct dir_context *ctx;
  struct baby_inode_info *inode_info;
};

static struct work_extra_arg_addlink { struct dir_record **record; };

static struct baby_walk_struct {
  __u32 block;
  int level;
  struct super_block *sb;
  void *work_extra_arg;  // work 函数的额外参数
  void (*work)(__u32, struct super_block *, void *);  // work 函数
  bool (*terminate)(void *);                          // walk 终止条件
};

// 从 inode 中读取 inode 类型
static inline unsigned char dt_type(struct inode *inode) {
  return (inode->i_mode >> 12) & 15;
}

// addlink 判断终止
bool termi_addlink(void *arg) {
  if (arg) return true;
  return false;
}

// 索引遍历终止判断，找到所有的字节点为止
bool termi_fillctx(void *work_extra_arg) {
  struct work_extra_arg_fillctx *work_extra_arg_fillctx =
      (struct work_extra_arg_fillctx *)work_extra_arg;
  struct dir_context *ctx = work_extra_arg_fillctx->ctx;
  struct baby_inode_info *inode_info = work_extra_arg_fillctx->inode_info;
  return (ctx->pos >= inode_info->i_subdir_num + 2);
}

// 寻找一个空闲的目录项
static void do_get_free_record(__u32 block, struct super_block *sb,
                               void *work_extra_arg) {
  struct work_extra_arg_addlink *work_extra_arg_addlink =
      (struct work_extra_arg_addlink *)work_extra_arg;
  struct dir_record **p_record = work_extra_arg_addlink->record;
  // 读取数据块
  struct buffer_head *bh = sb_bread(sb, block);
  // 开始查找目录项
  struct dir_record *record = (struct dir_record *)bh->b_data;
  int loop;
  for (; loop < BABYFS_BLOCK_SIZE; loop += BABYFS_DIR_RECORD_SIZE, ++record) {
    if (record->name_len == 0) break;
  }
  *p_record = (void *)record;
  brelse(bh);
}

// 查找父目录的所有子目录项
static void do_fill_ctx(__u32 block, struct super_block *sb,
                        void *work_extra_arg) {
  struct work_extra_arg_fillctx *work_extra_arg_fillctx =
      (struct work_extra_arg_fillctx *)work_extra_arg;
  struct dir_context *ctx = work_extra_arg_fillctx->ctx;
  struct baby_inode_info *inode_info = work_extra_arg_fillctx->inode_info;
  // 读取数据块
  struct buffer_head *bh = sb_bread(sb, block);
  // 开始查找目录项
  struct dir_record *record = (struct dir_record *)bh->b_data;
  int loop_size = 0;
  for (; loop_size < BABYFS_BLOCK_SIZE;
       loop_size += BABYFS_DIR_RECORD_SIZE, ++record) {
    if (record->name_len == 0) {
      continue;
    }
    if (!dir_emit(ctx, record->name, record->name_len,
                  le32_to_cpu(record->inode_no), DT_DIR)) {
      goto end;
    }
    ctx->pos++;
    if (ctx->pos >= inode_info->i_subdir_num + 2) {
      goto end;
    }
  }
end:
  brelse(bh);
}

// 索引遍历
static void walk_index(struct baby_walk_struct *walk_struct) {
  struct super_block *sb = walk_struct->sb;
  int level = walk_struct->level;
  struct buffer_head *bh = sb_bread(sb, walk_struct->block);
  __u32 *dblock = (__u32 *)bh->b_data;
  while (*dblock && (char *)dblock < BABYFS_BLOCK_SIZE) {
    if (level == 1)
      walk_struct->work(*dblock, sb, walk_struct->work_extra_arg);
    else {
      walk_struct->block = *dblock;
      walk_struct->level = level - 1;
      walk_index(walk_struct);
    }

    if (walk_struct->terminate(walk_struct->work_extra_arg)) {
      brelse(bh);
      return;
    }
    dblock++;
  }
}

// 在父目录中添加一个目录项
int baby_add_link(struct dentry *dentry, struct inode *inode) {
  struct inode *dir = d_inode(dentry->d_parent);  // 获取父目录的 inode
  struct super_block *sb = dir->i_sb;
  struct dir_record *record = NULL;
  __u32 block = 0, loop;
  struct baby_inode_info *dir_inode_info = BABY_I(dir);  // 获取 inode_info
  struct work_extra_arg_addlink work_extra_arg_addlink = {.record = &record};

  // 直接块中查找空闲的目录项
  for (loop = 0; loop < BABYFS_DIRECT_BLOCK; ++loop) {
    block = dir_inode_info->i_blocks[loop];
    do_get_free_record(block, sb, &work_extra_arg_addlink);
    if (record) goto find_record;
  }
  // 直接块没有找到 && 有一级索引存在
  struct baby_walk_struct walk_struct = {
      .block = dir_inode_info->i_blocks[BABYFS_PRIMARY_BLOCK],
      .level = 1,
      .sb = sb,
      .work_extra_arg = &work_extra_arg_addlink,
      .work = do_get_free_record,
      .terminate = termi_addlink};
  walk_struct.block = dir_inode_info->i_blocks[BABYFS_PRIMARY_BLOCK];
  if (!record && dir_inode_info->i_blocks[BABYFS_PRIMARY_BLOCK]) {
    walk_index(&walk_struct);
    if (record) goto find_record;
  }

  // 二级、三级同理
  walk_struct.level = 2;
  walk_struct.block = dir_inode_info->i_blocks[BABYFS_SECONDRTY_BLOCK];
  if (!record && dir_inode_info->i_blocks[BABYFS_SECONDRTY_BLOCK]) {
    walk_index(&walk_struct);
    if (record) goto find_record;
  }
  walk_struct.level = 3;
  walk_struct.block = dir_inode_info->i_blocks[BABYFS_THIRD_BLOCKS];
  if (!record && dir_inode_info->i_blocks[BABYFS_THIRD_BLOCKS]) {
    walk_index(&walk_struct);
    if (record) goto find_record;
  }

  return 0;

find_record:
  record->name_len = dentry->d_name.len;
  strcpy(record->name, dentry->d_name.name);
  record->inode_no = cpu_to_le32(inode->i_ino);
  record->file_type = dt_type(inode);
  dir->i_mtime = dir->i_ctime = BABYFS_CURRENT_TIME;  // 更新时间
  mark_inode_dirty(dir);  // 标记父目录的 inode 为脏
  return 0;
}

// 目录遍历函数
int baby_iterate(struct file *dir, struct dir_context *ctx) {
  struct inode *inode = file_inode(dir);
  // 检查文件是不是目录
  if (!S_ISDIR(inode->i_mode)) {
    return -ENOTDIR;
  }
  // 获取超级块和磁盘存储的 inode 结构体
  struct super_block *sb = inode->i_sb;
  struct baby_inode_info *inode_info = BABY_I(inode);
  // 检查是不是超过最大数量了
  if (ctx->pos >= inode_info->i_subdir_num + 2) {
    printk("ctx->pos: %lld, 超出\n", ctx->pos);
    return 0;
  }

  // "." 和 ".."
  if (!dir_emit_dots(dir, ctx)) return 0;

  // 初始化 work_extra_arg_fillctx
  struct work_extra_arg_fillctx fillctx;
  fillctx.ctx = ctx;
  fillctx.inode_info = inode_info;

  /* 直接索引遍历 */
  __le32 *index = inode_info->i_blocks;
  int i;
  for (i = 0; i < BABYFS_DIRECT_BLOCK; ++i) {
    do_fill_ctx(index[i], sb, &fillctx);
    if (ctx->pos >= inode_info->i_subdir_num + 2) return 0;
  }

  // 初始化 walk_struct
  struct baby_walk_struct walk_struct = {.block = index[BABYFS_PRIMARY_BLOCK],
                                         .level = 1,
                                         .sb = sb,
                                         .work_extra_arg = &fillctx,
                                         .work = do_fill_ctx,
                                         .terminate = termi_fillctx};

  /* 一级索引遍历 */
  if (index[BABYFS_PRIMARY_BLOCK]) {
    walk_index(&walk_struct);
    if (ctx->pos >= inode_info->i_subdir_num + 2) return 0;
  }

  /* 二级、三级索引遍历 */
  walk_struct.block = index[BABYFS_SECONDRTY_BLOCK];
  walk_struct.level = 2;
  if (index[BABYFS_SECONDRTY_BLOCK]) {
    walk_index(&walk_struct);
    if (ctx->pos >= inode_info->i_subdir_num + 2) return 0;
  }
  walk_struct.block = index[BABYFS_THIRD_BLOCKS];
  walk_struct.level = 3;
  if (index[BABYFS_THIRD_BLOCKS]) {
    walk_index(&walk_struct);
    if (ctx->pos >= inode_info->i_subdir_num + 2) return 0;
  }

  return 0;
}

// 目录文件操作函数
const struct file_operations baby_dir_operations = {
    .read = generic_read_dir,
    .iterate_shared = baby_iterate,
};

// static __u32 walk_index_fillctx(__u32 block, int level, struct super_block
// *sb, struct dir_context *ctx, struct baby_inode_info *inode_info){
//   struct buffer_head *bh = sb_bread(sb, block);
//   __u32 *dblock = (__u32 *)bh->b_data;
//   while(dblock && (char *)dblock < BABYFS_BLOCK_SIZE){
//     if(level == 1)
//       find_dir_record(dblock, sb, ctx, inode_info);
//     else
//       walk_index_fillctx(dblock, level - 1, sb, ctx, inode_info);
//     if(ctx->pos >= inode_info->i_subdir_num + 2){
//       brelse(bh);
//       return 0;
//     }
//     dblock++;
//   }
// }

// static struct dir_record *walk_index_findde(__u32 block, struct super_block
// *sb, int level){
//   struct buffer_head *bh = sb_bread(sb, block);
//   struct dir_record *record = NULL;
//   __u32 *dblock = (__u32 *)bh->b_data;
//   while(*dblock && (char *)dblock < BABYFS_BLOCK_SIZE){
//     if(level == 1)
//       record = find_free_record(dblock, sb);
//     else
//       record = walk_index_findde(dblock, sb, level - 1);
//     if(record){
//       brelse(bh);
//       return record;
//     }
//     dblock++;
//   }
// }