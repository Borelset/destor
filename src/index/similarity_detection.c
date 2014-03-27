/*
 * similarity_detection.c
 *
 *  Created on: Mar 25, 2014
 *      Author: fumin
 */
#include "index.h"
#include "kvstore.h"
#include "fingerprint_cache.h"
#include "segmentstore.h"

/* defined in index.c */
extern struct {
	/* map a fingerprint to a queue of indexElem */
	/* Index all fingerprints in the index buffer. */
	GHashTable *buffered_fingerprints;
	/* The number of buffered chunks */
	int num;
} index_buffer;

/*
 * Larger one comes before smaller one.
 * Descending order.
 */
static gint g_segment_cmp_feature_num(struct segmentRecipe* a,
		struct segmentRecipe* b, gpointer user_data) {
	gint ret = g_hash_table_size(b->features) - g_hash_table_size(a->features);
	if (ret == 0) {
		ret = b->id > a->id ? 1 : -1;
		return ret;
	} else
		return ret;
}

/*
 * Remove the features that are common with top from target.
 */
static void features_trim(struct segmentRecipe *target,
		struct segmentRecipe *top) {
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, top->features);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		g_hash_table_remove(target->features, key);
	}
}

/*
 * Select the top segments that are most similar with features.
 * (top-k * prefetching_num) cannot be larger than the segment cache size.
 */
static void top_segment_select(GHashTable* features) {
	/*
	 * Mapping segment IDs to similar segments that hold at least one of features.
	 */
	GHashTable *similar_segments = g_hash_table_new_full(g_int64_hash, g_int64_equal, NULL,
			free_segment_recipe);

	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, features);
	/* Iterate the features of the segment. */
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		/* Each feature is mapped to several segment IDs. */
		segmentid *ids = kvstore_lookup((fingerprint*) key);
		if (ids) {
			int i;
			for (i = 0; i < destor.index_value_length; i++) {
				if (ids[i] == TEMPORARY_ID)
					break;
				struct segmentRecipe* sr = g_hash_table_lookup(similar_segments, &ids[i]);
				if (!sr) {
					sr = new_segment_recipe();
					sr->id = ids[i];
					g_hash_table_insert(similar_segments, &sr->id, sr);
				}
				fingerprint *feature = (fingerprint*) malloc(
						sizeof(fingerprint));
				memcpy(feature, key, sizeof(fingerprint));
				assert(!g_hash_table_contains(sr->features, feature));
				g_hash_table_insert(sr->features, feature, NULL);
			}
		}
	}

	if (g_hash_table_size(similar_segments) != 0) {

		/* Sorting similar segments in order of their number of hit features. */
		GSequence *seq = g_sequence_new(NULL);
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, similar_segments);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			/* Insert similar segments into GSequence. */
			struct segmentRecipe* sr = value;
			NOTICE("candidate segment %ld with %d shared features", sr->id >> 24,
					g_hash_table_size(sr->features));
			g_sequence_insert_sorted(seq, (struct segmentRecipe*) value,
					g_segment_cmp_feature_num, NULL);
		}

		/* The number of selected similar segments */
		int num = g_sequence_get_length(seq)
						> destor.index_segment_selection_method[1] ?
						destor.index_segment_selection_method[1] :
						g_sequence_get_length(seq), i;

		NOTICE("select Top-%d in %d segments", num, g_sequence_get_length(seq));

		/* Prefetched top similar segments are pushed into the queue. */
		for (i = 0; i < num; i++) {
			/* Get the top segment */
			struct segmentRecipe *top = g_sequence_get(
					g_sequence_get_begin_iter(seq));
			NOTICE("read segment %ld", top->id >> 24);

			fingerprint_cache_prefetch(top->id);

			g_sequence_remove(g_sequence_get_begin_iter(seq));
			g_sequence_foreach(seq, features_trim, top);
			g_sequence_sort(seq, g_segment_cmp_feature_num, NULL);
		}
		g_sequence_free(seq);

	}

	g_hash_table_destroy(similar_segments);
}

extern struct{
	/* accessed in dedup phase */
	struct container *container_buffer;
	/* In order to facilitate sampling in container,
	 * we keep a queue for chunks in container buffer. */
	GQueue *chunks;
} storage_buffer;

void index_lookup_similarity_detection(struct segment *s){
	assert(s->features);
	top_segment_select(s->features);

	int len = g_queue_get_length(s->chunks), i;
	for (i = 0; i < len; ++i) {
		struct chunk* c = g_queue_peek_nth(s->chunks, i);

		if (CHECK_CHUNK(c, CHUNK_FILE_START) || CHECK_CHUNK(c, CHUNK_FILE_END))
			continue;

		/* First check it in the storage buffer */
		if(storage_buffer.container_buffer
				&& lookup_fingerprint_in_container(storage_buffer.container_buffer, &c->fp)){
			c->id = get_container_id(storage_buffer.container_buffer);
			SET_CHUNK(c, CHUNK_DUPLICATE);
		}
		/*
		 * First check the buffered fingerprints,
		 * recently backup fingerprints.
		 */
		GQueue *tq = g_hash_table_lookup(index_buffer.buffered_fingerprints, &c->fp);
		if (!tq) {
			tq = g_queue_new();
		} else if (!CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
			struct indexElem *be = g_queue_peek_head(tq);
			c->id = be->id;
			SET_CHUNK(c, CHUNK_DUPLICATE);
		}

		/* Check the fingerprint cache */
		if (!CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
			/* Searching in fingerprint cache */
			int64_t id = fingerprint_cache_lookup(&c->fp);
			if(id != TEMPORARY_ID){
				c->id = id;
				SET_CHUNK(c, CHUNK_DUPLICATE);
			}
		}

		/* Insert it into the index buffer */
		struct indexElem *ne = (struct indexElem*) malloc(
				sizeof(struct indexElem));
		ne->id = c->id;
		memcpy(&ne->fp, &c->fp, sizeof(fingerprint));

		g_queue_push_tail(tq, ne);
		g_hash_table_replace(index_buffer.buffered_fingerprints, &ne->fp, tq);

		index_buffer.num++;
	}

}
