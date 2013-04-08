/*
 * Synchronous I/O file backing store routine
 *
 * Copyright (C) 2006-2007 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2006-2007 Mike Christie <michaelc@cs.wisc.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */
#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/fs.h>
#include <sys/epoll.h>

#include "list.h"
#include "util.h"
#include "tgtd.h"
#include "scsi.h"
#include "spc.h"
#include "bs_thread.h"
#ifdef NUMA_CACHE
#include "cache.h"
#endif

#ifdef NUMA_CACHE
extern struct host_cache hc;
#endif

static void set_medium_error(int *result, uint8_t *key, uint16_t *asc)
{
	*result = SAM_STAT_CHECK_CONDITION;
	*key = MEDIUM_ERROR;
	*asc = ASC_READ_ERROR;
}

static void bs_sync_sync_range(struct scsi_cmd *cmd, uint32_t length,
			       int *result, uint8_t *key, uint16_t *asc)
{
	int ret;

	ret = fdatasync(cmd->dev->fd);
	if (ret)
		set_medium_error(result, key, asc);
}

static void bs_rdwr_request(struct scsi_cmd *cmd)
{
	int ret, fd = cmd->dev->fd;
	int fd_od = cmd->dev->fd_od;
	uint32_t length;
	int result = SAM_STAT_GOOD;
	uint8_t key;
	uint16_t asc;
	uint32_t info = 0;
	char *tmpbuf;
	size_t blocksize;
	uint64_t offset = cmd->offset;
	uint32_t tl     = cmd->tl;
	int do_verify = 0;
	int i;
	char *ptr;
	const char *write_buf = NULL;

#ifdef NUMA_CACHE
	struct sub_io_request *ior;
	struct cache_block *cb;
	struct numa_cache *nc;
	int sio_size;
#endif

	ret = length = 0;
	key = asc = 0;

#ifdef NUMA_CACHE
	dprintf("numa cache: cmd is %x\n", cmd->scb[0]);
#endif
	switch (cmd->scb[0])
	{
	case ORWRITE_16:
		length = scsi_get_out_length(cmd);

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = SAM_STAT_CHECK_CONDITION;
			key = HARDWARE_ERROR;
			asc = ASC_INTERNAL_TGT_FAILURE;
			break;
		}

		ret = pread64(fd, tmpbuf, length, offset);

		if (ret != length) {
			set_medium_error(&result, &key, &asc);
			free(tmpbuf);
			break;
		}

		ptr = scsi_get_out_buffer(cmd);
		for (i = 0; i < length; i++)
			ptr[i] |= tmpbuf[i];

		free(tmpbuf);

		write_buf = scsi_get_out_buffer(cmd);
		goto write;
	case COMPARE_AND_WRITE:
		/* Blocks are transferred twice, first the set that
		 * we compare to the existing data, and second the set
		 * to write if the compare was successful.
		 */
		length = scsi_get_out_length(cmd) / 2;
		if (length != cmd->tl) {
			result = SAM_STAT_CHECK_CONDITION;
			key = ILLEGAL_REQUEST;
			asc = ASC_INVALID_FIELD_IN_CDB;
			break;
		}

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = SAM_STAT_CHECK_CONDITION;
			key = HARDWARE_ERROR;
			asc = ASC_INTERNAL_TGT_FAILURE;
			break;
		}

		ret = pread64(fd, tmpbuf, length, offset);

		if (ret != length) {
			set_medium_error(&result, &key, &asc);
			free(tmpbuf);
			break;
		}

		if (memcmp(scsi_get_out_buffer(cmd), tmpbuf, length)) {
			uint32_t pos = 0;
			char *spos = scsi_get_out_buffer(cmd);
			char *dpos = tmpbuf;

			/*
			 * Data differed, this is assumed to be 'rare'
			 * so use a much more expensive byte-by-byte
			 * comparasion to find out at which offset the
			 * data differs.
			 */
			for (pos = 0; pos < length && *spos++ == *dpos++;
			     pos++)
				;
			info = pos;
			result = SAM_STAT_CHECK_CONDITION;
			key = MISCOMPARE;
			asc = ASC_MISCOMPARE_DURING_VERIFY_OPERATION;
			free(tmpbuf);
			break;
		}

		if (cmd->scb[1] & 0x10)
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);

		free(tmpbuf);

		write_buf = scsi_get_out_buffer(cmd) + length;
		goto write;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		/* TODO */
		length = (cmd->scb[0] == SYNCHRONIZE_CACHE) ? 0 : 0;

		if (cmd->scb[1] & 0x2) {
			result = SAM_STAT_CHECK_CONDITION;
			key = ILLEGAL_REQUEST;
			asc = ASC_INVALID_FIELD_IN_CDB;
		} else
			bs_sync_sync_range(cmd, length, &result, &key, &asc);
		break;
	case WRITE_VERIFY:
	case WRITE_VERIFY_12:
	case WRITE_VERIFY_16:
		do_verify = 1;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
