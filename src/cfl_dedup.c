/*
 * cfl_dedup.c
 *
 *  Created on: Sep 19, 2012
 *      Author: fumin
 */

#include "global.h"
#include "dedup.h"
#include "index/index.h"
#include "jcr.h"
#include "tools/sync_queue.h"
#include "storage/cfl_monitor.h"

extern int read_cache_size;
extern double cfl_require;

extern double container_usage_threshold;

/* input */
/* hash queue */
extern SyncQueue* prepare_queue;

/* output */
/* container queue */
extern SyncQueue* container_queue;

static Container* container_tmp;

/*
 * The fchunks with undetermined container id will be pushed into this queue.
 * If container_tmp is empty, this queue is empty too.
 * */
static Queue *waiting_chunk_queue;

/* Monitoring cfl in storage system. */
static CFLMonitor* monitor;

static void rewrite_container(Jcr *jcr){
    /*printf("queue_size: %d\n", queue_size(waiting_chunk_queue));*/
    Chunk *waiting_chunk = queue_pop(waiting_chunk_queue);
    while(waiting_chunk){
        FingerChunk* fchunk = (FingerChunk*)malloc(sizeof(FingerChunk));
        fchunk->container_id = waiting_chunk->container_id;
        fchunk->length = waiting_chunk->length;
        memcpy(&fchunk->fingerprint, &waiting_chunk->hash, sizeof(Fingerprint));
        fchunk->next = 0;
        if(waiting_chunk->container_id == TMP_CONTAINER_ID){
            Chunk *chunk = container_get_chunk(container_tmp, &waiting_chunk->hash);
            memcpy(&chunk->delegate, &waiting_chunk->delegate, sizeof(Fingerprint));
            free_chunk(waiting_chunk);
            while(container_add_chunk(jcr->write_buffer, chunk) == CONTAINER_FULL){
                int32_t nid = seal_container(jcr->write_buffer);
                if(nid < TMP_CONTAINER_ID){
                    dprint("failed to seal container");
                }
                sync_queue_push(container_queue, jcr->write_buffer);

                jcr->write_buffer = container_new_full();
                set_container_id(jcr->write_buffer);
            }
            jcr->rewritten_chunk_count++;
            jcr->rewritten_chunk_amount += chunk->length;
            index_insert(&chunk->hash, jcr->write_buffer->id, &chunk->delegate);
            fchunk->container_id = jcr->write_buffer->id;
            free_chunk(chunk);
        } else {
            /*printf("%s, %d: new chunk\n",__FILE__,__LINE__);*/
        }
        update_cfl(monitor, fchunk->container_id, fchunk->length);
        sync_queue_push(jcr->fingerchunk_queue, fchunk);
        waiting_chunk = queue_pop(waiting_chunk_queue);
    }
}

