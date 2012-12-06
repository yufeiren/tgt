/* NUMA-aware cache for iSCSI protocol
 * Yufei Ren (yufei.ren@stonybrook.edu)
 */

#include "cache.h"

int init_cache(struct host_cache *hc, struct cache_param *cp)
{
	int i;

	if (numa_available() != 0) {
		eprintf("Does not support NUMA API\n");
		return -1;
	}

	hc->numa_nodes = numa_num_configured_nodes(); /*numa_max_node();*/
	dprintf("numa cache: this host have %d numa nodes in total\n", \
		hc->numa_nodes);
	
	hc->nc = (struct numa_cache *) \
		malloc(hc->numa_nodes * sizeof(struct numa_cache));
	if (hc->nc == NULL) {
		eprintf("malloc failed\n");
		return -1;
	}

	for (i = 0; i < hc->numa_nodes; i ++) {
		if (alloc_nc(&(hc->nc[i]), cp, i) != 0) {
			eprintf("alloc numa cache %d failed\n", i);
			return -1;
		}
		dprintf("numa cache: alloc cache in node %d success\n", i);
	}

	return 0;
}

int alloc_nc(struct numa_cache *nc, struct cache_param *cp, int numa_index)
{
	int i;

	/* alloc cache memory trunk */
	nc->buffer_size = cp->buffer_size;
	nc->buffer = (char *) numa_alloc_onnode(nc->buffer_size, numa_index);
	if (nc->buffer == NULL) {
		eprintf("numa_alloc_onnode numa_cache buffer failed\n");
		return -1;
	}
	dprintf("numa cache: alloc %ld bytes in numa node %d\n", \
		nc->buffer_size, numa_index);

	/* alloc hash table */
	nc->cbs = cp->cbs;
	nc->nb = (int) (nc->buffer_size / nc->cbs);
	nc->ht.sz = nc->nb;
	dprintf("numa cache: hash table size is: %d\n", nc->ht.sz);
	nc->ht.tablecell = (struct cache_block *) \
		numa_alloc_onnode(nc->ht.sz * sizeof(struct cache_block), \
			numa_index);
	if (nc->ht.tablecell == NULL) {
		eprintf("numa_alloc_onnode hash table failed\n");
		return -1;
	}

	/* init hash table */
	dprintf("numa cache: init hash table lock %x\n", &(nc->ht.lock));
	cache_hash_table_lock_init(&(nc->ht));
	for (i = 0; i < nc->ht.sz; i ++) {
		INIT_LIST_HEAD(&(nc->ht.tablecell[i].list));
		cache_list_lock_init(&(nc->ht.tablecell[i]));
	}

	/* init unused list */
	INIT_LIST_HEAD(&(nc->unused_list.list));
	cache_list_lock_init(&(nc->unused_list));

	/* init hit list */
	INIT_LIST_HEAD(&(nc->hit_list.hit_list));
	cache_list_lock_init(&(nc->hit_list));

	/* alloc cache blocks info */
	nc->cb = (struct cache_block *) \
		numa_alloc_onnode(nc->nb * sizeof(struct cache_block), \
			numa_index);
	if (nc->cb == NULL) {
		eprintf("numa_alloc_onnode cache blocks failed\n");
		return -1;
	}

	/* add all cache blocks into unused list */
	for (i = 0; i < nc->nb; i ++) {
		nc->cb[i].is_valid = CACHE_INVALID;
		nc->cb[i].lba = -1;
		nc->cb[i].cbs = cp->cbs;
		nc->cb[i].hit_count = 0;
		nc->cb[i].addr = nc->buffer + i * cp->cbs;
		dprintf("numa cache: memory block addr %x\n", nc->cb[i].addr);
		INIT_LIST_HEAD(&(nc->cb[i].list));
		INIT_LIST_HEAD(&(nc->cb[i].hit_list));

		list_add_tail(&(nc->cb[i].list), &(nc->unused_list.list));
	}

	return 0;
}

