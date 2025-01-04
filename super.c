/**
 * @file	fs/vdfs2/super.c
 * @brief	The eMMCFS initialization and superblock operations.
 * @author	Dmitry Voytik, d.voytik@samsung.com
 * @author
 * @date	01/17/2012
 *
 * eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 *
 * In this file mount and super block operations are implemented.
 *
 * @see		TODO: documents
 *
 * Copyright 2011 by Samsung Electronics, Inc.,
 *
 * This software is the confidential and proprietary information
 * of Samsung Electronics, Inc. ("Confidential Information").  You
 * shall not disclose such Confidential Information and shall use
 * it only in accordance with the terms of the license agreement
 * you entered into with Samsung.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/vfs.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/version.h>
#include <linux/exportfs.h>
#include <linux/vmalloc.h>
#include <linux/writeback.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/blkdev.h>

#include "vdfs2_layout.h"
#include "vdfs2.h"
#include "btree.h"
#include "packtree.h"
#include "debug.h"

#include "cattree.h"
#include "exttree.h"
#include "hlinktree.h"
#include "xattrtree.h"

#define VDFS2_MOUNT_INFO(fmt, ...)\
do {\
	printk(KERN_INFO "[VDFS2] " fmt, ##__VA_ARGS__);\
} while (0)

/* writeback thread */
static struct task_struct *writeback_thread;

/* Prototypes */
static inline void get_dev_sectors_count(struct super_block *sb,
							sector_t *s_count);

static void vdfs2_free_debug_area(struct super_block *sb);

static void free_lru(struct vdfs2_btree *btree)
{
	struct list_head *pos, *tp;
	int i;

	list_for_each_safe(pos, tp, &btree->active_use)
		vdfs2_put_cache_bnode(list_entry(pos, struct vdfs2_bnode,
				lru_node));

	list_for_each_safe(pos, tp, &btree->passive_use)
		vdfs2_put_cache_bnode(list_entry(pos, struct vdfs2_bnode,
				lru_node));

	btree->active_use_count = 0;
	btree->passive_use_count = 0;

	for (i = 0; i < VDFS2_BNODE_HASH_SIZE; i++) {
		struct hlist_node *iterator;
		hlist_for_each(iterator, &btree->hash_table[i]) {
			VDFS2_DEBUG_BUG_ON(1);
			VDFS2_ERR("Lost bnode #%d!", hlist_entry(iterator,
				struct vdfs2_bnode, hash_node)->node_id);

		}

	}


}

/**
 * @brief			B-tree destructor.
 * @param [in,out]	btree	Pointer to btree that will be destroyed
 * @return		void
 */
void vdfs2_put_btree(struct vdfs2_btree *btree)
{

	free_lru(btree);
	vdfs2_destroy_free_bnode_bitmap(btree->bitmap);
	VDFS2_BUG_ON(!btree->head_bnode);
	/*vdfs2_put_bnode(btree->head_bnode);*/
	iput(btree->inode);
	kfree(btree->rw_tree_lock);
	kfree(btree->split_buff);
	kfree(btree);
}

/**
 * @brief			Inode bitmap destructor.
 * @param [in,out]	sbi	Pointer to sb info which free_inode_bitmap
 *				will be destroyed
 * @return		void
 */
static void destroy_free_inode_bitmap(struct vdfs2_sb_info *sbi)
{
	iput(sbi->free_inode_bitmap.inode);
	sbi->free_inode_bitmap.inode = NULL;
}

/** Debug mask is a module parameter. This parameter enables particular debug
 *  type printing (see VDFS2_DBG_* in fs/vdfs2/debug.h).
 */
unsigned int vdfs2_debug_mask = 0
		/*+ VDFS2_DBG_INO*/
		/*+ VDFS2_DBG_FSM*/
		/*+ VDFS2_DBG_SNAPSHOT*/
		/*+ VDFS2_DBG_TRANSACTION*/
		+ VDFS2_DBG_TMP
		;

/** The eMMCFS inode cache
 */
static struct kmem_cache *vdfs2_inode_cachep;

void vdfs2_init_inode(struct vdfs2_inode_info *inode)
{
	inode->name = NULL;
	inode->fork.total_block_count = 0;
	inode->record_type = 0;

	inode->fork.prealloc_block_count = 0;
	inode->fork.prealloc_start_block = 0;

	inode->bnode_hint.bnode_id = 0;
	inode->bnode_hint.pos = -1;
	inode->flags = 0;
	inode->is_in_dirty_list = 0;

	inode->ptree.symlink.data = NULL;
}
/**
 * @brief			Method to allocate inode.
 * @param [in,out]	sb	Pointer to eMMCFS superblock
 * @return			Returns pointer to inode, NULL on failure
 */
static struct inode *vdfs2_alloc_inode(struct super_block *sb)
{
	struct vdfs2_inode_info *inode;

	inode = kmem_cache_alloc(vdfs2_inode_cachep, GFP_KERNEL);
	if (!inode)
		return NULL;

	vdfs2_init_inode(inode);

	return &inode->vfs_inode;
}

/**
 * @brief			Method to destroy inode.
 * @param [in,out]	inode	Pointer to inode for destroy
 * @return		void
 */
static void vdfs2_destroy_inode(struct inode *inode)
{
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);

	kfree(inode_info->name);

	if (inode_info->record_type == VDFS2_CATALOG_PTREE_SYMLINK)
		kfree(inode_info->ptree.symlink.data);

	kmem_cache_free(vdfs2_inode_cachep, inode_info);
}

/**
 * @brief		Sync starting superblock.
 * @param [in]	sb	Superblock information
 * @return		Returns error code
 */
int vdfs2_sync_first_super(struct vdfs2_sb_info *sbi)
{
	struct vdfs2_extended_super_block *exsb = VDFS2_RAW_EXSB(sbi);
	__u32 checksum;
	int ret = 0;

	lock_page(sbi->superblocks);

	checksum = crc32(0, exsb, sizeof(*exsb) - sizeof(exsb->checksum));
	exsb->checksum = cpu_to_le32(checksum);
#if defined(CONFIG_VDFS2_META_SANITY_CHECK)
	if (!is_fork_valid(&exsb->small_area))
		BUG();
#endif
	set_page_writeback(sbi->superblocks);
	ret = vdfs2_write_page(sbi, sbi->superblocks, VDFS2_EXSB_OFFSET,
		VDFS2_EXSB_SIZE_SECTORS, 3 * SB_SIZE_IN_SECTOR * SECTOR_SIZE, 1);
#ifdef CONFIG_VDFS2_STATISTIC
	sbi->umount_written_bytes += (VDFS2_EXSB_SIZE_SECTORS
			<< SECTOR_SIZE_SHIFT);
#endif
	unlock_page(sbi->superblocks);

	return ret;
}

/**
 * @brief		Sync finalizing superblock.
 * @param [in]	sb	Superblock information
 * @return		Returns error code
 */
int vdfs2_sync_second_super(struct vdfs2_sb_info *sbi)
{
	int ret = 0;
	__u32 checksum;
	struct vdfs2_extended_super_block *exsb = VDFS2_RAW_EXSB(sbi);

	lock_page(sbi->superblocks);
	set_page_writeback(sbi->superblocks);
		/* Update cheksum */
	checksum = crc32(0, exsb, sizeof(*exsb) - sizeof(exsb->checksum));
	exsb->checksum = cpu_to_le32(checksum);
#if defined(CONFIG_VDFS2_META_SANITY_CHECK)
	if (!is_fork_valid(&exsb->small_area))
		BUG();
#endif
	ret = vdfs2_write_page(sbi, sbi->superblocks, VDFS2_EXSB_COPY_OFFSET,
		VDFS2_EXSB_SIZE_SECTORS, 3 * SB_SIZE_IN_SECTOR * SECTOR_SIZE, 1);
#ifdef CONFIG_VDFS2_STATISTIC
	sbi->umount_written_bytes += (VDFS2_EXSB_SIZE_SECTORS
			<< SECTOR_SIZE_SHIFT);
#endif
	unlock_page(sbi->superblocks);

	return ret;
}


/**
 * @brief		Method to write out all dirty data associated
 *				with the superblock.
 * @param [in,out]	sb	Pointer to the eMMCFS superblock
 * @param [in,out]	wait	Block on write completion
 * @return		Returns 0 on success, errno on failure
 */
int vdfs2_sync_fs(struct super_block *sb, int wait)
{
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct vdfs2_inode_info *inode_info;
	int ret = 0;
	if (wait) {
again:
		spin_lock(&VDFS2_SB(sb)->dirty_list_lock);
		while (!list_empty(&sbi->dirty_list_head)) {
			inode_info = list_entry(sbi->dirty_list_head.next,
					struct vdfs2_inode_info,
					dirty_list);
			inode_info->is_in_dirty_list = 0;
			list_del_init(&inode_info->dirty_list);

			/*
			 * Don't bother with inodes beeing freed,
			 * the writeout is handled by the freer.
			 */
			spin_lock(&inode_info->vfs_inode.i_lock);
			if (inode_info->vfs_inode.i_state &
				(I_FREEING | I_WILL_FREE)) {
				spin_unlock(&inode_info->vfs_inode.i_lock);
				continue;
			}

			atomic_inc(&inode_info->vfs_inode.i_count);
			spin_unlock(&inode_info->vfs_inode.i_lock);
			spin_unlock(&VDFS2_SB(sb)->dirty_list_lock);
			ret = filemap_write_and_wait(
					inode_info->vfs_inode.i_mapping);
			iput(&inode_info->vfs_inode);
			if (ret)
				return ret;
			spin_lock(&VDFS2_SB(sb)->dirty_list_lock);
		}
		spin_unlock(&VDFS2_SB(sb)->dirty_list_lock);
		down_write(&sbi->snapshot_info->transaction_lock);
		if (!list_empty(&sbi->dirty_list_head)) {
			up_write(&sbi->snapshot_info->transaction_lock);
			goto again;
		}
		ret = vdfs2_update_metadata(sbi);
		up_write(&sbi->snapshot_info->transaction_lock);
	}
	return ret;
}

