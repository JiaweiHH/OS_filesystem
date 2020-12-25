ifneq ($(KERNELRELEASE),)
obj-m := babyfs.o
babyfs-objs := inode.o super.o dir.o
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
	gcc -o mkfs.babyfs mkfs.babyfs.c && dd if=/dev/zero of=test.img bs=1M count=50 && ./mkfs.babyfs ./test.img
mount:
	mkdir test && sudo mount -t babyfs -o loop ./test.img ./test
umount:
	sudo umount ./test && rmdir test