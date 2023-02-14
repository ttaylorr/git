#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "blame-tree.h"
#include "commit.h"
#include "diff.h"
#include "diffcore.h"
#include "revision.h"
#include "log-tree.h"
#include "dir.h"
#include "commit-graph.h"
#include "prio-queue.h"
#include "commit-slab.h"
#include "hex.h"
#include "quote.h"
#include "chunk-format.h"
#include "csum-file.h"
#include "lockfile.h"
#include "object-file.h"
#include "trace2.h"
#include "strvec.h"
#include "run-command.h"
#include "path.h"
#include "config.h"
#include "environment.h"

/*
 * This is the default blame-tree output. It is used when
 * not writing to the blame-tree cache.
 */
static void show_entry(const char *path, const struct commit *commit, void *d)
{
	struct blame_tree *bt = d;

	if (commit->object.flags & BOUNDARY)
		putchar('^');
	printf("%s\t", oid_to_hex(&commit->object.oid));

	if (bt->rev.diffopt.line_termination)
		write_name_quoted(path, stdout, '\n');
	else
		printf("%s%c", path, '\0');

	fflush(stdout);
}

struct blame_tree_cache_result {
	struct object_id oid;
	size_t pathlen;
	char *path;
};

struct blame_tree_cache_writer {
	struct blame_tree_cache_result *results;
	size_t results_nr;
	size_t results_alloc;
	size_t results_size;

	struct commit *commit;

	int max_depth;
	size_t pathspec_len;
	const char *pathspec;
};

static size_t padded(size_t len)
{
	if (len % 4)
		len += 4 - (len % 4);
	return len;
}

/*
 * This callback is used when writing the cache to
 * a file. We first collect the rows in a list.
 */
static void store_row(const char *path, const struct commit *commit, void *d)
{
	struct blame_tree_cache_writer *writer = d;
	struct blame_tree_cache_result *result;
	ALLOC_GROW(writer->results, writer->results_nr + 1, writer->results_alloc);

	result = &writer->results[writer->results_nr++];

	oidcpy(&result->oid, &commit->object.oid);
	result->pathlen = strlen(path);
	result->path = xstrdup(path);

	writer->results_size += the_hash_algo->rawsz + 4 +
				padded(result->pathlen);
}

#define BLAME_TREE_CACHE_FORMAT 0x424C5443
#define META_CHUNK 0x4D455441
#define COMMIT_CHUNK 0x434D4D54
#define RESULTS_CHUNK 0x52534C54

static void write_btc_header(struct hashfile *f, struct chunkfile *cf)
{
	hashwrite_be32(f, BLAME_TREE_CACHE_FORMAT);

	hashwrite_u8(f, 1);
	hashwrite_u8(f, get_num_chunks(cf));
	hashwrite_u8(f, oid_version(the_hash_algo));
	hashwrite_u8(f, 0);
}

static void add_padding(struct hashfile *f, size_t len)
{
	switch (len % 4) {
	case 1:
		hashwrite_u8(f, 0);
		/* fallthrough */
	case 2:
		hashwrite_u8(f, 0);
		/* fallthrough */
	case 3:
		hashwrite_u8(f, 0);
	}
}

static int write_meta_chunk(struct hashfile *f, void *data)
{
	struct blame_tree_cache_writer *writer = data;
	hashwrite_be32(f, writer->max_depth);
	hashwrite_be32(f, padded(writer->pathspec_len));
	hashwrite(f, writer->pathspec, writer->pathspec_len);
	add_padding(f, writer->pathspec_len);

	return 0;
}

static int write_commit_chunk(struct hashfile *f, void *data)
{
	struct blame_tree_cache_writer *writer = data;
	if (writer->commit)
		hashwrite(f, writer->commit->object.oid.hash, the_hash_algo->rawsz);
	else
		hashwrite(f, null_oid()->hash, the_hash_algo->rawsz);
	return 0;
}

static int write_results_chunk(struct hashfile *f, void *data)
{
	struct blame_tree_cache_writer *writer = data;
	struct blame_tree_cache_result *result = writer->results;
	size_t remaining = writer->results_nr;

	while (remaining) {
		hashwrite(f, result->oid.hash, the_hash_algo->rawsz);
		hashwrite_be32(f, result->pathlen);
		hashwrite(f, result->path, result->pathlen);
		add_padding(f, result->pathlen);
		result++;
		remaining--;
	}

	return 0;
}

static void write_btc(struct blame_tree_cache_writer *writer,
		      struct hashfile *f, struct chunkfile *cf)
{
	size_t meta_len = 4 + 4 + padded(writer->pathspec_len);

