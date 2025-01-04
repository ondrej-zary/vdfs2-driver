/**
 * @file	fs/vdfs2/data.c
 * @brief	Basic data operations.
 * @author	Ivan Arishchenko, i.arishchenk@samsung.com
 * @date	05/05/2012
 *
 * eMMCFS -- Samsung eMMC chip oriented File System, Version 1.
 *
 * This file implements bio data operations and its related functions.
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

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>

#include "vdfs2.h"
#include "debug.h"


/**
 * @brief		Finalize IO writing.
 * param [in]	bio	BIO structure to be written.
 * @return	void
 */
static void end_io_write(struct bio *bio)
{
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;

		if (bio->bi_status) {
			SetPageError(page);
			if (page->mapping)
				set_bit(AS_EIO, &page->mapping->flags);
		}
		end_page_writeback(page);
	}

	if (bio->bi_private)
		complete(bio->bi_private);
	bio_put(bio);
}

/**
 * @brief		Finalize IO writing.
 * param [in]	bio	BIO structure to be read.
 * @return	void
 */
static void read_end_io(struct bio *bio)
{
	struct bio_vec *bvec;
	struct completion *wait = bio->bi_private;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;

		if (!bio->bi_status) {
			SetPageUptodate(page);
		} else {
			ClearPageUptodate(page);
			SetPageError(page);
		}
	}
	complete(wait);
	bio_put(bio);
}

/**
 * @brief			Allocate new BIO.
 * @param [in]	bdev		The eMMCFS superblock information.
 * @param [in]	first_sector	BIO first sector.
 * @param [in]	nr_vecs		Number of BIO pages.
 * @return			Returns pointer to allocated BIO structure.
 */
struct bio *vdfs2_allocate_new_bio(struct block_device *bdev, sector_t first_sector,
		int nr_vecs)
{
	/*int nr_vecs;*/
	gfp_t gfp_flags = GFP_NOFS | __GFP_HIGH;
	struct bio *bio = NULL;
	sector_t s_count = bdev->bd_inode->i_size >> SECTOR_SIZE_SHIFT;

	if ((first_sector > s_count) || ((first_sector + nr_vecs) > s_count))
		return ERR_PTR(-EFAULT);

	bio = bio_alloc(gfp_flags, nr_vecs);

	if (bio == NULL && (current->flags & PF_MEMALLOC)) {
		while (!bio && (nr_vecs /= 2))
			bio = bio_alloc(gfp_flags, nr_vecs);
	}

	if (bio) {
		bio_set_dev(bio, bdev);
		bio->bi_iter.bi_sector = first_sector;
	}

	return bio;
}


#if LINUX_VERSION_CODE == KERNEL_VERSION(2, 6, 35)
	#define SUBMIT_BIO_WAIT_FOR_FLUSH_FLAGS WRITE_BARRIER
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 20) || \
		LINUX_VERSION_CODE == KERNEL_VERSION(3, 0, 33) || \
		LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 5)
	#define SUBMIT_BIO_WAIT_FOR_FLUSH_FLAGS WRITE_FLUSH_FUA
#else
	BUILD_BUG();
#endif


static int get_sector(struct vdfs2_extent *extents, unsigned int extents_count,
		sector_t isector, unsigned int block_shift, sector_t *result,
		sector_t *length)
{
	int count;
	sector_t first_isector = 0;
	sector_t last_isector = 0;
	unsigned int shift = block_shift - SECTOR_SIZE_SHIFT;

	for (count = 0; count < extents_count; count++) {
		last_isector += le32_to_cpu(extents->length) << shift;
		if (isector >= first_isector && isector < last_isector) {
			sector_t offset = isector - first_isector;
			*result = (le64_to_cpu(extents->begin) << shift) +
					offset;
			*length = (le32_to_cpu(extents->length) << shift) -
					offset;
			return 0;
		}
		first_isector = last_isector;
		extents++;
	}
	return -EINVAL;
}

static int get_table_sector(struct vdfs2_sb_info *sbi, sector_t isector,
		sector_t *result, sector_t *length)
{
	struct vdfs2_extended_super_block *exsb = VDFS2_RAW_EXSB(sbi);
	struct vdfs2_extent *extent_table = &exsb->tables[0];
	int ret = 0;
	sector_t max_size = sbi->sb->s_bdev->bd_inode->i_size >>
			SECTOR_SIZE_SHIFT;

	ret = get_sector(extent_table, VDFS2_TABLES_EXTENTS_COUNT,
		isector, sbi->block_size_shift, result, length);

	if ((*length == 0) || (*result > max_size)) {
		if (!is_sbi_flag_set(sbi, IS_MOUNT_FINISHED)) {
			VDFS2_ERR("Error get block for metadata");
			ret = -EFAULT;
			goto error_exit;
		} else {
			BUG();
		}
	}
	/* TODO extended block */
	/* BUG_ON(ret); */
error_exit:
	return ret;
}



static int get_meta_block(struct vdfs2_sb_info *sbi, sector_t iblock,
		sector_t *result, sector_t *length)
{
	struct vdfs2_extended_super_block *exsb = VDFS2_RAW_EXSB(sbi);
	struct vdfs2_extent *extents = &exsb->meta[0];
	int count;
	sector_t first_iblock = 0;
	sector_t last_iblock = 0;

	for (count = 0; count < VDFS2_META_BTREE_EXTENTS; count++) {
		last_iblock += le64_to_cpu(extents->length);
		if (iblock >= first_iblock && iblock < last_iblock) {
			sector_t offset = iblock - first_iblock;
			*result = (le64_to_cpu(extents->begin)) +
					offset;
			*length = (le32_to_cpu(extents->length)) -
					offset;
			return 0;
		}
		first_iblock = last_iblock;
		extents++;
	}
	return -EINVAL;
}

static int get_block_meta_wrapper(struct inode *inode, pgoff_t page_index,
		sector_t *res_block, int type)
{
	struct vdfs2_sb_info *sbi = inode->i_sb->s_fs_info;
	sector_t meta_iblock, length, start_iblock;
	int ret;

	if (!type) {
		ret = vdfs2_get_meta_iblock(sbi, inode->i_ino, page_index,
			&start_iblock);
		if (ret)
			return ret;

		meta_iblock = start_iblock;
		if (is_tree(inode->i_ino)) {
			int mask;
			mask = (1 << (sbi->log_blocks_in_leb +
				sbi->block_size_shift - PAGE_SHIFT)) - 1;
			meta_iblock += (page_index & mask) << (PAGE_SHIFT
				- sbi->block_size_shift);
		}
		*res_block = 0;
		ret = get_meta_block(sbi, meta_iblock, res_block, &length);
		BUG_ON(*res_block == 0);
	} else {
		sector_t iblock;
		struct buffer_head bh_result;
		bh_result.b_blocknr = 0;
		iblock = ((sector_t)page_index) << (PAGE_SHIFT -
				sbi->block_size_shift);
		ret = vdfs2_get_block(inode, iblock, &bh_result, 0);
		*res_block = bh_result.b_blocknr;
	}
	return ret;
}

