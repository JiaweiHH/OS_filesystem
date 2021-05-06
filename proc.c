#include "babyfs.h"

#include "./test/test_information.h"
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>

#include <linux/kallsyms.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/**
 * proc_write 协议:
 * -r 表示后续的参数现暂存着
 * -a 获取 sb_info
 * -n [ino] 需要的 inode 编号
 * -i 获取 inode_info
 * -l 获取 alloc_info
 *
 * -w -n [ino] -d [simple data name] data_value
 * [simple data name]: -g;
 *
 * proc_read 协议：
 * 返回数据格式：根据参数的顺序，写入相应的结构体
 */

char proc_root_name[10] = "babyfs";
struct proc_dir_entry *proc_root;
unsigned short size = 64;
char s_argv[64] = {0};

/* 获取 sb 结构体 */
static struct super_block *find_sb(void) {
  spinlock_t *sb_lock_address;
  struct list_head *super_blocks_address;
  struct list_head *pos;
  struct super_block *sb = NULL;

  /* 获取 sb_lock 和 super_blocks 的虚拟地址 */
  sb_lock_address = (spinlock_t *)kallsyms_lookup_name("sb_lock");
  super_blocks_address = (struct list_head *)kallsyms_lookup_name("super_blocks");
  spin_lock(sb_lock_address);
  list_for_each(pos, super_blocks_address) {
    sb = list_entry(pos, struct super_block, s_list);
    // printk("proc.c::find_inode super block filesystem type is %s\n", sb->s_type->name);
    if (strcmp(sb->s_type->name, "babyfs") == 0) {
      goto find;
    }
  }
find:
  spin_unlock(sb_lock_address);
  return sb;
}

/* 根据 ino 获取 inode 结构体 */
static struct inode *find_inode(unsigned long ino) {
  struct list_head *linode;
  struct inode *inode;
  struct super_block *sb;

  /* 获取 sb */
  if (!(sb = find_sb())) {
    printk(KERN_ERR "proc.c::find_inode can not find sb\n");
    return NULL;
  }

  /* 遍历 super_blocks 链表 */
  list_for_each(linode, &sb->s_inodes) {
    inode = list_entry(linode, struct inode, i_sb_list);
#ifdef PROC_DEBUG
    printk("proc.c::find_inode inode->i_ino %ld\n", inode->i_ino);
#endif
    if (inode->i_ino == ino)
      return inode;
  }
  printk(KERN_ERR "proc.c::find_inode can not find inode, ino is %d\n", ino);
  return NULL;
}

/* 获取 sb_info 信息 */
static void get_sb_information(char __user *buf, unsigned int *bufsize) {
  struct super_block *sb = find_sb();
  if(!sb) {
    printk(KERN_ERR "proc.c::get_sb_information can not find sb\n");
    return;
  }
  struct baby_sb_info *mem_info;
  struct sb_information info;
  mem_info = BABY_SB(sb);
  info.__nr_free_blocks = mem_info->nr_free_blocks;
  info.__nr_free_inodes = mem_info->nr_free_inodes;
  info.__nr_blocks = mem_info->nr_blocks;
  info.__last_bitmap_bits = mem_info->last_bitmap_bits;

  copy_to_user(buf + (*bufsize), (char *)(&info), sizeof(info));
  bufsize += sizeof(info);
}

/* 获取 alloc_info 信息 */
static void get_alloc_information(unsigned long ino, char __user *buf,
                                  unsigned int *bufsize) {
  struct inode *inode;
  struct baby_block_alloc_info *alloc_info;
  inode = find_inode(ino);
  if (!inode) {
    printk(KERN_ERR "proc.c::get_alloc_information can not find inode\n");
    return;
  }
  struct alloc_information info;
  alloc_info = BABY_I(inode)->i_block_alloc_info;

  info.__rsv_goal_size = alloc_info->rsv_window_node.rsv_goal_size;
  info.__rsv_alloc_hit = alloc_info->rsv_window_node.rsv_alloc_hit;
  info.__rsv_start = alloc_info->rsv_window_node.rsv_window._rsv_start;
  info.__rsv_end = alloc_info->rsv_window_node.rsv_window._rsv_end;
  info.__last_alloc_logical_block =
      alloc_info->last_alloc_physical_block - NR_DSTORE_BLOCKS;

  copy_to_user(buf + (*bufsize), (char *)(&info), sizeof(info));
  *bufsize += sizeof(info);
}