#ifndef NUMA_CACHE
		length = scsi_get_out_length(cmd);
		write_buf = scsi_get_out_buffer(cmd);
write:
		ret = pwrite64(fd, write_buf, length,
			       offset);
		if (ret == length) {
			struct mode_pg *pg;
			/*
			 * it would be better not to access to pg
			 * directy.
			 */

			pg = find_mode_page(cmd->dev, 0x08, 0);
			if (pg == NULL) {
				result = SAM_STAT_CHECK_CONDITION;
				key = ILLEGAL_REQUEST;
				asc = ASC_INVALID_FIELD_IN_CDB;
				break;
			}
			if (((cmd->scb[0] != WRITE_6) && (cmd->scb[1] & 0x8)) ||
			    !(pg->mode_data[0] & 0x04))
				bs_sync_sync_range(cmd, length, &result, &key,
						   &asc);
		} else
			set_medium_error(&result, &key, &asc);

		if ((cmd->scb[0] != WRITE_6) && (cmd->scb[1] & 0x10))
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);

		if (do_verify)
			goto verify;
#else
write:
		dprintf("numa cache: =================================\n");
		dprintf("numa cache: start serving a WRITE io request\n");

		for (i = 0; i < cmd->nr_sior; i ++) {
			dprintf("numa cache: sub request %d\n", i);
			ior = &(cmd->sior[i]);
			nc = &(hc.nc[ior->nc_id]);

			nc_mutex_lock(&(nc->mutex));

			/* if block is in cache, invalidate it */
			cb = get_cache_block(ior->tid, ior->lun, \
					     ior->cb_id, nc);
			if (cb->is_valid == CACHE_VALID) {	/* hit */
				dprintf("numa cache: cache hit - update it adn write back\n");
				memcpy(cb->addr + ior->in_offset, \
				       scsi_get_out_buffer(cmd) + ior->m_offset,\
				       ior->length);
				/* write back whole cache block */
				/* optimize this ? */
				ret = pwrite64(fd, cb->addr, cb->cbs, \
					       ior->offset);
				nc_mutex_unlock(&(nc->mutex));
				continue;
			}

			dprintf("numa cache: cache not hit - read it, and update it, and write back \n");
			pread64(fd, cb->addr, cb->cbs, ior->offset);
			memcpy(cb->addr + ior->in_offset, \
			       scsi_get_out_buffer(cmd) + ior->m_offset,\
			       ior->length);

			/* write back whole cache */
			ret = pwrite64(fd, cb->addr, cb->cbs, ior->offset);

			/* update cb into cache */
			cb->is_valid = CACHE_VALID;
			cb->cb_id = ior->cb_id;
			cb->dev_id = ior->dev_id;
			cb->tid = ior->tid;
			cb->lun = ior->lun;

			dprintf("numa cache: insert cache block\n");
			insert_cache_block(cb, nc);

			nc_mutex_unlock(&(nc->mutex));
		}

		dprintf("numa cache: finish serve an io request\n");
		dprintf("numa cache: --------------------------------\n");
#endif

		break;
	case WRITE_SAME:
	case WRITE_SAME_16:
		/* WRITE_SAME used to punch hole in file */
		if (cmd->scb[1] & 0x08) {
			ret = unmap_file_region(fd, offset, tl);
			if (ret != 0) {
				eprintf("Failed to punch hole for WRITE_SAME"
					" command\n");
				result = SAM_STAT_CHECK_CONDITION;
				key = HARDWARE_ERROR;
				asc = ASC_INTERNAL_TGT_FAILURE;
				break;
			}
			break;
		}
		while (tl > 0) {
			blocksize = 1 << cmd->dev->blk_shift;
			tmpbuf = scsi_get_out_buffer(cmd);

			switch(cmd->scb[1] & 0x06) {
			case 0x02: /* PBDATA==0 LBDATA==1 */
				put_unaligned_be32(offset, tmpbuf);
				break;
			case 0x04: /* PBDATA==1 LBDATA==0 */
				/* physical sector format */
				put_unaligned_be64(offset, tmpbuf);
				break;
			}

			ret = pwrite64(fd, tmpbuf, blocksize, offset);
			if (ret != blocksize)
				set_medium_error(&result, &key, &asc);

			offset += blocksize;
			tl     -= blocksize;
		}
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
#ifndef NUMA_CACHE
		length = scsi_get_in_length(cmd);
		ret = pread64(fd, scsi_get_in_buffer(cmd), length,
			      offset);

		if (ret != length)
			set_medium_error(&result, &key, &asc);

		if ((cmd->scb[0] != READ_6) && (cmd->scb[1] & 0x10))
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);
#else
		/* length = scsi_get_in_length(cmd); */
		dprintf("numa cache: =================================\n");
		dprintf("numa cache: start serving a READ io request\n");

		for (i = 0; i < cmd->nr_sior; i ++) {
			dprintf("numa cache: sub request %d\n", i);
			ior = &(cmd->sior[i]);
			nc = &(hc.nc[ior->nc_id]);

			nc_mutex_lock(&(nc->mutex));

			/* chech if block is in cache */
			cb = get_cache_block(ior->tid, ior->lun, \
					     ior->cb_id, nc);
			if (cb->is_valid == CACHE_VALID) {	/* hit */
				dprintf("numa cache: cache hit\n");
				memcpy(scsi_get_in_buffer(cmd) + ior->m_offset, cb->addr + ior->in_offset, ior->length);
				nc_mutex_unlock(&(nc->mutex));
				continue;
			}

			dprintf("numa cache: cache not hit\n");
			/* not hit */
			/* load data (cache block) into cache memory */
			if ((ior->offset + cb->cbs) < cmd->dev->size)
				sio_size = cb->cbs;
			else
				sio_size = cmd->dev->size - ior->offset - (uint64_t) ior->in_offset;
			dprintf("numa cache: pread data %d bytes offset %ld\n", sio_size, ior->offset);
			ret = pread64(fd, cb->addr, sio_size, ior->offset);
			if (ret != cb->cbs)
				set_medium_error(&result, &key, &asc);

			/*			if ((cmd->scb[0] != READ_6) && (cmd->scb[1] & 0x10))
				posix_fadvise(fd, ior->offset, ior->length,
				POSIX_FADV_NOREUSE); */

			/* copy data into memory */
			dprintf("numa cache: copy data into memory\n");
			dprintf("numa cache: memcpy %" PRId64 " %" PRId64 " %d\n", scsi_get_in_buffer(cmd) + (uint64_t) ior->m_offset, cb->addr + ((uint64_t) ior->in_offset), ior->length);
			memcpy(scsi_get_in_buffer(cmd) + (uint64_t) ior->m_offset, cb->addr + ((uint64_t) ior->in_offset), ior->length);

			/* update cb into cache */
			cb->is_valid = CACHE_VALID;
			cb->cb_id = ior->cb_id;
			cb->dev_id = ior->dev_id;
			cb->tid = ior->tid;
			cb->lun = ior->lun;

			dprintf("numa cache: insert cache block\n");
			insert_cache_block(cb, nc);
			nc_mutex_unlock(&(nc->mutex));
		}

		dprintf("numa cache: finish serve an io request\n");
		dprintf("numa cache: --------------------------------\n");
#endif
		break;
	case PRE_FETCH_10:
	case PRE_FETCH_16:
		ret = posix_fadvise(fd, offset, cmd->tl,
				POSIX_FADV_WILLNEED);

		if (ret != 0)
			set_medium_error(&result, &key, &asc);
		break;
	case VERIFY_10:
	case VERIFY_12:
	case VERIFY_16:
verify:
		length = scsi_get_out_length(cmd);

		tmpbuf = malloc(length);
		if (!tmpbuf) {
			result = SAM_STAT_CHECK_CONDITION;
			key = HARDWARE_ERROR;
			asc = ASC_INTERNAL_TGT_FAILURE;
			break;
		}

		ret = pread64(fd, tmpbuf, length, offset);

		if (ret != length)
			set_medium_error(&result, &key, &asc);
		else if (memcmp(scsi_get_out_buffer(cmd), tmpbuf, length)) {
			result = SAM_STAT_CHECK_CONDITION;
			key = MISCOMPARE;
			asc = ASC_MISCOMPARE_DURING_VERIFY_OPERATION;
		}

		if (cmd->scb[1] & 0x10)
			posix_fadvise(fd, offset, length,
				      POSIX_FADV_NOREUSE);

		free(tmpbuf);
		break;
	case UNMAP:
		if (!cmd->dev->attrs.thinprovisioning) {
			result = SAM_STAT_CHECK_CONDITION;
			key = ILLEGAL_REQUEST;
			asc = ASC_INVALID_FIELD_IN_CDB;
			break;
		}

		length = scsi_get_out_length(cmd);
		tmpbuf = scsi_get_out_buffer(cmd);

		if (length < 8)
			break;

		length -= 8;
		tmpbuf += 8;

		while (length >= 16) {
			offset = get_unaligned_be64(&tmpbuf[0]);
			offset = offset << cmd->dev->blk_shift;

			tl = get_unaligned_be32(&tmpbuf[8]);
			tl = tl << cmd->dev->blk_shift;

			if (offset + tl > cmd->dev->size) {
				eprintf("UNMAP beyond EOF\n");
				result = SAM_STAT_CHECK_CONDITION;
				key = ILLEGAL_REQUEST;
				asc = ASC_LBA_OUT_OF_RANGE;
				break;
			}

			if (tl > 0) {
				if (unmap_file_region(fd, offset, tl) != 0) {
					eprintf("Failed to punch hole for"
						" UNMAP at offset:%" PRIu64
						" length:%d\n",
						offset, tl);
					result = SAM_STAT_CHECK_CONDITION;
					key = HARDWARE_ERROR;
					asc = ASC_INTERNAL_TGT_FAILURE;
					break;
				}
			}

			length -= 16;
			tmpbuf += 16;
		}
		break;
	default:
		break;
	}

	dprintf("io done %p %x %d %u\n", cmd, cmd->scb[0], ret, length);

	scsi_set_result(cmd, result);

	if (result != SAM_STAT_GOOD) {
		eprintf("io error %p %x %d %d %" PRIu64 ", %m\n",
			cmd, cmd->scb[0], ret, length, offset);
		sense_data_build(cmd, key, asc);
	}
}

static int bs_rdwr_open(struct scsi_lu *lu, char *path, int *fd, uint64_t *size)
{
	uint32_t blksize = 0;


#ifndef NUMA_CACHE
	*fd = backed_file_open(path, O_RDWR|O_LARGEFILE|lu->bsoflags, size,
				&blksize);
#else
	/* for direct io */
	*fd = backed_file_open(path, O_RDWR|O_LARGEFILE|O_DIRECT|lu->bsoflags, size,
				&blksize);
	if (*fd < 0)
		eprintf("open file with O_DIRECT fail - %d(%s)\n", \
			errno, strerror(errno));
#endif
	/* If we get access denied, try opening the file in readonly mode */
	if (*fd == -1 && (errno == EACCES || errno == EROFS)) {
#ifndef NUMA_CACHE
		*fd = backed_file_open(path, O_RDONLY|O_LARGEFILE|lu->bsoflags,
				       size, &blksize);
#else
		*fd = backed_file_open(path, O_RDONLY|O_LARGEFILE|O_DIRECT|lu->bsoflags,
				       size, &blksize);
		if (*fd < 0)
			eprintf("open file with O_DIRECT fail - %d(%s)\n", \
				errno, strerror(errno));
#endif
		lu->attrs.readonly = 1;
	}
	if (*fd < 0)
		return *fd;

	if (!lu->attrs.no_auto_lbppbe)
		update_lbppbe(lu, blksize);

	return 0;
}

static void bs_rdwr_close(struct scsi_lu *lu)
{
	close(lu->fd);
}

static tgtadm_err bs_rdwr_init(struct scsi_lu *lu)
{
	struct bs_thread_info *info = BS_THREAD_I(lu);

	return bs_thread_open(info, bs_rdwr_request, nr_iothreads);
}

static void bs_rdwr_exit(struct scsi_lu *lu)
{
	struct bs_thread_info *info = BS_THREAD_I(lu);

	bs_thread_close(info);
}

static struct backingstore_template rdwr_bst = {
	.bs_name		= "rdwr",
	.bs_datasize		= sizeof(struct bs_thread_info),
	.bs_open		= bs_rdwr_open,
	.bs_close		= bs_rdwr_close,
	.bs_init		= bs_rdwr_init,
	.bs_exit		= bs_rdwr_exit,
	.bs_cmd_submit		= bs_thread_cmd_submit,
	.bs_oflags_supported    = O_SYNC | O_DIRECT,
};

__attribute__((constructor)) static void bs_rdwr_constructor(void)
{
	register_backingstore_template(&rdwr_bst);
}
