#include "sparse_index.h"
#include "../jcr.h"

extern char working_path[];
extern void send_feature(Chunk *chunk);

int32_t segment_bits = 10;
int32_t sample_bits = 8;
int32_t champions_number = 2;

static GHashTable *sparse_index; //Fingerprint (hook) -> manifest id sequence

static int64_t manifest_volume_length;
static int64_t manifest_number;
static int mvol_fd = -1;

static BOOL manifest_volume_init() {
	char filename[256];
	strcpy(filename, working_path);
	strcat(filename, "index/manifest_volume");

	if ((mvol_fd = open(filename, O_CREAT | O_RDWR, S_IRWXU)) < 0) {
		dprint("failed to open manifest volume");
		return FALSE;
	}

	struct stat fileinfo;
	stat(filename, &fileinfo);
	size_t filesize = fileinfo.st_size;

	if (filesize > 0) {
		read(mvol_fd, &manifest_volume_length, 8);
		read(mvol_fd, &manifest_number, 8);
	} else {
		/* First 16 bytes are length and number. */
		manifest_volume_length = 16;
		manifest_number = 0;
	}

}

static void manifest_volume_destroy() {
	lseek(mvol_fd, 0, SEEK_SET);
	write(mvol_fd, &manifest_volume_length, 8);
	write(mvol_fd, &manifest_number, 8);

	if (mvol_fd > 0)
		close(mvol_fd);
	mvol_fd = -1;
}

BOOL sparse_index_init() {
	sparse_index = g_hash_table_new_full(g_int64_hash, g_fingerprint_cmp, free,
			g_sequence_free);

	char filename[256];
	strcpy(filename, working_path);
	strcat(filename, "index/sparse_index");

	int fd;
	if ((fd = open(filename, O_CREAT | O_RDWR, S_IRWXU)) < 0) {
		dprint("failed to open sparse_index");
		return FALSE;
	}

	struct stat fileinfo;
	stat(filename, &fileinfo);
	size_t filesize = fileinfo.st_size;

	if (filesize > 0) {
		int item_num = 0;
		read(fd, &item_num, 4);
		int i = 0;
		for (; i < item_num; ++i) {
			Fingerprint *finger = (Fingerprint*) malloc(sizeof(Fingerprint));
			GSequence *sequence = g_sequence_new(free);
			read(fd, finger, sizeof(Fingerprint));
			int mnum = 0;
			read(fd, &mnum, 4);
			int j = 0;
			for (; j < mnum; ++j) {
				int64_t *id = (int64_t*) malloc(sizeof(int64_t));
				read(fd, id, 8);
				g_sequence_append(id);
			}
			g_hash_table_insert(sparse_index, finger, sequence);
		}
	}
	close(fd);

	manifest_volume_init();

	return TRUE;
}

void sparse_index_destroy() {
	char filename[256];
	strcpy(filename, working_path);
	strcat(filename, "index/sparse_index");

	int fd;
	if ((fd = open(filename, O_CREAT | O_RDWR, S_IRWXU)) < 0) {
		dprint("failed to open sparse_index");
		return;
	}

	int item_num = g_hash_table_size(sparse_index);
	write(fd, &item_num, 4);
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, sparse_index);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		write(fd, key, sizeof(Fingerprint));
		GSequence *sequence = (Gsequence*) value;
		int mnum = g_sequence_get_length(sequence);
		write(fd, &mnum, 4);
		GSequenceIter *s_iter = g_sequence_get_begin_iter(sequence);
		while (!g_sequence_iter_is_end(s_iter)) {
			int64_t *id = g_sequence_get(s_iter);
			write(fd, id, 8);
			s_iter = g_sequence_iter_next(s_iter);
		}
	}

	g_hash_table_destroy(sparse_index);

}

/* ascending */
static gint manifest_cmp(Manifest *a, Manifest *b) {
	return a->id - b->id;
}

/* descending */
static gint manifest_cmp_length(Manifest *a, Manifest* b) {
	if (g_sequence_get_length(b->matched_hooks)
			== g_sequence_get_length(a->matched_hooks)) {
		/* we prefer recent manifests */
		return b->id - a->id;
	}
	return g_sequence_get_length(b->matched_hooks)
			- g_sequence_get_length(a->matched_hooks);
}

static void unscore(Manifest *base, Manifest *dest) {
	GSequenceIter *iter = g_sequence_get_begin_iter(base->matched_hooks);
	while (!g_sequence_iter_is_end(iter)) {
		if (g_sequence_get_length(dest) == 0)
			break;
		GSequenceIter remove_iter = g_sequence_lookup(dest,
				g_sequence_get(iter), g_fingerprint_cmp, NULL );
		if (remove_iter) {
			g_sequence_remove(remove_iter);
		}
		iter = g_sequence_iter_next(iter);
	}
}