static void vdfs2_delayed_commit(struct work_struct *work)
{
	struct vdfs2_sb_info *sbi = container_of(to_delayed_work(work),
			    struct vdfs2_sb_info, delayed_commit);
	struct super_block *sb = sbi->sb;

	if (down_read_trylock(&sb->s_umount)) {
		sync_filesystem(sb);
		up_read(&sb->s_umount);
	} else if (sb->s_flags & SB_ACTIVE) {
		mod_delayed_work(system_wq, &sbi->delayed_commit, HZ);
	}
}

/**
 * @brief			Method to free superblock (unmount).
 * @param [in,out]	sbi	Pointer to the eMMCFS superblock
 * @return		void
 */
static void destroy_super(struct vdfs2_sb_info *sbi)
{
	percpu_counter_destroy(&sbi->free_blocks_count);
	sbi->raw_superblock_copy = NULL;
	sbi->raw_superblock = NULL;

	if (sbi->superblocks) {
		kunmap(sbi->superblocks);
		__free_pages(sbi->superblocks, 0);
	}

	if (sbi->superblocks_copy) {
		kunmap(sbi->superblocks_copy);
		__free_pages(sbi->superblocks_copy, 0);
	}
}

/**
 * @brief			Method to free sbi info.
 * @param [in,out]	sb	Pointer to a superblock
 * @return		void
 */
static void vdfs2_put_super(struct super_block *sb)
{
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	cancel_delayed_work_sync(&sbi->delayed_commit);
	sbi->umount_time = 1;
	vdfs2_destroy_packtrees_list(sbi);
	vdfs2_put_btree(sbi->catalog_tree);
	sbi->catalog_tree = NULL;
	vdfs2_put_btree(sbi->extents_tree);
	sbi->extents_tree = NULL;
	vdfs2_put_btree(sbi->hardlink_tree);
	sbi->hardlink_tree = NULL;
	vdfs2_put_btree(sbi->xattr_tree);
	sbi->xattr_tree = NULL;

	if (sbi->free_inode_bitmap.inode)
		destroy_free_inode_bitmap(sbi);

	if (sbi->fsm_info)
		vdfs2_fsm_destroy_management(sb);

	if (VDFS2_DEBUG_PAGES(sbi))
		vdfs2_free_debug_area(sb);

	destroy_small_files_area_manager(sbi);
#ifdef CONFIG_VDFS2_QUOTA
	destroy_quota_manager(sbi);
#endif
	vdfs2_destroy_high_priority(&sbi->high_priority);
	if (sbi->snapshot_info)
		vdfs2_destroy_snapshot_manager(sbi);
	destroy_super(sbi);

#ifdef CONFIG_VDFS2_STATISTIC
	printk(KERN_INFO "Bytes written during umount : %lld\n",
			sbi->umount_written_bytes);
#endif
	kfree(sbi);
	VDFS2_DEBUG_SB("finished");
}

/**
 * @brief			This function handle umount start.
 * @param [in,out]	sb	Pointer to a superblock
 * @return		void
 */
static void vdfs2_umount_begin(struct super_block *sb)
{
#ifdef CONFIG_VDFS2_STATISTIC
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	/* reset the page counter */
	sbi->umount_written_bytes = 0;
#endif
}
/**
 * @brief			Force FS into a consistency state and
 *				lock it (for LVM).
 * @param [in,out]	sb	Pointer to the eMMCFS superblock
 * @remark			TODO: detailed description
 * @return			Returns 0 on success, errno on failure
 */
static int vdfs2_freeze(struct super_block *sb)
{
	/* d.voytik-TODO-29-12-2011-17-24-00: [vdfs2_freeze]
	 * implement vdfs2_freeze() */
	int ret = 0;
	VDFS2_DEBUG_SB("finished (ret = %d)", ret);
	return ret;
}

/**
 * @brief			Calculates metadata size, using extended
 *				superblock's forks
 * @param [in,out]	sbi	Pointer to the eMMCFS superblock
 * @return		metadata size in 4K-blocks
 */
static u64 calc_special_files_size(struct vdfs2_sb_info *sbi)
{
	u64 res = 0;
	struct vdfs2_extended_super_block *exsb  = VDFS2_RAW_EXSB(sbi);

	res += le64_to_cpu(exsb->meta_tbc);
	res += le64_to_cpu(exsb->tables_tbc);

	return res;
}

/**
 * @brief			Get FS statistics.
 * @param [in,out]	dentry	Pointer to directory entry
 * @param [in,out]	buf	Point to kstatfs buffer where information
 *				will be placed
 * @return			Returns 0 on success, errno on failure
 */
static int vdfs2_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block	*sb = dentry->d_sb;
	struct vdfs2_sb_info	*sbi = sb->s_fs_info;
#ifdef CONFIG_VDFS2_CHECK_FRAGMENTATION
	int count;
#endif

	buf->f_type = (long) VDFS2_SB_SIGNATURE;
	buf->f_bsize = sbi->block_size;
	buf->f_blocks = sbi->total_leb_count << sbi->log_blocks_in_leb;
	buf->f_bavail = buf->f_bfree =
			percpu_counter_sum(&sbi->free_blocks_count);
	buf->f_files = sbi->files_count + sbi->folders_count;
	buf->f_fsid.val[0] = sbi->volume_uuid & 0xFFFFFFFFUL;
	buf->f_fsid.val[1] = (sbi->volume_uuid >> 32) & 0xFFFFFFFFUL;
	buf->f_namelen = VDFS2_FILE_NAME_LEN;

#ifdef CONFIG_VDFS2_CHECK_FRAGMENTATION
	for (count = 0; count < VDFS2_EXTENTS_COUNT_IN_FORK; count++) {
		msleep(50);
		printk(KERN_INFO "in %d = %lu", count, sbi->in_fork[count]);
	}

	msleep(50);
	printk(KERN_INFO "in extents overflow = %lu", sbi->in_extents_overflow);
#endif

	{
		const char *device_name = sb->s_bdev->bd_device.kobj.name;
		u64 meta_size = calc_special_files_size(sbi) <<
			(sbi->block_size_shift - 10);
		u64 data_size = (buf->f_blocks - percpu_counter_sum(
				&sbi->free_blocks_count) -
				calc_special_files_size(sbi)) <<
			(sbi->block_size_shift - 10);

		printk(KERN_INFO "%s: Meta: %lluKB     Data: %lluKB\n",
				device_name, meta_size, data_size);
		printk(KERN_INFO "There are %llu tiny files at volume\n",
				atomic64_read(&sbi->tiny_files_counter));
	}

	VDFS2_DEBUG_SB("finished");
	return 0;
}

/**
 * @brief			Evict inode.
 * @param [in,out]	inode	Pointer to inode that will be evicted
 * @return		void
 */
static void vdfs2_evict_inode(struct inode *inode)
{
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	int error = 0;
	enum orphan_inode_type type;
	sector_t freed_runtime_iblocks;

	VDFS2_DEBUG_INO("evict inode %lu nlink\t%u",
			inode->i_ino, inode->i_nlink);

	inode->i_size = 0;
	truncate_inode_pages(&inode->i_data, 0);
	invalidate_inode_buffers(inode);

	/* todo unpacked inode - do nothing */
	if (VDFS2_I(inode)->record_type == VDFS2_CATALOG_UNPACK_INODE) {
		inode->i_state = I_FREEING | I_CLEAR; /* todo */
		return;
	}
	spin_lock(&VDFS2_SB(inode->i_sb)->dirty_list_lock);
	if (VDFS2_I(inode)->is_in_dirty_list) {
		list_del(&VDFS2_I(inode)->dirty_list);
		VDFS2_I(inode)->is_in_dirty_list = 0;
	}
	spin_unlock(&VDFS2_SB(inode->i_sb)->dirty_list_lock);


	/* Internal packtree inodes - do nothing */
	if (VDFS2_I(inode)->record_type >= VDFS2_CATALOG_PTREE_RECORD)
		goto no_delete;

	if (!is_vdfs2_inode_flag_set(inode, TINY_FILE) &&
			!is_vdfs2_inode_flag_set(inode, SMALL_FILE) &&
			(S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode))) {
		mutex_lock(&VDFS2_I(inode)->truncate_mutex);
		freed_runtime_iblocks = vdfs2_truncate_runtime_blocks(0,
			&VDFS2_I(inode)->runtime_extents);
		mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
		vdfs2_free_reserved_space(inode, freed_runtime_iblocks);
	}

	if (inode->i_nlink)
		goto no_delete;


	error = vdfs2_xattrtree_remove_all(sbi->xattr_tree, inode->i_ino);
	if (error) {
		VDFS2_ERR("can not clear xattrs for ino#%lu", inode->i_ino);
		goto no_delete;
	}

	if (is_vdfs2_inode_flag_set(inode, SMALL_FILE))
		type = ORPHAN_SMALL_FILE;
	else if (!is_vdfs2_inode_flag_set(inode, TINY_FILE) &&
			(S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode)))
		type = ORPHAN_REGULAR_FILE;
	else
		type = ORPHAN_TINY_OR_SPECIAL_FILE;
	BUG_ON(vdfs2_kill_orphan_inode(VDFS2_I(inode), type,
			&VDFS2_I(inode)->fork));