static void end_IO(struct bio *bio)
{
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;

		if (bio->bi_status) {
			SetPageError(page);
			if (page->mapping)
				set_bit(AS_EIO, &page->mapping->flags);
			VDFS2_BUG();
		}

	}

	if (bio->bi_private) {
		struct vdfs2_wait_list *wait = bio->bi_private;
		complete(&wait->wait);
	}

	bio_put(bio);
}

/**
 * @brief			Write meta data (struct page **)
 *				The function supports metadata fragmentation
 * @param [in]	sbi		The VDFS2 superblock information.
 * @param [in]	pages		Pointer to locked pages.
 * @param [in]	sector_addr	Start isector address.
 * @param [in]	page_count	Number of pages to be written.
 *				    and write snapshot head page in sync mode
 * @return			Returns 0 on success, errno on failure.
 */
int vdfs2_table_IO(struct vdfs2_sb_info *sbi, struct page **pages,
		s64 sectors_count, int opf, sector_t *isector)
{
	struct block_device *bdev = sbi->sb->s_bdev;
	struct bio *bio;
	sector_t start_sector = 0, length = 0, c_isector = *isector, sec_to_bio;
	sector_t total_sectors = sectors_count;
	int nr_vectr, ret;
	unsigned int count = 0, page_count = DIV_ROUND_UP(SECTOR_SIZE *
			sectors_count, PAGE_SIZE);
	struct blk_plug plug;
	struct list_head wait_list_head;
	struct vdfs2_wait_list *new_request;
	struct list_head *pos, *q; /* temporary storage */
	sector_t l_sector = 0;

	for (count = 0; count < page_count; count++) {
		lock_page(pages[count]);
		if (opf & REQ_OP_WRITE)
			set_page_writeback(pages[count]);
	}
	INIT_LIST_HEAD(&wait_list_head);
	blk_start_plug(&plug);

	do {
		unsigned int size;

		nr_vectr = (page_count < BIO_MAX_PAGES) ? page_count :
				BIO_MAX_PAGES;

		ret = get_table_sector(sbi, c_isector + l_sector,
				&start_sector, &length);
		if (ret)
			goto error_exit;


		bio = vdfs2_allocate_new_bio(bdev, start_sector, nr_vectr);
		if (IS_ERR_OR_NULL(bio)) {
			ret = -ENOMEM;
			goto error_exit;
		}

		new_request = (struct vdfs2_wait_list *)
				kzalloc(sizeof(*new_request), GFP_KERNEL);

		if (!new_request) {
			ret = -ENOMEM;
			bio_put(bio);
			goto error_exit;
		}
		INIT_LIST_HEAD(&new_request->list);
		init_completion(&new_request->wait);
		new_request->number = start_sector;
		list_add_tail(&new_request->list, &wait_list_head);
		bio->bi_end_io = end_IO;
		bio->bi_private = new_request;
		sec_to_bio = nr_vectr << (PAGE_SHIFT - SECTOR_SIZE_SHIFT);

		sec_to_bio = min(sec_to_bio, length);
		sec_to_bio = min(sec_to_bio, (sector_t)sectors_count);

		do {
			unsigned int add_size, add_offset, index;
			index = l_sector >> (PAGE_SHIFT -
					SECTOR_SIZE_SHIFT);
			add_offset = (l_sector & (SECTOR_PER_PAGE - 1)) *
					SECTOR_SIZE;
			add_size = min((unsigned int)PAGE_SIZE -
				add_offset, (unsigned int)sec_to_bio *
				SECTOR_SIZE);
			size = bio_add_page(bio, pages[index], add_size,
					add_offset);
			l_sector += (size >> SECTOR_SIZE_SHIFT);
			sectors_count -= (size >> SECTOR_SIZE_SHIFT);
			sec_to_bio -= (size >> SECTOR_SIZE_SHIFT);
			if (!size && (!bio->bi_vcnt)) {
				/* fail to add data into BIO */
				ret = -EFAULT;
				bio_put(bio);
				goto error_exit;
			} else if (!size) {
				/* no space left in bio */
				break;
			}
		} while (sec_to_bio);
		bio->bi_opf = opf;
		submit_bio(bio);

	} while (sectors_count > 0);
	BUG_ON(sectors_count < 0);

error_exit:
	blk_finish_plug(&plug);

	list_for_each_safe(pos, q, &wait_list_head) {
		new_request = list_entry(pos, struct vdfs2_wait_list, list);
		/* Synchronous write operation */
		wait_for_completion(&new_request->wait);
		list_del(pos);
		kfree(new_request);
	}

	for (count = 0; count < page_count; count++) {
		unlock_page(pages[count]);
		if (opf & REQ_OP_WRITE)
			end_page_writeback(pages[count]);
	}

	if (!ret)
		*isector += total_sectors;

	return ret;
}




/**
 * @brief			Read page from the given sector address.
 *				Fill the locked page with data located in the
 *				sector address. Read operation is synchronous,
 *				and caller must unlock the page.
 * @param [in]	bdev		Pointer to block device.
 * @param [in]	page		Pointer to locked page.
 * @param [in]	sector_addr	Sector address.
 * @param [in]	page_count	Number of pages to be read.
 * @return			Returns 0 on success, errno on failure
 */
int vdfs2_read_pages(struct block_device *bdev,
			struct page **page,
			sector_t sector_addr,
			unsigned int page_count)
{
	struct bio *bio;
	struct completion wait;
	unsigned int count = 0;
	int continue_load = 0;
	int nr_vectr = (page_count < BIO_MAX_PAGES) ?
				page_count : BIO_MAX_PAGES;

	struct blk_plug plug;


	init_completion(&wait);
again:
	blk_start_plug(&plug);

	/* Allocate a new bio */
	bio = vdfs2_allocate_new_bio(bdev, sector_addr, nr_vectr);
	if (IS_ERR_OR_NULL(bio)) {
		blk_finish_plug(&plug);
		VDFS2_ERR("failed to allocate bio\n");
		return PTR_ERR(bio);
	}

	bio->bi_end_io = read_end_io;

	/* Initialize the bio */
	for (; count < page_count; count++) {
		if ((unsigned) bio_add_page(bio, page[count],
				PAGE_SIZE, 0) < PAGE_SIZE) {
			if (bio->bi_vcnt) {
				continue_load = 1;
				sector_addr += (count << (PAGE_SHIFT -
						SECTOR_SIZE_SHIFT));
			} else {
				VDFS2_ERR("FAIL to add page to BIO");
				bio_put(bio);
				blk_finish_plug(&plug);
				return -EFAULT;
			}

			break;
		}
	}
	bio->bi_private = &wait;
	bio->bi_opf = REQ_OP_READ;
	submit_bio(bio);
	blk_finish_plug(&plug);

	/* Synchronous read operation */
	wait_for_completion(&wait);

	if (continue_load) {
		continue_load = 0;
		goto again;
	}

	return 0;
}


/**
 * @brief			Read page from the given sector address.
 *				Fill the locked page with data located in the
 *				sector address. Read operation is synchronous,
 *				and caller must unlock the page.
 * @param [in]	bdev		Pointer to block device.
 * @param [in]	page		Pointer to locked page.
 * @param [in]	sector_addr	Sector address.
 * @param [in]	sector_count	Number of sectors to be read.
 * @param [in]	offset		Offset value in page.
 * @return			Returns 0 on success, errno on failure.
 */
