#include "cache.h"

int ht_hash_key(uinit64_t lba, struct cache_hash_table *ht)
{
	return (int) (lba % ht->sz);
}

struct cache_block *ht_search(uinit64_t lba, \
			      struct cache_hash_table *ht, \
			      struct cache_block *hit_head)
{
	int key;
	struct list_head *pos;
	struct cache_block *clist;	/* hash table cell list */
	struct cache_block *cur;

	key = hash_key(lba, ht);

	clist = &(ht->tablecell[key]);

	/*cache_list_lock(clist); */
	cache_hash_table_lock(ht);
	/* search */
	cur = NULL;
	list_for_each_entry(cur, &(clist->list), list) {
		if (cur->lba == lba) {
			cur->hit_count ++;
			break;
		}
	}

	/* sort list */
	if (cur != NULL) {
		cur->hit_count ++;
		/* re-sort hash tablecell list */
		sort_cache_block(cur, clist);

		/* re-sort hit list */
		sort_cache_block(cur, hit_head);

		return cur;
	}

	/*cache_list_unlock(clist); */
	cache_hash_table_unlock(ht);

	return NULL;
}

void sort_cache_block(struct cache_block *cb, struct cache_block *head)
{
	struct list_head *pos;
	struct cache_block *pre;

	for (pos = &(cb->list); pos != &(head->list); pos = pos->prev) {
		pre = list_entry(pos, struct cache_block, list);
		if (pre->hit_count > cb->hit_count)
			break;
	}

	if (pos != &(cb->list)) {
		list_del(&(cb->list));
		cb->list.next = pos->next;
		cb->list->prev = pos;
		pos->next = cb->list;
		pos->next->prev = cb->list;
	}

	return;
}
