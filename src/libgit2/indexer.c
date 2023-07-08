/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include <sys/mman.h>

#include "indexer.h"
#include "pack.h"
#include "packfile_parser.h"
#include "repository.h"
#include "sizemap.h"
#include "delta.h"

#include "git2_util.h"

#include "git2/indexer.h"

/* TODO: tweak? also align with packfile_parser.c */
#define READ_CHUNK_SIZE (1024 * 256)

size_t git_indexer__max_objects = UINT32_MAX;

struct object_entry {
	git_object_t type;
	git_object_size_t position;
	/* TODO: this can be unsigned short */
	git_object_size_t header_size;
	git_object_size_t size;
	uint32_t crc32;
	git_oid id;
};

struct delta_entry {
	struct object_entry object;
	git_object_t final_type;
	union {
		git_oid ref_id;
		git_object_size_t ofs_position;
	} base;
};

struct object_data {
	/* TODO: can we shrink this to fit both in 64 bits */
	size_t len;
	git_object_t type;
	unsigned char data[GIT_FLEX_ARRAY];
};

struct git_indexer {
	git_odb *odb;
	git_oid_t oid_type;

	/* TODO: verify! connectivity checks! */
	unsigned int do_fsync:1,
	             do_verify:1;
	unsigned int mode;

	git_indexer_progress_cb progress_cb;
	void *progress_payload;

	git_str packfile_path;
	int packfile_fd;
	unsigned long long packfile_size;

	git_packfile_parser parser;

	uint32_t entries;
	unsigned int started : 1,
	             complete : 1;

	/* Current object / delta being parsed */
	/* TODO: pul these directly from the parser instead? */
	git_object_size_t current_position;
	git_object_t current_type;
	git_object_size_t current_header_size;
	git_object_size_t current_size;
	git_oid current_ref; /* current ref delta base */
	git_object_size_t current_offset; /* current ofs delta base */

	struct object_entry *current_object;
	struct delta_entry *current_delta;

	git_hash_ctx hash_ctx;
	git_zstream zstream;
	unsigned char *packfile_map;

	git_sizemap *positions; /* map of position to object */
	git_vector objects; /* vector of `struct object_entry` */
	git_vector deltas;  /* vector of `struct delta_entry` */

	git_sizemap *basecache; /* map of position to entry data */

	unsigned char packfile_trailer[GIT_HASH_MAX_SIZE];

	git_oid trailer_oid;
	char name[(GIT_HASH_MAX_SIZE * 2) + 1];

	size_t object_lookups;
	size_t cache_hits;

	git_indexer_progress progress;

	uint64_t index_start;
	uint64_t index_end;
	uint64_t delta_start;
	uint64_t delta_end;
};

int git_indexer_options_init(
	git_indexer_options *opts,
	unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(opts,
		version, git_indexer_options, GIT_INDEXER_OPTIONS_INIT);

	return 0;
}

static int do_progress_cb(git_indexer *indexer)
{
	if (!indexer->progress_cb)
		return 0;

	return git_error_set_after_callback_function(
		indexer->progress_cb(&indexer->progress, indexer->progress_payload),
		"indexer progress");
}

static int objects_cmp(const void *a, const void *b)
{
	const struct object_entry *entry_a = a;
	const struct object_entry *entry_b = b;

	return git_oid__cmp(&entry_a->id, &entry_b->id);
}

static int deltas_cmp(const void *a, const void *b)
{
	const struct delta_entry *entry_a = a;
	const struct delta_entry *entry_b = b;

	if (entry_a->object.type == GIT_OBJECT_OFS_DELTA &&
	    entry_b->object.type == GIT_OBJECT_OFS_DELTA) {
		if (entry_a->base.ofs_position < entry_b->base.ofs_position)
			return -1;
		else if (entry_a->base.ofs_position > entry_b->base.ofs_position)
			return 1;
		else
			return 0;
	} else if (entry_a->object.type == GIT_OBJECT_REF_DELTA &&
	           entry_b->object.type == GIT_OBJECT_REF_DELTA) {
		return git_oid_cmp(&entry_a->base.ref_id, &entry_b->base.ref_id);
	} else {
		return (entry_a->object.type == GIT_OBJECT_OFS_DELTA) ?  -1 : 1;
	}
}