no_delete:
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35) ||\
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	clear_inode(inode);
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) || \
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33)
	end_writeback(inode);
#endif
}

/*
 * Structure of the eMMCFS super block operations
 */
static struct super_operations vdfs2_sops = {
	.alloc_inode	= vdfs2_alloc_inode,
	.destroy_inode	= vdfs2_destroy_inode,
	.put_super	= vdfs2_put_super,
	.sync_fs	= vdfs2_sync_fs,
	.freeze_fs	= vdfs2_freeze,
	.statfs		= vdfs2_statfs,
	.umount_begin	= vdfs2_umount_begin,
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
	.delete_inode	= vdfs2_evict_inode,
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) || \
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	.evict_inode	= vdfs2_evict_inode,
#else
	BUILD_VDFS2_BUG();
#endif
};

/**
 * @brief		Determines volume size in sectors.
 * @param [in]	sb	VFS super block
 * @param [out]	s_count	Stores here sectors count of volume
 * @return	void
 */
static inline void get_dev_sectors_count(struct super_block *sb,
							sector_t *s_count)
{
	*s_count = sb->s_bdev->bd_inode->i_size >> SECTOR_SIZE_SHIFT;
}

/**
 * @brief		Determines volume size in blocks.
 * @param [in]	sb	VFS super block
 * @param [out] b_count	Stores here blocks count of volume
 * @return	void
 */
static inline void get_dev_blocks_count(struct super_block *sb,
							sector_t *b_count)
{
	*b_count = sb->s_bdev->bd_inode->i_size >> sb->s_blocksize_bits;
}

/**
 * @brief			Sanity check of eMMCFS super block.
 * @param [in]	esb		The eMMCFS super block
 * @param [in]	str_sb_type	Determines which SB is verified on error
 *				printing
 * @param [in]	silent		Doesn't print errors if silent is true
 * @return			Returns 0 on success, errno on failure
 */
static int vdfs2_verify_sb(struct vdfs2_super_block *esb, char *str_sb_type,
				int silent)
{
	__le32 checksum;
	/* check magic number */
	if (memcmp(esb->signature, VDFS2_SB_SIGNATURE,
				strlen(VDFS2_SB_SIGNATURE))) {
		if (!silent || vdfs2_debug_mask & VDFS2_DBG_SB)
			VDFS2_ERR("%s: bad signature - %.8s, "
				"expected - %.8s\n ", str_sb_type,
				esb->signature, VDFS2_SB_SIGNATURE);
		return -EINVAL;
	}

	if (memcmp(esb->layout_version, VDFS2_LAYOUT_VERSION,
		strlen(VDFS2_LAYOUT_VERSION))) {
		VDFS2_ERR("Invalid mkfs layout version: %.4s,\n"
			"driver uses %.4s version\n", esb->layout_version,
			VDFS2_LAYOUT_VERSION);
		return -EINVAL;
	}

	/* check version */
	if (esb->version.major != VDFS2_SB_VER_MAJOR ||\
			esb->version.minor != VDFS2_SB_VER_MINOR) {
		if (!silent || vdfs2_debug_mask & VDFS2_DBG_SB)
			VDFS2_ERR("%s: bad version (major = %d, minor = %d)\n",
					str_sb_type, (int)esb->version.major,
					(int)esb->version.minor);
		return -EINVAL;
	}
	/* check crc32 */
	checksum = crc32(0, esb, sizeof(*esb) - sizeof(esb->checksum));
	if (esb->checksum != checksum) {
		VDFS2_ERR("%s: bad checksum - 0x%x, must be 0x%x\n",
				str_sb_type, esb->checksum, checksum);
		return -EINVAL;
	}
	return 0;
}

/**
 * @brief		Fill run-time superblock from on-disk superblock.
 * @param [in]	esb	The eMMCFS super block
 * @param [in]	sb	VFS superblock
 * @return		Returns 0 on success, errno on failure
 */
static int fill_runtime_superblock(struct vdfs2_super_block *esb,
		struct super_block *sb)
{
	int ret = 0;
	unsigned long block_size;
	unsigned int bytes_in_leb;
	unsigned long long total_block_count;
	sector_t dev_block_count;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;

	/* check total block count in SB */
	if (*((long *) esb->mkfs_git_branch) &&
			*((long *) esb->mkfs_git_hash)) {
		VDFS2_MOUNT_INFO("mkfs git branch is \"%s\"\n",
				esb->mkfs_git_branch);
		VDFS2_MOUNT_INFO("mkfs git revhash \"%.40s\"\n",
				esb->mkfs_git_hash);
	}

	/* check if block size is supported and set it */
	block_size = 1 << esb->log_block_size;
	if (block_size & ~(512 | 1024 | 2048 | 4096)) {
		VDFS2_ERR("unsupported block size (%ld)\n", block_size);
		ret = -EINVAL;
		goto err_exit;
	}

	if (block_size == 512) {
		sbi->log_blocks_in_page = 3;
		sbi->log_block_size = 9;
	} else if (block_size == 1024) {
		sbi->log_blocks_in_page = 2;
		sbi->log_block_size = 10;
	} else if (block_size == 2048) {
		sbi->log_blocks_in_page = 1;
		sbi->log_block_size = 11;
	} else {
		sbi->log_blocks_in_page = 0;
		sbi->log_block_size = 12;
	}

	sbi->block_size = block_size;
	if (!sb_set_blocksize(sb, sbi->block_size))
		VDFS2_ERR("can't set block size\n");
	sbi->block_size_shift = esb->log_block_size;
	sbi->log_sectors_per_block = sbi->block_size_shift - SECTOR_SIZE_SHIFT;
	sbi->sectors_per_volume = esb->sectors_per_volume;

	sbi->offset_msk_inblock = 0xFFFFFFFF >> (32 - sbi->block_size_shift);

	/* check if LEB size is supported and set it */
	bytes_in_leb = 1 << esb->log_leb_size;
	if (bytes_in_leb == 0 || bytes_in_leb < block_size) {
		VDFS2_ERR("unsupported LEB size (%u)\n", bytes_in_leb);
		ret = -EINVAL;
		goto err_exit;
	}
	sbi->log_blocks_in_leb = esb->log_leb_size - esb->log_block_size;

	sbi->btree_node_size_blks = 1 << sbi->log_blocks_in_leb;


	sbi->total_leb_count = le64_to_cpu(esb->total_leb_count);
	total_block_count = sbi->total_leb_count << sbi->log_blocks_in_leb;

	get_dev_blocks_count(sb, &dev_block_count);

	if (((total_block_count > dev_block_count) &&
			(!test_option(sbi, STRIPPED))) ||
			(total_block_count == 0)) {
		VDFS2_ERR("bad FS block count: %llu, device has %llu blocks\n",
				total_block_count,
				(long long unsigned int)dev_block_count);
		ret = -EINVAL;
		goto err_exit;
	}

	sbi->lebs_bm_log_blocks_block =
		le32_to_cpu(esb->lebs_bm_log_blocks_block);
	sbi->lebs_bm_bits_in_last_block =
		le32_to_cpu(esb->lebs_bm_bits_in_last_block);
	sbi->lebs_bm_blocks_count =
		le32_to_cpu(esb->lebs_bm_blocks_count);

	sbi->log_super_page_size = le32_to_cpu(esb->log_super_page_size);
	/* squash 128 bit UUID into 64 bit by xoring */
	sbi->volume_uuid = le64_to_cpup((void *)esb->volume_uuid) ^
			le64_to_cpup((void *)esb->volume_uuid + sizeof(u64));

	if (esb->case_insensitive)
		set_option(sbi, CASE_INSENSITIVE);

err_exit:
	return ret;
}

/**
 * @brief		Add record to oops area
 * @param [in] sbi	Pointer to super block info data structure
 * @param [in] line	Function line number
 * @param [in] name	Function name
 * @param [in] err	Error code
 * @return		Returns 0 on success, errno on failure.
 */