static void free_manifest(Manifest *manifest) {
	g_sequence_free(manifest->matched_hooks);
	if (manifest->fingers)
		g_hash_table_destroy(manifest->fingers);
	free(manifest);
}
/*
 * Select champions.
 * hooks is the sampled features of a segment.
 */
static GSequence* select_champions(Hooks *hooks) {
	int i = 0;
	/* manifest sequence */
	GSequence *champions = g_sequence_new(free_manifest);
	for (; i < hooks->size; ++i) {
		/* Get all IDs of manifests associated with hooks. */
		GSequence *id_seq = g_hash_table_lookup(sparse_index, &hooks[i]);
		if (id_seq == NULL )
			continue;
		GSequenceIter *id_seq_iter = g_sequence_get_begin_iter(id_seq);
		while (!g_sequence_iter_is_end(id_seq_iter)) {
			int64_t *id = g_sequence_get(id_seq_iter);
			Manifest *manifest = g_sequence_lookup(champions, id);
			if (manifest == NULL ) {
				/* Construct a new manifest */
				manifest = (Manifest*) malloc(sizeof(Manifest));
				manifest->matched_hooks = g_sequence_new(free);
				manifest->fingers = NULL;

				g_sequence_insert_sorted(champions, manifest, manifest_cmp);
			}
			Fingerprint *matched_hook = (Fingerprint*) malloc(
					sizeof(Fingerprint));
			memcpy(matched_hook, &hooks[i], sizeof(Fingerprint));
			/* insert matched hook */
			g_sequence_insert_sorted(manifest->matched_hooks, matched_hook,
					g_fingerprint_cmp, NULL );

			seq_iter = g_sequence_iter_next(id_seq_iter);
		}
	}

	if (g_sequence_get_length(champions) > champions_number) {
		/* We now select the Top champion_number manifests. */
		g_sequence_sort(champions, manifest_cmp_length, NULL );
		GSequenceIter *base = g_sequence_get_begin_iter(champions);
		int i = 0;
		while (i < champions_number) {
			GSequenceIter *next = g_sequence_iter_next(base);
			while (!g_sequence_iter_is_end(next)) {
				unscore(g_sequence_get(base), g_sequence_get(next));
				next = g_sequence_iter_next(next);
			}

			g_sequence_sort(champions, manifest_cmp_length, NULL );
			base = g_sequence_iter_next(base);
			if (g_sequence_iter_is_end(base)) {
				dprint("It can't happen!");
			}
			i++;
		}

		GSequenceIter *loser = g_sequence_get_iter_at_pos(champions,
				champions_number);
		g_sequence_remove_range(loser, g_sequence_get_end_iter(champions));
		if (g_sequence_get_length(champions) != champions_number)
			printf("%s, %d: %d != champions_number.", __FILE__, __LINE__,
					g_sequence_get_length(champions));
	}
	return champions;
}

static void load_manifest(Manifest *manifest) {
	int64_t offset = manifest->id >> 0x10;
	int32_t length = manifest->id & 0x10;
	char buffer[length];

	lseek(mvol_fd, offset, SEEK_SET);
	read(mvol_fd, buffer, length);

	char *p = buffer;
	if (manifest->id != *(int64_t*) p)
		dprint("inconsistent manifest ID!");
	p += 8;

	manifest->fingers = g_hash_table_new_full(g_int64_hash, g_fingerprint_cmp,
			free, free);
	while ((p - buffer) < length) {
		Fingerprint *fingerprint = (Fingerprint*) malloc(sizeof(Fingerprint));
		ContainerId *cid = (ContainerId*) malloc(sizeof(ContainerId));
		memcpy(fingerprint, p, sizoef(Fingerprint));
		p += sizeof(Fingerprint);
		memcpy(cid, p, sizoef(ContainerId));
		p += sizeof(ContainerId);
		g_hash_table_insert(manifest->fingers, fingerprint, cid);
	}

	if ((p - buffer) != length)
		dprintf("p - buffer != length");
}

int64_t write_manifest(Manifest *manifest) {

}

static GSequence *champions = NULL;
static Hooks *current_hooks = NULL;
static Hooks *last_hooks = NULL;
/* completely duplicate with the top champion */
static BOOL is_completely_duplicate = TRUE;

