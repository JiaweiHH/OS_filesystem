# error 记录

> ls: cannot access 'heihei': No such file or directory

1. 一个可能因素是因为权限不够导致
2. 在 ls 系统调用会执行 `sys_lstat()`，这个函数会执行 `vfs_statx`，继续调用栈会执行 `user_path_at` 进行路径解析。路径解析的过程中会优先调用 `lookup_fast`，找不到的时候才会启动 `文件系统自己的 lookup`，还是找不到就会报错这个时候就会出现 `No such file or directory`

实现 `lookup 函数` 可以解决这个问题，主要工作是完成从父目录的 inode 根据文件名查找 ino

[Linux pseudo directory triggers the error “no such file or directory”](https://stackoverflow.com/questions/49888088/linux-pseudo-directory-triggers-the-error-no-such-file-or-directory)

---


> parentdir_add_inode:baby_add_link failed!
> 操作复现：mkdir hhh && touch a

1. 由于新建的 `raw_inode->i_blcoks` 没有初始化，没有指定可以用的数据块块号，导致在 `get_block()` 的时候出现错误
2. 之前添加 . 和 .. 目录项出现问题应该也是这个原因


---

> 无法使用 vim 打开、编辑文件
> 使用 vim 命令的时候会一直对同一个 block_no 重复 new_block 函数（一直在调用 new_block 函数）

函数调用情况：`baby_alloc_blocks` 中的 `while(1)` 循环无法停止

1. 由于标记已分配的块标记错误导致出现问题
2. 没有添加 `open` 和 `fsync` 操作