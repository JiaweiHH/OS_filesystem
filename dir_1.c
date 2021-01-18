#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

#include "babyfs.h"

#define ADD_DIR 0
#define FIND_FIR 1

// 根据 inode 和页索引找到 page
static struct page *baby_get_page(struct inode *dir, int n) {
	struct address_space *as = dir->i_mapping;
	struct page *page = read_mapping_page(as, n, NULL);
	if (!IS_ERR(page)) {
		kmap(page);
	}
	return page;
}

static inline void baby_put_page(struct page *page){
	kunmap(page);
	put_page(page);
}

static int baby_iterate(struct file *dir, struct dir_context *ctx) {
    loff_t pos = ctx->pos;  // ctx->pos 表示已经读取了多少字节的数据
    struct inode *inode = file_inode(dir);      // 获取 inode 数据结构
    // unsigned int offset = pos & ~PAGE_MASK;     // 清除 pos 的后面 12 位数据
    unsigned long npages = dir_pages(inode);    // inode 数据的最大页数
    unsigned long nstart = pos >> PAGE_SHIFT;   // 从第 nstart 页开始查找
    // 超出最大数据
    if(pos >= inode->i_size)
        return 0;

    /* 开始查找目录项 */
    unsigned long nloop;
    for(nloop = nstart; nloop < npages; ++nloop){
        char *kaddr;    // 保存 page 的起始地址
        struct dir_record *de;  // 保存目录项
        struct page *page = baby_get_page(inode, nloop);    // 获取 page 结构体
        if(IS_ERR(page)){
            ctx->pos += PAGE_SIZE;
            return PTR_ERR(page);
        }
        /* 现在开始在 page 内部查找 */
        kaddr = page_address(page);
        char *limit = kaddr + BABYFS_BLOCK_SIZE;
        de = (struct dir_record *)kaddr;
        for(; (char *)de < limit; ++de){
            // 必须要目录项存在并且 inode 编号大于 0
            if(de->name_len && de->inode_no){
                // TODO d_type 改成指定类型
                unsigned char d_type = DT_UNKNOWN;
                int ret = dir_emit(ctx, de->name, de->name_len, le32_to_cpu(de->inode_no), d_type);
                // printk(KERN_INFO "filename: %s, de 的地址: %p, ret: %d", de->name, (char *)de, ret);
                if(!ret){
                    baby_put_page(page);
                    return 0;
                }
            }
            ctx->pos += BABYFS_DIR_RECORD_SIZE; // 长度增加
        }
        baby_put_page(page);    // 释放 page
    }
    return 0;
}

const struct file_operations baby_dir_operations = {
    .read = generic_read_dir,
    .iterate_shared = baby_iterate,
};