	add_chunk(cf, META_CHUNK, meta_len, write_meta_chunk);
	add_chunk(cf, COMMIT_CHUNK, the_hash_algo->rawsz, write_commit_chunk);

	if (writer->results_size)
		add_chunk(cf, RESULTS_CHUNK, writer->results_size, write_results_chunk);

	write_btc_header(f, cf);
	write_chunkfile(cf, writer);
	finalize_hashfile(f, NULL, FSYNC_COMPONENT_PACK_METADATA,
			  CSUM_HASH_IN_STREAM | CSUM_FSYNC);
}

static char *get_cache_id(int max_depth, const char *pathspec)
{
	struct git_hash_ctx ctx;
	unsigned char hash[GIT_MAX_RAWSZ];
	struct strbuf input = STRBUF_INIT;
	strbuf_addf(&input, "%d %s", max_depth, pathspec);

	the_hash_algo->init_fn(&ctx);
	the_hash_algo->update_fn(&ctx, input.buf, input.len);
	the_hash_algo->final_fn(hash, &ctx);

	strbuf_release(&input);
	return xstrdup(hash_to_hex(hash));
}

static char *get_cache_filename(const char *object_dir, const char *cache_id)
{
	return xstrfmt("%s/info/blame-tree/%s.btc", object_dir, cache_id);
}

static int write_blame_tree_cache(struct repository *r,
				  struct blame_tree_cache_writer *writer)
{
	struct lock_file lk = LOCK_INIT;
	struct hashfile *f;
	struct chunkfile *cf;
	int res = 0;
	char *cache_id = get_cache_id(writer->max_depth, writer->pathspec);
	char *filename = get_cache_filename(r->objects->odb->path, cache_id);

	if (safe_create_leading_directories(filename)) {
		error(_("unable to create leading directories of %s"),
			filename);
		res = -1;
		goto cleanup;
	}

	if (hold_lock_file_for_update_mode(&lk, filename,
					   LOCK_REPORT_ON_ERROR, 0644) < 0) {
		res = -1;
		goto cleanup;
	}

	f = hashfd(lk.tempfile->fd, lk.tempfile->filename.buf);
	cf = init_chunkfile(f);

	write_btc(writer, f, cf);

	free_chunkfile(cf);
	commit_lock_file(&lk);

cleanup:
	free(cache_id);
	free(filename);
	return res;
}

static int unlink_blame_tree_cache(struct repository *r,
				   struct blame_tree_cache_writer *writer)
{
	char *cache_id = get_cache_id(writer->max_depth, writer->pathspec);
	char *filename = get_cache_filename(r->objects->odb->path, cache_id);
	int res = unlink_or_warn(filename);

	free(cache_id);
	free(filename);
	return res;
}

static void write_placeholder_cache_file(struct blame_tree *bt)
{
	struct repository *r = bt->rev.repo;
	struct blame_tree_cache_writer writer;

	/*
	 * Cache does not support multiple pathspecs.
	 * No pathspec means the root tree, which is
	 * automatically cached.
	 */
	if (bt->rev.diffopt.pathspec.nr != 1)
		return;

	memset(&writer, 0, sizeof(writer));

	writer.pathspec = bt->rev.diffopt.pathspec.items[0].original;
	writer.pathspec_len = strlen(writer.pathspec);
	writer.max_depth = bt->rev.diffopt.max_depth;

	bt->goal_end_time = 0;
	if (write_blame_tree_cache(r, &writer))
		warning(_("failed to create placeholder for pathspec '%s'"),
			writer.pathspec);
}

struct blame_tree_cache_reader {
	const unsigned char *data;
	size_t size;

	int max_depth;
	size_t pathspec_len;
	char *pathspec;

	struct object_id oid;

	int fd;
	struct chunkfile *cf;
};

static int read_meta_chunk(const unsigned char *chunk_start, size_t chunk_size,
			   void *data)
{
	struct blame_tree_cache_reader *reader = data;

	if (chunk_size < 8) {
		warning(_("blame-tree cache: META chunk is too small"));
		return -1;
	}

	reader->max_depth = get_be32(chunk_start);
	reader->pathspec_len = get_be32(chunk_start + 4);
	reader->pathspec = xstrndup((const char *)(chunk_start + 8),
				    reader->pathspec_len);

	return 0;
}

