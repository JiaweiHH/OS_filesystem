#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/pagemap.h>

#include "babyfs.h"

// 获取 ".." 磁盘目录项
struct dir_record *baby_dotdot(struct inode *dir, struct page **p) {
  struct page *page = baby_get_page(dir, 0);
  struct dir_record *de = NULL;
  if (!IS_ERR(page)) {
    de = (struct dir_record *)page_address(page);
    *p = page;
  }
  return de;
}

// @de: dir 中的一个磁盘目录项数据结构
// @page: dir 中存储 de 的那一页
void baby_set_link(struct inode *dir, struct dir_record *de, struct page *page,
                   struct inode *inode, int update_times) {
  // 获取 de 在 dir 中的偏移
  loff_t pos = page_offset(page) + (char *)de - (char *)page_address(page);
  int err;
  // 写数据
  lock_page(page);
  err = baby_prepare_chunk(page, pos, de->name_len);
  de->inode_no = cpu_to_le32(inode->i_ino);
  baby_set_de_type(de, inode);
  err = baby_commit_chunk(page, pos, de->name_len);
  baby_put_page(page);
  if(update_times)
    dir->i_mtime = dir->i_ctime = current_time(dir);
  mark_inode_dirty(dir);
}

// 根据 inode 和页索引找到 page
struct page *baby_get_page(struct inode *dir, int n) {
  struct address_space *as = dir->i_mapping;
  struct page *page = read_mapping_page(as, n, NULL);
  if (!IS_ERR(page)) {
    kmap(page);
  }
  return page;
}

inline void baby_put_page(struct page *page) {
  kunmap(page);
  put_page(page);
}

// 获取当前页面的最后地址
static unsigned baby_last_byte(struct inode *inode, unsigned long page_nr) {
  unsigned last_byte = inode->i_size;
  last_byte -= page_nr << PAGE_SHIFT;
  return last_byte > PAGE_SIZE ? PAGE_SIZE : last_byte;
}

/*
 * 直接调用 __block_write_begin，保证写数据的时候先和磁盘同步，避免数据覆盖
 * 1. 通过 page 创建一个 bh
 * 2. 根据 pos 和 rec_len 计算要写的数据区间 [from, to]
 * 3. 根据 page->index 计算文件内的逻辑块号，并与 bh 绑定
 * 4. 同步 [from, to] 之间的数据
 */
int baby_prepare_chunk(struct page *page, loff_t pos, unsigned len) {
  return __block_write_begin(page, pos, len, baby_get_block);
}

int baby_commit_chunk(struct page *page, loff_t pos, unsigned len) {
  struct address_space *mapping = page->mapping;
  struct inode *dir = mapping->host;
  int err = 0;
  inode_inc_iversion(dir);
  block_write_end(NULL, mapping, pos, len, len, page,
                  NULL);  // 标记在 [from, to] 区间内的 bh 为脏

  if (pos + len > dir->i_size) {
    i_size_write(dir, pos + len);
    mark_inode_dirty(dir);
  }
  err = write_one_page(page);  // 调用 aops->writepage，函数执行结束 page 被解锁
  if (!err) sync_inode_metadata(dir, 1);  // 将 inode 写到磁盘
  return err;
}

void baby_set_de_type(struct dir_record *de, struct inode *inode) {
  de->file_type = 0;
  if (S_ISDIR(inode->i_mode))
    de->file_type = S_IFDIR;
  else if (S_ISREG(inode->i_mode))
    de->file_type = S_IFREG;
  return;
}

static inline int baby_match(int len, const char * const name,
          struct dir_record *de){
  if(len != de->name_len)
    return 0;
  if(!de->inode_no)
    return 0;
  return !memcmp(name, de->name, len);
}

/*
 * 添加一个磁盘目录项
 * @dentry. 待添加的目录项
 * @inode. 待添加目录项的 inode
 * 此时 inode 和 dentry 还没有建立联系，因此要传递两个参数
 */