static void selective_dedup(Jcr *jcr, Chunk *new_chunk){
    new_chunk->container_id = TMP_CONTAINER_ID;
    ContainerId cid = index_search(&new_chunk->hash, &new_chunk->delegate);
    if(cid != TMP_CONTAINER_ID){
        /* existed */
        if(container_tmp->chunk_num != 0
                && container_tmp->id != cid){
            /* determining whether rewrite container_tmp */
            if(container_get_usage(container_tmp) < container_usage_threshold){
                /* rewrite */
                rewrite_container(jcr);
            }else{
                Chunk* waiting_chunk = queue_pop(waiting_chunk_queue);
                while(waiting_chunk){
                    FingerChunk* fchunk = (FingerChunk*)malloc(sizeof(FingerChunk));
                    fchunk->container_id = waiting_chunk->container_id;
                    fchunk->length = waiting_chunk->length;
                    memcpy(&fchunk->fingerprint, &waiting_chunk->hash, sizeof(Fingerprint));
                    fchunk->next = 0;
                    if(fchunk->container_id == TMP_CONTAINER_ID){
                        jcr->dedup_size += fchunk->length;
                        jcr->number_of_dup_chunks++;
                        fchunk->container_id = container_tmp->id;
                    }
                    update_cfl(monitor, fchunk->container_id, fchunk->length);
                    sync_queue_push(jcr->fingerchunk_queue, fchunk);
                    free_chunk(waiting_chunk);
                    waiting_chunk = queue_pop(waiting_chunk_queue);
                }
            }
            container_free_full(container_tmp);
            container_tmp = container_new_full();
        }
        if(container_add_chunk(container_tmp, new_chunk)==CONTAINER_FULL){
            printf("%s, %d: container_tmp is full\n",__FILE__,__LINE__);
        }
        free(new_chunk->data);
        new_chunk->data = 0;
        container_tmp->id = cid;
        queue_push(waiting_chunk_queue, new_chunk);
    } else {
        while (container_add_chunk(jcr->write_buffer, new_chunk)
                == CONTAINER_FULL) {
            Container *container = jcr->write_buffer;

            int32_t nid = seal_container(container);
            if(nid < TMP_CONTAINER_ID){
                dprint("failed to seal container");
            }
            sync_queue_push(container_queue, container);

            jcr->write_buffer = container_new_full();
            set_container_id(jcr->write_buffer);
        } 
        new_chunk->container_id = jcr->write_buffer->id;
        index_insert(&new_chunk->hash, new_chunk->container_id, &new_chunk->delegate);
        if(queue_size(waiting_chunk_queue) == 0){
            FingerChunk* fchunk = (FingerChunk*)malloc(sizeof(FingerChunk));
            fchunk->container_id = new_chunk->container_id;
            fchunk->length = new_chunk->length;
            memcpy(&fchunk->fingerprint, &new_chunk->hash, sizeof(Fingerprint));
            fchunk->next = 0;
            free_chunk(new_chunk);
            update_cfl(monitor, fchunk->container_id, fchunk->length);
            sync_queue_push(jcr->fingerchunk_queue, fchunk);
        }else{
            free(new_chunk->data);
            new_chunk->data = 0;
            queue_push(waiting_chunk_queue, new_chunk);
        }
    }
}

static void typical_dedup(Jcr *jcr, Chunk *new_chunk){
    FingerChunk* fchunk = (FingerChunk*)malloc(sizeof(FingerChunk));
    fchunk->container_id = TMP_CONTAINER_ID;// tmp value
    fchunk->length = new_chunk->length;
    memcpy(&fchunk->fingerprint, &new_chunk->hash, sizeof(Fingerprint));
    fchunk->next = 0;

    fchunk->container_id = index_search(&new_chunk->hash, &new_chunk->delegate);
    if(fchunk->container_id != TMP_CONTAINER_ID){
        jcr->dedup_size += fchunk->length;
        jcr->number_of_dup_chunks++;
    }else{
        while (container_add_chunk(jcr->write_buffer, new_chunk)
                == CONTAINER_FULL) {
            Container *container = jcr->write_buffer;

            int32_t nid = seal_container(container);
            if(nid < TMP_CONTAINER_ID){
                dprint("failed to seal container");
            }
            sync_queue_push(container_queue, container);

            jcr->write_buffer = container_new_full();
            set_container_id(jcr->write_buffer);
        }
        fchunk->container_id = jcr->write_buffer->id;
        index_insert(&fchunk->fingerprint, fchunk->container_id, &new_chunk->delegate);
    }
    update_cfl(monitor, fchunk->container_id, fchunk->length);
    sync_queue_push(jcr->fingerchunk_queue, fchunk);
    free_chunk(new_chunk);
}

/*
 * Intuitively, cfl_filter() would not works well with ddfs_index.
 * Due of the locality preserved caching in ddfs_index, 
 * some unfortunate chunks in previous containers may be rewritten repeatedly.
 */
/* --------------------------------------------------------------------------*/
/**
 * @Synopsis  Assuring Demanded Read Performance of Data Deduplication Storage
 *            with Backup Datasets. In MASCOTS'12.
 *
 * @Param arg
 *
 * @Returns   
 */