int vdfs2_log_error(struct vdfs2_sb_info *sbi, unsigned int line,
		const char *name, int err)
{
	struct vdfs2_debug_descriptor *debug_descriptor;
	struct vdfs2_debug_record *new_record = NULL;
	struct page **debug_pages;
	void *debug_area = NULL;
	unsigned int checksum;
	int ret = 0;
	int offset = 0;
	int count = 0;
	struct vdfs2_extended_super_block *exsb;
	unsigned int debug_page_count = VDFS2_DEBUG_AREA_PAGE_COUNT(sbi);
	int len = strlen(name);

	if (VDFS2_IS_READONLY(sbi->sb))
		return 0;

	debug_pages = VDFS2_DEBUG_PAGES(sbi);
	for (count = 0; count < debug_page_count; count++) {
		if (!debug_pages[count])
			return -EINVAL;
	}
	exsb = VDFS2_RAW_EXSB(sbi);
	debug_descriptor = (struct vdfs2_debug_descriptor *)
			VDFS2_DEBUG_AREA(sbi);
	if (!debug_descriptor)
		return -EINVAL;
	debug_area = VDFS2_DEBUG_AREA(sbi);
	if (!debug_area)
		return -EINVAL;

	offset = le32_to_cpu(debug_descriptor->offset_to_next_record);
	debug_descriptor->record_count = cpu_to_le32(le32_to_cpu(
		debug_descriptor->record_count) + 1);

	new_record = (struct vdfs2_debug_record *)((void *)debug_area +
			le32_to_cpu(debug_descriptor->offset_to_next_record));



	BUG_ON(((void *)new_record - (void *)debug_area
		+ sizeof(struct vdfs2_debug_record) + sizeof(checksum))
		> debug_page_count * PAGE_SIZE);

	memset(new_record, 0, sizeof(*new_record));
	new_record->error_code = cpu_to_le32(err);
	new_record->fail_number = cpu_to_le32(debug_descriptor->record_count);
	new_record->uuid = cpu_to_le32(sbi->volume_uuid);
	new_record->fail_time = cpu_to_le32(jiffies);
	new_record->mount_count = cpu_to_le32(exsb->mount_counter);

	snprintf(new_record->line, DEBUG_FUNCTION_LINE_LENGTH, "%d", line);
	memcpy(new_record->function, name, len < DEBUG_FUNCTION_NAME_LENGTH ?
			len : DEBUG_FUNCTION_NAME_LENGTH);

	/* calculate offset to next error record */
	offset += sizeof(struct vdfs2_debug_record);
	/* check if we have space for next record*/
	if ((offset + sizeof(struct vdfs2_debug_record) + sizeof(checksum))
			>=  debug_page_count * PAGE_SIZE)
		offset = sizeof(struct vdfs2_debug_descriptor);

	debug_descriptor->offset_to_next_record = cpu_to_le32(offset);
	checksum = crc32(0, debug_area, DEBUG_AREA_CRC_OFFSET(sbi));
	*((__le32 *)((char *)debug_area + DEBUG_AREA_CRC_OFFSET(sbi)))
			= cpu_to_le32(checksum);

	for (count = 0; count < debug_page_count; count++) {
		lock_page(debug_pages[count]);
		set_page_writeback(debug_pages[count]);
		ret = vdfs2_write_page(sbi, debug_pages[count],
			((VDFS2_DEBUG_AREA_START(sbi)) <<
			(PAGE_SHIFT - SECTOR_SIZE_SHIFT)) +
			 + (count << (PAGE_SHIFT - SECTOR_SIZE_SHIFT)),
			 sbi->block_size / SECTOR_SIZE, 0, 1);
		unlock_page(debug_pages[count]);
		if (ret)
			break;
	}

	return ret;
}

/**
 * @brief			VDFS2 volume sainty check : it is vdfs2 or not
 * @param [in]		sb	VFS superblock info stucture
 * @return			Returns 0 on success, errno on failure
 *
 */
static int vdfs2_volume_check(struct super_block *sb)
{
	int ret = 0;
	struct page *superblocks;
	void *raw_superblocks;
	struct vdfs2_super_block *esb;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;

	superblocks = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!superblocks)
		return -ENOMEM;

	lock_page(superblocks);
	/* size of the VDFS2 superblock is fixed = 4K or 8 sectors */
	ret = vdfs2_read_page(sb->s_bdev, superblocks, VDFS2_SB_ADDR,
		PAGE_TO_SECTORS(1), 0);
	unlock_page(superblocks);

	if (ret)
		goto error_exit;

	raw_superblocks = kmap(superblocks);
	sbi->raw_superblock = raw_superblocks;

	/* check superblocks */
	esb = (struct vdfs2_super_block *)raw_superblocks;
	ret = vdfs2_verify_sb(esb, "SB", 0);
	if (ret)
		goto error_exit;

	raw_superblocks++;
	ret = vdfs2_verify_sb(esb, "SB", 0);
	if (ret)
		goto error_exit;
	sbi->superblocks = superblocks;
	return 0;
error_exit:
	sbi->superblocks = NULL;
	sbi->raw_superblock = NULL;
	kunmap(superblocks);
	__free_page(superblocks);
	return ret;
}


/**
 * @brief				Load debug area from disk to memory.
 * @param [in]	sb			Pointer to superblock
 * @return				Returns 0 on success, errno on failure
 */
static int vdfs2_load_debug_area(struct super_block *sb)
{
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct page **debug_pages;
	struct vdfs2_debug_descriptor *debug_descriptor = NULL;
	void *debug_area = NULL;
	int ret = 0, is_oops_area_present = 0;
	unsigned int checksum;
	int count = 0;
	unsigned int debug_page_count = VDFS2_DEBUG_AREA_PAGE_COUNT(sbi);

	debug_pages = kzalloc(sizeof(struct page *) * debug_page_count,
			GFP_KERNEL);

	if (!debug_pages)
		return -ENOMEM;

	for (count = 0; count < debug_page_count; count++) {
		debug_pages[count] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!debug_pages[count]) {
			count--;
			for (; count >= 0; count--) {
				unlock_page(debug_pages[count]);
				__free_page(debug_pages[count]);
			}
			kfree(debug_pages);
			return -ENOMEM;
		}
		lock_page(debug_pages[count]);
	}

	ret = vdfs2_read_pages(sb->s_bdev, debug_pages,
		VDFS2_DEBUG_AREA_START(sbi) << (PAGE_SHIFT -
		SECTOR_SIZE_SHIFT), debug_page_count);

	for (count = 0; count < debug_page_count; count++)
		unlock_page(debug_pages[count]);

	if (ret)
		goto exit_free_page;

	debug_area = vmap(debug_pages, debug_page_count, VM_MAP, PAGE_KERNEL);

	if (!debug_area) {
		ret = -ENOMEM;
		goto exit_free_page;
	}

	debug_descriptor = (struct vdfs2_debug_descriptor *)debug_area;
	is_oops_area_present = !(strncmp(debug_descriptor->signature,
			VDFS2_OOPS_MAGIC, sizeof(VDFS2_OOPS_MAGIC) - 1));

	if (!is_oops_area_present) {
		memset((void *)debug_area, 0,
				VDFS2_DEBUG_AREA_LENGTH_BYTES(sbi));
		debug_descriptor->offset_to_next_record = cpu_to_le32(
				sizeof(struct vdfs2_debug_descriptor));
		/* copy magic number */
		memcpy((void *)&debug_descriptor->signature, VDFS2_OOPS_MAGIC,
				sizeof(VDFS2_OOPS_MAGIC) - 1);

		checksum = crc32(0, debug_area, DEBUG_AREA_CRC_OFFSET(sbi));
		*((__le32 *)((char *)debug_area + DEBUG_AREA_CRC_OFFSET(sbi)))
				= cpu_to_le32(checksum);

		for (count = 0; count < debug_page_count; count++) {
			lock_page(debug_pages[count]);
			set_page_writeback(debug_pages[count]);
		}

		for (count = 0; count < debug_page_count; count++) {
			sector_t sector_to_write;

			sector_to_write = VDFS2_DEBUG_AREA_START(sbi) <<
					(PAGE_SHIFT - SECTOR_SIZE_SHIFT);

			sector_to_write += count << (PAGE_SHIFT -
					SECTOR_SIZE_SHIFT);

			ret = vdfs2_write_page(sbi, debug_pages[count],
					sector_to_write, 8, 0, 1);
			if (ret)
				goto exit_unmap_page;
		}

		for (count = 0; count < debug_page_count; count++)
			unlock_page(debug_pages[count]);
	}

	sbi->debug_pages = debug_pages;
	sbi->debug_area = debug_area;
	return 0;

exit_unmap_page:
	vunmap(debug_area);
exit_free_page:
	for (count = 0; count < debug_page_count; count++) {
		unlock_page(debug_pages[count]);
		__free_page(debug_pages[count]);
	}
	kfree(debug_pages);

	return ret;
}

/**
 * @brief				Free debug memory
 * @param [in]	sb			Pointer to superblock
 * @return				none
 */
static void vdfs2_free_debug_area(struct super_block *sb)
{
	int count = 0;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	unsigned int debug_pages_count = VDFS2_DEBUG_AREA_PAGE_COUNT(sbi);

	vunmap(sbi->debug_area);
	sbi->debug_area = NULL;
	for (count = 0; count < debug_pages_count; count++)
		__free_page(sbi->debug_pages[count]);
	kfree(sbi->debug_pages);
	sbi->debug_pages = NULL;
}

/**
 * @brief		Reads and checks and recovers eMMCFS super blocks.
 * @param [in]	sb	The VFS super block
 * @param [in]	silent	Do not print errors if silent is true
 * @return		Returns 0 on success, errno on failure
 */