static int parse_packfile_header(
	uint32_t version,
	uint32_t entries,
	void *idx)
{
	git_indexer *indexer = (git_indexer *)idx;

	GIT_UNUSED(version);

	if (indexer->started) {
		git_error_set(GIT_ERROR_INDEXER, "unexpected packfile header");
		return -1;
	}

/*	printf("--> header cb: %d %d\n", (int)version, (int)entries);*/

	/* TODO: is 50% a good number? not convinced. */
	if (git_sizemap_new(&indexer->positions) < 0 ||
	    git_sizemap_new(&indexer->basecache) < 0 ||
	    git_vector_init(&indexer->objects, entries, objects_cmp) < 0 ||
	    git_vector_init(&indexer->deltas, entries / 2, deltas_cmp) < 0)
		return -1;

	indexer->started = 1;
	indexer->entries = entries;

	indexer->progress.total_objects = entries;

	return 0;
}

static int parse_object_start(
	git_object_size_t position,
	git_object_size_t header_size,
	git_object_t type,
	git_object_size_t size,
	void *idx)
{
	git_indexer *indexer = (git_indexer *)idx;
	struct object_entry *entry;

	/* TODO: pool? */
	entry = git__malloc(sizeof(struct object_entry));
	GIT_ERROR_CHECK_ALLOC(entry);

	entry->type = type;
	entry->position = position;
	entry->header_size = header_size;
	entry->size = size;

	indexer->current_object = entry;

	return 0;
}

static int parse_object_complete(
	git_object_size_t compressed_size,
	uint32_t compressed_crc,
	git_oid *oid,
	void *idx)
{
	git_indexer *indexer = (git_indexer *)idx;
	struct object_entry *entry = indexer->current_object;

	GIT_UNUSED(compressed_size);

	/* TODO: we can use compressed_size if we mmap */

	git_oid_cpy(&entry->id, oid);
	entry->crc32 = compressed_crc;

	if (git_sizemap_set(indexer->positions, entry->position, entry) < 0 ||
	    git_vector_insert(&indexer->objects, entry) < 0)
		return -1;

	indexer->current_object = NULL;

	indexer->progress.received_objects++;
	indexer->progress.indexed_objects++;

	return do_progress_cb(indexer);
}

static int parse_delta_start(
	git_object_size_t position,
	git_object_t type,
	git_object_size_t header_size,
	git_object_size_t size,
	git_oid *delta_ref,
	git_object_size_t delta_offset,
	void *idx)
{
	git_indexer *indexer = (git_indexer *)idx;
	struct delta_entry *entry;

	/* TODO: pool? */
	entry = git__malloc(sizeof(struct delta_entry));
	GIT_ERROR_CHECK_ALLOC(entry);

	/* TODO: avoid double copy - preallocate the entry */
	entry->object.position = position;
	entry->object.type = type;
	entry->object.header_size = header_size;
	entry->object.size = size;

	if (type == GIT_OBJECT_REF_DELTA) {
		git_oid_cpy(&entry->base.ref_id, delta_ref);
	} else {
		if (delta_offset > position) {
			git_error_set(GIT_ERROR_INDEXER, "invalid delta offset (base would be negative)");
			return -1;
		}

		entry->base.ofs_position = position - delta_offset;
	}

	indexer->current_delta = entry;

	return 0;
}

static int parse_delta_data(
	void *delta_data,
	size_t delta_data_len,
	void *idx)
{
	git_indexer *indexer = (git_indexer *)idx;

	GIT_UNUSED(delta_data);
	GIT_UNUSED(delta_data_len);
	GIT_UNUSED(indexer);

	return 0;
}

static int parse_delta_complete(
	git_object_size_t compressed_size,
	uint32_t compressed_crc,
	void *idx)
{
	git_indexer *indexer = (git_indexer *)idx;
	struct delta_entry *entry = indexer->current_delta;

	GIT_UNUSED(compressed_size);

	entry->object.crc32 = compressed_crc;
	entry->final_type = 0;

	if (git_sizemap_set(indexer->positions, entry->object.position, entry) < 0 ||
	    git_vector_insert(&indexer->objects, entry) < 0 ||
	    git_vector_insert(&indexer->deltas, entry) < 0)
		return -1;

	indexer->current_delta = NULL;

	indexer->progress.received_objects++;

	return do_progress_cb(indexer);
}