/* 获取 inode_info 信息 */
static void get_inode_information(unsigned long ino, char __user *buf,
                                  unsigned int *bufsize) {
  struct inode *inode;
  inode = find_inode(ino);
  if (!inode) {
    printk(KERN_ERR "proc.c::get_inode_information can not find inode\n");
    return;
  }
  struct inode_information info;
  unsigned int *i_blocks;
  i_blocks = BABY_I(inode)->i_blocks;
  int i;
  for (i = 0; i < BABYFS_N_BLOCKS; ++i)
    info.__i_blocks[i] = i_blocks[i];
  info.__i_subdir_num = BABY_I(inode)->i_subdir_num;

  copy_to_user(buf + (*bufsize), (char *)(&info), sizeof(info));
  *bufsize += sizeof(info);
}

/* 根据不同的参数设置不同的数据，目前仅可以设置 goal 后续需要在添加其他数据 */
void set_data(unsigned int ino, char *buf) {
  struct inode *inode;
  struct baby_block_alloc_info *alloc_info;
  struct super_block *sb;
  inode = find_inode(ino);
  unsigned long long value = 0;
  int i;
  if (!inode) {
    printk(KERN_ERR "proc.c::set_data can not find inode\n");
    return;
  }

  switch (buf[0]) {
  case 'g':
    goto set_goal;
  
  default:
    break;
  }

set_goal:
  alloc_info = BABY_I(inode)->i_block_alloc_info;
  if(!alloc_info) {
    printk(KERN_ERR "want to set goal but i_block_alloc_info is NULL\n");
    goto finish;
  }
  if(buf[2] == '-') {
    printk(KERN_ERR "want to set goal with a negtive value\n");
    goto finish;
  }
  for(i = 2; buf[i] != '\0'; ++i) {
    value = value * 10 + buf[i] - '0';
  }
  alloc_info->last_alloc_physical_block = (value + NR_DSTORE_BLOCKS) % (BABY_SB(inode->i_sb)->nr_blocks) - 1;
  goto finish;

finish:
  return;
}

/* read 参数处理 */
void parse_sargv_read(char __user *buf) {
  unsigned long ino = 0;
  unsigned int bufsize = 0;

  int i;
  for (i = 0; s_argv[i] != '\0'; ++i) {
    switch (s_argv[i]) {
    case '-':
      break;
    case 'n':
      ino = s_argv[i + 2] - '0';
      i += 2;
      while(s_argv[i + 1] != ' ') {
        ino = ino * 10 + s_argv[i + 1] - '0';
        i++;
      }
#ifdef PROC_DEBUG
      printk("parse_sargv: ino is %ld\n", ino);
#endif
      break;
    case 'l':
      get_alloc_information(ino, buf, &bufsize);
      break;
    case 'i':
      get_inode_information(ino, buf, &bufsize);
      break;
    case 'a':
      get_sb_information(buf, &bufsize);
      break;
    default:
      break;
    }
  }
}

/* write 参数处理 */
void parse_sargv_write(char *argv) {
  unsigned long ino = 0;
  char data_buf[size];
  int buf_set_start = -1, pos;

  int i;
  for (i = 0; argv[i] != '\0'; ++i) {
    switch (argv[i]) {
    case '-':
      break;
    case 'n':
      ino = argv[i + 2] - '0';
      i += 2;
      while(argv[i + 1] != ' ') {
        ino = ino * 10 + argv[i + 1] - '0';
        i++;
      }
#ifdef PROC_DEBUG
      printk("parse_sargv: ino is %ld\n", ino);
#endif
      break;
    case 's':
      if(buf_set_start < 0) {
        buf_set_start = i + 2;
      }
      else {
        memset(data_buf, 0, sizeof(data_buf));
        pos = 0;
        for( ; buf_set_start < i - 1; ++buf_set_start) {
          data_buf[pos++] = argv[buf_set_start];
        }
        data_buf[pos] = '\0';
        set_data(ino, data_buf);
        buf_set_start = -1;
      }
      i += 2;
      break;
    default:
      break;
    }
  }

  if(buf_set_start > 0) {
    if(argv[i - 1] == ' ')
      i--;
    memset(data_buf, 0, sizeof(data_buf));
    pos = 0;
    for( ; buf_set_start < i; ++buf_set_start) {
      data_buf[pos++] = argv[buf_set_start];
    }
    data_buf[pos] = '\0';
    set_data(ino, data_buf);
  }
}