static int read_commit_chunk(const unsigned char *chunk_start, size_t chunk_size,
			     void *data)
{
	struct blame_tree_cache_reader *reader = data;

	if (chunk_size != the_hash_algo->rawsz) {
		warning(_("blame-tree cache: COMMIT chunk is wrong size"));
		return -1;
	}

	hashcpy(reader->oid.hash, chunk_start, the_hash_algo);
	return 0;
}

static struct blame_tree_cache_reader *init_blame_tree_cache_reader(int fd, struct stat *st)
{
	struct blame_tree_cache_reader *reader;
	size_t size = xsize_t(st->st_size);
	int format;
	unsigned char version, num_chunks, hash_version, unused;
	const unsigned char *data;

	if (size < 8) {
		warning(_("blame-tree cache file is too short"));
		return NULL;
	}

	data = xmmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

	format = get_be32(data);

	if (format != BLAME_TREE_CACHE_FORMAT) {
		warning(_("blame-tree cache file has incorrect header %08x"),
			format);
		goto fail;
	}

	version = *(data + 4);
	num_chunks = *(data + 5);
	hash_version = *(data + 6);
	unused = *(data + 7);

	if (version != 1 || hash_version != oid_version(the_hash_algo) || unused) {
		warning(_("incompatible blame-tree header information"));
		goto fail;
	}

	CALLOC_ARRAY(reader, 1);
	reader->fd = fd;
	reader->data = data;
	reader->cf = init_chunkfile(NULL);
	read_table_of_contents(reader->cf, data, size, 8, num_chunks, 1);

	read_chunk(reader->cf, META_CHUNK, read_meta_chunk, reader);
	read_chunk(reader->cf, COMMIT_CHUNK, read_commit_chunk, reader);

	return reader;

fail:
	munmap((void *)data, size);
	return NULL;
}

static void free_blame_tree_cache_reader(struct blame_tree_cache_reader *reader)
{
	munmap((void *)reader->data, reader->size);
	free(reader->pathspec);
	free(reader->cf);
	close(reader->fd);
}

struct blame_tree_entry {
	struct hashmap_entry hashent;
	struct object_id oid;
	struct commit *commit;
	int diff_idx;
	struct bloom_key key;
	const char path[FLEX_ARRAY];
};

static void add_from_diff(struct diff_queue_struct *q,
			  struct diff_options *opt UNUSED,
			  void *data)
{
	struct blame_tree *bt = data;
	int i;

	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		struct blame_tree_entry *ent;
		const char *path = p->two->path;

		FLEX_ALLOC_STR(ent, path, path);
		oidcpy(&ent->oid, &p->two->oid);
		if (bt->rev.bloom_filter_settings)
			fill_bloom_key(path, strlen(path), &ent->key,
				       bt->rev.bloom_filter_settings);
		hashmap_entry_init(&ent->hashent, strhash(ent->path));
		hashmap_add(&bt->paths, &ent->hashent);
	}
}

static int add_from_revs(struct blame_tree *bt)
{
	unsigned int i;
	int count = 0;
	struct diff_options diffopt;

	memcpy(&diffopt, &bt->rev.diffopt, sizeof(diffopt));
	diffopt.output_format = DIFF_FORMAT_CALLBACK;
	diffopt.format_callback = add_from_diff;
	diffopt.format_callback_data = bt;

	for (i = 0; i < bt->rev.pending.nr; i++) {
		struct object_array_entry *obj = bt->rev.pending.objects + i;

		if (obj->item->flags & UNINTERESTING)
			continue;

		if (count++)
			return error("can only blame one tree at a time");

		diff_tree_oid(the_hash_algo->empty_tree, &obj->item->oid, "", &diffopt);
		diff_flush(&diffopt);
	}

	return 0;
}

static int blame_tree_entry_hashcmp(const void *unused UNUSED,
				    const struct hashmap_entry *he1,
				    const struct hashmap_entry *he2,
				    const void *path)
{
	const struct blame_tree_entry *e1 =
		container_of(he1, const struct blame_tree_entry, hashent);
	const struct blame_tree_entry *e2 =
		container_of(he2, const struct blame_tree_entry, hashent);
	return strcmp(e1->path, path ? path : e2->path);
}

