#include "destor.h"
#include "jcr.h"
#include "tools/rabin_chunking.h"
#include "backup.h"

static pthread_t chunk_t;

static void* chunk_thread(void *arg) {
	int leftlen = 0;
	int leftoff = 0;
	unsigned char leftbuf[DEFAULT_BLOCK_SIZE + destor.chunk_max_size];

	char zeros[destor.chunk_max_size];
	bzero(zeros, destor.chunk_max_size);
	unsigned char data[destor.chunk_max_size];

	struct chunk* c = NULL;

	while (1) {

		/* Try to receive a CHUNK_FILE_START. */
		c = sync_queue_pop(read_queue);

		if (c == NULL) {
			sync_queue_term(chunk_queue);
			break;
		}

		assert(CHECK_CHUNK(c, CHUNK_FILE_START));
		sync_queue_push(chunk_queue, c);

		/* Try to receive normal chunks. */
		c = sync_queue_pop(read_queue);
		if (!CHECK_CHUNK(c, CHUNK_FILE_END)) {
			memcpy(leftbuf, c->data, c->size);
			leftlen += c->size;
			free_chunk(c);
		}

		while (leftlen > 0) {
			if ((leftlen < destor.chunk_max_size)
					&& !CHECK_CHUNK(c, CHUNK_FILE_END)) {
				c = sync_queue_pop(read_queue);
				memmove(leftbuf, leftbuf + leftoff, leftlen);
				leftoff = 0;
				memcpy(leftbuf + leftlen, c->data, c->size);
				leftlen += c->size;
				free_chunk(c);
			}

			TIMER_DECLARE(b, e);
			TIMER_BEGIN(b);

			int chunk_size = 0;
			if (destor.chunk_algorithm == CHUNK_RABIN
					|| destor.chunk_algorithm == CHUNK_NORMALIZED_RABIN)
				chunk_size = rabin_chunk_data(leftbuf + leftoff, leftlen);
			else
				chunk_size =
						destor.chunk_avg_size > leftlen ?
								leftlen : destor.chunk_avg_size;

			TIMER_END(jcr.chunk_time, b, e);

			struct chunk *nc = new_chunk(chunk_size);
			memcpy(nc->data, leftbuf + leftoff, chunk_size);
			leftlen -= chunk_size;
			leftoff += chunk_size;

			if (memcmp(zeros, nc->data, chunk_size) == 0) {
				jcr.zero_chunk_num++;
				jcr.zero_chunk_size += chunk_size;
			}

			sync_queue_push(chunk_queue, nc);
		}

		sync_queue_push(chunk_queue, c);
		leftoff = 0;
		windows_reset();
		c = NULL;
	}

	return NULL;
}

void start_chunk_phase() {
	chunkAlg_init();
	chunk_queue = sync_queue_new(100);
	pthread_create(&chunk_t, NULL, chunk_thread, NULL);
}

void stop_chunk_phase() {
	pthread_join(chunk_t, NULL);
}