static int parse_packfile_complete(
	const unsigned char *checksum,
	size_t checksum_len,
	void *idx)
{
	git_indexer *indexer = (git_indexer *)idx;

	GIT_ASSERT(checksum_len == git_oid_size(indexer->oid_type));

	memcpy(indexer->packfile_trailer, checksum, checksum_len);

	indexer->complete = 1;
	return 0;
}

static int indexer_new(
	git_indexer **out,
	const char *parent_path,
	git_oid_t oid_type,
	unsigned int mode,
	git_odb *odb,
	git_indexer_options *in_opts)
{
	git_indexer_options opts = GIT_INDEXER_OPTIONS_INIT;
	git_indexer *indexer;
	git_str path = GIT_STR_INIT;
	git_hash_algorithm_t hash_type;
	int error;

	if (in_opts)
		memcpy(&opts, in_opts, sizeof(opts));

	indexer = git__calloc(1, sizeof(git_indexer));
	GIT_ERROR_CHECK_ALLOC(indexer);

	indexer->oid_type = oid_type;
	indexer->odb = odb;
	indexer->progress_cb = opts.progress_cb;
	indexer->progress_payload = opts.progress_cb_payload;
	indexer->mode = mode ? mode : GIT_PACK_FILE_MODE;
	indexer->do_verify = opts.verify;

	if (git_repository__fsync_gitdir)
		indexer->do_fsync = 1;

	hash_type = git_oid_algorithm(oid_type);

	if ((error = git_packfile_parser_init(&indexer->parser, oid_type)) < 0 ||
	    (error = git_hash_ctx_init(&indexer->hash_ctx, hash_type)) < 0 ||
		(error = git_zstream_init(&indexer->zstream, GIT_ZSTREAM_INFLATE)) < 0 ||
	    (error = git_str_joinpath(&path, parent_path, "pack")) < 0 ||
	    (error = indexer->packfile_fd = git_futils_mktmp(&indexer->packfile_path, path.ptr, indexer->mode)) < 0)
		goto done;

	indexer->parser.packfile_header = parse_packfile_header;
	indexer->parser.object_start = parse_object_start;
	indexer->parser.object_complete = parse_object_complete;
	indexer->parser.delta_start = parse_delta_start;
	indexer->parser.delta_data = parse_delta_data;
	indexer->parser.delta_complete = parse_delta_complete;
	indexer->parser.packfile_complete = parse_packfile_complete;
	indexer->parser.callback_data = indexer;

done:
	git_str_dispose(&path);

	if (error < 0) {
		git__free(indexer);
		return -1;
	}

	*out = indexer;
	return 0;
}

#ifdef GIT_EXPERIMENTAL_SHA256
int git_indexer_new(
	git_indexer **out,
	const char *path,
	git_oid_t oid_type,
	git_indexer_options *opts)
{
	unsigned int mode = opts ? opts->mode : 0;
	git_odb *odb = opts ? opts->odb : NULL;

	return indexer_new(out, path, oid_type, mode, odb, opts);
}
#else
int git_indexer_new(
	git_indexer **out,
	const char *path,
	unsigned int mode,
	git_odb *odb,
	git_indexer_options *opts)
{
	return indexer_new(out, path, GIT_OID_SHA1, mode, odb, opts);
}
#endif

void git_indexer__set_fsync(git_indexer *indexer, int do_fsync)
{
	indexer->do_fsync = !!do_fsync;
}

const char *git_indexer_name(const git_indexer *indexer)
{
	return indexer->name;
}

#ifndef GIT_DEPRECATE_HARD
const git_oid *git_indexer_hash(const git_indexer *indexer)
{
	return &indexer->trailer_oid;
}
#endif

static int append_data(
	git_indexer *indexer,
	const void *data,
	size_t len)
{
	size_t chunk_len;

	while (len > 0) {
		chunk_len = min(len, SSIZE_MAX);

		if ((p_write(indexer->packfile_fd, data, chunk_len)) < 0)
			return -1;

		data += chunk_len;
		len -= chunk_len;

		indexer->packfile_size += chunk_len;
		indexer->progress.received_bytes += chunk_len;
	}

	return 0;
}

