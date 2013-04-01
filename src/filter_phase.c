#include "global.h"
#include "dedup.h" 
#include "jcr.h"
#include "index/index.h"
#include "storage/protos.h"

extern int rewriting_algorithm;

extern int recv_feature(Chunk **chunk);
extern ContainerId save_chunk(Chunk* chunk);

void* simply_filter(void* arg){
    Jcr* jcr = (Jcr*) arg;
    GHashTable* historical_sparse_containers = 0;
    historical_sparse_containers = load_historical_sparse_containers(jcr->job_id);
    ContainerUsageMonitor* monitor =container_usage_monitor_new();
    while (TRUE) {
        BOOL update = FALSE;
        Chunk* chunk = NULL;
        int signal = recv_feature(&chunk);

        TIMER_DECLARE(b1, e1);
        TIMER_BEGIN(b1);
        if (signal == STREAM_END) {
            free_chunk(chunk);
            break;
        }

        /* init FingerChunk */
        FingerChunk *new_fchunk = (FingerChunk*)malloc(sizeof(FingerChunk));
        new_fchunk->container_id = TMP_CONTAINER_ID;
        new_fchunk->length = chunk->length;
        memcpy(&new_fchunk->fingerprint, &chunk->hash, sizeof(Fingerprint));
        new_fchunk->container_id = chunk->container_id;

        if (new_fchunk->container_id != TMP_CONTAINER_ID) {
            if(rewriting_algorithm == HBR_REWRITING && historical_sparse_containers!=0 && 
                    g_hash_table_lookup(historical_sparse_containers, &new_fchunk->container_id) != NULL){
                /* this chunk is in a sparse container */
                /* rewrite it */
                chunk->duplicate = FALSE;
                new_fchunk->container_id = save_chunk(chunk);

                update = TRUE;
                jcr->rewritten_chunk_count++;
                jcr->rewritten_chunk_amount += new_fchunk->length;
            }else{
                chunk->duplicate = TRUE;
                jcr->dedup_size += chunk->length;
                ++jcr->number_of_dup_chunks;
            }
        } else {
            chunk->duplicate = FALSE;
            new_fchunk->container_id = save_chunk(chunk); 
            update = TRUE;
        }
        container_usage_monitor_update(monitor, new_fchunk->container_id,
                &new_fchunk->fingerprint, new_fchunk->length);
        index_insert(&new_fchunk->fingerprint, new_fchunk->container_id, &chunk->feature, update);
        sync_queue_push(jcr->fingerchunk_queue, new_fchunk);
        TIMER_END(jcr->filter_time, b1, e1);
        free_chunk(chunk);
    }//while(TRUE) end

    save_chunk(NULL);

    FingerChunk* fchunk_sig = (FingerChunk*)malloc(sizeof(FingerChunk));
    fchunk_sig->container_id = STREAM_END;
    sync_queue_push(jcr->fingerchunk_queue, fchunk_sig);

    jcr->sparse_container_num = g_hash_table_size(monitor->sparse_map);
    jcr->total_container_num = g_hash_table_size(monitor->dense_map) + jcr->sparse_container_num;
    while((jcr->inherited_sparse_num = container_usage_monitor_print(monitor, 
                    jcr->job_id, historical_sparse_containers))<0){
        dprint("retry!");
    }
    if(historical_sparse_containers)
        destroy_historical_sparse_containers(historical_sparse_containers);
    container_usage_monitor_free(monitor);
    return NULL;
}