void blame_tree_init(struct blame_tree *bt, int flags,
		     int argc, const char **argv, const char *prefix)
{
	struct repository *r = the_repository;
	struct hashmap_iter iter;
	struct blame_tree_entry *e;
	const char *pathspec = "";
	int limit_millis = 1000;

	memset(bt, 0, sizeof(*bt));
	hashmap_init(&bt->paths, blame_tree_entry_hashcmp, NULL, 0);

	repo_init_revisions(the_repository, &bt->rev, prefix);
	bt->rev.def = "HEAD";
	bt->rev.combine_merges = 1;
	bt->rev.show_root_diff = 1;
	bt->rev.boundary = 1;
	bt->rev.no_commit_id = 1;
	bt->rev.diff = 1;
	bt->rev.diffopt.flags.recursive = 1;
	bt->rev.diffopt.no_free = 1;

	setenv(GIT_LITERAL_PATHSPECS_ENVIRONMENT, "1", 1);

	if (setup_revisions(argc, argv, &bt->rev, NULL) > 1)
		die("unknown blame-tree argument: %s\n", argv[1]);

	if ((flags & BLAME_TREE_WRITE_CACHE) &&
	    !bt->rev.diffopt.max_depth_valid)
		die("refusing to cache without --max-depth");

	(void)generation_numbers_enabled(r);
	if (r->objects->commit_graph)
		bt->rev.bloom_filter_settings = get_bloom_filter_settings(r);

	if (add_from_revs(bt) < 0)
		die("unable to setup blame-tree");

	bt->all_paths = xcalloc(hashmap_get_size(&bt->paths), sizeof(const char *));
	bt->all_paths_nr = 0;
	hashmap_for_each_entry(&bt->paths, &iter, e, hashent) {
		e->diff_idx = bt->all_paths_nr++;
		bt->all_paths[e->diff_idx] = e->path;
	}

	/* Skip caching for scenarios that do not work */

	/* Multiple pathspecs? */
	if (bt->rev.diffopt.pathspec.nr > 1)
		return;
	/*
	 * Multiple starting commits?
	 * This usually means the arguments are something like
	 * "git blame-tree HEAD ^HEAD~100", so they include
	 * negative arguments. Multiple starting points would also
	 * be invalid.
	 */
	if (bt->rev.pending.nr != 1)
		return;

	if (bt->rev.diffopt.pathspec.nr == 1)
		pathspec = bt->rev.diffopt.pathspec.items[0].original;

	if (flags & BLAME_TREE_WRITE_CACHE) {
		CALLOC_ARRAY(bt->writer, 1);
		bt->writer->commit = lookup_commit(bt->rev.repo,
						   &bt->rev.pending.objects[0].item->oid);
		bt->writer->pathspec = pathspec;
		bt->writer->pathspec_len = strlen(pathspec);
		bt->writer->results_alloc = 16;
		ALLOC_ARRAY(bt->writer->results, bt->writer->results_alloc);
	}

	/* Skip the cache if requested _or_ if --max-depth is not provided. */
	if (!(flags & BLAME_TREE_SKIP_CACHE) &&
	    bt->rev.diffopt.max_depth_valid) {
		/* look for a cache file */
		struct object_directory *odb;
		struct stat st;
		char *cache_id = get_cache_id(bt->writer->max_depth, pathspec);
		prepare_alt_odb(r);

		for (odb = r->objects->odb; odb; odb = odb->next) {
			char *filename = get_cache_filename(odb->path, cache_id);
			int fd = git_open(filename);

			free(filename);

			if (fd < 0)
				continue;
			if (fstat(fd, &st)) {
				close(fd);
				fd = -1;
				continue;
			} else {
				bt->reader = init_blame_tree_cache_reader(fd, &st);
				break;
			}
		}
	}

	/*
	 * In the case that we did not find a cache file,
	 * determine a maximum window of time before we
	 * should write one of our own as a helper.
	 */
	if (!bt->writer && !bt->reader &&
	    bt->rev.diffopt.pathspec.nr == 1) {
		repo_config_get_int(r, "blametree.limitmilliseconds", &limit_millis);
		bt->goal_end_time = clock() + (CLOCKS_PER_SEC * limit_millis) / 1000;
	}
}

int blame_tree_release(struct blame_tree *bt)
{
	int res = 0;

	hashmap_clear_and_free(&bt->paths, struct blame_tree_entry, hashent);
	free(bt->all_paths);

	if (bt->goal_end_time &&
	    clock() > bt->goal_end_time)
		write_placeholder_cache_file(bt);

	if (bt->reader) {
		free_blame_tree_cache_reader(bt->reader);
		FREE_AND_NULL(bt->reader);
	}

	if (bt->writer) {
		if (bt->all_paths_nr)
			res = write_blame_tree_cache(bt->rev.repo, bt->writer);
		else
			res = unlink_blame_tree_cache(bt->rev.repo, bt->writer);

		free(bt->writer->results);
		FREE_AND_NULL(bt->writer);
	}

	return res;
}

