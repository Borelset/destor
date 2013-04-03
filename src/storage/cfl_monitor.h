#ifndef CFL_MONITOR_H_
#define CFL_MONITOR_H_
#include "../global.h"
#include "../tools/lru_cache.h"

typedef struct cfl_monitor_tag CFLMonitor;

struct cfl_monitor_tag {
    int64_t total_size;

    int ocf; //data amount/CONTAINER_SIZE
    int ccf;
    double cfl; //ocf/ccf

    LRUCache *cache;
};

CFLMonitor* cfl_monitor_new(int read_cache_size);
void cfl_monitor_free(CFLMonitor *monitor);
void update_cfl(CFLMonitor *monitor, ContainerId id, int32_t chunklen);
void update_cfl_directly(CFLMonitor* monitor, int32_t chunklen, BOOL is_new);
double get_cfl(CFLMonitor *monitor);
void print_cfl(CFLMonitor *monitor);
BOOL is_container_already_in_cache(CFLMonitor* monitor, ContainerId id);
#endif