static int vdfs2_sb_read(struct super_block *sb, int silent)
{
	void *raw_superblocks;
	void *raw_superblocks_copy;
	struct vdfs2_super_block *esb;
	struct vdfs2_super_block *esb_copy;
	int ret = 0;
	int check_sb;
	int check_sb_copy;
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct page *superblocks, *superblocks_copy;

	superblocks = sbi->superblocks;
	raw_superblocks = sbi->raw_superblock;
	if (!raw_superblocks) {
		ret = -EINVAL;
		goto error_exit;
	}

	/* alloc page for superblock copy*/
	superblocks_copy = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (IS_ERR(superblocks_copy)) {
		VDFS2_ERR("Fail to alloc page for reading superblocks copy");
		ret = -ENOMEM;
		goto error_exit;
	}
	/* read super block and extended super block from volume */
	/*  0    1    2    3      8    9   10   11     15   */
	/*  | SB | SB | SB |ESB   | SB | SB | SB |ESB   |   */
	/*  |    superblocks      |   superblocks_copy  |   */

	lock_page(superblocks_copy);
	ret = vdfs2_read_page(sb->s_bdev, superblocks_copy, VDFS2_SB_COPY_ADDR,
			PAGE_TO_SECTORS(1), 0);
	unlock_page(superblocks_copy);

	if (ret)
		goto err_superblock_copy;

	raw_superblocks_copy = kmap(superblocks_copy);
	if (!raw_superblocks_copy) {
		ret = -ENOMEM;
		goto err_superblock_copy;
	}

	/* check superblocks */
	esb = (struct vdfs2_super_block *)(raw_superblocks + VDFS2_ESB_OFFSET);
	esb_copy =  (struct vdfs2_super_block *)(raw_superblocks_copy +
			VDFS2_ESB_OFFSET);

	check_sb = vdfs2_verify_sb(esb, "SB", silent);
	check_sb_copy = vdfs2_verify_sb(esb_copy, "SB_COPY", silent);

	if (check_sb && check_sb_copy) {
		/*both superblocks are corrupted*/
		ret = check_sb;
		if (!silent || vdfs2_debug_mask & VDFS2_DBG_SB)
			VDFS2_ERR("can't find an VDFS2 filesystem on dev %s",
					sb->s_id);
	} else if ((!check_sb) && check_sb_copy) {
		/*first superblock is ok, copy is corrupted, recovery*/
		memcpy(esb_copy, esb, sizeof(*esb_copy));
		/* write superblock COPY to disk */
		lock_page(superblocks_copy);
		set_page_writeback(superblocks_copy);
		ret = vdfs2_write_page(sbi, superblocks_copy,
				VDFS2_SB_COPY_OFFSET, 2, SB_SIZE * 2, 1);
		unlock_page(superblocks_copy);
	} else if (check_sb && (!check_sb_copy)) {
		/*first superblock is corrupted, recovery*/
		memcpy(esb, esb_copy, sizeof(*esb));
		/* write superblock to disk */
		lock_page(superblocks);
		set_page_writeback(superblocks);
		ret = vdfs2_write_page(sbi, superblocks,
				VDFS2_SB_OFFSET, 2, SB_SIZE * 2, 1);
		unlock_page(superblocks);
	}

	if (ret)
		goto err_superblock_copy_unmap;
	ret = fill_runtime_superblock(esb, sb);

	if (ret)
		goto err_superblock_copy_unmap;

	sbi->raw_superblock_copy = raw_superblocks_copy;
	sbi->superblocks_copy = superblocks_copy;

	sbi->erase_block_size = le32_to_cpu(esb->erase_block_size);
	sbi->log_erase_block_size = esb->log_erase_block_size;
	sbi->erase_block_size_in_blocks = sbi->erase_block_size >>
			sbi->block_size_shift;
	sbi->log_erase_block_size_in_blocks = sbi->log_erase_block_size -
			sbi->block_size_shift;
	if (esb->read_only) {
		VDFS2_SET_READONLY(sb);
		set_option(VDFS2_SB(sb), STRIPPED);
	}
	return 0;
err_superblock_copy_unmap:
	kunmap(superblocks_copy);
err_superblock_copy:
	__free_page(superblocks_copy);
error_exit:
	sbi->raw_superblock = NULL;
	kunmap(sbi->superblocks);
	__free_page(sbi->superblocks);
	sbi->superblocks = NULL;
	return ret;
}

/**
 * @brief		Verifies eMMCFS extended super block checksum.
 * @param [in]	exsb	The eMMCFS super block
 * @return		Returns 0 on success, errno on failure
 */
static int vdfs2_verify_exsb(struct vdfs2_extended_super_block *exsb)
{
	__le32 checksum;

	if (!VDFS2_EXSB_VERSION(exsb)) {
		VDFS2_ERR("Bad version of extended super block");
		return -EINVAL;
	}

	/* check crc32 */
	checksum = crc32(0, exsb, sizeof(*exsb) - sizeof(exsb->checksum));

	if (exsb->checksum != checksum) {
		VDFS2_ERR("bad checksum of extended super block - 0x%x, "\
				"must be 0x%x\n", exsb->checksum, checksum);
		return -EINVAL;
	} else
		return 0;
}

/**
 * @brief		Reads and checks eMMCFS extended super block.
 * @param [in]	sb	The VFS super block
 * @return		Returns 0 on success, errno on failure
 */
static int vdfs2_extended_sb_read(struct super_block *sb)
{
	struct vdfs2_sb_info *sbi = sb->s_fs_info;
	struct vdfs2_extended_super_block *exsb = VDFS2_RAW_EXSB(sbi);
	struct vdfs2_extended_super_block *exsb_copy =
			VDFS2_RAW_EXSB_COPY(sbi);
	int ret = 0;
	int check_exsb;
	int check_exsb_copy;

	check_exsb = vdfs2_verify_exsb(exsb);
	check_exsb_copy = vdfs2_verify_exsb(exsb_copy);

	if (check_exsb && check_exsb_copy) {
		VDFS2_ERR("Extended superblocks are corrupted");
		ret = check_exsb;
		return ret;
	} else if ((!check_exsb) && check_exsb_copy) {
		/* extended superblock copy are corrupted, recovery */
		lock_page(sbi->superblocks);
		memcpy(exsb_copy, exsb, sizeof(*exsb_copy));
		set_page_writeback(sbi->superblocks);
		ret = vdfs2_write_page(sbi, sbi->superblocks,
			VDFS2_EXSB_COPY_OFFSET, VDFS2_EXSB_SIZE_SECTORS,
			(SB_SIZE_IN_SECTOR * 3) * SECTOR_SIZE, 1);
		unlock_page(sbi->superblocks);
	} else if (check_exsb && (!check_exsb_copy)) {
		/* main extended superblock are corrupted, recovery */
		lock_page(sbi->superblocks_copy);
		memcpy(exsb, exsb_copy, sizeof(*exsb));
		set_page_writeback(sbi->superblocks_copy);
		ret = vdfs2_write_page(sbi, sbi->superblocks_copy,
				VDFS2_EXSB_OFFSET, VDFS2_EXSB_SIZE_SECTORS,
				(SB_SIZE_IN_SECTOR * 3) * SECTOR_SIZE, 1);
		unlock_page(sbi->superblocks_copy);
	}

	atomic64_set(&sbi->tiny_files_counter,
			le64_to_cpu(exsb->tiny_files_counter));

	return ret;
}

/**
 * @brief		The eMMCFS B-tree common constructor.
 * @param [in]	sbi	The eMMCFS superblock info
 * @param [in]	btree	The eMMCFS B-tree
 * @param [in]	inode	The inode
 * @return		Returns 0 on success, errno on failure
 */
int vdfs2_fill_btree(struct vdfs2_sb_info *sbi,
		struct vdfs2_btree *btree, struct inode *inode)
{
	struct vdfs2_bnode *head_bnode, *bnode;
	struct vdfs2_raw_btree_head *raw_btree_head;
	__u64 bitmap_size;
	int err = 0;
	enum vdfs2_get_bnode_mode mode;
	int loop;

	btree->sbi = sbi;
	btree->inode = inode;
	btree->pages_per_node = 1 << (sbi->log_blocks_in_leb +
			sbi->block_size_shift - PAGE_SHIFT);
	btree->log_pages_per_node = sbi->log_blocks_in_leb +
			sbi->block_size_shift - PAGE_SHIFT;
	btree->node_size_bytes = btree->pages_per_node << PAGE_SHIFT;

	btree->rw_tree_lock = kzalloc(sizeof(rw_mutex_t), GFP_KERNEL);
	if (!btree->rw_tree_lock)
		return -ENOMEM;

	init_mutex(btree->rw_tree_lock);
	if (VDFS2_IS_READONLY(sbi->sb) ||
			(btree->btree_type == VDFS2_BTREE_PACK))
		mode = VDFS2_BNODE_MODE_RO;
	else
		mode = VDFS2_BNODE_MODE_RW;

	mutex_init(&btree->hash_lock);

	for (loop = 0; loop < VDFS2_BNODE_HASH_SIZE; loop++)
		INIT_HLIST_HEAD(&btree->hash_table[loop]);

	btree->active_use_count = 0;
	INIT_LIST_HEAD(&btree->active_use);