struct commit_active_paths {
	char *active;
	int nr;
};

define_commit_slab(active_paths, struct commit_active_paths);
static struct active_paths active_paths;

struct blame_tree_callback_data {
	struct commit *commit;
	struct hashmap *paths;
	int num_interesting;

	blame_tree_callback callback;
	void *callback_data;
};

static void mark_path(const char *path, const struct object_id *oid,
		      struct blame_tree_callback_data *data,
		      int check_active)
{
	struct blame_tree_entry *ent;
	struct commit_active_paths *active;

	/* Is it even a path that we are interested in? */
	ent = hashmap_get_entry_from_hash(data->paths, strhash(path), path,
					  struct blame_tree_entry, hashent);
	if (!ent)
		return;

	/* Have we already blamed a commit? */
	if (ent->commit)
		return;

	/* Are we inactive on the current commit? */
	if (check_active) {
		active = active_paths_at(&active_paths, data->commit);
		if (active && active->active &&
		    !active->active[ent->diff_idx])
			return;
	}

	/*
	 * Is it arriving at a version of interest, or is it from a side branch
	 * which did not contribute to the final state?
	 */
	if (oid && oidcmp(oid, &ent->oid))
		return;

	ent->commit = data->commit;
	data->num_interesting--;
	if (data->callback)
		data->callback(path, data->commit, data->callback_data);
	hashmap_remove(data->paths, &ent->hashent, path);
}

static void blame_diff(struct diff_queue_struct *q,
		       struct diff_options *opt UNUSED, void *cbdata)
{
	struct blame_tree_callback_data *data = cbdata;
	int i;

	for (i = 0; i < q->nr; i++) {
		struct diff_filepair *p = q->queue[i];
		switch (p->status) {
		case DIFF_STATUS_DELETED:
			/*
			 * There's no point in feeding a deletion, as it could
			 * not have resulted in our current state, which
			 * actually has the file.
			 */
			break;

		default:
			/*
			 * Otherwise, we care only that we somehow arrived at
			 * a final path/sha1 state. Note that this covers some
			 * potentially controversial areas, including:
			 *
			 *  1. A rename or copy will be blamed, as it is the
			 *     first time the content has arrived at the given
			 *     path.
			 *
			 *  2. Even a non-content modification like a mode or
			 *     type change will trigger it.
			 *
			 * We take the inclusive approach for now, and blame
			 * anything which impacts the path. Options to tweak
			 * the behavior (e.g., to "--follow" the content across
			 * renames) can come later.
			 */
			mark_path(p->two->path, &p->two->oid, data, 1);
			break;
		}
	}
}

static char *scratch;

static void pass_to_parent(struct commit_active_paths *c,
			   struct commit_active_paths *p,
			   int i)
{
	c->active[i] = 0;
	c->nr--;
	p->active[i] = 1;
	p->nr++;
}

#define PARENT1 (1u<<16) /* used instead of SEEN */
#define PARENT2 (1u<<17) /* used instead of BOTTOM, BOUNDARY */

static int diff2idx(struct blame_tree *bt, char *path)
{
	struct blame_tree_entry *ent;
	ent = hashmap_get_entry_from_hash(&bt->paths, strhash(path), path,
					  struct blame_tree_entry, hashent);
	return ent ? ent->diff_idx : -1;
}

static int maybe_changed_path(struct blame_tree *bt,
			      struct commit *origin,
			      struct commit_active_paths *active)
{
	struct bloom_filter *filter;
	struct blame_tree_entry *e;
	struct hashmap_iter iter;
	int i;

	if (!bt->rev.bloom_filter_settings)
		return 1;

	if (commit_graph_generation(origin) == GENERATION_NUMBER_INFINITY)
		return 1;

	filter = get_bloom_filter(bt->rev.repo, origin);
	if (!filter)
		return 1;

	for (i = 0; i < bt->rev.bloom_keys_nr; i++) {
		if (!(bloom_filter_contains(filter,
					    &bt->rev.bloom_keys[i],
					    bt->rev.bloom_filter_settings)))
			return 0;
	}

	hashmap_for_each_entry(&bt->paths, &iter, e, hashent) {
		if (active && !active->active[e->diff_idx])
			continue;
		if (bloom_filter_contains(filter, &e->key,
					  bt->rev.bloom_filter_settings))
			return 1;
	}
	return 0;
}

