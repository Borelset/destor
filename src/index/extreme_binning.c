/**
 * @file extreme_binning.c
 * @Synopsis MASCOTS'09, EXtreme Binning without file-level dedup.
 *
 * @author fumin, fumin@hust.edu.cn
 * @version 1
 * @date 2013-01-09
 */
#include "../dedup.h"
#include "extreme_binning.h"

#define BIN_LEVEL_COUNT 20
const static int64_t bvolume_head_size = 20;
const static int64_t level_factor = 512;

/* feature and size */
const static int32_t bin_head_size = 24;

/* feature:bin_address pairs */
static GHashTable *primary_index;
static BinVolume *bin_volume_array[BIN_LEVEL_COUNT];
/* 
 * This can be optimized.
 * When using with cfl_filter,
 * there may be multiple active bins.
 * So a LRU cache can be employed for optimizing.
 */
/* current active bin */
static Bin *current_bin;

extern char working_path[];

static BinVolume* bin_volume_init(int64_t level){
    BinVolume* bvol = (BinVolume*)malloc(sizeof(BinVolume));
    bvol->level = level;
    bvol->current_bin_num = 0;
    bvol->current_volume_length = bvolume_head_size;

    char volname[20];
    sprintf(volname, "bvol%ld", level);
    strcpy(bvol->filename, working_path);
    strcat(bvol->filename, "index/");
    strcat(bvol->filename, volname);

    int fd;
    if((fd = open(bvol->filename, O_CREAT | O_RDWR, S_IRWXU))<0){
        printf("%s, %d:failed to open bin volume %ld\n",
                __FILE__, __LINE__, level);
        return bvol;
    }

    int64_t tmp = -1;
    if(read(fd, &tmp, 8)!=8 || tmp != level){
        printf("%s, %d: read an empty bin volume.\n", __FILE__,
                __LINE__);
        lseek(fd, 0, SEEK_SET);
        write(fd, &level, 8);
        write(fd, &bvol->current_bin_num, 4);
        write(fd, &bvol->current_volume_length, 8);
        close(fd);
        return bvol;
    }
    read(fd, &bvol->current_bin_num, 4);
    read(fd, &bvol->current_volume_length, 8);
    close(fd);
    return bvol;
}

static BOOL bin_volume_destroy(BinVolume *bvol){
    if(!bvol)
        return FALSE;
    int fd;
    if((fd=open(bvol->filename, O_CREAT|O_RDWR, S_IRWXU))<0){
        printf("%s, %d: failed to open bin volume %ld\n",
                __FILE__, __LINE__, bvol->level);
        return FALSE;
    }
    lseek(fd, 0, SEEK_SET);
    write(fd, &bvol->level, 8);
    write(fd, &bvol->current_bin_num, 4);
    write(fd, &bvol->current_volume_length, 8);

    free(bvol);
    return TRUE;
}

#define BIN_ADDR_MASK (0xffffffffffffff)

/*
 * addr == 0 means this is really a new bin.
 */
static Bin *bin_new(int64_t addr, Fingerprint *feature){
    Bin *nbin = (Bin*)malloc(sizeof(Bin));

    nbin->address = addr;
    nbin->dirty = FALSE;
    memcpy(&nbin->feature, feature, sizeof(Fingerprint));

    nbin->fingers = g_hash_table_new_full(g_int64_hash, g_fingerprint_cmp,
            free, free);
    return nbin;
}

static void bin_free(Bin *bin){
    g_hash_table_destroy(bin->fingers);
    bin->fingers = 0;
    free(bin);
}

static int64_t no_to_level(int64_t chunk_num){
    /* 24 + count*(20+4 +1) */
    int64_t bin_size = bin_head_size + chunk_num * 25;
    bin_size /= level_factor;
    int level = 0;
    while(bin_size){
        ++level;
        bin_size >>= 1;
    }
    return level;
}

static int64_t level_to_size(int64_t level){
    if(level<0 || level >= BIN_LEVEL_COUNT){
        printf("%s, %d: invalid level %ld\n",__FILE__,__LINE__,level);
        return -1;
    }
    int64_t size = (1<<level)*level_factor;
    return size;
}