ContainerId sparse_index_search(Fingerprint *fingerprint, Hooks *hooks) {
	if (current_hooks == NULL || current_hooks->size != hooks->size
			|| memcmp(&current_hooks->hooks, &hooks->hooks,
					hooks->size * sizeof(Fingerprint)) != 0) {
		/* hooks is changed. So we need to update the champions. */
		if (champions)
			g_sequence_free(champions);
		champions = select_champions(hooks);
		GSequenceIter *champion_iter = g_sequence_get_begin_iter(champions);
		while (!g_sequence_iter_is_end(champion_iter)) {
			Manifest *manifest = g_sequence_get(champion_iter);
			load_manifest(manifest);
			champion_iter = g_sequence_iter_next(champion_iter);
		}

		if (last_hooks)
			dprint("last_hooks is not NULL!");
		last_hooks = current_hooks;
		current_hooks = (Hooks*) malloc(
				sizeof(Hooks) + sizeof(Fingerprint) * hooks->size);
		memcpy(current_hooks, hooks,
				sizeof(Hooks) + sizeof(Fingerprint) * hooks->size);
	}

	GSequenceIter *champion_iter = g_sequence_get_begin_iter(champions);
	while (!g_sequence_iter_is_end(champion_iter)) {
		if (!g_sequence_iter_is_begin(champion_iter))
			is_completely_duplicate = FALSE;
		Manifest *manifest = g_sequence_get(champion_iter);
		ContainerId *cid = g_hash_table_lookup(manifest->fingers, fingerprint);
		if (cid)
			return *cid;
		champion_iter = g_sequence_iter_next(champion_iter);
	}
	is_completely_duplicate = FALSE;

	return TMP_CONTAINER_ID;
}

static Manifest *new_manifest = NULL;

void sparse_index_update(Fingerprint *fingerprint, ContainerId container_id,
		Hooks *hooks) {
	if (last_hooks) {
		/* last_hooks is not NULL,
		 * which indicates the new_manifest is required to be written.
		 */
		int64_t manifest_id = write_manifest(new_manifest);
		free_manifest(new_manifest);
		int i = 0;
		for (; i < last_hooks->size; ++i) {
			GSequence *id_seq = g_hash_table_lookup(sparse_index,
					&last_hooks->hooks[i]);
			if (id_seq == NULL ) {
				id_seq = g_sequence_new(free);
				g_hash_table_insert(sparse_index, &last_hooks->hooks[i],
						id_seq);
			}
			int64_t *id = (int64_t*) malloc(sizeof(int4_t));
			*id = manifest_id;
			g_sequence_append(id_seq, id);
		}

		free(last_hooks);
		last_hooks = NULL;
	}

	Fingerprint *new_finger = (Fingerprint*) malloc(sizeof(Fingerprint));
	memcpy(new_finger, fingerprint, sizeof(Fingerprint));
	ContainerId *cid = (ContainerId*) malloc(sizeof(ContainerId));
	*cid = container_id;

	g_hash_table_insert(new_manifest->fingers, new_finger, cid);
}

void* sparse_prepare(void* arg) {
	Jcr *jcr = (Jcr*) arg;
	Recipe *processing_recipe = 0;
	Queue *segment = queue_new();
	Queue *current_hooks = queue_new();

	int32_t segment_mask = 1 << segment_bits - 1;
	int32_t hook_mask = 1 << sample_bits - 1;

	while (TRUE) {
		Chunk *chunk = NULL;
		int signal = recv_hash(&chunk);
		if (signal == STREAM_END) {
			send_feature(chunk);
			break;
		}
		if (processing_recipe == 0) {
			processing_recipe = sync_queue_pop(jcr->waiting_files_queue);
			puts(processing_recipe->filename);
		}
		if (signal == FILE_END) {
			/* TO-DO */
			close(processing_recipe->fd);
			sync_queue_push(jcr->completed_files_queue, processing_recipe);
			processing_recipe = 0;
			free_chunk(chunk);
			/* FILE notion is meaningless for following threads */
			continue;
		}
		/* TO-DO */
		if ((chunk->hash & segment_mask) == 0/*segment boundary is found*/) {
			int cnt = queue_size(current_hooks);
			Hooks *hooks = (Hooks*) malloc(
					sizeof(Hooks) + cnt * sizeof(Fingerprint));
			hooks->size = 0;
			Fingerprint *buffered_hook = queue_pop(current_hooks);
			while (buffered_hook) {
				memcpy(&hooks->hooks[hooks->size], buffered_hook,
						sizeof(Fingerprint));
				hooks->size++;
				free(buffered_hook);
				buffered_hook = queue_pop(current_hooks);
			}

			/* segment is full, push it */
			Chunk *buffered_chunk = queue_pop(segment);
			while (buffered_chunk) {
				Hooks *new_hooks = (Hooks*) malloc(
						sizeof(Hooks) + cnt * sizeof(Fingerprint));
				memcpy(new_hooks, hooks,
						sizeof(Hooks) + cnt * sizeof(Fingerprint));
				buffered_chunk->feature = new_hooks;

				send_feature(buffered_chunk);
				buffered_chunk = queue_pop(buffered_chunk_queue);
			}

			free(hooks);
		}

		if ((chunk->hash & hook_mask) == 0) {
			/* sample */
			Fingerprint *hook = (Fingerprint*) malloc(sizeof(Fingerprint));
			memcpy(hook, &chunk->hash, sizeof(Fingerprint));
			queue_push(current_hooks, hook);
		}

		processing_recipe->chunknum++;
		processing_recipe->filesize += chunk->length;

		queue_push(segment, chunk);
	}
	return NULL ;
}