int baby_add_link(struct dentry *dentry, struct inode *inode) {
  struct inode *dir = d_inode(dentry->d_parent);  // 父目录 inode
  const char *name = dentry->d_name.name;         // 目录项的 name
  int namelen = dentry->d_name.len;               // 目录项 namelen
  unsigned long npages = dir_pages(dir);          // 父目录的页数
  loff_t pos;
  struct dir_record *de;
  unsigned long nloop = 0;
  struct page *page;
  char *kaddr;
  unsigned short rec_len;
  int err = 0;
  /* 开始查找空闲的目录项 */
  for (nloop = 0; nloop <= npages; ++nloop) {
    page = baby_get_page(dir, nloop);  // 按编号查找 page
    if (IS_ERR(page)) goto out;
    lock_page(page);
    kaddr = page_address(page);  // page 起始地址
    char *dir_end =
        kaddr +
        baby_last_byte(dir, nloop);  // 当前页面的最后地址，page 可能不满一页
                                     // last_byte 正常情况下返回 BLOCK_SIZE
    de = (struct dir_record *)kaddr;
    while ((char *)de < kaddr + PAGE_SIZE) {
      // 到达 i_size
      if ((char *)de == dir_end) {
        rec_len = BABYFS_BLOCK_SIZE;
        goto got_it;
      }
      if (baby_match (namelen, name, de))	// 判断重名
        goto page_unlock;
      rec_len = BABYFS_DIR_RECORD_SIZE;
      // 无效 de
      if (!de->inode_no && !de->name_len) goto got_it;
      ++de;
    }
    unlock_page(page);
    baby_put_page(page);
  }
  return -EINVAL;

got_it:
  // 获取文件内偏移量; page->index >> PAGE_SHIFT + delta
  pos = page_offset(page) + (char *)de - (char *)page_address(page);
  // 直接调用 __block_write_begin，保证写数据的时候先和磁盘同步，避免数据覆盖
  err = baby_prepare_chunk(page, pos, rec_len);
  // printk(KERN_INFO "add_link---err_baby_prepare_chunk: %d", err);
  if (err) goto page_unlock;
  de->name_len = namelen;
  memcpy(de->name, name, namelen);
  de->inode_no = cpu_to_le32(inode->i_ino);
  baby_set_de_type(de, inode);
  // 提交 change，把 page 写到磁盘
  err = baby_commit_chunk(page, pos, rec_len);
  // printk(KERN_INFO "add_link---err_baby_commit_chunk: %d", err);
  dir->i_mtime = dir->i_ctime = BABYFS_CURRENT_TIME;
  mark_inode_dirty(dir);
page_put:
  baby_put_page(page);
out:
  return err;
page_unlock:
  unlock_page(page);
  goto page_put;
}

// 遍历目录项
static int baby_iterate(struct file *dir, struct dir_context *ctx) {
  loff_t pos = ctx->pos;  // ctx->pos 表示已经读取了多少字节的数据
  struct inode *inode = file_inode(dir);  // 获取 inode 数据结构
  // unsigned int offset = pos & ~PAGE_MASK;     // 清除 pos 的后面 12 位数据
  unsigned long npages = dir_pages(inode);   // inode 数据的最大页数
  unsigned long nstart = pos >> PAGE_SHIFT;  // 从第 nstart 页开始查找
  // 超出最大数据
  if (pos >= inode->i_size) return 0;

  /* 开始查找目录项 */
  unsigned long nloop;
  for (nloop = nstart; nloop < npages; ++nloop) {
    char *kaddr;            // 保存 page 的起始地址
    struct dir_record *de;  // 保存目录项
    struct page *page = baby_get_page(inode, nloop);  // 获取 page 结构体
    if (IS_ERR(page)) {
      ctx->pos += PAGE_SIZE;
      return PTR_ERR(page);
    }
    /* 现在开始在 page 内部查找 */
    kaddr = page_address(page);
    char *limit = kaddr + baby_last_byte(inode, nloop);
    de = (struct dir_record *)kaddr;
    for (; (char *)de < limit; ++de) {
      // 必须要目录项存在并且 inode 编号大于 0
      if (de->name_len && de->inode_no) {
        // TODO d_type 改成指定类型
        unsigned char d_type = de->file_type;
        printk(KERN_INFO "de->name: %s, de->inode_no: %d, de->namelen: %d, de->file_type: %d", de->name, de->inode_no, de->name_len, de->file_type);
        int ret = dir_emit(ctx, de->name, de->name_len,
                           le32_to_cpu(de->inode_no), d_type);
        // printk(KERN_INFO "filename: %s, de 的地址: %p, ret: %d", de->name,
        // (char *)de, ret);
        if (!ret) {
          baby_put_page(page);
          return 0;
        }
      }
      ctx->pos += BABYFS_DIR_RECORD_SIZE;  // 长度增加
    }
    baby_put_page(page);  // 释放 page
  }
  return 0;
}

// 实现从 dir 中根据 child 文件名匹配目录项
struct dir_record *baby_find_entry(struct inode *dir,
      const struct qstr *child, struct page **res_page) {
  struct dir_record *de = NULL;
  unsigned long nstart = 0, nloop, npages = dir_pages(dir);  // 获取 page number
  struct page *page = NULL;
  struct baby_inode_info *bbi = BABY_I(dir);
  char *kaddr, *limit;
  if(npages == 0)
    goto out;
  for(nloop = nstart; nloop <= npages; ++nloop) {
    page = baby_get_page(dir, nloop);
    if(IS_ERR(page))
      goto out;
    kaddr = page_address(page);
    limit = kaddr + baby_last_byte(dir, nloop);
    de = (struct dir_record *)kaddr;
    for(; (char *)de < limit; ++de){
      if(!de->name_len){
        baby_put_page(page);
        goto out;
      }
      if(baby_match(child->len, child->name, de)) {
        *res_page = page;
        return de;
      }
    }
  }
out:
  return NULL;
}