int git_indexer_append(
	git_indexer *indexer,
	const void *data,
	size_t len,
	git_indexer_progress *stats)
{
	GIT_ASSERT_ARG(indexer && (!len || data));

	GIT_UNUSED(stats);

	if (!indexer->index_start)
		indexer->index_start = git_time_monotonic();


	/*
	 * Take two passes with the data given to us: first, actually do the
	 * appending to the packfile. Next, do whatever parsing we can.
	 */

	if (append_data(indexer, data, len) < 0)
		return -1;

	if (git_packfile_parser_parse(&indexer->parser, data, len) < 0)
		return -1;

	if (stats)
		memcpy(stats, &indexer->progress, sizeof(git_indexer_progress));

	return 0;
}

/* TODO: this should live somewhere else -- maybe in packfile parser? */
static int load_raw_object(
	struct object_data **out,
	git_indexer *indexer,
	struct object_entry *object)
{
	struct object_data *data;
	size_t data_remain, raw_position;
	unsigned char *compressed_ptr, *data_ptr;

	/* TODO: overflow checking */
	raw_position = object->position + object->header_size;

	/* TODO: overflow checking */
	data = git__malloc(sizeof(struct object_data) + object->size + 1);
	GIT_ERROR_CHECK_ALLOC(data);

	data->len = object->size;
	data->type = object->type;

	data_ptr = data->data;
	data_remain = object->size;

	/* TODO: we need to be more thoughtful about file descriptors for multithreaded unpacking */
	compressed_ptr = indexer->packfile_map + raw_position;

	/* TODO: more thoughtful about this for multiple threads, too */
	git_zstream_reset(&indexer->zstream);

/* TODO: we know where the compressed data ends based on the next offset */
	if (git_zstream_set_input(&indexer->zstream, compressed_ptr, (indexer->packfile_size - raw_position)) < 0)
		return -1;

	while (data_remain && !git_zstream_eos(&indexer->zstream)) {
		size_t data_written = data_remain;

	    if (git_zstream_get_output(data_ptr, &data_written, &indexer->zstream) < 0)
			return -1;

		data_ptr += data_remain;
		data_remain -= data_written;
	}

	if (data_remain > 0 || !git_zstream_eos(&indexer->zstream)) {
		git_error_set(GIT_ERROR_INDEXER, "object data did not match expected size");
		return -1;
	}

	/* TODO - sanity check type */
	*out = data;

	return 0;
}

static int load_resolved_object(
	struct object_data **out,
	git_indexer *indexer,
	struct object_entry *object,
	struct object_entry *base);

GIT_INLINE(int) load_resolved_ofs_object(
	struct object_data **out,
	git_indexer *indexer,
	struct object_entry *_delta,
	struct object_entry *base)
{
	struct delta_entry *delta = (struct delta_entry *)_delta;
	struct object_data *base_data, *delta_data, *result_data;
	size_t base_size, result_size;

	/* load the base */
	if (!base)
		base = git_sizemap_get(indexer->positions, delta->base.ofs_position);

	if (!base) {
		git_error_set(GIT_ERROR_INDEXER, "corrupt packfile - no object at offset position %llu", delta->base.ofs_position);
		return -1;
	}

	if (load_resolved_object(&base_data, indexer, base, NULL) < 0 ||
	    load_raw_object(&delta_data, indexer, _delta) < 0 ||
		git_delta_read_header(&base_size, &result_size,
			delta_data->data, delta_data->len) < 0)
		return -1;

	/* TODO: overflow check */
	result_data = git__malloc(sizeof(struct object_data) + result_size + 1);
	result_data->data[result_size] = '\0';
	result_data->len = result_size;
	result_data->type = base_data->type;

	if (git_delta_apply_to_buf(result_data->data, result_data->len,
			base_data->data, base_data->len,
			delta_data->data, delta_data->len) < 0) {
		git__free(result_data);
		return -1;
	}

	*out = result_data;

	return 0;
}

static int load_resolved_object(
	struct object_data **out,
	git_indexer *indexer,
	struct object_entry *object,
	struct object_entry *base)
{
	struct object_data *data;

