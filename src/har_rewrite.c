/*
 * har_rewrite.c
 *
 *  Created on: Nov 27, 2013
 *      Author: fumin
 */

#include "destor.h"
#include "rewrite_phase.h"
#include "storage/containerstore.h"
#include "jcr.h"

struct {
	/* Containers of enough utilization are in this map. */
	GHashTable *dense_map;
	/* Containers of not enough utilization are in this map. */
	GHashTable *sparse_map;

	int64_t total_size;
} container_utilization_monitor;

static GHashTable *inherited_sparse_containers;

void init_har() {

	container_utilization_monitor.dense_map = g_hash_table_new_full(
			g_int64_hash, g_int64_equal, NULL, free);
	container_utilization_monitor.sparse_map = g_hash_table_new_full(
			g_int64_hash, g_int64_equal, NULL, free);

	container_utilization_monitor.total_size = 0;

	inherited_sparse_containers = g_hash_table_new_full(g_int64_hash,
			g_int64_equal, NULL, free);

	/* The first backup doesn't have inherited sparse containers. */
	if (jcr.id == 0)
		return;

	sds fname = sdsdup(destor.working_directory);
	fname = sdscat(fname, "recipe/sparse");
	char s[20];
	sprintf(s, "%d", jcr.id - 1);
	fname = sdscat(fname, s);

	FILE* sparse_file = fopen(fname, "r");

	if (sparse_file) {
		struct containerRecord tmp;
		while (fscanf(sparse_file, "%ld %d", &tmp.cid, &tmp.size) != EOF) {
			struct containerRecord *record = (struct containerRecord*) malloc(
					sizeof(struct containerRecord));
			memcpy(record, &tmp, sizeof(struct containerRecord));
			g_hash_table_insert(inherited_sparse_containers, &record->cid,
					record);
		}
		fclose(sparse_file);
	}

	sdsfree(fname);
}

void har_monitor_update(containerid id, int32_t size) {
	TIMER_DECLARE(1);
	TIMER_BEGIN(1);
	struct containerRecord* record = g_hash_table_lookup(
			container_utilization_monitor.dense_map, &id);
	if (record) {
		record->size += size + CONTAINER_META_ENTRY;
	} else {
		record = g_hash_table_lookup(container_utilization_monitor.sparse_map,
				&id);
		if (!record) {
			record = (struct containerRecord*) malloc(
					sizeof(struct containerRecord));
			record->cid = id;
			record->size = CONTAINER_HEAD;
			g_hash_table_insert(container_utilization_monitor.sparse_map,
					&record->cid, record);
		}
		record->size += size;
		double usage = record->size / (double) CONTAINER_SIZE;
		if (usage > destor.rewrite_har_utilization_threshold) {
			g_hash_table_steal(container_utilization_monitor.sparse_map,
					&record->cid);
			g_hash_table_insert(container_utilization_monitor.dense_map,
					&record->cid, record);
		}
	}
	TIMER_END(1, jcr.rewrite_time);
}

void close_har() {
	sds fname = sdsdup(destor.working_directory);
	fname = sdscat(fname, "recipes/sparse");
	char s[20];
	sprintf(s, "%d", jcr.id);
	fname = sdscat(fname, s);

	FILE* fp = fopen(fname, "w");
	if (!fp) {
		fprintf(stderr, "Can not create sparse file");
		perror("The reason is");
		exit(1);
	}

	/* sparse containers */
	int inherited_sparse_num = 0;
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, container_utilization_monitor.sparse_map);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		struct containerRecord* cr = (struct containerRecord*) value;
		if (inherited_sparse_containers
				&& g_hash_table_lookup(inherited_sparse_containers, &cr->cid)) {
			inherited_sparse_num++;
		}
		fprintf(fp, "%lld %d\n", cr->cid, cr->size);
	}
	fclose(fp);

	sdsfree(fname);
}

void har_check(struct chunk* c) {
	if (!CHECK_CHUNK(c, CHUNK_FILE_START) && !CHECK_CHUNK(c, CHUNK_FILE_END)
	&& CHECK_CHUNK(c, CHUNK_DUPLICATE))
		if (g_hash_table_lookup(inherited_sparse_containers, &c->id))
			SET_CHUNK(c, CHUNK_SPARSE);
}
