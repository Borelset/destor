#include "destor.h"
#include "jcr.h"
#include "rewrite_phase.h"
#include "storage/containerstore.h"
#include "backup.h"

static int stream_context_push(struct chunk *c) {

	rewrite_buffer_push(c);

	if (rewrite_buffer.num == destor.rewrite_algorithm[1])
		return 1;

	return 0;
}

static struct chunk* stream_context_pop() {
	return rewrite_buffer_pop();
}

static double get_rewrite_utility(struct chunk *c) {
	double rewrite_utility = 1;
	GSequenceIter *iter = g_sequence_lookup(rewrite_buffer.container_record_seq,
			&c->id, g_record_cmp_by_id, NULL);
	if (iter) {
		struct containerRecord *record = g_sequence_get(iter);
		double coverage = (record->size + c->size) / (double) CONTAINER_SIZE;
		rewrite_utility = coverage >= 1 ? 0 : rewrite_utility - coverage;
	}
	return rewrite_utility;
}

static void mark_not_out_of_order(struct chunk *c, containerid *container_id) {
	if (c->id == *container_id)
		UNSET_CHUNK(c, CHUNK_OUT_OF_ORDER);
}

struct {
	int32_t chunk_num;
	double current_utility_threshold;
	int min_index;
	/* [0,1/10000), [1/10000, 2/10000), ... , [9999/10000, 1] */
	int32_t buckets[10000];
} utility_buckets;

/* init utility buckets */
void init_utility_buckets() {
	utility_buckets.chunk_num = 0;
	utility_buckets.min_index =
			destor.rewrite_cbr_minimal_utility == 1 ?
					9999 : destor.rewrite_cbr_minimal_utility * 10000;
	utility_buckets.current_utility_threshold =
			destor.rewrite_cbr_minimal_utility;
	bzero(&utility_buckets.buckets, sizeof(utility_buckets.buckets));
}

static void utility_buckets_update(double rewrite_utility) {
	utility_buckets.chunk_num++;
	int index = rewrite_utility >= 1 ? 9999 : rewrite_utility * 10000;
	utility_buckets.buckets[index]++;
	if (utility_buckets.chunk_num >= 100) {
		int best_num = utility_buckets.chunk_num * destor.rewrite_cbr_limit;
		int current_index = 9999;
		int count = 0;
		for (; current_index >= utility_buckets.min_index; --current_index) {
			count += utility_buckets.buckets[current_index];
			if (count >= best_num) {
				break;
			}
		}
		utility_buckets.current_utility_threshold = (current_index + 1)
				/ 10000.0;
	}
}

/* --------------------------------------------------------------------------*/
/**
 * @Synopsis  Reducing impact of data fragmentation caused by in-line deduplication.
 *            In SYSTOR'12.
 *
 * @Param arg
 *
 * @Returns   
 */
/* ----------------------------------------------------------------------------*/
void *cbr_rewrite(void* arg) {

	init_utility_buckets();

	/* content-based rewrite*/
	while (1) {
		struct chunk *c = sync_queue_pop(dedup_queue);
		if (c == NULL)
			break;

		if (CHECK_CHUNK(c, CHUNK_DUPLICATE))
			SET_CHUNK(c, CHUNK_OUT_OF_ORDER);

		if (!stream_context_push(c))
			continue;

		struct chunk *decision_chunk = stream_context_pop();

		if (!(CHECK_CHUNK(decision_chunk, CHUNK_FILE_START)
				|| CHECK_CHUNK(decision_chunk, CHUNK_FILE_END))) {
			/* A normal chunk */
			double rewrite_utility = 0;

			if (CHECK_CHUNK(decision_chunk, CHUNK_DUPLICATE)) {
				/* a duplicate chunk */
				if (CHECK_CHUNK(decision_chunk, CHUNK_OUT_OF_ORDER)) {
					rewrite_utility = get_rewrite_utility(decision_chunk);
					if (rewrite_utility < destor.rewrite_cbr_minimal_utility
							|| rewrite_utility
									< utility_buckets.current_utility_threshold) {
						/* mark all physically adjacent chunks not out-of-order */
						UNSET_CHUNK(decision_chunk, CHUNK_OUT_OF_ORDER);
						g_queue_foreach(rewrite_buffer.chunk_queue,
								mark_not_out_of_order, &decision_chunk->id);
					}
				} else
					/* if marked as not out of order*/
					rewrite_utility = 0;
			}

			utility_buckets_update(rewrite_utility);
		}

		sync_queue_push(rewrite_queue, decision_chunk);
	}

	/* process the remaining chunks in stream context */
	struct chunk *remaining_chunk = NULL;
	while ((remaining_chunk = stream_context_pop()))
		sync_queue_push(rewrite_queue, remaining_chunk);

	sync_queue_term(rewrite_queue);

	return NULL;
}
