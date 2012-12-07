/* NUMA-aware cache for iSCSI protocol
 * Yufei Ren (yufei.ren@stonybrook.edu)
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <numa.h>

#include "list.h"
#include "log.h"
#include "tgtd.h"

#define CACHE_INVALID	0
#define CACHE_VALID	1

struct cache_param {
	long buffer_size;
	int cbs;
	char *mem;	/* malloc or shm */
};


struct cache_block {
	int is_valid;
	uint64_t lba;	/* logical block address */
	uint32_t cbs;	/* cache block size */
	int hit_count;	/* times of cache hit */
	char *addr;
	pthread_mutex_t lock;
	struct list_head list; /* a block either in hash table or in unused list*/
	struct list_head hit_list;	/* hit count list - deascending */
};

struct cache_hash_table {
	int sz;
	pthread_mutex_t lock;
	struct cache_block *tablecell;
};

struct numa_cache {
	int buffer_size;
	char *buffer;
	uint32_t cbs;	/* cache block size */
	int nb;		/* number of cache blocks */
	struct cache_block *cb;
	/* hash table and linked list are used for cache */
	struct cache_hash_table ht;
	struct cache_block unused_list;
	struct cache_block hit_list;
};

struct host_cache {
	int numa_nodes;
	int buffer_size;
	int cbs;
	struct numa_cache *nc;
};

uint64_t offset2segid(uint64_t offset, struct host_cache *hc);

int offset2ncid(uint64_t offset, struct host_cache *hc);

int alloc_nc(struct numa_cache *nc, struct cache_param *cp, int numa_index);

int init_cache(struct host_cache *hc, struct cache_param *cp);

int cache_list_lock_init(struct cache_block *cb);
int cache_list_lock(struct cache_block *cb);
int cache_list_unlock(struct cache_block *cb);

int cache_hash_table_lock_init(struct cache_hash_table *ht);
int cache_hash_table_lock(struct cache_hash_table *ht);
int cache_hash_table_unlock(struct cache_hash_table *ht);

struct cache_block *search_numa_cache(uint64_t lba, \
				      struct numa_cache *nc);

/* hash table functions */

int ht_hash_key(uint64_t lba, struct cache_hash_table *ht);

/* search hash table, if hit, sort current cell and hit count list */

void sort_tablecell_list(struct cache_block *cb, struct cache_block *head);

void sort_hit_list(struct cache_block *cb, struct cache_block *head);

void update_cache_block(struct numa_cache *nc, uint64_t lba, char *data);



