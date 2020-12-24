# 闪存文件系统

## 测试

``` shell
# 编译
make
# 安装
make install
# 查看注册的新文件系统
cat /proc/filesystem
# 卸载
make uninstall

# 支持挂载文件系统，查看状态
# 生成测试文件
dd if=/dev/zero of=test.img bs=1M count=50
make mkfs
./mkfs.babyfs test.img
# 下列命令封装挂载命令 sudo mount -t my none /mnt
make mount
# 可用以下命令查看挂载的文件系统
mount
stat -f /mnt
stat /mnt
# 下列命令封装卸载文件系统命令 sudo umount /mnt
make umount
```
## 设计

## 实现过程

mkfs.babyfs

1. 创建 `super_block`，填充数据并写到设备文件上。计算数据块开始的 block，`nr_dstore_blocks`
2. 写 `inode_bitmap`，第一个 64bit 设置为 `0xfffffffffffffffe`，其余的都是 `0xffffffffffffffff`。0 表示占用，1 表示空闲
3. 写 `inode_table`。创建 `root_inode` 并填充数据，写到 inode block。此时还没有分配数据块给这个 inode
4. 写 `datablock_bitmap`，第一个 64bit 设置为 `0xfffffffffffffffe`，等会要给 `root_inode` 分配一个目录块填充目录项，其余的也是 `0xffffffffffffffff`。0 表示占用，1 表示空   闲
5. 写 `first_block`，往里面添加 "." 和 ".." `目录项`，设置 `root_inode->i_blocks` 索引数组

## 参考

《Linux 内核设计与实现》第 13、14、16 和 17 章

《深入理解 Linux 内核》

API

[https://www.debugger.wiki/article/html/1558843231417175](https://www.debugger.wiki/article/html/1558843231417175)

[https://linux-kernel-labs.github.io/refs/heads/master/labs/filesystems_part1.html#fillsupersection](https://linux-kernel-labs.github.io/refs/heads/master/labs/filesystems_part1.html#fillsupersection)

mount 系统调用过程

[https://zhuanlan.zhihu.com/p/67686817](https://zhuanlan.zhihu.com/p/67686817)

open 系统调用过程

[https://juejin.cn/post/6844903937036779533](https://juejin.cn/post/6844903937036779533)

buffer_head 结构体

[https://www.huliujia.com/blog/b332e9cef68c8d3efe84778931ffb98a6173812d/](https://www.huliujia.com/blog/b332e9cef68c8d3efe84778931ffb98a6173812d/)

ext2文件系统

[https://wenku.baidu.com/view/946bb6c48bd63186bcebbcb5.html](https://wenku.baidu.com/view/946bb6c48bd63186bcebbcb5.html)

网上已经实现的简单文件系统参考

[tinyfs](https://blog.csdn.net/qq_35536179/article/details/109013447) [ouichefs](https://github.com/rgouicem/ouichefs) [dummyfs](https://github.com/gotoco/dummyfs) [naivefs](https://github.com/z0gSh1u/naivefs)