static int64_t write_bin_to_volume(Bin *bin){
    int64_t level = bin->address >> 56;
    int64_t offset = bin->address & BIN_ADDR_MASK;

    int64_t new_level = no_to_level(g_hash_table_size(bin->fingers));
    if(new_level != level){
        printf("%s, %d: level up %ld -> %ld\n", __FILE__,__LINE__,level, new_level);
        offset = 0;
    }

    char *buffer = malloc(level_to_size(new_level));
    ser_declare;
    ser_begin(buffer, 0);
    ser_bytes(&bin->feature, sizeof(Fingerprint));
    ser_int32(g_hash_table_size(bin->fingers));

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, bin->fingers);
    while(g_hash_table_iter_next(&iter, &key, &value)){
        ser_bytes(key, sizeof(Fingerprint));
        ser_bytes(value, sizeof(ContainerId));
        ser_int8('\t');
    }

    if(ser_length(buffer) < level_to_size(new_level)){
        memset(buffer + ser_length(buffer), 0xcf, 
                level_to_size(new_level) - ser_length(buffer));
    }

    BinVolume *bvol = bin_volume_array[new_level];
    if(offset == 0){
        offset = bvol->current_volume_length;
    }
    int fd = open(bvol->filename, O_RDWR);
    lseek(fd, offset, SEEK_SET);
    if(level_to_size(new_level) != write(fd, buffer, level_to_size(new_level))){
        dprint("failed to write bin!");
        close(fd);
        free(buffer);
        return 0;
    }
    close(fd);
    bvol->current_bin_num ++;
    bvol->current_volume_length += level_to_size(new_level);
    free(buffer);
    bin->dirty = FALSE;
    return (new_level << 56)+offset;
}

static Bin* read_bin_from_volume(int64_t address){
    if(address == 0){
        return NULL;
    }
    int64_t level = address >> 56;
    int64_t offset = address & BIN_ADDR_MASK;

    BinVolume *bvol = bin_volume_array[level];
    char *buffer = malloc(level_to_size(level));

    int fd = open(bvol->filename, O_RDWR);
    lseek(fd, offset, SEEK_SET);
    read(fd, buffer, level_to_size(level));
    close(fd);

    unser_declare;
    unser_begin(buffer, 0);
    Fingerprint feature;
    int32_t chunk_num;
    unser_bytes(&feature, sizeof(Fingerprint));
    unser_int32(chunk_num);

    Bin *bin = bin_new(address, &feature);
    int i;
    for(i=0; i<chunk_num; ++i){
        Fingerprint *finger = (Fingerprint*)malloc(sizeof(Fingerprint));
        ContainerId *cid = (ContainerId*)malloc(sizeof(ContainerId));
        unser_bytes(finger, sizeof(Fingerprint));
        unser_bytes(cid, sizeof(ContainerId));
        char tmp;
        unser_int8(tmp);
        if(tmp != '\t')
            dprint("corrupted bin!");
        g_hash_table_insert(bin->fingers, finger, cid);
    }
    free(buffer);

    bin->dirty = FALSE;
    return bin;
}

BOOL extreme_binning_init(){
    primary_index = g_hash_table_new_full(g_int64_hash, g_fingerprint_cmp,
            NULL, free);

    char filename[256];
    strcpy(filename, working_path);
    strcat(filename, "index/primary_index.map");

    int fd;
    if((fd = open(filename, O_CREAT | O_RDWR, S_IRWXU)) < 0){
        dprint("failed to open primary_index.map");
        return FALSE;
    }

    int item_num = 0;
    read(fd, &item_num, 4);
    int i = 0;
    for(; i<item_num; ++i){
        PrimaryItem *item = (PrimaryItem*)malloc(sizeof(PrimaryItem));
        read(fd, &item->feature, sizeof(Fingerprint));
        read(fd, &item->bin_addr, sizeof(item->bin_addr));
        g_hash_table_insert(primary_index, &item->feature, item);
    }
    close(fd);

    i = 0;
    for(; i<BIN_LEVEL_COUNT; ++i){
        bin_volume_array[i] = bin_volume_init((int64_t)i);
    }
    current_bin = 0;

    return TRUE;
}

