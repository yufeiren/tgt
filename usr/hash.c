#include "cache.h"

int ht_hash_key(uint64_t lba, struct cache_hash_table *ht)
{
	return (int) (lba % ht->sz);
}

struct cache_block *get_cache_block(uint64_t cb_id, \
				    struct numa_cache *nc)
{
	int key;
	struct list_head *pos;
	struct cache_block *clist;	/* hash table cell list */
	struct cache_block *cur;
	struct cache_hash_table *ht = &(nc->ht);
	struct cache_block *hit_head = &(nc->hit_list);
	int is_found;

	key = ht_hash_key(cb_id, ht);
	dprintf("numa cache: this io key is %d\n", key);

	clist = &(ht->tablecell[key]);

	/* search */
	cur = NULL;
	is_found = 0;
	list_for_each_entry(cur, &(clist->list), list) {
		if (cur->cb_id == cb_id) {
			cur->hit_count;
			dprintf("numa cache: hit cache %d times\n", \
				cur->hit_count);
			is_found = 1;
			break;
		}
	}

	if (is_found) {
		dprintf("numa cache: hit cache in hash table\n");
		/* cur->hit_count ++; */
		/* lrure-sort hash tablecell list */
		dprintf("numa cache: re-sort hash tablecell\n");
		/* sort_tablecell_list(cur, clist); */
		lru_tablecell_list(cur, clist);

		/* re-sort hit list */
		dprintf("numa cache: re-sort hit list\n");
		/* sort_hit_list(cur, hit_head); */
		lru_hit_list(cur, hit_head);

		return cur;
	} else {
		/* admit a cache block */
		/* check ununsed list */
		dprintf("numa cache: check unused list\n");
		if (!list_empty(&(nc->unused_list.list))) {
			dprintf("numa cache: find a block in unused list\n");
			cur = list_first_entry(&(nc->unused_list.list), \
				struct cache_block, list);
			list_del(&(cur->list));

			cur->hit_count = 0;
			cur->is_valid = CACHE_INVALID;

			return cur;
		}
		
		/* check hit list */
		dprintf("numa cache: check hit count list\n");
		if (!list_empty(&(nc->hit_list.hit_list))) {
			/* get the last item */
			dprintf("numa cache: LRU replacement: get the last block in hit list\n");
			pos = nc->hit_list.hit_list.prev;
			cur = list_entry(pos, struct cache_block, hit_list);

			dprintf("numa cache: delete from hash table\n");
			/* delete from hash table */
			list_del(&(cur->list));

			cur->hit_count = 0;
			cur->is_valid = CACHE_INVALID;

			return cur;
		}
	}
}

void sort_tablecell_list(struct cache_block *cb, struct cache_block *head)
{
	struct list_head *pos;
	struct cache_block *pre;

	dprintf("numa cache: sort cache block: start for\n");
	for (pos = &(cb->list); pos != &(head->list); pos = pos->prev) {
		pre = list_entry(pos, struct cache_block, list);
		if (pre->hit_count > cb->hit_count)
			break;
	}
	dprintf("numa cache: sort cache block: end for\n");

	list_del(&(cb->list));
	/* insert cb into pos's next */
	cb->list.next = pos->next;
	cb->list.prev = pos;
	pos->next = &(cb->list);
	cb->list.next->prev = &(cb->list);

	return;
}

void sort_hit_list(struct cache_block *cb, struct cache_block *head)
{
	struct list_head *pos;
	struct cache_block *pre;

	dprintf("numa cache: sort hit list: start for\n");
	for (pos = &(cb->hit_list); pos != &(head->hit_list); pos = pos->prev) {
		pre = list_entry(pos, struct cache_block, hit_list);
		if (pre->hit_count > cb->hit_count)
			break;
	}
	dprintf("numa cache: sort hit list: end for\n");

	list_del(&(cb->hit_list));
	/* insert cb into pos's next */
	cb->hit_list.next = pos->next;
	cb->hit_list.prev = pos;
	pos->next = &(cb->hit_list);
	cb->hit_list.next->prev = &(cb->hit_list);

	return;
}

void lru_hit_list(struct cache_block *cb, struct cache_block *head)
{
	list_del(&(cb->hit_list));
	list_add(&(cb->hit_list), &(head->hit_list));
	return;
}

void lru_tablecell_list(struct cache_block *cb, struct cache_block *head)
{
	list_del(&(cb->list));
	list_add(&(cb->list), &(head->list));
	return;
}

