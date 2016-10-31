# NUMA-aware Cache for Linux iSCSI/iSER Framework

The NUMA-aware Cache is built on top of Linux iSCSI/iSER Framework,
[tgt](https://github.com/fujita/tgt). It replaces the kernel page
cache for tgt's caching layer. It aligns cache memory with local NUMA
nodes and threads, and then schedule I/O requests to those threads
that are local to the data being accessed. This NUMA-aware solution
results in lower access latency and higher system throughput.

# Reference

[Design, Implementation, and Evaluation of a NUMA-Aware Cache for
    iSCSI Storage
    Servers](http://ieeexplore.ieee.org/document/6767115/), IEEE
    Transactions on Parallel and Distributed Systems (TPDS), Feb. 2015