	/* cache lookup */

	if (object->type == GIT_OBJECT_REF_DELTA) {
		abort();
	} else if (object->type == GIT_OBJECT_OFS_DELTA) {
		if (load_resolved_ofs_object(&data, indexer, object, base) < 0)
			return -1;
	} else {
		if (load_raw_object(&data, indexer, object) < 0)
			return -1;
	}

	/* cache set */

	*out = data;
	return 0;
}

GIT_INLINE(int) resolve_delta(
	git_indexer *indexer,
	struct delta_entry *delta,
	struct object_entry *base)
{
	struct object_data *result;
	char header[64];
	size_t header_len;

	if (load_resolved_object(&result, indexer,
			(struct object_entry *)delta, base) < 0)
		return -1;

	/* TODO: hash ctx per thread */
	if (git_hash_init(&indexer->hash_ctx) < 0 ||
	    git_odb__format_object_header(&header_len, header, sizeof(header), result->len, result->type) < 0 ||
	    git_hash_update(&indexer->hash_ctx, header, header_len) < 0 ||
	    git_hash_update(&indexer->hash_ctx, result->data, result->len) < 0 ||
	    git_hash_final(delta->object.id.id, &indexer->hash_ctx) < 0)
		return -1;

	delta->final_type = result->type;

#ifdef GIT_EXPERIMENTAL_SHA256
	delta->object.id.type = indexer->oid_type;
#endif

	indexer->progress.indexed_deltas++;
	indexer->progress.indexed_objects++;

	return do_progress_cb(indexer);
}

static int resolve_deltas(git_indexer *indexer)
{
	struct object_entry *object_entry;
	struct delta_entry *delta_entry;
	size_t object_idx, delta_idx = 0;

	git_vector_sort(&indexer->deltas);

	/*
	 * At this point, our deltas are sorted. If this object is a base
	 * for a delta, then it will be the next in the delta list. Apply
	 * it to the current delta and any subsequent with the same base.
	 * Otherwise, continue.
	 */
	git_vector_foreach(&indexer->objects, object_idx, object_entry) {
		while (delta_idx < git_vector_length(&indexer->deltas)) {
			delta_entry = git_vector_get(&indexer->deltas, delta_idx);


			/* TODO */
			if (delta_entry->object.type != GIT_OBJECT_OFS_DELTA)
				abort();

			GIT_ASSERT(delta_entry->base.ofs_position >= object_entry->position);

			if (delta_entry->base.ofs_position > object_entry->position)
				break;

			if (resolve_delta(indexer, delta_entry, object_entry) < 0)
				return -1;

			delta_idx++;
		}
	}

	return 0;
}

GIT_INLINE(int) hash_and_write(
	git_indexer *indexer,
	int fd,
	const void *data,
	size_t len)
{
	if (p_write(fd, data, len) < 0 ||
	    git_hash_update(&indexer->hash_ctx, data, len) < 0)
		return -1;

	return 0;
}