void extreme_binning_destroy(){
    if(current_bin){
        int64_t new_addr = write_bin_to_volume(current_bin);
        if(new_addr != current_bin->address){
            PrimaryItem* item = g_hash_table_lookup(primary_index, &current_bin->feature);
            if(item == NULL){
                item = (PrimaryItem*)malloc(sizeof(PrimaryItem));
                memcpy(&item->feature, &current_bin->feature, sizeof(Fingerprint));
                g_hash_table_insert(primary_index, &item->feature, item);
            }
            item->bin_addr = new_addr;
        }
        bin_free(current_bin);
        current_bin = 0;
    }
    char filename[256];
    strcpy(filename, working_path);
    strcat(filename, "index/primary_index.map");

    int fd;
    if((fd = open(filename, O_CREAT | O_RDWR, S_IRWXU)) < 0){
        dprint("failed to open primary_index.map");
        return FALSE;
    }

    int item_num = g_hash_table_size(primary_index);
    write(fd, &item_num, 4);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, primary_index);
    while(g_hash_table_iter_next(&iter, &key, &value)){
        PrimaryItem* item = (PrimaryItem*)value;
        write(fd, &item->feature, sizeof(Fingerprint));
        write(fd, &item->bin_addr, sizeof(item->bin_addr));
    }
    close(fd);
    g_hash_table_destroy(primary_index);

    int i = 0;
    for(; i<BIN_LEVEL_COUNT; ++i){
        bin_volume_destroy(bin_volume_array[i]);
        bin_volume_array[i] = 0;
    }
}

ContainerId extreme_binning_search(Fingerprint *fingerprint,
        Fingerprint *feature){
    if((current_bin == 0)|| 
            (memcmp(&current_bin->feature, feature, sizeof(Fingerprint)) != 0)){
        /* write current_bin to volume */
        if(current_bin){
            int64_t new_addr = write_bin_to_volume(current_bin);
            if(new_addr != current_bin->address){
                PrimaryItem* item = g_hash_table_lookup(primary_index, &current_bin->feature);
                if(item == NULL){
                    item = (PrimaryItem*)malloc(sizeof(PrimaryItem));
                    memcpy(&item->feature, &current_bin->feature, sizeof(Fingerprint));
                    g_hash_table_insert(primary_index, &item->feature, item);
                }
                item->bin_addr = new_addr;
            }
            bin_free(current_bin);
        }
        /* read bin according to feature */
        PrimaryItem* item = g_hash_table_lookup(primary_index, feature);
        if(item){
            current_bin = read_bin_from_volume(item->bin_addr);
            if(memcmp(current_bin->feature, feature, sizeof(Fingerprint))!=0){
                puts("error");
            }
        }else{
            current_bin = bin_new(0, feature);
        }
    }

    ContainerId *cid = g_hash_table_lookup(current_bin->fingers, fingerprint);
    if(cid){
        return *cid;
    }else{
        return TMP_CONTAINER_ID;
    }
}

void extreme_binning_update(Fingerprint *finger, ContainerId container_id,
        Fingerprint* feature){
    Fingerprint *new_finger = (Fingerprint*)malloc(sizeof(Fingerprint));
    memcpy(new_finger, finger, sizeof(Fingerprint));
    ContainerId* new_id = (ContainerId*)malloc(sizeof(ContainerId));
    *new_id = container_id;

    if(memcmp(&current_bin->feature, feature, sizeof(Fingerprint)) != 0){
        /* it's possible with cfl_filter */
        int64_t new_addr = write_bin_to_volume(current_bin);
        if(new_addr != current_bin->address){
            PrimaryItem* item = g_hash_table_lookup(primary_index, &current_bin->feature);
            if(item == NULL){
                item = (PrimaryItem*)malloc(sizeof(PrimaryItem));
                memcpy(&item->feature, &current_bin->feature, sizeof(Fingerprint));
                g_hash_table_insert(primary_index, &item->feature, item);
            }
            item->bin_addr = new_addr;
        }
        bin_free(current_bin);
        /* read bin according to feature */
        PrimaryItem* item = g_hash_table_lookup(primary_index, feature);
        if(item){
            current_bin = read_bin_from_volume(item->bin_addr);
        }else{
            current_bin = bin_new(0, feature);
        }
    }
    g_hash_table_insert(current_bin->fingers, new_finger, new_id);
    current_bin->dirty = TRUE;
}