int vdfs2_read_page(struct block_device *bdev,
			struct page *page,
			sector_t sector_addr,
			unsigned int sector_count,
			unsigned int offset)
{
	struct bio *bio;
	struct completion wait;
	struct blk_plug plug;

	if (sector_count > SECTOR_PER_PAGE + 1)
		return -EINVAL;

	if (PageUptodate(page))
		return 0;

	/* Allocate a new bio */
	bio = vdfs2_allocate_new_bio(bdev, sector_addr, 1);
	if (IS_ERR_OR_NULL(bio))
		return PTR_ERR(bio);

	init_completion(&wait);
	blk_start_plug(&plug);

	/* Initialize the bio */
	bio->bi_end_io = read_end_io;
	if ((unsigned) bio_add_page(bio, page, SECTOR_SIZE * sector_count,
			offset)	< SECTOR_SIZE * sector_count) {
		VDFS2_ERR("FAIL to add page to BIO");
		bio_put(bio);
		blk_finish_plug(&plug);
		return -EFAULT;
	}
	bio->bi_private = &wait;
	bio->bi_opf = REQ_OP_READ;
	submit_bio(bio);
	blk_finish_plug(&plug);

	/* Synchronous read operation */
	wait_for_completion(&wait);
	return 0;
}

/**
 * @brief			Write page to the given sector address.
 *				Write the locked page to the sector address.
 *				Write operation is synchronous, and caller
 *				must unlock the page.
 * @param [in]	bdev		The eMMCFS superblock information.
 * @param [in]	page		Pointer to locked page.
 * @param [in]	sector_addr	Sector address.
 * @param [in]	sector_count	Number of sector to be written.
 * @param [out] written_bytes	Number of actually written bytes
 * @return			Returns 0 on success, errno on failure.
 */
int vdfs2_write_page(struct vdfs2_sb_info *sbi,
			struct page *page,
			sector_t sector_addr,
			unsigned int sector_count,
			unsigned int offset, int sync_mode)
{
	struct bio *bio;
	struct completion wait;
	struct block_device *bdev = sbi->sb->s_bdev;
	struct blk_plug plug;

	if (sector_count > SECTOR_PER_PAGE)
		return -EINVAL;

	if (VDFS2_IS_READONLY(sbi->sb)) {
		end_page_writeback(page);
		return 0;
	}

	/* Allocate a new bio */
	bio = vdfs2_allocate_new_bio(bdev, sector_addr, 1);
	if (IS_ERR_OR_NULL(bio))
		return PTR_ERR(bio);

	blk_start_plug(&plug);
	if (sync_mode)
		init_completion(&wait);

	/* Initialize the bio */
	bio->bi_end_io = end_io_write;
	if ((unsigned) bio_add_page(bio, page, SECTOR_SIZE * sector_count,
			offset) < SECTOR_SIZE * sector_count) {
		VDFS2_ERR("FAIL to add page to BIO");
		bio_put(bio);
		blk_finish_plug(&plug);
		end_page_writeback(page);
		return -EFAULT;
	}
	if (sync_mode)
		bio->bi_private = &wait;

	bio->bi_opf = REQ_OP_WRITE | REQ_PREFLUSH | REQ_FUA;
	submit_bio(bio);
	blk_finish_plug(&plug);
	if (sync_mode) {
		/* Synchronous write operation */
		wait_for_completion(&wait);
		if (PageError(page))
			return -EFAULT;
	}
	return 0;
}
/**
 * @brief			Calculate aligned value.
 * @param [in]	value		Value to align.
 * @param [in]	gran_log2	Alignment granularity log2.
 * @return			val, aligned up to granularity boundary.
 */
static inline sector_t align_up(sector_t val, int gran_log2)
{
	return (val + (1<<gran_log2) - 1) & ~((1<<gran_log2) - 1);
}

/**
 * @brief		Sign  pages with crc number
 * @param [in]	mapping	Mapping with pages to sign
 * @param [in]	magic_len	Length of the magic string, the first
 *				magic_len bytes will be skiped during crc
 *				calculation.
 * @return			0 - if page signed successfully
 *				or error code otherwise
 */
int vdfs2_check_and_sign_pages(struct page *page, char *magic, int magic_len,
		__u64 version)
{
	void *data;
	data = kmap(page);
	if (!data) {
		VDFS2_ERR("Can not allocate virtual memory");
		return -ENOMEM;
	}
#if defined(CONFIG_VDFS2_META_SANITY_CHECK)
	if (memcmp(data, magic, magic_len - VERSION_SIZE) !=
					0) {
		printk(KERN_ERR "invalid bitmap magic for %s,"
			" %lu, actual = %s\n", magic,
			page->mapping->host->i_ino, (char *)data);
		BUG();
	}
#endif
	memcpy(data + magic_len - VERSION_SIZE, &version, VERSION_SIZE);
	vdfs2_update_block_crc(data, PAGE_SIZE, magic_len);
	kunmap(page);
	return 0;
}
/**
 * @brief		Validata page crc and magic number
 * @param [in]	page	page to validate
 * @param [in]	magic	magic to validate
 * @param [in]	magic_len	magic len in bytes
 * @return			0 - if crc and magic are valid
 *				1 - if crc or magic is invalid
 */
int vdfs2_validate_page(struct page *page)
{
	void *data;
	int ret_val = 0;
	char *magic;
	int magic_len;
	int ino = page->mapping->host->i_ino;
	switch (ino) {
	case VDFS2_SMALL_AREA_BITMAP:
		magic = SMALL_AREA_BITMAP_MAGIC;
		magic_len = SMALL_AREA_BITMAP_MAGIC_LEN;
		break;
	case VDFS2_FREE_INODE_BITMAP_INO:
		magic = INODE_BITMAP_MAGIC;
		magic_len = INODE_BITMAP_MAGIC_LEN;
		break;
	case VDFS2_SPACE_BITMAP_INO:
		magic = FSM_BMP_MAGIC;
		magic_len = FSM_BMP_MAGIC_LEN;
		break;
	default:
		return ret_val;
	}
	data = kmap(page);
	if (!data) {
		VDFS2_ERR("Can not allocate virtual memory");
		return -ENOMEM;
	}
	ret_val = vdfs2_validate_crc(data, PAGE_SIZE, magic, magic_len);
	kunmap(page);
	return ret_val;
}

/**
 * @brief				Update the buffer with magic and crc
 *					numbers. the magic will be placed in
 *					first bytes, the crc will be placed
 *					in last 4 bytes.
 * @param [in]	buff			Buffer to update.
 * @param [in]	block_size		Block size
 * @param [in]	magic_len		Length of the magic string, the first
 *				magic_len bytes will be skiped during crc
 *				calculation.
 */