unsigned int baby_inode_by_name(struct inode *dir, const struct qstr *child) {
  unsigned int ino = 0;
  struct dir_record *de;
  struct page *page;
  de = baby_find_entry(dir, child, &page);
  if(de) {
    ino = le32_to_cpu(de->inode_no);
    baby_put_page(page);
  }
  return ino;
}

int baby_make_empty(struct inode *inode, struct inode *parent) {
  struct page *page = grab_cache_page(inode->i_mapping, 0); // 从 page_cache 中获取 index = 0 的 page，不存在的话就会创建一个
  struct dir_record *de = NULL;
  int err;
  void *kaddr;
  err = baby_prepare_chunk(page, 0, BABYFS_BLOCK_SIZE); // 保证写数据的时候先和磁盘同步
  if(err) {
    printk(KERN_ERR "baby_make_empty: baby_prepare_chunk failed");
    unlock_page(page);
    goto fail;
  }
  kaddr = kmap_atomic(page); // 关闭内核抢占，这个函数里面会调用 page_address()
  memset(kaddr, 0, BABYFS_BLOCK_SIZE);
  de = (struct dir_record *)kaddr;
  de->name_len = 1;
  memcpy(de->name, ".", 1);
  de->inode_no = cpu_to_le32(inode->i_ino);
  baby_set_de_type(de, inode);
  ++de;

  de->name_len = 2;
  de->inode_no = cpu_to_le32(parent->i_ino);
  memcpy(de->name, "..", 2);
  baby_set_de_type(de, inode);
  kunmap_atomic(kaddr);
  err = baby_commit_chunk(page, 0, BABYFS_BLOCK_SIZE);
fail:
  put_page(page);
  return err;
}

/**
 * 删除一个目录项，先找到那个目录项，把inode号和name_len变成0，
 * 
 * @param de 待删除目录项
 * @param page 待删除目录项所在的page
 * @return int 
 */
int baby_delete_entry (struct dir_record * de, struct page * page)
{
  struct inode *inode = page->mapping->host;
  loff_t pos = page_offset(page) + (char *)de - (char *)page_address(page);
  int err;

  lock_page(page);
  err = baby_prepare_chunk(page, pos, sizeof(struct dir_record));
  BUG_ON(err);
  /*要删除的目录项inode号和name_len变成0*/
  de->inode_no = 0;
  de->name_len = 0;
  err = baby_commit_chunk(page, pos, sizeof(struct dir_record)); // 提交修改
  inode->i_ctime = inode->i_mtime = current_time(inode); // 更新文件修改时间
  mark_inode_dirty(inode);

  baby_put_page(page);
  return err;
}

/*检查一个目录是不是空的，inode是要检查的目录的inode结构体，空=1 */
int baby_empty_dir (struct inode * inode) {
  struct page *page = NULL;
  unsigned long i, npages = dir_pages(inode);

  for (i = 0; i < npages; i++) { /*遍历目录的每一个页*/
    char *kaddr;
    struct dir_record * de;
    page = baby_get_page(inode, i); /*获得遍历到的当前页*/
    if (IS_ERR(page)) {
      printk(KERN_ERR "baby_empty_dir: baby_get_page err!\n");
      break;
    }

    kaddr = page_address(page);
    de = (struct dir_record *)kaddr;
    kaddr += baby_last_byte(inode, i);

    while ((char *)de < kaddr) { // 遍历页内的所有目录项
      if (de->inode_no != 0) { // 检查有效项
        /* check for . and .. */
        if (de->name[0] != '.') // 不是以.开头的必有效
					goto not_empty;
				if (de->name_len > 2) // 以.开头的长度大于2的有效
					goto not_empty;
				if (de->name_len < 2) { // name = .
					if (de->inode_no != cpu_to_le32(inode->i_ino))
						goto not_empty;
				} else if (de->name[1] != '.') // 第一个为.且name_len为2且第二个不是.也有效
					goto not_empty;
      }
      de++;
    }
    baby_put_page(page);
  }
  return 1;

not_empty:
  baby_put_page(page);
  return 0;
}

const struct file_operations baby_dir_operations = {
    .read           = generic_read_dir,   // 读目录文件
    .iterate_shared = baby_iterate,       // 遍历目录项
    .fsync          = generic_file_fsync  // 异步同步目录内容
};