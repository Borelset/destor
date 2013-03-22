/**
 * @file silo.c
 * @Synopsis SiLo, in ATC'11.
 * SHTable, stored in ${working_path}/index/shtable
 * SiloBlock, stored in ${working_path}/index/block.vol,
 *      block_num, block1, block2, block3, ... (just append),
 * which are indexed by block id.
 *
 * The only difference is without segment-level dedup.
 * @author fumin, fumin@hust.edu.cn
 * @version 1
 * @date 2013-01-06
 */
#include "silo.h"

extern int32_t silo_segment_size;//KB
extern int32_t silo_block_size;//MB
extern uint32_t rabin_chunk_size;
extern char working_path[];
/* silo_x_size/average chunk size */
int32_t silo_segment_hash_size;
int32_t silo_block_hash_size;

int32_t silo_item_size = sizeof(Fingerprint)+sizeof(ContainerId);

#define BLOCK_HEAD_SIZE 8
/* segment delegate-block id mapping */
static GHashTable *SHTable;

static SiloBlock *write_buffer;
/* now we only maintain one block-sized cache */
static SiloBlock *read_cache;

static int32_t block_num;
static int block_vol_fd;

static SiloBlock* silo_block_new(){
    SiloBlock* block = (SiloBlock*)malloc(sizeof(SiloBlock));
    block->LHTable = g_hash_table_new_full(g_int64_hash,
            g_fingerprint_cmp, free, free);
    block->id = -1;
    block->delegate_table = g_hash_table_new(g_int64_hash, g_fingerprint_cmp);
    block->size = 0;

    return block;
}

static void silo_block_free(SiloBlock* block){
    g_hash_table_destroy(block->LHTable);
    g_hash_table_destroy(block->delegate_table);

    free(block);
}

/* id,num, finger-id */
static void append_block_to_volume(SiloBlock *block){
    block->id = block_num;
    char buffer[silo_block_hash_size+BLOCK_HEAD_SIZE];
    ser_declare;
    ser_begin(buffer, 0);
    ser_int32(block->id);
    ser_uint32(g_hash_table_size(block->LHTable));

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, block->LHTable);
    while(g_hash_table_iter_next(&iter, &key, &value)){
        ser_bytes(key, sizeof(Fingerprint));
        ser_bytes(value, sizeof(ContainerId));
    }

    int length = ser_length(buffer);
    if(length < (silo_block_hash_size+BLOCK_HEAD_SIZE)){
        memset(buffer + length, 0, silo_block_hash_size+BLOCK_HEAD_SIZE-length);
    }

    lseek(block_vol_fd, sizeof(block_num) +
            block_num*(silo_block_hash_size+BLOCK_HEAD_SIZE), SEEK_SET);
    write(block_vol_fd, buffer, BLOCK_HEAD_SIZE+silo_block_hash_size);

    block_num ++;
}

static SiloBlock* read_block_from_volume(int32_t block_id){
    if(block_id<0){
        dprint("invalid id!");
        return NULL;
    }
    lseek(block_vol_fd, sizeof(block_num) + block_id*(silo_block_hash_size+BLOCK_HEAD_SIZE), SEEK_SET);
    char buffer[silo_block_hash_size+BLOCK_HEAD_SIZE];
    read(block_vol_fd, buffer, silo_block_hash_size+BLOCK_HEAD_SIZE);

    SiloBlock* block = silo_block_new();

    unser_declare;
    unser_begin(buffer, 0);
    unser_int32(block->id);
    if(block->id != block_id){
        dprint("inconsistent block id!");
        return NULL;
    }
    int32_t num = 0;
    unser_uint32(num);
    int i;
    for(i=0; i<num; ++i){
        Fingerprint *finger = (Fingerprint*)malloc(sizeof(Fingerprint));
        ContainerId *cid = (ContainerId*)malloc(sizeof(ContainerId));
        unser_bytes(finger, sizeof(Fingerprint));
        unser_bytes(cid, sizeof(ContainerId));
        g_hash_table_insert(block->LHTable, finger, cid);
    }

    return block;
}

BOOL silo_init(){
    silo_segment_hash_size = silo_segment_size*1024/
        (rabin_chunk_size+1)*silo_item_size;
    silo_block_hash_size = silo_block_size*1024*1024/
        (rabin_chunk_size+1)*silo_item_size;

    char filename[256];
    /* init SHTable */
    SHTable = g_hash_table_new_full(g_int64_hash, g_fingerprint_cmp, 
            free, free);

    strcpy(filename, working_path);
    strcat(filename, "index/shtable");
    int fd;
    if((fd = open(filename, O_CREAT | O_RDWR, S_IRWXU))<0){
        dprint("failed to open shtable!");
        return FALSE;
    }
    int num = 0;
    if(read(fd, &num, 4) == 4){
        /* existed, read it */
        int i;
        for(i=0; i<num; ++i){
            Fingerprint* delegate = (Fingerprint*)malloc(sizeof(Fingerprint));
            int32_t* block_id = (int32_t*)malloc(sizeof(int32_t));
            read(fd, delegate, sizeof(Fingerprint));
            read(fd, block_id, sizeof(int32_t));
            g_hash_table_insert(SHTable, delegate, block_id);
        }
    }
    close(fd);

    /* init block volume */
    block_num = 0;
    strcpy(filename, working_path);
    strcat(filename, "index/block.vol");
    if((block_vol_fd = open(filename, O_CREAT | O_RDWR, S_IRWXU))<0){
        dprint("failed to open block.vol!");
        return FALSE;
    }
    if(read(block_vol_fd, &block_num, 4) != 4){
        /* empty */
        block_num = 0;
        lseek(block_vol_fd, 0, SEEK_SET);
        write(block_vol_fd, &block_num, 4);
    }

    /*init read cache */
    read_cache = silo_block_new();
    write_buffer = silo_block_new();
}