/**
 * 参数分割
 * 处理 s_argv 里面的命令，分为 -w 和 -r 两个部分
 * -w 本次处理 
 * -r 下一次 read 的时候处理 
 */
void parse_argv(void) {
  char *pos_w, *pos_r;
  char buf[size];
  int pos = 0;
  pos_w = strchr(s_argv, 'w');
  pos_r = strchr(s_argv, 'r');
  if(!pos_r)
    parse_sargv_write(pos_w);
  else if(!pos_w)
    return;
  else if(pos_w < pos_r) {
    // 先获取 [-w, -r] 之间的字符串给 parse_sargv_write 处理，然后保留 -r 的内容等待下次 read 的时候调用
    strncpy(buf, pos_w, pos_r - pos_w - 1);
    buf[pos_r - pos_w - 1] = '\0';
    parse_sargv_write(buf);

    memset(buf, 0, sizeof(buf));
    strcpy(buf, pos_r);
    memset(s_argv, 0, sizeof(s_argv));
    strcpy(s_argv, buf);
  }else {
    // 与上面刚好相反
    memset(buf, 0, sizeof(buf));
    strcpy(buf, pos_w);
    parse_sargv_write(buf);
    printk("write buf is: %s\n", buf);
    *pos_w = '\0';
    printk("read buf is: %s\n", s_argv);
  }
}

static ssize_t baby_proc_write(struct file *file, const char __user *buf,
                               size_t count, loff_t *ppos) {
#ifdef PROC_DEBUG
  printk(KERN_DEBUG "write handler\n");
#endif
  memset(s_argv, 0, sizeof(s_argv));
  copy_from_user(s_argv, buf, count);
  parse_argv();
  return count;
}

static ssize_t baby_proc_read(struct file *file, char __user *buf, size_t count,
                              loff_t *ppos) {
#ifdef PROC_DEBUG
  printk(KERN_DEBUG "read handler\n");
#endif
  parse_sargv_read(buf);
  memset(s_argv, 0, sizeof(s_argv));
  return count;
}

static struct file_operations baby_proc_fops = {
    // .owner = THIS_MODULE,
    .read = baby_proc_read,
    .write = baby_proc_write,
};

inline void baby_init_proc_alloc_info(struct inode *inode) {
  struct baby_inode_info *inode_info;
  inode_info = BABY_I(inode);
  sprintf(inode_info->i_proc_name, "%ld.txt", inode->i_ino);
}

inline void baby_create_proc_file(struct inode *inode) {
#ifdef PROC_DEBUG
  printk("baby_create_proc_file. ino %ld\n", inode->i_ino);
#endif
  if (inode->i_ino == 0) {
    proc_root = proc_mkdir(proc_root_name, NULL);
    return;
  }

  char *name = BABY_I(inode)->i_proc_name;
  sprintf(name, "%ld.txt", inode->i_ino);
#ifdef PROC_DEBUG
  printk("create proc file. proc_file_name is %s\n", name);
#endif
  proc_create(name, 0666, proc_root, &baby_proc_fops);
}

inline void baby_remove_proc_entry(struct inode *inode) {
#ifdef PROC_DEBUG
  printk("remove. inode is %ld\n", inode->i_ino);
#endif
  if (inode->i_ino == 0)
    return;
  struct baby_inode_info *inode_info;
  inode_info = BABY_I(inode);
  remove_proc_entry(inode_info->i_proc_name, proc_root);
}