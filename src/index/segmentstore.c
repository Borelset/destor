/*
 * segmentstore.c
 *
 *  Created on: Nov 24, 2013
 *      Author: fumin
 */

#include "segmentstore.h"
#include "../tools/serial.h"

#define VOLUME_HEAD 20

static pthread_mutex_t mutex;

static struct {
	FILE *fp;
	int64_t segment_num;
	int64_t current_length;
} segment_volume;

static inline int64_t id_to_offset(segmentid id) {
	int64_t off = id >> 24;
	assert(off >= 0);
	return off;
}

static inline int64_t id_to_length(segmentid id) {
	return id & (0xffffff);
}

static inline segmentid make_segment_id(int64_t offset, int64_t length) {
	return (offset << 24) + length;
}

void init_segment_management() {

	sds fname = sdsnew(destor.working_directory);
	fname = sdscat(fname, "index/segment.volume");

	int32_t flag = -1;
	if ((segment_volume.fp = fopen(fname, "r+"))) {
		/* exist */
		fread(&flag, sizeof(flag), 1, segment_volume.fp);
		assert(flag == 0xff00ff00);
		/* Invalid */
		fread(&segment_volume.segment_num, sizeof(segment_volume.segment_num),
				1, segment_volume.fp);
		fread(&segment_volume.current_length,
				sizeof(segment_volume.current_length), 1, segment_volume.fp);
		sdsfree(fname);
		return;
	}

	destor_log(DESTOR_NOTICE, "Create index/segment.volume.");

	if (!(segment_volume.fp = fopen(fname, "w+"))) {

		perror("Can not create index/segment.volume because");
		exit(1);
	}

	segment_volume.current_length = 4 + 8 + 8;
	segment_volume.segment_num = 0;

	flag = 0xff00ff00;
	fwrite(&flag, sizeof(flag), 1, segment_volume.fp);
	fwrite(&segment_volume.segment_num, sizeof(segment_volume.segment_num), 1,
			segment_volume.fp);
	fwrite(&segment_volume.current_length,
			sizeof(segment_volume.current_length), 1, segment_volume.fp);

	sdsfree(fname);

	pthread_mutex_init(&mutex, NULL);
}

void close_segment_management() {

	fseek(segment_volume.fp, 0, SEEK_SET);
	int32_t flag = 0xff00ff00;
	fwrite(&flag, sizeof(flag), 1, segment_volume.fp);
	fwrite(&segment_volume.segment_num, sizeof(segment_volume.segment_num), 1,
			segment_volume.fp);
	fwrite(&segment_volume.current_length,
			sizeof(segment_volume.current_length), 1, segment_volume.fp);
	fclose(segment_volume.fp);

	pthread_mutex_destroy(&mutex);
}

struct segmentRecipe* retrieve_segment(segmentid id) {
	if (id == TEMPORARY_ID)
		return NULL;

	int64_t offset = id_to_offset(id);
	int64_t length = id_to_length(id);

	pthread_mutex_lock(&mutex);

	char buf[length];
	fseek(segment_volume.fp, offset, SEEK_SET);
	if (fread(buf, length, 1, segment_volume.fp) != 1) {
		WARNING("Dedup phase: Prefetch an unready segment of %lld offset",
				offset);
		exit(1);
	}

	pthread_mutex_unlock(&mutex);

	VERBOSE("Dedup phase: Read similar segment of %lld offset", offset);

	struct segmentRecipe* sr = new_segment_recipe();

	unser_declare;
	unser_begin(buf, length);
	unser_bytes(&sr->id, sizeof(segmentid));
	assert(sr->id == id);

	int num, i;
	unser_int32(num);
	for (i = 0; i < num; i++) {
		fingerprint *feature = (fingerprint*) malloc(sizeof(fingerprint));
		unser_bytes(feature, sizeof(fingerprint));
		g_hash_table_insert(sr->features, feature, feature);
	}

	unser_int32(num);
	for (i = 0; i < num; i++) {
		struct indexElem* e = (struct indexElem*) malloc(
				sizeof(struct indexElem));
		unser_int64(e->id);
		unser_bytes(&e->fp, sizeof(fingerprint));
		g_hash_table_insert(sr->index, &e->fp, e);
	}

	unser_end(buf, length);

	return sr;
}

GQueue* prefetch_segments(segmentid id, int prefetch_num) {
	if (id == TEMPORARY_ID) {
		assert(id != TEMPORARY_ID);
		return NULL;
	}

	/* All prefetched segment recipes */
	GQueue *segments = g_queue_new();

	int64_t offset = id_to_offset(id);

	pthread_mutex_lock(&mutex);

	fseek(segment_volume.fp, offset, SEEK_SET);

	VERBOSE("Dedup phase: Read similar segment of %lld offset", offset);

	int j;
	for (j = 0; j < prefetch_num; j++) {
		segmentid rid;
		if (fread(&rid, sizeof(rid), 1, segment_volume.fp) != 1) {
			NOTICE("Dedup phase: no more segments can be prefetched");
			return segments;
		}
		int64_t length = id_to_length(rid);
		VERBOSE(
				"Dedup phase: Prefetch %dth segment of %lld offset and %lld length",
				j, id_to_offset(rid), length);

		char buf[length];
		if (fread(buf, length - sizeof(rid), 1, segment_volume.fp) != 1) {
			WARNING("Dedup phase: Prefetch an unready segment of %lld offset",
					id_to_offset(id));
			exit(1);
		}

		struct segmentRecipe* sr = new_segment_recipe();
		sr->id = rid;

		unser_declare;
		unser_begin(buf, length);

		int num, i;
		unser_int32(num);
		for (i = 0; i < num; i++) {
			fingerprint *feature = (fingerprint*) malloc(sizeof(fingerprint));
			unser_bytes(feature, sizeof(fingerprint));
			g_hash_table_insert(sr->features, feature, feature);
		}

		unser_int32(num);
		for (i = 0; i < num; i++) {
			struct indexElem* e = (struct indexElem*) malloc(
					sizeof(struct indexElem));
			unser_int64(e->id);
			unser_bytes(&e->fp, sizeof(fingerprint));
			g_hash_table_insert(sr->index, &e->fp, e);
		}

		unser_end(buf, length);

		g_queue_push_tail(segments, sr);
	}

	pthread_mutex_unlock(&mutex);

	return segments;
}

struct segmentRecipe* update_segment(struct segmentRecipe* sr) {
	int64_t offset = segment_volume.current_length;
	int64_t length = 8 + 4
			+ g_hash_table_size(sr->features) * sizeof(fingerprint) + 4
			+ g_hash_table_size(sr->index)
					* (sizeof(fingerprint) + sizeof(containerid));
	sr->id = make_segment_id(offset, length);

	char buf[length];
	ser_declare;
	ser_begin(buf, length);

	ser_bytes(&sr->id, sizeof(segmentid));
	int num = g_hash_table_size(sr->features);
	ser_int32(num);

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, sr->features);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		ser_bytes(key, sizeof(fingerprint));
	}

	num = g_hash_table_size(sr->index);
	ser_int32(num);

	g_hash_table_iter_init(&iter, sr->index);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		struct indexElem* e = (struct indexElem*) value;
		ser_int64(e->id);
		ser_bytes(&e->fp, sizeof(fingerprint));
	}
	ser_end(buf, length);

	pthread_mutex_lock(&mutex);

	fseek(segment_volume.fp, offset, SEEK_SET);
	fwrite(buf, length, 1, segment_volume.fp);

	pthread_mutex_unlock(&mutex);

	segment_volume.current_length += length;
	segment_volume.segment_num++;
	return sr;
}
