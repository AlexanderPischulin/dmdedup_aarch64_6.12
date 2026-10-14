obj-m += dm-dedup.o

# Phase 2: INRAM + COWBTREE backends
dm-dedup-objs := dm-dedup-hash.o dm-dedup-ram.o dm-dedup-rw.o dm-dedup-target.o dm-dedup-cbt.o
EXTRA_CFLAGS += -DCONFIG_DM_DEDUP_COWBTREE=1
EXTRA_CFLAGS += -I/lib/modules/$(shell uname -r)/build/drivers/md

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean