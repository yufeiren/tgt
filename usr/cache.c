/* NUMA-aware cache for iSCSI protocol
 * Yufei Ren (yufei.ren@stonybrook.edu)
 */

#include "cache.h"

int offset2ncid(uint64_t offset, struct host_cache *hc)
{
	return (int) (offset / (uint64_t) hc->cbs) % (hc->nr_numa_nodes * hc->nr_cache_area);
}

int offset2nodeid(uint64_t offset, struct host_cache *hc)
{
	return (int) (offset / (uint64_t) hc->cbs) % hc->nr_numa_nodes;
}

int ncid2nodeid(int nc_id, struct host_cache *hc)
{
	return nc_id / hc->nr_cache_area;
}

int init_cache(struct host_cache *hc, struct cache_param *cp)
{
	int i;
	int total_nc;

	if (numa_available() != 0) {
		eprintf("Does not support NUMA API\n");
		return -1;
	}

	hc->nr_numa_nodes = numa_num_configured_nodes();
	eprintf("numa cache: this host have %d numa nodes in total\n", \
		hc->nr_numa_nodes);

	hc->buffer_size = cp->buffer_size;
	hc->cbs = cp->cbs;
	if (cp->cache_way == 0)
		hc->nr_cache_area = 1;
	else
		hc->nr_cache_area = cp->cache_way;

	hc->seed = getpid();
	srand(hc->seed);

	total_nc = hc->nr_numa_nodes * hc->nr_cache_area;
	hc->nc = (struct numa_cache *) \
		malloc(total_nc * sizeof(struct numa_cache));
	if (hc->nc == NULL) {
		eprintf("malloc failed\n");
		return -1;
	}

	struct bitmask *nodemask;
	nodemask = numa_get_run_node_mask();

	for (i = 0; i < total_nc; i ++) {
		hc->nc[i].id = i;
		hc->nc[i].on_numa_node = ncid2nodeid(i, hc);
		if (alloc_nc(&(hc->nc[i]), hc) != 0) {
			eprintf("alloc numa cache %d on node %d failed\n", \
				i, hc->nc[i].on_numa_node);
			return -1;
		}
		eprintf("numa cache: alloc cache[%d] in node %d success\n", \
			i, hc->nc[i].on_numa_node);
	}

	numa_run_on_node_mask(nodemask);

	return 0;
}

int alloc_nc(struct numa_cache *nc, struct host_cache *hc)
{
	int i;
	int ret;

	nc_mutex_init(&(nc->mutex));

	/* alloc cache memory trunk */
	/* even if we alloc memory by numa_alloc_onnode(), page cache
	 * is not at this time. So, we call numa_run_onnode() for thread
	 * and set memory content which will trigger memory allocation.
	 */
	ret = numa_run_on_node(nc->on_numa_node);
	if (ret == -1) {
		eprintf("numa cache: numa_run_on_node(%d) failed.\n", \
			nc->on_numa_node);
		return -1;
	}

	nc->buffer_size = hc->buffer_size / (hc->nr_numa_nodes * hc->nr_cache_area);
	nc->buffer = (char *) numa_alloc_onnode(nc->buffer_size, nc->on_numa_node);
	if (nc->buffer == NULL) {
		eprintf("numa_alloc_onnode numa_cache buffer failed\n");
		return -1;
	}
	dprintf("numa cache[%d]: alloc %ld bytes in numa node %d\n", \
		nc->id, nc->buffer_size, nc->on_numa_node);

	memset(nc->buffer, '\0', nc->buffer_size);

	/* alloc hash table */
	nc->cbs = hc->cbs;
	nc->nb = (int) (nc->buffer_size / nc->cbs);
	nc->ht.sz = nc->nb;
	dprintf("numa cache: hash table size is: %d\n", nc->ht.sz);
	nc->ht.tablecell = (struct cache_block *) \
		numa_alloc_onnode(nc->ht.sz * sizeof(struct cache_block), \
				  nc->on_numa_node);
	if (nc->ht.tablecell == NULL) {
		eprintf("numa_alloc_onnode hash table failed\n");
		return -1;
	}

	/* init hash table */
	dprintf("numa cache: init hash table lock %x\n", &(nc->ht.lock));
	for (i = 0; i < nc->ht.sz; i ++) {
		INIT_LIST_HEAD(&(nc->ht.tablecell[i].list));
	}

	/* init unused list */
	INIT_LIST_HEAD(&(nc->unused_list.list));

	/* init hit list */
	INIT_LIST_HEAD(&(nc->hit_list.hit_list));

	/* alloc cache blocks info */
	nc->cb = (struct cache_block *) \
		numa_alloc_onnode(nc->nb * sizeof(struct cache_block), \
				  nc->on_numa_node);
	if (nc->cb == NULL) {
		eprintf("numa_alloc_onnode cache blocks failed\n");
		return -1;
	}

	/* add all cache blocks into unused list */
	for (i = 0; i < nc->nb; i ++) {
		nc->cb[i].is_valid = CACHE_INVALID;
		nc->cb[i].cb_id = -1;
		nc->cb[i].cbs = hc->cbs;
		nc->cb[i].hit_count = 0;
		nc->cb[i].addr = nc->buffer + (uint64_t) i * hc->cbs;
		/*		dprintf("numa cache: cb addr: %" PRId64 "\n", nc->cb[i].addr); */
		INIT_LIST_HEAD(&(nc->cb[i].list));
		INIT_LIST_HEAD(&(nc->cb[i].hit_list));

		list_add_tail(&(nc->cb[i].list), &(nc->unused_list.list));
	}

	return 0;
}

void insert_cache_block(struct cache_block *cb, struct numa_cache *nc)
{
	int key;
	struct cache_block *clist;	/* hash table cell list */

	key = ht_hash_key(cb->cb_id, &(nc->ht));
	clist = &(nc->ht.tablecell[key]);

	/* insert into hash table */
	list_add(&(cb->list), &(clist->list));
	/* insert into hit list */
	list_add(&(cb->hit_list), &(nc->hit_list.hit_list));

	return;
}

int nc_mutex_init(pthread_mutex_t *mutex)
{
	pthread_mutex_init(mutex, NULL);
	return 0;
}

int nc_mutex_lock(pthread_mutex_t *mutex)
{
	if (pthread_mutex_lock(mutex) != 0) {
		eprintf("lock fail\n");
		return -1;
	}
	return 0;
}

int nc_mutex_unlock(pthread_mutex_t *mutex)
{
	if (pthread_mutex_unlock(mutex) != 0) {
		eprintf("lock fail\n");
		return -1;
	}
	return 0;
}

/* return most affinitied node */
int split_io(struct scsi_cmd *cmd, struct host_cache *hc)
{
	/* V is in request data (each V is a logic block)
	 *
	 * |uuuVVVVV|VVVVVVVV|VVVuuuuu|u...
	 *  ^  ^                ^      ^
         *  a_shadow
	 *     cmd->offset
	 *                      cmd->offset + length
	 *                             b_shadow
	 */

	int i;
	int nodeid;
	int aff[32];
	int aff_max;
	uint32_t length;
	uint64_t a_shadow;
	uint64_t b_shadow;
	struct sub_io_request *ior;
	uint64_t lba;

	struct iser_membuf *data_buf;
	struct tcp_data_buf *tcp_buf;

	dprintf("numa cache: start split io\n");
	lba = scsi_rw_offset(cmd->scb);

	cmd->offset = lba << cmd->dev->blk_shift;

	switch (cmd->scb[0])
	{
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		length = scsi_get_out_length(cmd);
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		length = scsi_get_in_length(cmd);
		break;
	default:
		dprintf("numa cache: command not support 0x%x\n", \
			cmd->scb[0]);
		return 0;
		break;
	}

	cmd->nr_sior = 0;

	/* take care of x.999999999  = 1 */
	a_shadow = (uint64_t) cmd->offset - (cmd->offset % (uint64_t) hc->cbs);
	b_shadow = (uint64_t) (cmd->offset + (uint64_t) length - 1) - ((cmd->offset + (uint64_t) length - 1) % (uint64_t) hc->cbs) + hc->cbs;
	cmd->nr_sior = (b_shadow - a_shadow) / (uint64_t) hc->cbs;

	for (i = 0; i < hc->nr_numa_nodes; i ++)
		aff[i] = 0;

	for (i = 0; i < cmd->nr_sior; i ++) {
		ior = &(cmd->sior[i]);
		if (cmd->nr_sior == 1) {		/* first and last */
			ior->offset = a_shadow;
			ior->in_offset = (uint32_t) cmd->offset - a_shadow;
			ior->m_offset = 0;
			ior->length = length;
			ior->cb_id = a_shadow / hc->cbs;
		} else if (i == 0) {		/* first but not last */
			ior->offset = a_shadow;
			ior->in_offset = (uint32_t) cmd->offset - a_shadow;
			ior->m_offset = 0;
			ior->length = hc->cbs - ior->in_offset;
			ior->cb_id = a_shadow / hc->cbs;
		} else if (i == cmd->nr_sior - 1) {	/* last sub io */
			ior->offset = b_shadow - hc->cbs;
			ior->in_offset = 0;
			ior->m_offset = i * hc->cbs  - (cmd->offset - a_shadow);
			ior->length = hc->cbs - (b_shadow - (cmd->offset + length));
			ior->cb_id = b_shadow / hc->cbs;
		} else {			/* middle sub io */
			ior->offset = a_shadow + (uint64_t) i * hc->cbs;
			ior->in_offset = 0;
			ior->m_offset = i * hc->cbs  - (cmd->offset - a_shadow);
			ior->length = hc->cbs;
			ior->cb_id = ior->offset / hc->cbs;
		}
		ior->dev_id = cmd->dev_id;
		ior->tid = cmd->tid;
		ior->lun = cmd->dev->lun;

		ior->nc_id = offset2ncid(ior->offset, hc);
		dprintf("numa cache: ior[%d] tid %d lun %ld off %ld, in_off %d, len %d, cb_id %ld, nc_id %d\n", \
			i, ior->tid, ior->lun, ior->offset, \
			ior->in_offset, ior->length, ior->cb_id, ior->nc_id);

		aff[ncid2nodeid(ior->nc_id, hc)] ++;
	}

	nodeid = 0;
	aff_max = aff[0];
	for (i = 1; i < hc->nr_numa_nodes; i ++) {
		if (aff[i] > aff_max) {
			nodeid = i;
			aff_max = aff[i];
		}
	}

	dprintf("numa cache: start parse numa node split io\n");

	/* reset network buffer location */
	if (cmd->rdma == 1) {
		data_buf = (struct iser_membuf *) cmd->netbuf;
		/* ONLY read operation need reset nodeid */
	switch (cmd->scb[0])
	{
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		data_buf->cur_node = nodeid;
		break;
	default:
		break;
	}
		cmd->nodeid = nodeid;
		data_buf->addr = data_buf->numa_addr[data_buf->cur_node];
	} else {
		tcp_buf = (struct tcp_data_buf *) cmd->netbuf;
		tcp_buf->cur_node = nodeid;
		cmd->nodeid = nodeid;
		tcp_buf->cur_addr = tcp_buf->addr[tcp_buf->cur_node];
	}
	dprintf("numa cache: parse this task to node %d\n", nodeid);
	dprintf("numa cache: update network buf address\n");

	switch (cmd->scb[0])
	{
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		if (cmd->rdma == 1) {
			scsi_set_out_buffer(cmd, data_buf->addr);
		} else {
			scsi_set_out_buffer(cmd, tcp_buf->cur_addr);
		}
		break;
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		if (cmd->rdma == 1) {
			scsi_set_in_buffer(cmd, data_buf->addr);
		} else {
			scsi_set_in_buffer(cmd, tcp_buf->cur_addr);
		}
		break;
	default:
		dprintf("numa cache: command not support 0x%x\n", \
			cmd->scb[0]);
		return 0;
		break;
	}

	return nodeid;
}