static int write_index(git_indexer *indexer)
{
	git_str path = GIT_STR_INIT;
	struct object_entry *entry;
	uint8_t fanout = 0;
	uint32_t fanout_count = 0, nl, long_offset = 0;
	uint64_t nll;
	size_t oid_size, i = 0;
	unsigned char index_trailer[GIT_HASH_MAX_SIZE];
	int fd = -1;

	/* TODO: configurable file mode */
	if (git_str_join(&path, '.', indexer->packfile_path.ptr, "idx") < 0 ||
	    (fd = p_open(path.ptr, O_RDWR|O_CREAT, 0666)) < 0 ||
		git_hash_init(&indexer->hash_ctx) < 0 ||
	    hash_and_write(indexer, fd, "\377tOc\000\000\000\002", 8) < 0)
		goto on_error;

	/* Write fanout section */
	do {
		while (i < git_vector_length(&indexer->objects) &&
		       (entry = indexer->objects.contents[i]) &&
			   entry->id.id[0] == fanout) {
			/* TODO; overflow checking? */
			/* we don't really need it here if we clamp the number of objects to uint32 elsewhere -- and we should. */
			fanout_count++;
			i++;
		}

		nl = htonl(fanout_count);

		if (hash_and_write(indexer, fd, &nl, 4) < 0)
			goto on_error;
	} while (fanout++ < 0xff);

	/* Write object IDs */
	oid_size = git_oid_size(indexer->oid_type);
	git_vector_foreach(&indexer->objects, i, entry) {
		if (hash_and_write(indexer, fd, entry->id.id, oid_size) < 0)
			goto on_error;
	}

	/* Write the CRC32s */
	git_vector_foreach(&indexer->objects, i, entry) {
		nl = htonl(entry->crc32);

		if (hash_and_write(indexer, fd, &nl, sizeof(uint32_t)) < 0)
			goto on_error;
	}

	/* Small (31-bit) offsets */
	git_vector_foreach(&indexer->objects, i, entry) {
		if (entry->position >= 0x40000000000) {
			long_offset++;

			nl = htonl(0x40000000000 | long_offset);
		} else {
			nl = htonl(entry->position);
		}

		if (hash_and_write(indexer, fd, &nl, sizeof(uint32_t)) < 0)
			goto on_error;
	}

	/* Long (>31-bit) offsets */
	/* TODO; needs testing -- is this really _index_ or is it _offset */
	if (long_offset > 0) {
		git_vector_foreach(&indexer->objects, i, entry) {
			if (entry->position >= 0x40000000000) {
				nll = htonll(entry->position);

				if (hash_and_write(indexer, fd, &nll, sizeof(uint64_t)) < 0)
					goto on_error;
			}
		}
	}

	/* Packfile trailer */
	if (hash_and_write(indexer, fd, indexer->packfile_trailer,
			git_oid_size(indexer->oid_type)) < 0)
		goto on_error;

	if (git_hash_final(index_trailer, &indexer->hash_ctx) < 0 ||
	    p_write(fd, index_trailer, git_oid_size(indexer->oid_type)) < 0)
		goto on_error;

	git_str_dispose(&path);
	return 0;

on_error:
	if (fd >= 0)
		p_close(fd);

	git_str_dispose(&path);
	return -1;
}

int git_indexer_commit(git_indexer *indexer, git_indexer_progress *stats)
{
	struct object_entry *entry;
	size_t i;
	int error;

	GIT_ASSERT_ARG(indexer);

	if (!indexer->complete) {
		git_error_set(GIT_ERROR_INDEXER, "incomplete packfile");
		return -1;
	}

	/* Freeze the number of deltas */
	indexer->progress.total_deltas =
		indexer->progress.total_objects - indexer->progress.indexed_objects;

	if (stats)
		memcpy(stats, &indexer->progress, sizeof(git_indexer_progress));

	if ((error = do_progress_cb(indexer)) != 0)
		return error;


	indexer->index_end = git_time_monotonic();
	/*printf("elapsed: %llu\n", (indexer->index_end - indexer->index_start));*/

	indexer->packfile_map = mmap(NULL, indexer->packfile_size, PROT_READ, MAP_SHARED, indexer->packfile_fd, 0);

	if (resolve_deltas(indexer) < 0)
		return -1;

	/* TODO: zap */
	if (0) {
	git_vector_foreach(&indexer->objects, i, entry) {
		git_object_t type = git_object__is_delta(entry->type) ?
			((struct delta_entry *)entry)->final_type : entry->type;

		printf("%s %-6s ... ... %llu",
			git_oid_tostr_s(&entry->id),
			git_object_type2string(type),
			entry->position);
		}

		printf("\n");
	}
	/* TODO: /zap */

	git_vector_sort(&indexer->objects);

	if (write_index(indexer) < 0)
		return -1;

	/*printf("==========\n");*/
	/*printf("object lookups: %lu / cache hits: %lu\n", indexer->object_lookups, indexer->cache_hits);*/

	return 0;
}

void git_indexer_free(git_indexer *indexer)
{
	if (!indexer)
		return;

	git_zstream_free(&indexer->zstream);
	git_str_dispose(&indexer->packfile_path);
	git_hash_ctx_cleanup(&indexer->hash_ctx);
	git_sizemap_free(indexer->positions);
	git_vector_free(&indexer->deltas);
	git_vector_free_deep(&indexer->objects);
	git_packfile_parser_dispose(&indexer->parser);
	git__free(indexer);
}