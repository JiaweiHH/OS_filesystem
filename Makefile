ifneq ($(KERNELRELEASE),)
obj-m := babyfs.o
babyfs-objs := inode.o super.o dir.o file.o balloc.o
# CFLAGS_balloc.o += -DRSV_DEBUG
# CFLAGS_inode.o += -DRSV_DEBUG
# CFLAGS_inode.o += -DCLEAR_DEBUG
else
KDIR:=/lib/modules/$(shell uname -r)/build
all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	rm -f *.ko *.o *.mod.o *.mod.c *.symvers *.order mkfs.babyfs
endif

install:
	sudo insmod babyfs.ko
uninstall:
	sudo rmmod babyfs
mkimg:
	gcc -o mkfs.babyfs mkfs.babyfs.c && dd if=/dev/zero of=fs.img bs=1M count=50 && ./mkfs.babyfs ./fs.img
mount:
	mkdir imgdir && sudo mount -t babyfs -o loop ./fs.img ./imgdir
umount:
	sudo umount ./imgdir && rmdir imgdir
ssd:
	sudo mount -t babyfs /dev/sda1 ~/Desktop/ssd-baby/
ssdumt:
	sudo umount ~/Desktop/ssd-baby
runtest:
	cd ./test && sudo bash runtest.sh


# datablock_total - bitmap_num	<= bitmap_num * (blocksize << 3) 
# (bitmap_num - 1) * (blocksize << 3) <= datablock_total - bitmap_num