void vdfs2_update_block_crc(char *buff, int block_size, int magic_len)
{
#ifdef CONFIG_VDFS2_CRC_CHECK
	int crc = 0;
	/* set crc to the end of the buffer */
	crc = cpu_to_le32(crc32(0, buff + magic_len,
		block_size - (CRC32_SIZE  +
		magic_len)));
	memcpy(VDFS2_CRC32_OFFSET(buff, block_size), &crc, CRC32_SIZE);
#endif
}
/**
 * @brief				Validate the buffer magic and crc
 *					the magic should be in first bytes,
 *					the crc should be in last 4 bytes
 * @param [in]	buff			Buffer to validate.
 * @param [in]	buff_size		Size of the buffer
 * @param [in]	magic			Magic word to validate. If it's null
 *					the magic validation will be skipped.
 * @param [in]	magic_len		Length of the magic word in bytes
 * @return				0 - if crc and magic are valid
 *					1 - if crc or magic is invalid
  */
int vdfs2_validate_crc(char *buff, int buff_size, const char *magic,
		int magic_len)
{
#ifdef CONFIG_VDFS2_CRC_CHECK
	int crc = VDFS2_VALID;
#endif
	int ret_val = 0;
	if (magic) {
		/* if magic is not valid */
		if (memcmp(buff, magic, magic_len - VERSION_SIZE) != 0) {
			VDFS2_ERR("Magic is NOT valid. magic: %s", magic);
			ret_val = VDFS2_INVALID;
		}
	}
#ifdef CONFIG_VDFS2_CRC_CHECK
	/* if magic is valid */
	if (ret_val == 0) {
		crc = cpu_to_le32(crc32(0, buff + magic_len, buff_size -
			(CRC32_SIZE + magic_len)));
		if (memcmp(VDFS2_CRC32_OFFSET(buff, buff_size),
				&crc, CRC32_SIZE) != 0) {
			VDFS2_ERR("CRC is NOT valid. Buffer: %s", magic);
			ret_val = VDFS2_INVALID;
		}
	}
#endif
	return ret_val;
}

/**
 * @brief				Set bits inside buffer and update
 *					sign and crc values for updated
 *					buffer.
 * @param [in]	buff			Buffer to validate.
 * @param [in]	buff_size		Size of the buffer
 * @param [in]	offset			Offset of the start bit for setting
 * @param [in]	count			Number of bits to be set
 * @param [in]	magic_len		Length of the magic word in bytes
 * @param [in]	block_size		Size of block for block device.

 */
int vdfs2_set_bits(char *buff, int buff_size, int offset, int count,
		int magic_len, int block_size) {
	/* data block size in bits */
	const int datablock_size = (block_size - (magic_len
				+ CRC32_SIZE))<<3;
	/* pointer to begin of start block */
	char *start_blck = buff + ((offset / datablock_size) * block_size);
	/* pointer to last block */
	char *end_blck =  buff + (((offset + count - 1) / datablock_size) *
			block_size);
	char *cur_blck = NULL;
	int cur_position = 0;
	u_int32_t length = 0;
	int i = 0;
	char *end_buff;


	for (cur_blck = start_blck; cur_blck <= end_blck;
				cur_blck += block_size) {
		/* if it first block */
		if (cur_blck == start_blck)
			cur_position = offset % datablock_size;
		else
			cur_position = 0;

		length = (datablock_size - cur_position);
		if (count < length)
			length = count;
		else
			count -= length;
		end_buff = cur_blck + block_size - CRC32_SIZE;
		/* set bits */
		for (i = 0; i < length; i++) {
			/* check the bound of array */
			if ((cur_blck + (cur_position>>3) +
					magic_len) > end_buff)
				return -EFAULT;
			/* set bits */
			if (test_and_set_bit(cur_position, (void *)(cur_blck +
					magic_len)))
				return -EFAULT;

			cur_position++;
		}
	}
	return 0;
}

/**
 * @brief			Clear bits inside buffer and update
 *					sign and crc values for updated
 *					buffer.
 * @param [in]	buff			Buffer to validate.
 * @param [in]	buff_size		Size of the buffer
 * @param [in]	offset			Offset of the start bit for setting
 * @param [in]	count			Number of bits to be set
 * @param [in]	magic_len		Length of the magic word in bytes
 * @param [in]	block_size		Size of block for block device.
 * @return				Error code or 0 if success
 */
int vdfs2_clear_bits(char *buff, int buff_size, int offset, int count,
		int magic_len, int block_size) {
	/* data block size in bits */
	const int datablock_size = (block_size - (magic_len
				+ CRC32_SIZE))<<3;
	/* pointer to begin of start block */
	char *start_blck = buff + ((offset / datablock_size) * block_size);
	/* pointer to last block */
	char *end_blck =  buff + (((offset + count - 1) / datablock_size) *
			block_size);
	char *cur_blck = NULL;
	int cur_position = 0;
	u_int32_t length = 0;
	int i = 0;
	char *end_buff;

	/* go through all blcoks */
	for (cur_blck = start_blck; cur_blck <= end_blck;
				cur_blck += block_size) {
		/* if it first block */
		if (cur_blck == start_blck)
			cur_position = offset % datablock_size;
		else
			cur_position = 0;

		length = (datablock_size - cur_position);
		if (count < length) {
			length = count;
			count -= length;
		} else
			count -= length;
		end_buff = cur_blck + block_size - CRC32_SIZE;
		/* set bits */
		for (i = 0; i < length; i++) {
			/* check the boundary of array */
			if ((cur_blck + (cur_position>>3) + magic_len)
					> end_buff)
				return -EFAULT;

			/* set bits */
			if (!test_and_clear_bit(cur_position,
					(void *)(cur_blck + magic_len)))
				return -EFAULT;
			cur_position++;
		}
	}
	return 0;
}
static int  __find_next_bit_in_block(const void *addr, unsigned int size,
		unsigned long offset, unsigned int magic_len, char bit)
{
	int result = 0;
	/* start of the block */
	const unsigned long *int_addr = (const unsigned long *)(((char *)addr)
			+ magic_len);
	/* size of the block in bits */
	/*unsigned long int_size = ((block_size - (CRC32_SIZE +
			magic_len)) << 3);*/
	if (bit == '0')
		result = find_next_zero_bit(int_addr, size, offset);
	else
		result = find_next_bit(int_addr, size, offset);

	if (result >= size)
		return -1;
	else
		return result;
}
/**
 * @brief			Find next zero bit in the array.
 *					this function take care about
 *					all special symbols in the array
 *					like magic and crc number.
 * @param [in]	addr			Source buffer with data.
 * @param [in]	size			Size of the buffer in bits
 * @param [in]	offset			start bit position for the searching
 * @param [in]	block_size		Size of block for block device.
 * @param [in]	magic_len		Length of the magic
 * @return						Previous state of the bit
 */
