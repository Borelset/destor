/*
 * In the phase,
 * we mark the chunks required to be rewriting.
 */
#include "destor.h"
#include "jcr.h"
#include "rewrite_phase.h"
#include "backup.h"

static pthread_t rewrite_t;

/* Descending order */
gint g_record_descmp_by_length(struct containerRecord* a,
		struct containerRecord* b, gpointer user_data) {
	return b->size - a->size;
}

gint g_record_cmp_by_id(struct containerRecord* a, struct containerRecord* b,
		gpointer user_data) {
	return a->cid - b->cid;
}

static void init_rewrite_buffer() {
	rewrite_buffer.chunk_queue = g_queue_new();
	rewrite_buffer.container_record_seq = g_sequence_new(free);
	rewrite_buffer.num = 0;
}

void rewrite_buffer_push(struct chunk* c) {
	g_queue_push_tail(rewrite_buffer.chunk_queue, c);

	if (CHECK_CHUNK(c, CHUNK_FILE_START) || CHECK_CHUNK(c, CHUNK_FILE_END))
		return;

	if (CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
		struct containerRecord tmp_record;
		tmp_record.cid = c->id;
		GSequenceIter *iter = g_sequence_lookup(
				rewrite_buffer.container_record_seq, &tmp_record,
				g_record_cmp_by_id,
				NULL);
		if (iter == NULL) {
			struct containerRecord* record = malloc(
					sizeof(struct containerRecord));
			record->cid = c->id;
			record->size = c->size;
			g_sequence_insert_sorted(rewrite_buffer.container_record_seq,
					record, g_record_cmp_by_id, NULL);
		} else {
			struct containerRecord* record = g_sequence_get(iter);
			assert(record->cid == c->id);
			record->size += c->size;
		}
	}

	rewrite_buffer.num++;
}

struct chunk* rewrite_buffer_pop() {
	struct chunk* c = g_queue_pop_tail(rewrite_buffer.chunk_queue);

	if (!CHECK_CHUNK(c, CHUNK_FILE_START) && !CHECK_CHUNK(c, CHUNK_FILE_END)) {
		/* A normal chunk */
		if (CHECK_CHUNK(c, CHUNK_DUPLICATE)) {
			GSequenceIter *iter = g_sequence_lookup(
					rewrite_buffer.container_record_seq, &c->id,
					g_record_cmp_by_id, NULL);
			assert(iter);
			struct containerRecord* record = g_sequence_get(iter);
			record->size -= c->size;
			if (record->size == 0)
				g_sequence_remove(iter);
		}
		rewrite_buffer.num--;
	}

	return c;
}

/*
 * If rewrite is disable.
 */
static void* no_rewrite(void* arg) {
	while (1) {
		struct chunk* c = sync_queue_pop(dedup_queue);

		if (c == NULL)
			break;

		sync_queue_push(rewrite_queue, c);

	}

	sync_queue_term(rewrite_queue);

	return NULL;
}

void start_rewrite_phase() {
	rewrite_queue = sync_queue_new(1000);
	init_rewrite_buffer();

	init_har();

	init_restore_aware();

	if (destor.rewrite_algorithm[0] == REWRITE_NO) {
		pthread_create(&rewrite_t, NULL, no_rewrite, NULL);
	} else if (destor.rewrite_algorithm[0]
			== REWRITE_CFL_SELECTIVE_DEDUPLICATION) {
		pthread_create(&rewrite_t, NULL, cfl_rewrite, NULL);
	} else if (destor.rewrite_algorithm[0] == REWRITE_CONTEXT_BASED) {
		pthread_create(&rewrite_t, NULL, cbr_rewrite, NULL);
	} else if (destor.rewrite_algorithm[0] == REWRITE_CAPPING) {
		pthread_create(&rewrite_t, NULL, cap_rewrite, NULL);
	} else {
		fprintf(stderr, "Invalid rewrite algorithm\n");
		exit(1);
	}

}

void stop_rewrite_phase() {
	close_har();
	pthread_join(rewrite_t, NULL);
}