	btree->passive_use_count = 0;
	INIT_LIST_HEAD(&btree->passive_use);

	head_bnode =  __vdfs2_get_bnode(btree, 0, mode);
	if (IS_ERR(head_bnode)) {
		err = PTR_ERR(head_bnode);
		goto free_mem;
	}

	raw_btree_head = head_bnode->data;

	/* Check the magic */
	if (memcmp(raw_btree_head->magic, VDFS2_BTREE_HEAD_NODE_MAGIC,
				sizeof(VDFS2_BTREE_HEAD_NODE_MAGIC) - 1)) {
		err = -EINVAL;
		goto err_put_bnode;
	}

	btree->head_bnode = head_bnode;

	/* Fill free bnode bitmpap */
	bitmap_size = btree->node_size_bytes -
		((void *) &raw_btree_head->bitmap - head_bnode->data) -
		VDFS2_BNODE_FIRST_OFFSET;
	btree->bitmap = vdfs2_build_free_bnode_bitmap(&raw_btree_head->bitmap, 0,
			bitmap_size, head_bnode);
	if (IS_ERR(btree->bitmap)) {
		err = PTR_ERR(btree->bitmap);
		btree->bitmap = NULL;
		goto err_put_bnode;
	}

	/* Check if root bnode non-empty */
	bnode = __vdfs2_get_bnode(btree, vdfs2_btree_get_root_id(btree),
			VDFS2_BNODE_MODE_RO);

	if (IS_ERR(bnode)) {
		err = PTR_ERR(bnode);
		goto err_put_bnode;
	}

	if (VDFS2_BNODE_DSCR(bnode)->recs_count == 0) {
		vdfs2_put_bnode(bnode);
		err = -EINVAL;
		goto err_put_bnode;
	}
	vdfs2_put_bnode(bnode);

	mutex_init(&btree->split_buff_lock);
	btree->split_buff = kzalloc(btree->node_size_bytes, GFP_KERNEL);
	if (!btree->split_buff)
		goto err_put_bnode;

	return 0;

err_put_bnode:
	vdfs2_put_bnode(head_bnode);
	kfree(btree->bitmap);
free_mem:
	kfree(btree->rw_tree_lock);
	return err;
}

/**
 * @brief			Catalog tree constructor.
 * @param [in,out]	sbi	The eMMCFS super block info
 * @return			Returns 0 on success, errno on failure
 */
static int vdfs2_fill_cat_tree(struct vdfs2_sb_info *sbi)
{
	struct inode *inode = NULL;
	struct vdfs2_btree *cat_tree;

	int err = 0;

	BUILD_BUG_ON(sizeof(struct vdfs2_btree_gen_record) !=
			sizeof(struct vdfs2_cattree_record));

	cat_tree = kzalloc(sizeof(*cat_tree), GFP_KERNEL);
	if (!cat_tree)
		return -ENOMEM;

	cat_tree->btree_type = VDFS2_BTREE_CATALOG;
	cat_tree->max_record_len = sizeof(struct vdfs2_cattree_key) +
			sizeof(struct vdfs2_catalog_file_record) + 1;
	inode = vdfs2_special_iget(sbi->sb, VDFS2_CAT_TREE_INO);
	if (IS_ERR(inode)) {
		int ret = PTR_ERR(inode);
		kfree(cat_tree);
		return ret;
	}

	err = vdfs2_fill_btree(sbi, cat_tree, inode);
	if (err)
		goto err_put_inode;

	cat_tree->comp_fn = test_option(sbi, CASE_INSENSITIVE) ?
			vdfs2_cattree_cmpfn_ci : vdfs2_cattree_cmpfn;
	sbi->catalog_tree = cat_tree;

	/*sbi->max_cattree_height = count_max_cattree_height(sbi);*/
	return 0;

err_put_inode:
	iput(inode);
	VDFS2_ERR("can not read catalog tree");
	kfree(cat_tree);
	return err;
}

/**
 * @brief			Extents tree constructor.
 * @param [in,out]	sbi	The eMMCFS super block info
 * @return			Returns 0 on success, errno on failure
 */
static int vdfs2_fill_ext_tree(struct vdfs2_sb_info *sbi)
{
	struct inode *inode = NULL;
	struct vdfs2_btree *ext_tree;
	int err = 0;

	BUILD_BUG_ON(sizeof(struct vdfs2_btree_gen_record) !=
			sizeof(struct vdfs2_exttree_record));

	ext_tree = kzalloc(sizeof(*ext_tree), GFP_KERNEL);
	if (!ext_tree)
		return -ENOMEM;

	ext_tree->btree_type = VDFS2_BTREE_EXTENTS;
	ext_tree->max_record_len = sizeof(struct vdfs2_exttree_key) +
			sizeof(struct vdfs2_exttree_lrecord);
	inode = vdfs2_special_iget(sbi->sb, VDFS2_EXTENTS_TREE_INO);
	if (IS_ERR(inode)) {
		int ret;
		kfree(ext_tree);
		ret = PTR_ERR(inode);
		return ret;
	}

	err = vdfs2_fill_btree(sbi, ext_tree, inode);
	if (err)
		goto err_put_inode;

	ext_tree->comp_fn = vdfs2_exttree_cmpfn;
	sbi->extents_tree = ext_tree;
	return 0;

err_put_inode:
	iput(inode);
	VDFS2_ERR("can not read extents overflow tree");
	kfree(ext_tree);
	return err;
}


/**
 * @brief			xattr tree constructor.
 * @param [in,out]	sbi	The eMMCFS super block info
 * @return			Returns 0 on success, errno on failure
 */
static int vdsf_fill_xattr_tree(struct vdfs2_sb_info *sbi)

{
	struct inode *inode = NULL;
	struct vdfs2_btree *xattr_tree;
	int err = 0;

	BUILD_BUG_ON(sizeof(struct vdfs2_btree_gen_record) !=
			sizeof(struct vdfs2_xattrtree_record));

	xattr_tree = kzalloc(sizeof(*xattr_tree), GFP_KERNEL);
	if (!xattr_tree)
		return -ENOMEM;

	xattr_tree->btree_type = VDFS2_BTREE_XATTRS;
	xattr_tree->max_record_len = sizeof(struct vdfs2_xattrtree_key) +
			VDFS2_XATTR_VAL_MAX_LEN;
	inode = vdfs2_special_iget(sbi->sb, VDFS2_XATTR_TREE_INO);
	if (IS_ERR(inode)) {
		int ret;
		kfree(xattr_tree);
		ret = PTR_ERR(inode);
		return ret;
	}

	err = vdfs2_fill_btree(sbi, xattr_tree, inode);
	if (err)
		goto err_put_inode;

	xattr_tree->comp_fn = vdfs2_xattrtree_cmpfn;
	sbi->xattr_tree = xattr_tree;
	return 0;

err_put_inode:
	iput(inode);
	VDFS2_ERR("can not read extended attributes tree");
	kfree(xattr_tree);
	return err;
}
/**
 * @brief			Hardlinks area constructor.
 * @param [in,out]	sbi	The eMMCFS super block info
 * @return			Returns 0 on success, errno on failure
 */
static int vdfs2_init_hlinks_area(struct vdfs2_sb_info *sbi)
{
	struct inode *inode = NULL;
	struct vdfs2_btree *hardlink_tree;
	int err = 0;

	hardlink_tree = kzalloc(sizeof(*hardlink_tree), GFP_KERNEL);
	if (!hardlink_tree)
		return -ENOMEM;

	hardlink_tree->btree_type = VDFS2_BTREE_HARD_LINK;
	hardlink_tree->max_record_len = sizeof(struct vdfs2_hdrtree_record);
	inode = vdfs2_special_iget(sbi->sb, VDFS2_HARDLINKS_TREE_INO);

	if (IS_ERR(inode)) {
		int ret;
		kfree(hardlink_tree);
		ret = PTR_ERR(inode);
		return ret;
	}

	err = vdfs2_fill_btree(sbi, hardlink_tree, inode);
	if (err)
		goto err_put_inode;

	hardlink_tree->comp_fn = vdfs2_hlinktree_cmpfn;
	sbi->hardlink_tree = hardlink_tree;
	return 0;

err_put_inode:
	iput(inode);
	VDFS2_ERR("can not read hard link tree");
	return err;
}

/**
 * @brief			Free inode bitmap constructor.
 * @param [in,out]	sbi	The eMMCFS super block info
 * @return			Returns 0 on success, errno on failure
 */
static int build_free_inode_bitmap(struct vdfs2_sb_info *sbi)
{
	int ret = 0;
	struct inode *inode;

	inode = vdfs2_special_iget(sbi->sb, VDFS2_FREE_INODE_BITMAP_INO);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		return ret;
	}

	atomic64_set(&sbi->free_inode_bitmap.last_used, VDFS2_1ST_FILE_INO);
	sbi->free_inode_bitmap.inode = inode;
	return ret;
}


#ifdef CONFIG_VDFS2_NFS_SUPPORT
/**
 * @brief			Encode inode for export via NFS.
 * @param [in]	ino	Inode to be exported via nfs
 * @param [in,out]	fh	File handle allocated by nfs, this function
 *				fill this memory ino and generation.
 * @param [in,out]	max_len	max len of file handle,
 *			nfs pass to this function max len of the fh
 *			this function return real size of the fh.
 * @param [in] parent	Parent inode of the ino.
 * @return			Returns FILEID_INO32_GEN_PARENT
 */