static int process_parent(struct blame_tree *bt,
			   struct prio_queue *queue,
			   struct commit *c, struct commit_active_paths *active_c,
			   struct commit *parent, int parent_i)
{
	int i, ret = 0;
	struct commit_active_paths *active_p;

	repo_parse_commit(bt->rev.repo, parent);

	active_p = active_paths_at(&active_paths, parent);
	if (!active_p->active) {
		active_p->active = xcalloc(sizeof(char), bt->all_paths_nr);
		active_p->nr = 0;
	}

	/*
	 * Before calling 'diff_tree_oid()' on our first parent, see if Bloom
	 * filters will tell us the diff is conclusively uninteresting.
	 */
	if (parent_i || maybe_changed_path(bt, c, active_c)) {
		diff_tree_oid(&parent->object.oid,
			      &c->object.oid, "", &bt->rev.diffopt);
		diffcore_std(&bt->rev.diffopt);
	}

	if (!diff_queued_diff.nr) {
		/*
		 * No diff entries means we are TREESAME on the base path, and
		 * so all active paths get passed onto this parent.
		 */
		for (i = 0; i < bt->all_paths_nr; i++) {
			if (active_c->active[i])
				pass_to_parent(active_c, active_p, i);
		}

		if (!(parent->object.flags & PARENT1)) {
			parent->object.flags |= PARENT1;
			prio_queue_put(queue, parent);
		}
		ret = 1;
		goto cleanup;
	}

	/*
	 * Otherwise, test each path for TREESAME-ness against the parent, and
	 * pass those along.
	 *
	 * First, set each position in 'scratch' to be zero for TREESAME paths,
	 * and one otherwise. Then, pass active and TREESAME paths to the
	 * parent.
	 */
	for (i = 0; i < diff_queued_diff.nr; i++) {
		struct diff_filepair *fp = diff_queued_diff.queue[i];
		int k = diff2idx(bt, fp->two->path);
		if (0 <= k && active_c->active[k])
			scratch[k] = 1;
		diff_free_filepair(fp);
	}
	for (i = 0; i < bt->all_paths_nr; i++) {
		if (active_c->active[i] && !scratch[i])
			pass_to_parent(active_c, active_p, i);
	}

	if (active_p->nr && !(parent->object.flags & PARENT1)) {
		parent->object.flags |= PARENT1;
		prio_queue_put(queue, parent);
	}

cleanup:
	diff_queue_clear(&diff_queued_diff);
	memset(scratch, 0, bt->all_paths_nr);

	return ret;
}

struct read_cache_results_data {
	struct blame_tree *bt;
	struct commit_active_paths *active_c;
	struct blame_tree_callback_data *data;
};

static int read_results_chunk(const unsigned char *chunk_start, size_t chunk_size,
			      void *data)
{
	struct read_cache_results_data *read_data = data;
	struct blame_tree *bt = read_data->bt;
	struct commit_active_paths *active_c = read_data->active_c;
	struct blame_tree_callback_data *cdata = read_data->data;
	const unsigned char *chunk_end = chunk_start + chunk_size;
	const unsigned char *cur_pos = chunk_start;

	while (cur_pos < chunk_end) {
		struct blame_tree_cache_result result;
		struct blame_tree_entry *ent;

		oidread(&result.oid, cur_pos, the_hash_algo);
		cur_pos += the_hash_algo->rawsz;

		result.pathlen = get_be32(cur_pos);
		cur_pos += 4;

		result.path = xstrndup((const char *)cur_pos, result.pathlen);
		cur_pos += padded(result.pathlen);

		/* check if this result should be emitted */
		ent = hashmap_get_entry_from_hash(
			&bt->paths, strhash(result.path), result.path,
			struct blame_tree_entry, hashent);

		if (!ent || !active_c->active[ent->diff_idx])
			goto cleanup_result;

		cdata->commit = lookup_commit(the_repository, &result.oid);
		mark_path(result.path, NULL, cdata, 0);

	cleanup_result:
		free(result.path);
	}

	if (cur_pos > chunk_end) {
		warning(_("blame-tree results table has wrong size"));
		return -1;
	}

	return 0;
}

