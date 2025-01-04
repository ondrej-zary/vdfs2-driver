#
# Makefile for the linux vdfs2-filesystem routines.
#

ifdef VDFS2_NO_WARN
EXTRA_CFLAGS+=-Werror
endif

obj-$(CONFIG_VDFS2_FS) += vdfs2.o

vdfs2-y	:= btree.o bnode.o cattree.o file.o inode.o \
		   options.o super.o fsm.o fsm_btree.o ioctl.o \
		   extents.o snapshot.o orphan.o data.o \
		   cattree-helper.o hlinktree.o xattr.o \
		   packtree_inode.o packtree.o small.o quota.o

vdfs2-$(CONFIG_VDFS2_DEBUG)		+= debug.o
vdfs2-$(CONFIG_VDFS2_PROC)		+= procfs.o

GIT_BRANCH = vdfs2.0004-2015_05_13
GIT_REV_HASH = 42549d6525bee544fc427a6a3ef9efb0e63fd43c
VERSION = vdfs2.0004-2015_05_13

ifneq ($(GIT_BRANCH),)
CFLAGS_super.o				+= -DVDFS2_GIT_BRANCH=\"$(GIT_BRANCH)\"
CFLAGS_procfs.o				+= -DVDFS2_GIT_BRANCH=\"$(GIT_BRANCH)\"
CFLAGS_packtree_inode.o			+= -DVDFS2_GIT_BRANCH=\"$(GIT_BRANCH)\"
endif
ifneq ($(GIT_REV_HASH),)
CFLAGS_super.o				+= -DVDFS2_GIT_REV_HASH=\"$(GIT_REV_HASH)\"
CFLAGS_procfs.o				+= -DVDFS2_GIT_REV_HASH=\"$(GIT_REV_HASH)\"
CFLAGS_packtree_inode.o			+= -DVDFS2_GIT_REV_HASH=\"$(GIT_REV_HASH)\"
endif
ifneq ($(VERSION),)
CFLAGS_super.o				+= -DVDFS2_VERSION=\"$(VERSION)\"
CFLAGS_procfs.o				+= -DVDFS2_VERSION=\"$(VERSION)\"
CFLAGS_packtree_inode.o			+= -DVDFS2_VERSION=\"$(VERSION)\"
endif