void silo_destroy(){
    if(write_buffer->size > 0){
        append_block_to_volume(write_buffer);
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, write_buffer->delegate_table);
        while(g_hash_table_iter_next(&iter, &key, &value)){
            g_hash_table_insert(SHTable, key, value);
        }
        silo_block_free(write_buffer);
        write_buffer = 0;
    }
    silo_block_free(read_cache);

    /*update block volume*/
    lseek(block_vol_fd, 0, SEEK_SET);
    write(block_vol_fd, &block_num, 4);
    close(block_vol_fd);
    block_vol_fd = -1;

    /*update shtable*/
    char filename[256];
    strcpy(filename, working_path);
    strcat(filename, "index/shtable");
    int fd;
    if((fd = open(filename, O_CREAT | O_RDWR, S_IRWXU))<0){
        dprint("failed to open shtable!");
        return FALSE;
    }
    uint32_t hash_num = g_hash_table_size(SHTable);
    write(fd, &hash_num, 4);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, SHTable);
    while(g_hash_table_iter_next(&iter, &key, &value)){
        write(fd, key, sizeof(Fingerprint));
        write(fd, value, sizeof(int32_t));
    }
    g_hash_table_destroy(SHTable);
    SHTable = 0;
    close(fd);
}

ContainerId silo_search(Fingerprint* fingerprint, Fingerprint* delegate){
    /* is write_buffer full? */
    if((write_buffer->size + silo_item_size) > silo_block_hash_size){
        /* this chunk does not belong to this block */
        append_block_to_volume(write_buffer);
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, write_buffer->delegate_table);
        while(g_hash_table_iter_next(&iter, &key, &value)){
            g_hash_table_insert(SHTable, key, value);
        }
        silo_block_free(write_buffer);
        write_buffer = silo_block_new();
    }
    if(g_hash_table_lookup(write_buffer->delegate_table, 
                delegate) == NULL){
        /* a new segement */
        if(write_buffer->size + silo_segment_hash_size > silo_block_hash_size)
            dprint("It can't happen!");
        Fingerprint *new_delegate = (Fingerprint*)malloc(sizeof(Fingerprint));
        memcpy(new_delegate, delegate, sizeof(Fingerprint));
        int32_t *new_bid = (int32_t*)malloc(sizeof(int32_t));
        *new_bid = block_num;
        g_hash_table_insert(write_buffer->delegate_table, new_delegate, new_bid);
    }
    write_buffer->size += silo_item_size;

    /* is existed in write_buffer? */
    ContainerId *cid = g_hash_table_lookup(write_buffer->LHTable, fingerprint);
    if(cid){
        return *cid;
    }

    /* is existed in SHTable? */
    int32_t *bid = g_hash_table_lookup(SHTable, delegate);
    if(bid != 0 && *bid != read_cache->id){
        /* its block is not in read cache */
        silo_block_free(read_cache);
        read_cache = read_block_from_volume(*bid);
    }

    /* filter the fingerprint in read cache */
    cid = g_hash_table_lookup(read_cache->LHTable, fingerprint);
    if(cid == 0){
        return TMP_CONTAINER_ID;
    }else{
        Fingerprint* new_fingerprint = (Fingerprint*)malloc(sizeof(Fingerprint));
        memcpy(new_fingerprint, fingerprint, sizeof(Fingerprint));
        ContainerId* new_cid = (ContainerId*)malloc(sizeof(ContainerId));
        memcpy(new_cid, cid, sizeof(ContainerId));
        g_hash_table_insert(write_buffer->LHTable, new_fingerprint, new_cid);
        return *cid;
    }
}

void silo_insert(Fingerprint* fingerprint, ContainerId container_id,
        Fingerprint *delegate){
    if(!g_hash_table_lookup(write_buffer->delegate_table, delegate)){
        dprint("It can not happen!");
    }
    Fingerprint* new_finger = (Fingerprint*)malloc(sizeof(Fingerprint));
    memcpy(new_finger, fingerprint, sizeof(Fingerprint));
    ContainerId* new_cid = (ContainerId*)malloc(sizeof(ContainerId));
    memcpy(new_cid, &container_id, sizeof(ContainerId));
    g_hash_table_insert(write_buffer->LHTable, new_finger, new_cid);
}
