ifneq ($(KERNELRELEASE),)
obj-m := babyfs.o
babyfs-objs := inode.o super.o dir.o file.o
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
	gcc -o mkfs.babyfs mkfs.babyfs.c && dd if=/dev/zero of=test.img bs=1M count=2048 && ./mkfs.babyfs ./test.img
mount:
	mkdir test && sudo mount -t babyfs -o loop ./test.img ./test
umount:
	sudo umount ./test && rmdir test
ssd:
	sudo mkdir /home/jbmaster/Desktop/ssd-ext2 && sudo mount -t ext2 /dev/sdb2 /home/jbmaster/Desktop/ssd-ext2 \
	&& sudo mkdir /home/jbmaster/Desktop/ssd-ext4 && sudo mount -t ext4 /dev/sdb3 /home/jbmaster/Desktop/ssd-ext4 \
	&& sudo mkdir /home/jbmaster/Desktop/ssd-f2fs && sudo mount -t f2fs /dev/sdb4 /home/jbmaster/Desktop/ssd-f2fs
ssdumt:
	sudo umount /mnt/ssd-ext2 && sudo rmdir /mnt/ssd-ext2 \
	&& sudo umount /mnt/ssd-ext4 && sudo rmdir /mnt/ssd-ext4 \
	&& sudo umount /mnt/ssd-f2fs && sudo rmdir /mnt/ssd-f2fs
random_create_file:
	g++ -Wall -std=c++11 -pthraed ./rw_test/test -o test

# datablock_total - bitmap_num	<= bitmap_num * (blocksize << 3) 
# (bitmap_num - 1) * (blocksize << 3) <= datablock_total - bitmap_num
