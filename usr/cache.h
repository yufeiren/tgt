/* NUMA-aware cache for iSCSI protocol
 * Yufei Ren (yufei.ren@stonybrook.edu)
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <numa.h>

#include "list.h"
#include "log.h"
#include "tgtd.h"
#include "scsi.h"
#include "bs_thread.h"

#define CACHE_INVALID	0
#define CACHE_VALID	1

struct cache_param {
	size_t buffer_size;
	int cbs;
	int cache_way;	/* # of caches per node */
	char *mem;	/* malloc or shm */
};

struct cache_block {
	int is_valid;
	int tid;		/* target id */
	uint64_t lun;		/* logical unit number */
	uint64_t dev_id;	/* correspondent device id */
	uint64_t cb_id;		/* cache block id */
	uint32_t cbs;		/* cache block size */
	int hit_count;		/* times of cache hit */
	char *addr;
	pthread_mutex_t lock;
	struct list_head list; /* a block either in hash table or in unused list*/
	struct list_head hit_list;	/* hit count list - lru */
};

struct cache_hash_table {
	int sz;
	pthread_mutex_t lock;
	struct cache_block *tablecell;
};

struct numa_cache {
	int id;
	int on_numa_node;	/* numa node this cache located */
	size_t buffer_size;	/* cache size for this numa cache */
	char *buffer;
	uint32_t cbs;		/* cache block size */
	int nb;			/* number of cache blocks */
	struct cache_block *cb;
	/* hash table and linked list are used for cache management */
	struct cache_hash_table ht;
	struct cache_block unused_list;
	struct cache_block hit_list;

	pthread_mutex_t mutex;
};

struct host_cache {
	int nr_numa_nodes;	/* number of numa nodes */
	int nr_cache_area;	/* cache area for each numa nodes */
	size_t buffer_size;	/* cache size of all nodes together */
	int cbs;		/* cache block size */
	int dio_align;	/* memory alignment and IO size for direct IO */
	unsigned seed;
	struct numa_cache *nc;  /* pointer to numa caches */
};

int offset2ncid(uint64_t offset, struct host_cache *hc);

int offset2nodeid(uint64_t offset, struct host_cache *hc);

int ncid2nodeid(int nc_id, struct host_cache *hc);

int alloc_nc(struct numa_cache *nc, struct host_cache *hc);

int init_cache(struct host_cache *hc, struct cache_param *cp);

int nc_mutex_init(pthread_mutex_t *mutex);
int nc_mutex_lock(pthread_mutex_t *mutex);
int nc_mutex_unlock(pthread_mutex_t *mutex);

/* cache replacement */
struct cache_block *get_cache_block(int tid, uint64_t lun, uint64_t cb_id, \
				    struct numa_cache *nc);

void insert_cache_block(struct cache_block *cb, struct numa_cache *nc);

void invalidate_cache_block(int tid, uint64_t lun, \
			    uint64_t cb_id, struct numa_cache *nc);

/* hash table functions */

int ht_hash_key(uint64_t lba, struct cache_hash_table *ht);

/* search hash table, if hit, sort current cell and hit count list */

void sort_tablecell_list(struct cache_block *cb, struct cache_block *head);

/* consider hit times */
void sort_hit_list(struct cache_block *cb, struct cache_block *head);

/* pure lru without hit involved */
void lru_tablecell_list(struct cache_block *cb, struct cache_block *head);

void lru_hit_list(struct cache_block *cb, struct cache_block *head);

/* split io-request into sub-tasks
 * return value is a numa node id
 */
int split_io(struct scsi_cmd *cmd, struct host_cache *hc);