static unsigned long  __find_next_bit(const void *addr,
		unsigned long size, unsigned long offset,
		unsigned int block_size, unsigned int magic_len,
		char bit)
{
	/* find first and last block in the buffer */
	unsigned long start_block = (offset>>3) / ((block_size - (CRC32_SIZE +
			magic_len)));
	unsigned long result = size;
	int res = 0;
	unsigned long cur_block = 0;
	/* size of the block in bits */
	unsigned long int_size = ((block_size - (CRC32_SIZE +
		magic_len)) << 3);
	unsigned long int_offset = offset % int_size;
	unsigned int int_block_size = 0;
	unsigned long end_block = (size - 1) / int_size;
	for (cur_block =  start_block; cur_block <= end_block;
			cur_block++) {
			int_block_size = size - cur_block * int_size;
			int_block_size = int_block_size > int_size ?
					int_size : int_block_size;

			res  = __find_next_bit_in_block(addr +
					(cur_block * block_size),
					int_block_size, int_offset,
					magic_len, bit);
			if (res < 0) {
				int_offset = 0;
				continue;
			} else {
				result = (unsigned long)res + cur_block *
						int_size;
				break;
			}
	}
	return result;
}
/**
 * @brief			Find next zero bit in the array.
 *					this function take care about
 *					all special symbols in the array
 *					like magic and crc number.
 * @param [in]	addr			Source buffer with data.
 * @param [in]	size			Size of the buffer in bits
 * @param [in]	offset			start bit position for the searching
 * @param [in]	block_size		Size of block for block device.
 * @param [in]	magic_len		Length of the magic
 * @return				Error code or bit position
 */

inline unsigned long  vdfs2_find_next_zero_bit(const void *addr,
		unsigned long size, unsigned long offset,
		unsigned int block_size, unsigned int magic_len)
{
	return __find_next_bit(addr, size, offset, block_size,
			magic_len, '0');
}
/**
 * @brief			Find next nonzero bit in the array.
 *					this function take care about all
 *					special symbols in the array like
 *					magic and crc number.
 * @param [in]	addr		Source buffer with data.
 * @param [in]	size		Size of the buffer
 * @param [in]	offset		start bit position for the searching
 * @param [in]	block_size	Size of block for block device.
 * @return					Previous state of the bit
 */
inline unsigned long vdfs2_find_next_bit(const void *addr, unsigned long size,
			unsigned long offset, unsigned int block_size,
			unsigned int magic_len)
{
	return __find_next_bit(addr, size, offset, block_size,
			magic_len, '1');
}


/**
 * @brief			Fill buffer with zero and update the
 *				buffer with magic.
 * @param [in]	buff		Buffer to update.
 * @param [in]	block_size	Block size
 * @param [in]	ino		Inode number
 */
static int vdfs2_init_bitmap_page(struct vdfs2_sb_info *sbi, ino_t ino_n,
		struct page *page)
{
	void *bitmap;
	__u64 version = ((__le64)VDFS2_RAW_EXSB(sbi)->mount_counter << 32) |
			sbi->snapshot_info->sync_count;
	if (ino_n == VDFS2_FREE_INODE_BITMAP_INO) {
		bitmap = kmap(page);
		if (!bitmap)
			return -ENOMEM;
		memset(bitmap, 0, PAGE_SIZE);
		memcpy(bitmap, INODE_BITMAP_MAGIC, INODE_BITMAP_MAGIC_LEN -
				VERSION_SIZE);
		memcpy(bitmap + INODE_BITMAP_MAGIC_LEN - VERSION_SIZE,
				&version, VERSION_SIZE);
		kunmap(page);
	} else if (ino_n == VDFS2_SMALL_AREA_BITMAP) {
		bitmap = kmap(page);
		if (!bitmap)
			return -ENOMEM;
		memset(bitmap, 0, PAGE_SIZE);
		memcpy(bitmap, SMALL_AREA_BITMAP_MAGIC,
				SMALL_AREA_BITMAP_MAGIC_LEN - VERSION_SIZE);
		memcpy(bitmap + SMALL_AREA_BITMAP_MAGIC_LEN - VERSION_SIZE,
				&version, VERSION_SIZE);
		kunmap(page);
	}

	return 0;
}
static struct address_space *vdfs2_next_mapping(struct vdfs2_sb_info *sbi,
		struct address_space *current_mapping)
{
	ino_t ino;

	ino = (current_mapping == NULL) ? 0 : current_mapping->host->i_ino;

	switch (ino) {
	case (0):
		return sbi->catalog_tree->inode->i_mapping;
	break;
	case (VDFS2_CAT_TREE_INO):
		return sbi->fsm_info->bitmap_inode->i_mapping;
	break;
	case (VDFS2_SPACE_BITMAP_INO):
		return sbi->extents_tree->inode->i_mapping;
	break;
	case (VDFS2_EXTENTS_TREE_INO):
		return sbi->free_inode_bitmap.inode->i_mapping;
	break;
	case (VDFS2_FREE_INODE_BITMAP_INO):
		return sbi->hardlink_tree->inode->i_mapping;
	break;
	case (VDFS2_HARDLINKS_TREE_INO):
		return sbi->xattr_tree->inode->i_mapping;
	case (VDFS2_XATTR_TREE_INO):
		return sbi->small_area->bitmap_inode->i_mapping;
	case (VDFS2_SMALL_AREA_BITMAP):
		return NULL;
	default:
	return NULL;
	}
}



static int get_pages_from_mapping(struct vdfs2_sb_info *sbi,
		struct address_space **current_mapping,
		struct pagevec *pvec, pgoff_t *index)
{
	int nr_pages = 0;
	ino_t ino;
	int count;
	unsigned long size;

	do {
		if (*current_mapping) {
			size = (is_tree(current_mapping[0]->host->i_ino)) ?
					PAGEVEC_SIZE - (PAGEVEC_SIZE %
					(1 << (sbi->log_blocks_in_page
					+ sbi->log_blocks_in_leb))) :
					PAGEVEC_SIZE;
			pvec->nr = find_get_pages_range_tag(*current_mapping,
					index, (pgoff_t)-1, PAGECACHE_TAG_DIRTY,
					size, pvec->pages);
			nr_pages = pagevec_count(pvec);

			ino = current_mapping[0]->host->i_ino;

			for (count = 0; count < nr_pages; count++) {
				lock_page(pvec->pages[count]);
				BUG_ON(!PageDirty(pvec->pages[count]));
				if (PageWriteback(pvec->pages[count]))
					wait_on_page_writeback(
							pvec->pages[count]);
				BUG_ON(PageWriteback(pvec->pages[count]));
				clear_page_dirty_for_io(pvec->pages[count]);
				unlock_page(pvec->pages[count]);

			}
		}

		if (!nr_pages) {
			*current_mapping = vdfs2_next_mapping(sbi,
					*current_mapping);
			*index = 0;
		}

	} while ((!nr_pages) && *current_mapping);

#ifdef CONFIG_VDFS2_DEBUG
	if (nr_pages)
		vdfs2_check_moved_iblocks(sbi, pvec->pages, nr_pages);
#endif

	return nr_pages;
}




static void meta_end_IO(struct bio *bio)
{
	struct bio_vec *bvec;
	struct bvec_iter_all iter_all;

	bio_for_each_segment_all(bvec, bio, iter_all) {
		struct page *page = bvec->bv_page;

		if (bio->bi_status) {
			SetPageError(page);
			if (page->mapping)
				set_bit(AS_EIO, &page->mapping->flags);
			VDFS2_BUG();
		}

		SetPageUptodate(page);

		if (bio_op(bio) == WRITE) {
			end_page_writeback(page);
		} else {
			BUG_ON(!PageLocked(page));
			/* if it's async read*/
			if (!bio->bi_private)
				unlock_page(page);
		}

	}

	if (bio->bi_private) {
		struct vdfs2_wait_list *wait = bio->bi_private;
		complete(&wait->wait);
	}

	bio_put(bio);
}

