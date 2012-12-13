#include "cache.h"

int ht_hash_key(uint64_t lba, struct cache_hash_table *ht)
{
	return (int) (lba % ht->sz);
}

struct cache_block *search_numa_cache(uint64_t lba, \
				      struct numa_cache *nc)
{
	int key;
	struct list_head *pos;
	struct cache_block *clist;	/* hash table cell list */
	struct cache_block *cur;
	struct cache_hash_table *ht = &(nc->ht);
	struct cache_block *hit_head = &(nc->hit_list);
	int is_found;

	key = ht_hash_key(lba, ht);
	dprintf("numa cache: this io key is %d\n", key);

	clist = &(ht->tablecell[key]);

	/* cache_list_lock(clist); */
	cache_hash_table_lock(ht);

	/* search */
	cur = NULL;
	is_found = 0;
	list_for_each_entry(cur, &(clist->list), list) {
		if (cur->lba == lba) {
			cur->hit_count;
			dprintf("numa cache: hit cache %d times\n", \
				cur->hit_count);
			is_found = 1;
			break;
		}
	}

	/* sort list */
	if (is_found) {
		dprintf("numa cache: hit cache in hash table\n");
		cur->hit_count ++;
		/* re-sort hash tablecell list */
		dprintf("numa cache: re-sort hash tablecell\n");
		/* sort_tablecell_list(cur, clist); */

		/* re-sort hit list */
		dprintf("numa cache: re-sort hit list\n");
		/* sort_hit_list(cur, hit_head); */
		lru_hit_list(cur, hit_head);

	}

	cache_hash_table_unlock(ht);

	if (is_found)
		return cur;

	return NULL;
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
