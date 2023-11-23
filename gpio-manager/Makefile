# kbuild part of makefile
obj-m := gpioman.o

ifeq ($(USE_HR_TIMERS),y)
$(info "Compiling with high-resolution timers")
ccflags-y := -DUSE_HR_TIMERS
endif

# change this to match the TOPDIR of your kernel repo
KERNEL_SRC ?= /home/vcsaturninus/common/playground/kernel/ubuntu-jammy-kernel/

all default: modules

install: modules_install

modules modules_install help clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(shell pwd) $@
