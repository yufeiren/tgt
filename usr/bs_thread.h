typedef void (request_func_t) (struct scsi_cmd *);

#define MAX_NR_NUMA_NODES	128

struct bs_thread_info {
	pthread_t *worker_thread;
	int nr_worker_threads;

	int nr_numa_nodes;
	int thr_node_id;	/* current thread node id for numa */

	/* wokers sleep on this and signaled by tgtd */
	pthread_cond_t pending_cond[MAX_NR_NUMA_NODES];
	/* locked by tgtd and workers */
	pthread_mutex_t pending_lock[MAX_NR_NUMA_NODES];
	/* protected by pending_lock */
	struct list_head pending_list[MAX_NR_NUMA_NODES];

	pthread_mutex_t startup_lock;

	int stop;

	request_func_t *request_fn;
};

static inline struct bs_thread_info *BS_THREAD_I(struct scsi_lu *lu)
{
	return (struct bs_thread_info *) ((char *)lu + sizeof(*lu));
}

extern tgtadm_err bs_thread_open(struct bs_thread_info *info, request_func_t *rfn,
				 int nr_threads);
extern void bs_thread_close(struct bs_thread_info *info);
extern int bs_thread_cmd_submit(struct scsi_cmd *cmd);