static struct bio *allocate_new_request(struct vdfs2_sb_info *sbi, sector_t
		start_block, struct list_head *wait_list_head, int size)
{
	struct bio *bio;
	sector_t start_sector = start_block << (sbi->block_size_shift -
			SECTOR_SIZE_SHIFT);
	struct vdfs2_wait_list *new_request;
	struct block_device *bdev = sbi->sb->s_bdev;
	int bio_size = (size > BIO_MAX_PAGES) ? BIO_MAX_PAGES : size;


	bio = vdfs2_allocate_new_bio(bdev, start_sector, bio_size);
	if (!bio) {
		bio = ERR_PTR(-ENOMEM);
		goto exit;
	}

	if (wait_list_head) {
		new_request = kzalloc(sizeof(*new_request), GFP_KERNEL);

		if (!new_request) {
			bio_put(bio);
			bio = ERR_PTR(-ENOMEM);
			goto exit;
		}

		INIT_LIST_HEAD(&new_request->list);
		init_completion(&new_request->wait);
		new_request->number = start_sector;
		list_add_tail(&new_request->list, wait_list_head);
		bio->bi_private = new_request;
	} else
		bio->bi_private = NULL;

	bio->bi_end_io = meta_end_IO;

exit:
	return bio;

}

static int vdfs2_sign_mapping_pages(struct vdfs2_sb_info *sbi,
		struct pagevec *pvec, unsigned long ino)
{
	int ret = 0;
	__u64 version = ((__le64)VDFS2_RAW_EXSB(sbi)->mount_counter << 32) |
			sbi->snapshot_info->sync_count;
	int magic_len = 0;

	if (is_tree(ino)) {
		struct vdfs2_btree *tree;
		int i;
		switch (ino) {
		case VDFS2_CAT_TREE_INO:
			tree = sbi->catalog_tree;
			break;
		case VDFS2_EXTENTS_TREE_INO:
			tree = sbi->extents_tree;
			break;
		case VDFS2_HARDLINKS_TREE_INO:
			tree = sbi->hardlink_tree;
			break;
		case VDFS2_XATTR_TREE_INO:
			tree = sbi->xattr_tree;
			break;
		default:
			return -EFAULT;
		}
		for (i = 0; i < pvec->nr; i += tree->pages_per_node) {
			if ((pvec->nr - i) < tree->pages_per_node)
				break;
			ret = vdfs2_check_and_sign_dirty_bnodes(&pvec->pages[i],
					tree, version);
		}
	} else {
		int i;
		char *magic = NULL;
		switch (ino) {
		case VDFS2_SMALL_AREA_BITMAP:
			magic = SMALL_AREA_BITMAP_MAGIC;
			magic_len = SMALL_AREA_BITMAP_MAGIC_LEN;
			break;
		case VDFS2_FREE_INODE_BITMAP_INO:
			magic = INODE_BITMAP_MAGIC;
			magic_len = INODE_BITMAP_MAGIC_LEN;
			break;
		case VDFS2_SPACE_BITMAP_INO:
			magic = FSM_BMP_MAGIC;
			magic_len = FSM_BMP_MAGIC_LEN;
			break;
		default:
			return -EFAULT;
		}
		for (i = 0; i < pvec->nr; i++) {
			ret = vdfs2_check_and_sign_pages(pvec->pages[i], magic,
				magic_len, version);
		}
	}
	return ret;
}

/**
 * @brief			Write meta data (struct page **)
 *				The function supports metadata fragmentation
 * @param [in]	sbi		The VDFS2 superblock information.
 * @param [in]	pages		Pointer to locked pages.
 * @param [in]	sector_addr	Start isector address.
 * @param [in]	page_count	Number of pages to be written.
 *				    and write snapshot head page in sync mode
 * @return			Returns 0 on success, errno on failure.
 */
static int vdfs2_meta_write(struct vdfs2_sb_info *sbi,
		unsigned int *written_pages_count)
{
	struct bio *bio = NULL;
	struct address_space *current_mapping = NULL;
	struct blk_plug plug;
	struct list_head wait_list_head;
	struct vdfs2_wait_list *new_request;
	struct list_head *pos, *q; /* temporary storage */
	pgoff_t index, page_index = 0;
	unsigned int nr_pages;
	unsigned int size;
	int ret;
	struct pagevec pvec;
	sector_t last_block = 0, block;
	unsigned int blocks_per_page = 1 << (PAGE_SHIFT -
			sbi->block_size_shift);

	INIT_LIST_HEAD(&wait_list_head);
	blk_start_plug(&plug);
	pagevec_init(&pvec);


	index = 0;
	nr_pages = get_pages_from_mapping(sbi, &current_mapping, &pvec,
			&page_index);
	if (nr_pages) {
		ret = vdfs2_sign_mapping_pages(sbi, &pvec,
				current_mapping->host->i_ino);
		if (ret) {
			blk_finish_plug(&plug);
			return ret;
		}
	}
	while (nr_pages) {
		ret = get_block_meta_wrapper(current_mapping->host,
				pvec.pages[index]->index, &block, 0);
		BUG_ON(ret);

		if (last_block + blocks_per_page != block) {
			if (bio) {
				bio->bi_opf = REQ_OP_WRITE | REQ_FUA;
				submit_bio(bio);
			}
again:
			bio = allocate_new_request(sbi, block, &wait_list_head,
					nr_pages);
			if (!bio)
				goto again;
		} else if (!bio) {
			blk_finish_plug(&plug);
			return -EINVAL;
		}

		set_page_writeback(pvec.pages[index]);
		size = bio_add_page(bio, pvec.pages[index], PAGE_SIZE, 0);
		if (size < PAGE_SIZE) {
			bio->bi_opf = REQ_OP_WRITE | REQ_FUA;
			submit_bio(bio);
			bio = NULL;
			last_block = 0;
		} else {
			nr_pages--;
			index++;
			last_block = block;
		}
#ifdef CONFIG_VDFS2_STATISTIC
	sbi->umount_written_bytes += size;
#endif
		if (!nr_pages) {
			pagevec_reinit(&pvec);
			/*pagevec_release(&pvec);*/
			nr_pages = get_pages_from_mapping(sbi, &current_mapping,
					&pvec, &page_index);
			index = 0;
			if (nr_pages) {
				ret = vdfs2_sign_mapping_pages(sbi,
						&pvec,
						current_mapping->host->i_ino);
				if (ret) {
					if (bio)
						bio_put(bio);
					blk_finish_plug(&plug);
					return ret;
				}
			}

		}
	};

	if (bio) {
		bio->bi_opf = REQ_OP_WRITE | REQ_FUA;
		submit_bio(bio);
	}

	blk_finish_plug(&plug);

	list_for_each_safe(pos, q, &wait_list_head) {
		new_request = list_entry(pos, struct vdfs2_wait_list, list);
		/* Synchronous write operation */
		wait_for_completion(&new_request->wait);
		list_del(pos);
		kfree(new_request);
	}



	return ret;
}