int blame_tree_run(struct blame_tree *bt)
{
	int found_cached_commit = 0;
	int max_count, queue_popped = 0;
	int loop_count = 0, clock_check_rate = 100;
	struct prio_queue queue = { compare_commits_by_gen_then_commit_date };
	struct prio_queue not_queue = { compare_commits_by_gen_then_commit_date };
	struct blame_tree_callback_data data;
	struct commit_list *list;

	data.paths = &bt->paths;
	data.num_interesting = hashmap_get_size(&bt->paths);
	data.callback = bt->writer ? store_row : show_entry;
	data.callback_data = bt->writer ? (void*)bt->writer : (void*)bt;

	bt->rev.diffopt.output_format = DIFF_FORMAT_CALLBACK;
	bt->rev.diffopt.format_callback = blame_diff;
	bt->rev.diffopt.format_callback_data = &data;
	bt->rev.no_walk = 1;
	bt->rev.blob_objects = 1;
	bt->rev.tree_objects = 1;

	prepare_revision_walk(&bt->rev);

	if (bt->rev.pending.nr)
		die("not a commit: %s", oid_to_hex(&bt->rev.pending.objects[0].item->oid));

	max_count = bt->rev.max_count;

	init_active_paths(&active_paths);
	scratch = xcalloc(bt->all_paths_nr, sizeof(char));

	/*
	 * bt->rev.commits holds the set of boundary commits for our walk.
	 *
	 * Loop through each such commit, and place it in the appropriate queue.
	 */
	for (list = bt->rev.commits; list; list = list->next) {
		struct commit *c = list->item;

		if (c->object.flags & BOTTOM) {
			prio_queue_put(&not_queue, c);
			c->object.flags |= PARENT2;
		} else if (!(c->object.flags & PARENT1)) {
			/*
			 * If the commit is a starting point (and hasn't been
			 * seen yet), then initialize the set of interesting
			 * paths, too.
			 */
			struct commit_active_paths *active;

			prio_queue_put(&queue, c);
			c->object.flags |= PARENT1;

			active = active_paths_at(&active_paths, c);
			active->active = xcalloc(sizeof(char), bt->all_paths_nr);
			memset(active->active, 1, bt->all_paths_nr);
			active->nr = bt->all_paths_nr;
		}
	}

	while (queue.nr) {
		int parent_i;
		struct commit_list *p;
		struct commit *c = prio_queue_get(&queue);
		struct commit_active_paths *active_c = active_paths_at(&active_paths, c);

		/*
		 * Check during the loop to ensure we write even if the
		 * process is killed due to a timeout.
		 */
		if (bt->goal_end_time &&
		    ++loop_count >= clock_check_rate) {
			loop_count = 0;

			if (clock() > bt->goal_end_time)
				write_placeholder_cache_file(bt);
		}

		if (bt->reader &&
		    oideq(&c->object.oid, &bt->reader->oid)) {
			struct read_cache_results_data results_data;

			results_data.bt = bt;
			results_data.active_c = active_c;
			results_data.data = &data;

			read_chunk(bt->reader->cf, RESULTS_CHUNK,
				   read_results_chunk, &results_data);

			found_cached_commit = 1;
			goto cleanup;
		}

		if ((0 <= max_count && max_count < ++queue_popped) ||
		    (c->object.flags & PARENT2)) {
			/*
			 * Either a boundary commit, or we have already seen too
			 * many others. Either way, stop here.
			 */
			c->object.flags |= PARENT2 | BOUNDARY;
			data.commit = c;
			diff_tree_oid(the_hash_algo->empty_tree,
				      &c->object.oid,
				      "", &bt->rev.diffopt);
			diff_flush(&bt->rev.diffopt);
			goto cleanup;
		}

		/*
		 * Otherwise, keep going, but make sure that 'c' isn't reachable
		 * from anything in the '--not' queue.
		 */
		repo_parse_commit(bt->rev.repo, c);

		while (not_queue.nr) {
			struct commit_list *np;
			struct commit *n = prio_queue_get(&not_queue);

			repo_parse_commit(bt->rev.repo, n);

			for (np = n->parents; np; np = np->next) {
				if (!(np->item->object.flags & PARENT2)) {
					prio_queue_put(&not_queue, np->item);
					np->item->object.flags |= PARENT2;
				}
			}

			if (commit_graph_generation(n) < commit_graph_generation(c))
				break;
		}

		/*
		 * Look at each remaining interesting path, and pass it onto
		 * parents in order if TREESAME.
		 */
		for (p = c->parents, parent_i = 0; p; p = p->next, parent_i++) {
			if (process_parent(bt, &queue,
					   c, active_c,
					   p->item, parent_i) > 0 )
				break;
		}

		if (active_c->nr)  {
			/* Any paths that remain active were changed by 'c'. */
			data.commit = c;
			for (int i = 0; i < bt->all_paths_nr; i++) {
				if (active_c->active[i])
					mark_path(bt->all_paths[i], NULL, &data, 1);
			}
		}

cleanup:
		FREE_AND_NULL(active_c->active);
		active_c->nr = 0;
	}

	clear_active_paths(&active_paths);
	free(scratch);

	trace2_data_intmax("blame-tree", bt->rev.repo,
			   found_cached_commit ?
				"cached-commit-true.count" :
				"cached-commit-false.count",
			   1);

	return 0;
}