void update_cache_block(struct numa_cache *nc, uint64_t lba, char *data)
{
	int key;
	int is_found;
	struct cache_block *clist;	/* hash table cell list */
	struct cache_block *cur;
	struct list_head *pos;

	key = ht_hash_key(lba, &(nc->ht));
	clist = &(nc->ht.tablecell[key]);

	/* keep each cbs only ONE copy! */

	/* search hash table first */
	cache_hash_table_lock(&(nc->ht));
	/* search */
	cur = NULL;
	is_found = 0;
	list_for_each_entry(cur, &(clist->list), list) {
		if (cur->lba == lba) {
			is_found = 1;
			break;
		}
	}

	if (is_found) { /* already had one copy */
		dprintf("numa cache: already cached, update cache\n");
		memcpy(cur->addr, data, cur->cbs);
		goto finish;
	} else {
		dprintf("numa cache: search a block and update its data\n");
		/* check ununsed list */
		dprintf("numa cache: check unused list\n");
		if (!list_empty(&(nc->unused_list.list))) {
			dprintf("numa cache: find a block in unused list\n");
			cur = list_first_entry(&(nc->unused_list.list), \
				struct cache_block, list);
			list_del(&(cur->list));

			memcpy(cur->addr, data, cur->cbs);
			cur->hit_count = 1;
			cur->lba = lba;
			cur->is_valid = CACHE_VALID;

			dprintf("numa cache: add block into ht\n");
			list_add_tail(&(cur->list), &(clist->list));

			/* insert into hit list */
			dprintf("numa cache: add block into hit list\n");
			list_add_tail(&(cur->hit_list), &(nc->hit_list.hit_list));
			goto finish;
		}
		
		/* check hit count list */
		dprintf("numa cache: check hit count list\n");
		if (!list_empty(&(nc->hit_list.hit_list))) {
			/* get the last item */
			dprintf("numa cache: LRU replacement: get the last block in hit list\n");
			pos = nc->hit_list.hit_list.prev;
			cur = list_entry(pos, struct cache_block, hit_list);

			dprintf("numa cache: delete from hash table\n");
			/* delete from hash table */
			list_del(&(cur->list));

			dprintf("numa cache: copy data into memory %x\n", cur->addr);
			memcpy(cur->addr, data, cur->cbs);
			cur->hit_count = 1;
			cur->lba = lba;
			cur->is_valid = CACHE_VALID;

			/* move the last one to the head of hit time is 1 */
			sort_hit_list(cur, &(nc->hit_list));

			/* add into hash table */
			list_add_tail(&(cur->list), &(clist->list));

			list_for_each_entry(cur, &(nc->hit_list.hit_list), hit_list) {
				dprintf("numa cache: lba %ld, hit: %d, addr: %x\n", cur->lba, cur->hit_count, cur->addr);
			}

			goto finish;
		}
	}

finish:
	cache_hash_table_unlock(&(nc->ht));
	return;
}


int cache_list_lock_init(struct cache_block *cb)
{
	pthread_mutex_init(&(cb->lock), NULL);
	return 0;
}

int cache_list_lock(struct cache_block *cb)
{
	if (pthread_mutex_lock(&(cb->lock)) != 0) {
		eprintf("lock fail\n");
		return -1;
	}
	return 0;
}

int cache_list_unlock(struct cache_block *cb)
{
	if (pthread_mutex_unlock(&(cb->lock)) != 0) {
		eprintf("unlock fail\n");
		return -1;
	}
	return 0;
}

int cache_hash_table_lock_init(struct cache_hash_table *ht)
{
	if (pthread_mutex_init(&(ht->lock), NULL) != 0) {
		eprintf("init lock fail\n");
		return -1;
	}
	return 0;
}

int cache_hash_table_lock(struct cache_hash_table *ht)
{
	if (pthread_mutex_lock(&(ht->lock)) != 0) {
		eprintf("lock fail\n");
		return -1;
	}
	return 0;
}

int cache_hash_table_unlock(struct cache_hash_table *ht)
{
	if (pthread_mutex_unlock(&(ht->lock)) != 0) {
		eprintf("unlock fail\n");
		return -1;
	}
	return 0;
}