/**
 * @brief			Write meta data (struct page **)
 *				The function supports metadata fragmentation
 * @param [in]	sbi		The VDFS2 superblock information.
 * @param [in]	pages		Pointer to locked pages.
 * @param [in]	sector_addr	Start isector address.
 * @param [in]	page_count	Number of pages to be written.
 *				    and write snapshot head page in sync mode
 * @return			Returns 0 on success, errno on failure.
 */
static int vdfs2_meta_read(struct inode *inode, int type, struct page **pages,
		int page_count, int async)
{
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	struct bio *bio = NULL;
	struct blk_plug plug;
	struct list_head wait_list_head;
	struct vdfs2_wait_list *new_request;
	struct list_head *pos, *q; /* temporary storage */
	unsigned int size;
	int ret = 0;
	sector_t last_block = 0, block;

	unsigned int blocks_per_page = 1 << (PAGE_SHIFT -
			sbi->block_size_shift);

	int count;

	INIT_LIST_HEAD(&wait_list_head);
	blk_start_plug(&plug);

	for (count = 0; count < page_count; count++) {
		struct page *page = pages[count];

		BUG_ON(!PageLocked(page));
		if (PageUptodate(page)) {
			/* if it's async call*/
			if (async)
				unlock_page(pages[count]);
			continue;
		}

		ret = get_block_meta_wrapper(inode, page->index, &block, type);
		if (ret || (block == 0)) {
			ret = (block == 0) ? -EINVAL : ret;
			goto exit;
		}

		if (last_block + blocks_per_page != block) {
			if (bio) {
				bio->bi_opf = REQ_OP_READ;
				submit_bio(bio);
			}
again:
			if (async)
				bio = allocate_new_request(sbi, block, NULL,
						page_count - count);
			else
				bio = allocate_new_request(sbi, block,
					&wait_list_head, page_count - count);
			if (!bio)
				goto again;
		} else if (!bio) {
			blk_finish_plug(&plug);
			return -EINVAL;
		}

		size = bio_add_page(bio, page, PAGE_SIZE, 0);
		if (size < PAGE_SIZE) {
			bio->bi_opf = REQ_OP_READ;
			submit_bio(bio);
			bio = NULL;
			goto again;
		}
		last_block = block;
	};

exit:
	if (bio) {
		bio->bi_opf = REQ_OP_READ;
		submit_bio(bio);
	}

	blk_finish_plug(&plug);

	if (async)
		return ret;

	list_for_each_safe(pos, q, &wait_list_head) {
		new_request = list_entry(pos, struct vdfs2_wait_list, list);
		/* Synchronous write operation */
		wait_for_completion(&new_request->wait);
		list_del(pos);
		kfree(new_request);
	}

	return ret;
}

int vdfs2_sync_metadata(struct vdfs2_sb_info *sbi)
{
	int ret = 0;
	unsigned int pages_count = 0;

	if (sbi->snapshot_info->dirty_pages_count == 0)
		return 0;

	if (is_sbi_flag_set(sbi, EXSB_DIRTY)) {
		ret = vdfs2_sync_second_super(sbi);
		sbi->snapshot_info->use_base_table = 1;
		if (ret)
			return ret;
	}

	ret = vdfs2_meta_write(sbi, &pages_count);
	if (ret)
		return ret;

	fsm_btree_destroy(&sbi->fsm_info->tree);
	percpu_counter_set(&sbi->free_blocks_count, 0);
	sbi->next_free_blocks = 0;
	ret = fsm_get_tree(sbi->fsm_info);
	if (ret)
		return ret;

	vdfs2_update_bitmaps(sbi);
	ret = vdfs2_update_translation_tables(sbi);
	if (ret)
		return ret;

	if (is_sbi_flag_set(sbi, EXSB_DIRTY)) {
		ret = vdfs2_sync_first_super(sbi);
		if (!ret)
			clear_sbi_flag(sbi, EXSB_DIRTY);
	}

	return ret;
}


int vdfs2_read_or_create_pages(struct vdfs2_sb_info *sbi, struct inode *inode,
		struct page **pages, pgoff_t index, int page_count, int async)
{
	struct address_space *mapping = inode->i_mapping;
	int count, ret = 0, packtree_or_small_area = 0;
	struct page *page;
	char is_new = 0;
	int validate_crc = 0;

	if (inode->i_ino > VDFS2_LSFILE) {
		/* small area or pack tree */
		packtree_or_small_area = 1;
		if (inode->i_ino == VDFS2_SMALL_AREA) {
			struct buffer_head *bh_result = alloc_buffer_head(GFP_NOFS);
			struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
			__u32 inode_ttb = inode_info->fork.total_block_count;
			sector_t iblock;
			struct vdfs2_extended_super_block *exsb =
					VDFS2_RAW_EXSB(sbi);

			if (!bh_result)
				return -ENOMEM;

			iblock = ((sector_t)index) << (PAGE_SHIFT -
					sbi->block_size_shift);
			iblock += (((sector_t)1) << (PAGE_SHIFT -
					sbi->block_size_shift)) - 1;

			ret = vdfs2_get_block(inode, iblock, bh_result, 1);
			mutex_lock_nested(&VDFS2_I(inode)->truncate_mutex,
					SMALL_AREA);
			if (inode_ttb != inode_info->fork.total_block_count) {
				is_new = 1;
				vdfs2_form_fork(&exsb->small_area, inode);
				i_size_write(inode,
					inode_info->fork.total_block_count
					<< sbi->block_size_shift);
				set_sbi_flag(sbi, EXSB_DIRTY);
			}
			mutex_unlock(&VDFS2_I(inode)->truncate_mutex);
			free_buffer_head(bh_result);
		}
	} else
		ret = vdfs2_check_page_offset(sbi, inode->i_ino, index, &is_new);

	if (ret)
		return ret;

	for (count = 0; count < page_count; count++) {
again:
		page = find_get_page(mapping, index + count);
		if (!page) {
			page = page_cache_alloc(mapping);
			if (!page) {
				ret = -ENOMEM;
				goto exit_alloc_page;
			}
			ret = add_to_page_cache_lru(page, mapping,
					index + count, GFP_KERNEL);
			if (unlikely(ret)) {
				put_page(page);
				goto again;
			}
			/* this flag indicates that pages are from disk.*/
			validate_crc = 1;
			if (is_new)
				SetPageUptodate(page);
		} else {
			lock_page(page);
			if (!async)
				BUG_ON(!PageUptodate(page));
		}
		pages[count] = page;
		if ((is_new) && (!packtree_or_small_area)) {
			ret = vdfs2_init_bitmap_page(sbi, inode->i_ino, page);
			if (ret)
				goto exit_alloc_locked_page;
		}
	}

	ret = vdfs2_meta_read(inode, packtree_or_small_area, pages,
			page_count, async);
	if (ret) {
		count = 0;
		goto exit_validate_page;
	}

	/*validate pages only for bitmaps*/
	for (count = 0; count < page_count; count++) {
		if (validate_crc && !is_tree(inode->i_ino) &&
				(!packtree_or_small_area)) {
			ret = vdfs2_validate_page(pages[count]);
			if (ret) {
				VDFS2_ERR("Error in validate page");
				goto exit_validate_page;
			}
		}
		/* if it's sync call*/
		if (!async)
			unlock_page(pages[count]);
	}