#if defined(CONFIG_SAMSUNG_VD_DTV_PLATFORM) || \
	(LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5))
static int vdfs2_encode_fh(
		struct inode *ino, __u32 *fh, int *max_len,
			struct inode *parent
		)
{

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5))
	struct dentry *dentry = d_find_any_alias(ino);
#else
	struct dentry *dentry = list_entry(ino->i_dentry.next,
					    struct dentry, d_alias);
#endif
#else
static int vdfs2_encode_fh(
		struct dentry *dentry, __u32 *fh, int *max_len,
					int connectable
		)
{

	struct inode *ino = dentry->d_inode;
#endif
	struct vdfs2_indirect_key coords;

	struct vdfs2_sb_info *sbi = ino->i_sb->s_fs_info;
	int ret  = 0;
	struct vdfs2_inode_info *inode_i = VDFS2_I(ino);
	struct vdfs2_cattree_key *record;

	coords.ino = ino->i_ino;
	coords.generation = ino->i_generation;

	coords.inode_type = inode_i->record_type;
	memcpy(fh, (void *) &coords, sizeof(struct vdfs2_indirect_key));
	*max_len = (sizeof(struct vdfs2_indirect_key) +
			(sizeof(u32) - 1)) / sizeof(u32);
	if (inode_i->record_type > VDFS2_CATALOG_PTREE_RECORD)
		return FILEID_INO32_GEN_PARENT;

	record = vdfs2_alloc_cattree_key(dentry->d_name.len,
			inode_i->record_type);

	if (IS_ERR(record))
		return PTR_ERR(record);

	vdfs2_fill_cattree_key(record, inode_i->parent_id, dentry->d_name.name,
			dentry->d_name.len);

	ret = vdfs2_add_indirect_index(sbi->catalog_tree, coords.generation,
			coords.ino, record);
	return FILEID_INO32_GEN_PARENT;
}

/**
 * @brief			Decode file handle to dentry for NFS.
 * @param [in]	sb	Superblock struct
 * @param [in]	fid	File handle allocated by nfs.
 * @param [in]	fh_len	len of file handle,
 * @param [in]	fh_type	file handle type
 * @return			Returns decoded dentry
 */
static struct dentry *vdfs2_fh_to_dentry(struct super_block *sb,
		struct fid *fid, int fh_len, int fh_type)
{
	struct vdfs2_indirect_key *key = (struct vdfs2_indirect_key *)fid;
	struct inode *inode = NULL;
	int ret = 0;

	switch (fh_type) {
	case FILEID_INO32_GEN_PARENT:
		if (key->inode_type > VDFS2_CATALOG_PTREE_RECORD)
			inode = vdfs2_get_packtree_indirect_inode(sb, key);
		else
			inode = vdfs2_get_indirect_inode(VDFS2_SB(sb)->
				catalog_tree, key);
		if (IS_ERR(inode)) {
			ret = PTR_ERR(inode);
			goto exit;
		}
		break;
	default:
		ret = -ENOENT;
		break;
	}

exit:
	if (ret)
		return ERR_PTR(ret);
	else
		return d_obtain_alias(inode);
}


static const struct export_operations vdfs2_export_ops = {
	.encode_fh = vdfs2_encode_fh,
	.fh_to_dentry = vdfs2_fh_to_dentry,
};
#endif

/**
 * @brief			Dirty extended super page.
 * @param [in,out]	sb	The VFS superblock
 */
void vdfs2_dirty_super(struct vdfs2_sb_info *sbi)
{
	if (!(VDFS2_IS_READONLY(sbi->sb)))
		set_sbi_flag(sbi, EXSB_DIRTY);

}

/**
 * @brief			Initialize the eMMCFS filesystem.
 * @param [in,out]	sb	The VFS superblock
 * @param [in,out]	data	FS private information
 * @param [in]		silent	Flag whether to print error message
 * @remark			Reads super block and FS internal data
 *				structures for futher use.
 * @return			Return 0 on success, errno on failure
 */
static int vdfs2_fill_super(struct super_block *sb, void *data, int silent)
{
	int ret	= 0;
	struct vdfs2_sb_info *sbi;
	struct inode *root_inode;
	char bdev_name[BDEVNAME_SIZE];

#ifdef CONFIG_VDFS2_PRINT_MOUNT_TIME
	unsigned long mount_start = jiffies;
#endif
	BUILD_BUG_ON(sizeof(struct vdfs2_tiny_file_data)
			!= sizeof(struct vdfs2_fork));

#if defined(VDFS2_GIT_BRANCH) && defined(VDFS2_GIT_REV_HASH) && \
		defined(VDFS2_VERSION)
	printk(KERN_ERR "%.5s", VDFS2_VERSION);
	VDFS2_MOUNT_INFO("version is \"%s\"", VDFS2_VERSION);
	VDFS2_MOUNT_INFO("git branch is \"%s\"", VDFS2_GIT_BRANCH);
	VDFS2_MOUNT_INFO("git revhash \"%.40s\"", VDFS2_GIT_REV_HASH);
#endif
#ifdef CONFIG_VDFS2_NOOPTIMIZE
	printk(KERN_WARNING "[VDFS2-warning]: Build optimization "
			"is switched off");
#endif

	BUILD_BUG_ON(sizeof(struct vdfs2_extended_super_block) != 2048);

	if (!sb)
		return -ENXIO;
	if (!sb->s_bdev)
		return -ENXIO;

	VDFS2_MOUNT_INFO("mounting %s", bdevname(sb->s_bdev, bdev_name));
	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	sbi->sb = sb;
#ifdef CONFIG_VDFS2_HW2_SUPPORT
	/* Check support of hw decompression by block device */
	if (sb->s_bdev->bd_disk) {
		const struct block_device_operations *ops;
		ops = sb->s_bdev->bd_disk->fops;
		if (ops->hw_decompress_vec) {
			sbi->use_hw_decompressor = 1;
			VDFS2_MOUNT_INFO("hw decompression enabled\n");
		}
	}
#endif
	INIT_LIST_HEAD(&sbi->packtree_images.list);
	mutex_init(&sbi->packtree_images.lock_pactree_list);
	INIT_LIST_HEAD(&sbi->dirty_list_head);
	spin_lock_init(&sbi->dirty_list_lock);
	percpu_counter_init(&sbi->free_blocks_count, 0, GFP_KERNEL);
	sb->s_maxbytes = VDFS2_MAX_FILE_SIZE_IN_BYTES;
	INIT_DELAYED_WORK(&sbi->delayed_commit, vdfs2_delayed_commit);
	ret = vdfs2_parse_options(sb, data);
	if (ret) {
		VDFS2_ERR("unable to parse mount options\n");
		ret = -EINVAL;
		goto vdfs2_parse_options_error;
	}

	ret = vdfs2_volume_check(sb);
	if (ret)
		goto not_vdfs2_volume;

	ret = vdfs2_sb_read(sb, silent);
	if (ret)
		goto vdfs2_sb_read_error;

	ret = vdfs2_extended_sb_read(sb);
	if (ret)
		goto vdfs2_extended_sb_read_error;

	if (!(VDFS2_IS_READONLY(sb))) {
		ret = vdfs2_load_debug_area(sb);
		if (ret)
			goto vdfs2_extended_sb_read_error;
	}

#ifdef CONFIG_VDFS2_CRC_CHECK
	if (VDFS2_RAW_EXSB(sbi)->crc == CRC_ENABLED)
		set_sbi_flag(sbi, VDFS2_META_CRC);
	else {
		VDFS2_MOUNT_INFO("Driver supports only signed volumes\n");
		ret  = -1;
		goto vdfs2_extended_sb_read_error;
	}
#else
	/* if the image is signed reset the signed flag */
	if (VDFS2_RAW_EXSB(sbi)->crc == CRC_ENABLED)
		VDFS2_RAW_EXSB(sbi)->crc = CRC_DISABLED;
#endif
	VDFS2_MOUNT_INFO("mounted %d times\n",
			VDFS2_RAW_EXSB(sbi)->mount_counter);

	/*vdfs2_debug_print_sb(sbi)*/;

	sb->s_op = &vdfs2_sops;
	sb->s_xattr = vdfs2_xattr_handlers;
#ifdef CONFIG_VDFS2_NFS_SUPPORT
	sb->s_export_op = &vdfs2_export_ops;
#endif
	/* s_magic is 4 bytes on 32-bit; system */
	sb->s_magic = (unsigned long) VDFS2_SB_SIGNATURE;

	sbi->max_cattree_height = 5;
	ret = vdfs2_build_snapshot_manager(sbi);
	if (ret)
		goto vdfs2_build_snapshot_manager_error;

	if (!(VDFS2_IS_READONLY(sb))) {
		ret = vdfs2_fsm_build_management(sb);
		if (ret)
			goto vdfs2_fsm_create_error;
	}

	if (test_option(sbi, DEBUG_AREA_CHECK)) {
		int debug_ctr = 0, records_count = 73;
		for (; debug_ctr < records_count; debug_ctr++) {
			ret = VDFS2_LOG_ERROR(sbi, debug_ctr);
			if (ret)
				goto vdfs2_fill_ext_tree_error;
		}
		ret = -EAGAIN;
		goto vdfs2_fill_ext_tree_error;
	}

	ret = vdfs2_fill_ext_tree(sbi);
	if (ret)
		goto vdfs2_fill_ext_tree_error;

	ret = vdfs2_fill_cat_tree(sbi);
	if (ret)
		goto vdfs2_fill_cat_tree_error;

	ret = vdsf_fill_xattr_tree(sbi);
	if (ret)
		goto vdfs2_fill_xattr_tree_error;

	ret = vdfs2_init_hlinks_area(sbi);
	if (ret)
		goto vdfs2_hlinks_area_err;

	ret = init_small_files_area_manager(sbi);
	if (ret)
		goto init_small_files_area_error;

#ifdef CONFIG_VDFS2_QUOTA
	ret = build_quota_manager(sbi);
	if (ret)
		goto build_quota_error;
#endif
	if (!(VDFS2_IS_READONLY(sb))) {
		ret = build_free_inode_bitmap(sbi);
		if (ret)
			goto build_free_inode_bitmap_error;

		ret = vdfs2_process_orphan_inodes(sbi);
		if (ret)
			goto process_orphan_inodes_error;
	}

	/* allocate root directory */
	root_inode = vdfs2_get_root_inode(sbi->catalog_tree);
	if (IS_ERR(root_inode)) {
		VDFS2_ERR("failed to load root directory\n");
		ret = PTR_ERR(root_inode);
		goto vdfs2_iget_err;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 5)
	sb->s_root = d_alloc_root(root_inode);
#else
	sb->s_root = d_make_root(root_inode);
#endif
	if (!sb->s_root) {
		VDFS2_ERR("unable to get root inode\n");
		ret = -EINVAL;
		goto d_alloc_root_err;
	}

	if (DATE_RESOLUTION_IN_NANOSECONDS_ENABLED)
		sb->s_time_gran = 1;
	else
		printk(KERN_WARNING
			"Date resolution in nanoseconds is disabled\n");

#ifdef CONFIG_VDFS2_CHECK_FRAGMENTATION
	for (count = 0; count < VDFS2_EXTENTS_COUNT_IN_FORK; count++)
		sbi->in_fork[count] = 0;
	sbi->in_extents_overflow = 0;
#endif

	if (test_option(sbi, BTREE_CHECK)) {
		ret = vdfs2_check_btree_links(sbi->catalog_tree, NULL);
		if (ret) {
			VDFS2_ERR("Bad btree!!!\n");
			goto btree_not_verified;
		}
	}


	if (!(VDFS2_IS_READONLY(sb))) {
		/* If somebody will switch power off right atre mounting,
		 * mount count will not inclreased
		 * But this operation takes a lot of time, and dramatically
		 * increase mount time */
		le32_add_cpu(&(VDFS2_RAW_EXSB(sbi)->mount_counter), 1);
		le32_add_cpu(&(VDFS2_RAW_EXSB(sbi)->generation), 1);
		vdfs2_dirty_super(sbi);
	}

	set_sbi_flag(sbi, IS_MOUNT_FINISHED);
	VDFS2_DEBUG_SB("finished ok");

#ifdef CONFIG_VDFS2_PRINT_MOUNT_TIME
	{
		unsigned long result = jiffies - mount_start;
		printk(KERN_INFO "Mount time %lu ms\n", result * 1000 / HZ);
	}
#endif

	vdfs2_init_high_priority(&sbi->high_priority);
	return 0;
btree_not_verified:
	dput(sb->s_root);
	sb->s_root = NULL;
	root_inode = NULL;

d_alloc_root_err:
	iput(root_inode);

vdfs2_iget_err:
process_orphan_inodes_error:
	destroy_free_inode_bitmap(sbi);

build_free_inode_bitmap_error:
#ifdef CONFIG_VDFS2_QUOTA
	destroy_quota_manager(sbi);

build_quota_error:
#endif
	destroy_small_files_area_manager(sbi);

init_small_files_area_error:
	vdfs2_put_btree(sbi->hardlink_tree);
	sbi->hardlink_tree = NULL;

vdfs2_hlinks_area_err:
	vdfs2_put_btree(sbi->xattr_tree);
	sbi->xattr_tree = NULL;

vdfs2_fill_xattr_tree_error:
	vdfs2_put_btree(sbi->catalog_tree);
	sbi->catalog_tree = NULL;

vdfs2_fill_cat_tree_error:
	vdfs2_put_btree(sbi->extents_tree);
	sbi->extents_tree = NULL;

vdfs2_fill_ext_tree_error:
	vdfs2_fsm_destroy_management(sb);

vdfs2_fsm_create_error:
	vdfs2_destroy_snapshot_manager(sbi);

vdfs2_build_snapshot_manager_error:
	if (!(VDFS2_IS_READONLY(sb)))
		vdfs2_free_debug_area(sb);

vdfs2_extended_sb_read_error:
vdfs2_sb_read_error:
vdfs2_parse_options_error:
not_vdfs2_volume:
	destroy_super(sbi);

	sb->s_fs_info = NULL;
	kfree(sbi);
	VDFS2_DEBUG_SB("finished with error = %d", ret);

	return ret;
}


