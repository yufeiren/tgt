/* NUMA-aware cache for iSCSI protocol
 * Yufei Ren (yufei.ren@stonybrook.edu)
 */

#define CACHE_INVALID	0
#define CACHE_VALID	1

struct cache_param {
	int buffer_size;
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
	struct numa_cache *nc;
};

int alloc_nc(struct numa_cache *nc, struct cache_param *cp, int numa_index);

int init_cache(struct host_cache *hc, struct cache_param *cp);

int cache_list_lock_init(struct cache_block *cb);
int cache_list_lock(struct cache_block *cb);
int cache_list_unlock(struct cache_block *cb);

int cache_hash_table_lock_init(struct cache_hash_table *cb);
int cache_hash_table_lock(struct cache_hash_table *cb);
int cache_hash_table_unlock(struct cache_hash_table *cb);

/* hash table functions */

int ht_hash_key(uinit64_t lba, struct cache_hash_table *ht);

/* search hash table, if hit, sort current cell and hit count list */
struct cache_block *ht_search(uint64_t lba, \
			      struct cache_hash_table *ht, \
			      struct cache_block *hit_head);

void sort_cache_block(struct cache_block *cb, struct cache_block *head);

void update_cache_block(struct numa_cache *nc, uint64_t lba, char *data);