	return ret;
exit_alloc_page:
	VDFS2_ERR("Error in allocate page");
	for (; count > 0; count--) {
		unlock_page(pages[count - 1]);
		put_page(pages[count - 1]);
	}
	return ret;
exit_alloc_locked_page:
	VDFS2_ERR("Error in init bitmap page");
	for (; count >= 0; count--) {
		unlock_page(pages[count]);
		put_page(pages[count]);
	}
	return ret;
exit_validate_page:
	VDFS2_ERR("Error in exit_validate_page");
	for (; count < page_count; count++) {
		SetPageError(pages[count]);
		if (!async)
			unlock_page(pages[count]);
	}

	return ret;
}

struct page *vdfs2_read_or_create_page(struct inode *inode, pgoff_t index)
{
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	struct page *pages[1];
	int err = 0;

	err = vdfs2_read_or_create_pages(sbi, inode, pages, index, 1, 0);
	if (err)
		return ERR_PTR(err);

	return pages[0];
}

struct page *vdfs2_read_or_create_small_area_page(struct inode *inode,
		pgoff_t index)
{
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	struct page *pages[4];
	int err = 0;
	int count = 1;
	int i;

	count = min_t(int, 4, VDFS2_I(inode)->fork.total_block_count -
		index - 1);

	if (count <= 0)
		count = 1;

	err = vdfs2_read_or_create_pages(sbi, inode, pages, index, count, 0);
	if (err)
		return ERR_PTR(err);

	for (i = 1; i < count; i++)
		put_page(pages[i]);

	return pages[0];
}

struct bio *vdfs2_mpage_bio_submit(int opf, struct bio *bio)
{
	bio->bi_end_io = end_io_write;
	bio->bi_opf = opf;
	submit_bio(bio);
	return NULL;
}


int vdfs2_mpage_writepage(struct page *page,
		struct writeback_control *wbc, void *data)
{
	struct vdfs2_mpage_data *mpd = data;
	struct bio *bio = mpd->bio;
	struct address_space *mapping = page->mapping;
	struct inode *inode = page->mapping->host;
	struct vdfs2_sb_info *sbi = VDFS2_SB(inode->i_sb);
	struct block_device *bdev = sbi->sb->s_bdev;
	sector_t offset_alloc_hint = 0;
	unsigned blocksize;
	sector_t block_in_file;
	struct vdfs2_extent_info extent;
	const unsigned blkbits = inode->i_blkbits;
	int err = 0;
	sector_t boundary_block = 0;
	unsigned long end_index;
	struct vdfs2_inode_info *inode_info = VDFS2_I(inode);
	struct buffer_head *bh;
	loff_t i_size = i_size_read(inode);
	memset(&extent, 0x0, sizeof(extent));
	block_in_file = (sector_t)page->index << (PAGE_SHIFT - blkbits);
	blocksize = 1 << inode->i_blkbits;
	if (page_has_buffers(page)) {
		bh = page_buffers(page);
		BUG_ON(buffer_locked(bh));
		if (buffer_mapped(bh)) {
			if (bh->b_blocknr == VDFS2_INVALID_BLOCK) {
				clear_buffer_delay(bh);
				/* get extent which contains a iblock*/
				err = vdfs2_get_iblock_extent(&inode_info->vfs_inode,
					block_in_file, &extent,
					&offset_alloc_hint);
				/* buffer was allocated during writeback
				 * operation */
				if ((extent.first_block == 0) || err) {
					if (vdfs2_get_block_da(inode,
						block_in_file, bh, 1))
						goto confused;
				} else {
					bh->b_blocknr = extent.first_block +
						(block_in_file - extent.iblock);
				}
				clean_bdev_bh_alias(bh);
			}
		} else {
			/*
			* unmapped dirty buffers are created by
			* __set_page_dirty_buffers -> mmapped data
			*/
			if (buffer_dirty(bh))
				goto confused;
		}

		if (!buffer_dirty(bh) || !buffer_uptodate(bh))
			goto confused;

	} else {
		/*
		* The page has no buffers: map it to disk
		*/
		BUG_ON(!PageUptodate(page));

		create_empty_buffers(page, blocksize, 0);
		bh = page_buffers(page);

		bh->b_state = 0;
		bh->b_size = 1 << blkbits;
		if (vdfs2_get_block(inode, block_in_file, bh, 1))
			goto confused;
		if (buffer_new(bh))
			clean_bdev_bh_alias(bh);

	}

	boundary_block = bh->b_blocknr;
	end_index = i_size >> PAGE_SHIFT;
	if (page->index >= end_index) {
		/*
		 * The page straddles i_size.  It must be zeroed out on each
		 * and every writepage invocation because it may be mmapped.
		 * "A file is mapped in multiples of the page size.  For a file
		 * that is not a multiple of the page size, the remaining memory
		 * is zeroed when mapped, and writes to that region are not
		 * written out to the file."
		 */
		unsigned offset = i_size & (PAGE_SIZE - 1);

		if (page->index > end_index || !offset)
			goto confused;
		zero_user_segment(page, offset, PAGE_SIZE);
	}

	/*
	 * If it's the end of contiguous chunk, submit the BIO.
	 */
	if (bio && mpd->last_block_in_bio != boundary_block - 1)
		bio = vdfs2_mpage_bio_submit(REQ_OP_WRITE, bio);


alloc_new:
	bdev = bh->b_bdev;
	boundary_block = bh->b_blocknr;
	if (boundary_block == 0)
		BUG();
	if (IS_ERR_OR_NULL(bio)) {
		bio = vdfs2_allocate_new_bio(bdev, boundary_block << (blkbits - 9),
				BIO_MAX_PAGES);
		if (IS_ERR_OR_NULL(bio))
			goto confused;
	}

	/*
	 * TODO: replace PAGE_SIZE with real user data size?
	 */
	if (bio_add_page(bio, page, PAGE_SIZE, 0) < PAGE_SIZE) {
		bio = vdfs2_mpage_bio_submit(REQ_OP_WRITE, bio);
		goto alloc_new;
	}

	/*
	 * OK, we have our BIO, so we can now mark the buffers clean.  Make
	 * sure to only clean buffers which we know we'll be writing.
	 */
	if (page_has_buffers(page)) {
		struct buffer_head *head = page_buffers(page);

		clear_buffer_dirty(head);
	}

	BUG_ON(PageWriteback(page));
	set_page_writeback(page);
	unlock_page(page);
	mpd->last_block_in_bio = boundary_block;
	goto out;

confused:
	if (IS_ERR_OR_NULL(bio))
		bio = NULL;
	else
		bio = vdfs2_mpage_bio_submit(REQ_OP_WRITE, bio);
	if (buffer_mapped(bh))
		if (bh->b_blocknr == 0)
			BUG();
	err = mapping->a_ops->writepage(page, wbc);
	/*
	 * The caller has a ref on the inode, so *mapping is stable
	 */
	mapping_set_error(mapping, err);
out:
	mpd->bio = bio;
	return err;
}