/**
 * @brief				Method to get the eMMCFS super block.
 * @param [in,out]	fs_type		Describes the eMMCFS file system
 * @param [in]		flags		Input flags
 * @param [in]		dev_name	Block device name
 * @param [in,out]	data		Private information
 * @param [in,out]	mnt		Mounted eMMCFS filesystem information
 * @return				Returns 0 on success, errno on failure
 */
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
static int vdfs2_get_sb(struct file_system_type *fs_type, int flags,
		const char *dev_name, void *data, struct vfsmount *mnt)
{
	return get_sb_bdev(fs_type, flags, dev_name, data,
		vdfs2_fill_super, mnt);
}
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) || \
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
static struct dentry *vdfs2_mount(struct file_system_type *fs_type, int flags,
				const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, vdfs2_fill_super);
}
#endif

static void vdfs2_kill_block_super(struct super_block *sb)
{
	kill_block_super(sb);
}

/*
 * Structure of the eMMCFS filesystem type
 */
static struct file_system_type vdfs2_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "vdfs2",
#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
	.get_sb		= vdfs2_get_sb,
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) || \
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	.mount		= vdfs2_mount,
#else
	BUILD_VDFS2_BUG();
#endif
	.kill_sb	= vdfs2_kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

/**
 * @brief				Inode storage initializer.
 * @param [in,out]	generic_inode	Inode for init
 * @return		void
 */
static void vdfs2_init_inode_once(void *generic_inode)
{
	struct vdfs2_inode_info *inode = generic_inode;

	mutex_init(&inode->truncate_mutex);
	inode_init_once(&inode->vfs_inode);
	INIT_LIST_HEAD(&inode->runtime_extents);
}

/**
 * @brief	Initialization of the eMMCFS module.
 * @return	Returns 0 on success, errno on failure
 */
static int __init init_vdfs2_fs(void)
{
	int ret;

	vdfs2_inode_cachep = kmem_cache_create("vdfs2_icache",
				sizeof(struct vdfs2_inode_info), 0,
				SLAB_HWCACHE_ALIGN, vdfs2_init_inode_once);
	if (!vdfs2_inode_cachep) {
		VDFS2_ERR("failed to initialise inode cache\n");
		return -ENOMEM;
	}

	ret = vdfs2_init_btree_caches();
	if (ret)
		goto fail_create_btree_cache;

	ret = vdfs2_hlinktree_cache_init();
	if (ret)
		goto fail_create_hlinktree_cache;

	ret = vdfs2_exttree_cache_init();
	if (ret)
		goto fail_create_exttree_cache;

	ret = vdfs2_fsm_cache_init();

	if (ret)
		goto fail_create_fsm_cache;
	ret = register_filesystem(&vdfs2_fs_type);
	if (ret)
		goto failed_register_fs;
	writeback_thread = NULL;
	return 0;

failed_register_fs:
	vdfs2_fsm_cache_destroy();
fail_create_fsm_cache:
	vdfs2_exttree_cache_destroy();
fail_create_exttree_cache:
	vdfs2_hlinktree_cache_destroy();
fail_create_hlinktree_cache:
	vdfs2_destroy_btree_caches();
fail_create_btree_cache:

	return ret;
}

/**
 * @brief		Module unload callback.
 * @return	void
 */
static void __exit exit_vdfs2_fs(void)
{
	vdfs2_fsm_cache_destroy();
	unregister_filesystem(&vdfs2_fs_type);
	kmem_cache_destroy(vdfs2_inode_cachep);
	vdfs2_exttree_cache_destroy();
	vdfs2_hlinktree_cache_destroy();
}

module_init(init_vdfs2_fs)
module_exit(exit_vdfs2_fs)

module_param(vdfs2_debug_mask, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(vdfs2_debug_mask, "Debug mask (1 - print debug for superblock ops,"\
				" 2 - print debug for inode ops)");
MODULE_AUTHOR("Samsung R&D Russia - System Software Lab - VD Software Group");
MODULE_VERSION(__stringify(VDFS2_VERSION));
MODULE_DESCRIPTION("Vertically Deliberate improved performance File System");
MODULE_LICENSE("GPL");