static int call_blame_tree_cache(int max_depth, const char *pathspec,
				 const char *revision)
{
	struct child_process cmd = CHILD_PROCESS_INIT;

	cmd.git_cmd = 1;
	strvec_pushl(&cmd.args, "blame-tree", "--cache", revision, NULL);
	strvec_pushf(&cmd.args, "--max-depth=%d", max_depth);

	if (pathspec && *pathspec)
		strvec_pushl(&cmd.args, "--", pathspec, NULL);

	return run_command(&cmd);
}

static int update_cache(const char *filename, const char *revision,
			int *updated)
{
	int res = 0;
	int max_depth;
	char *pathspec;
	struct blame_tree_cache_reader *reader;
	struct stat st;
	int fd = git_open(filename);

	if (fd < 0)
		return -1;
	if (fstat(fd, &st)) {
		close(fd);
		return -1;
	}

	reader = init_blame_tree_cache_reader(fd, &st);

	max_depth = reader->max_depth;
	pathspec = xstrdup(reader->pathspec);

	/*
	 * Free reader before replacing the file, since
	 * otherwise we still have a handle on the file.
	 */
	free_blame_tree_cache_reader(reader);

	/* max_depth=0 with root pathspec is handled separately */
	if (reader->max_depth) {
		res = call_blame_tree_cache(max_depth,
					    pathspec,
					    revision);
		*updated = 1;
	} else {
		*updated = 0;
	}

	free(pathspec);
	return res;
}

struct path_and_mtime {
	char *name;
	time_t mtime;
};

static int compare_mtime(const void *a_, const void *b_)
{
	const struct path_and_mtime *a = a_;
	const struct path_and_mtime *b = b_;

	if (a->mtime < b->mtime)
		return -1;
	else if (a->mtime == b->mtime)
		return 0;
	return 1;
}

/*
 * Iterate through all blame-tree cache files and
 * recompute them starting at the given commit.
 */
int update_blame_tree_caches(const char *revision)
{
	struct repository *r = the_repository;
	size_t i, res = 0;
	struct strbuf path = STRBUF_INIT;
	size_t dirlen;
	DIR *dir;
	struct dirent *de;
	int write_count = 0, max_writes = 10;
	struct path_and_mtime *list;
	size_t list_nr = 0, list_alloc = 16;

	trace2_region_enter("blame-tree", "update-caches", r);

	git_config_get_int("blametree.maxwrites", &max_writes);

	/* update the root by default */
	call_blame_tree_cache(0, NULL, revision);

	strbuf_addstr(&path, repo_common_path(r, "objects/info/blame-tree"));

	dir = opendir(path.buf);
	if (!dir) {
		strbuf_release(&path);
		return 0;
	}

	strbuf_addch(&path, '/');
	dirlen = path.len;

	ALLOC_ARRAY(list, list_alloc);
	while (!res && (de = readdir(dir)) != NULL) {
		struct stat st;
		if (is_dot_or_dotdot(de->d_name))
			continue;

		strbuf_setlen(&path, dirlen);
		strbuf_addstr(&path, de->d_name);

		if (stat(path.buf, &st))
			continue;

		ALLOC_GROW(list, list_nr + 1, list_alloc);
		list[list_nr].name = xstrdup(de->d_name);
		list[list_nr].mtime = st.st_mtime;
		list_nr++;
	}

	QSORT(list, list_nr, compare_mtime);

	for (i = 0; i < list_nr && write_count < max_writes; i++) {
		int updated = 0;

		strbuf_setlen(&path, dirlen);
		strbuf_addstr(&path, list[i].name);

		res = update_cache(path.buf, revision, &updated);

		/*
		 * Clear the cache if there are problems, so
		 * we don't end up in a spiral of bad files
		 */
		if (res) {
			warning(_("error while computing cache file '%s'"),
				list[i].name);
			unlink(path.buf);
		}

		write_count += updated;
	}
	for (i = 0; i < list_nr; i++)
		free(list[i].name);
	free(list);

	closedir(dir);
	strbuf_release(&path);

	trace2_region_leave("blame-tree", "update-caches", r);

	return res;
}