/* ----------------------------------------------------------------------------*/
void *cfl_filter(void* arg){
    Jcr* jcr = (Jcr*) arg;
    jcr->write_buffer = container_new_full();
    set_container_id(jcr->write_buffer);
    monitor = cfl_monitor_new(read_cache_size, cfl_require);
    while(TRUE){
        Chunk* new_chunk = sync_queue_pop(prepare_queue);

        if (new_chunk->length == STREAM_END) {
            free_chunk(new_chunk);
            break;
        }

        TIMER_DECLARE(b1, e1);
        TIMER_BEGIN(b1);
        if(monitor->enable_selective){
            /* selective deduplication */
            selective_dedup(jcr, new_chunk);

            if(get_cfl(monitor) > monitor->high_water_mark){
                monitor->enable_selective = FALSE;
                Chunk *chunk = queue_pop(waiting_chunk_queue);
                if(chunk){
                    chunk->container_id = container_tmp->id;
                    FingerChunk* fchunk = (FingerChunk*)malloc(sizeof(FingerChunk));
                    fchunk->container_id = chunk->container_id;
                    fchunk->length = chunk->length;
                    memcpy(&fchunk->fingerprint, &chunk->hash, sizeof(Fingerprint));
                    fchunk->next = 0;
                    free_chunk(chunk);    
                    update_cfl(monitor, fchunk->container_id, fchunk->length);
                    sync_queue_push(jcr->fingerchunk_queue, fchunk);
                }
                if(queue_size(waiting_chunk_queue)!=0){
                    printf("%s, %d: irregular state in queue. size=%d.\n",__FILE__,__LINE__, queue_size(waiting_chunk_queue));
                }
                queue_free(waiting_chunk_queue, 0);
                waiting_chunk_queue = 0;

                container_free_full(container_tmp);
                container_tmp = 0;
            }   
        }else{
            /* typical dedup */
            typical_dedup(jcr, new_chunk);

            if(get_cfl(monitor) < monitor->low_water_mark){
                monitor->enable_selective = TRUE;
                if(container_tmp == 0)
                    container_tmp = container_new_full();
                else
                    printf("%s, %d: error\n",__FILE__,__LINE__);
                if(waiting_chunk_queue == 0)
                    waiting_chunk_queue = queue_new();
                else
                    printf("%s, %d: error\n",__FILE__,__LINE__);
            }
        }
        TIMER_END(jcr->filter_time, b1, e1);
    }

    /* Handle container_tmp*/
    if(monitor->enable_selective){
        if (container_get_usage(container_tmp)
                < container_usage_threshold) {
            //rewrite container_tmp
            rewrite_container(jcr);
        } else {
            Chunk* waiting_chunk = queue_pop(waiting_chunk_queue);
            while(waiting_chunk){
                FingerChunk* fchunk = (FingerChunk*)malloc(sizeof(FingerChunk));
                fchunk->container_id = waiting_chunk->container_id;
                fchunk->length = waiting_chunk->length;
                memcpy(&fchunk->fingerprint, &waiting_chunk->hash, sizeof(Fingerprint));
                fchunk->next = 0;
                if(fchunk->container_id == TMP_CONTAINER_ID){
                    jcr->dedup_size += fchunk->length;
                    jcr->number_of_dup_chunks++;
                    fchunk->container_id = container_tmp->id;
                }
                update_cfl(monitor, fchunk->container_id, fchunk->length);
                sync_queue_push(jcr->fingerchunk_queue, fchunk);
                free_chunk(waiting_chunk);
                waiting_chunk = queue_pop(waiting_chunk_queue);
            }
        }
        container_free_full(container_tmp);
        container_tmp = 0;
        queue_free(waiting_chunk_queue, 0);
        waiting_chunk_queue = 0;
    }else{
        if(waiting_chunk_queue != 0){
            printf("%s, %d: irregular situation!\n",__FILE__,__LINE__);
        }
    }

    FingerChunk* fc_sig = (FingerChunk*)malloc(sizeof(FingerChunk));
    fc_sig->container_id = STREAM_END;
    sync_queue_push(jcr->fingerchunk_queue, fc_sig);

    /* Append write_buffer */
    Container *container = jcr->write_buffer;
    if(seal_container(container) != TMP_CONTAINER_ID){
        sync_queue_push(container_queue, container);
    }else{
        container_free_full(container);
    }
    jcr->write_buffer = 0;

    Container *signal = container_new_meta_only();
    signal->id = STREAM_END;
    sync_queue_push(container_queue, signal);

    print_cfl(monitor);
    cfl_monitor_free(monitor);
    return NULL;
}
