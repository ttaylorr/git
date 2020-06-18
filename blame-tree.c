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

struct blame_tree_entry {
	struct hashmap_entry hashent;
	struct object_id oid;
	struct commit *commit;
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

void blame_tree_init(struct blame_tree *bt, int argc, const char **argv,
		     const char *prefix)
{
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
	setup_revisions(argc, argv, &bt->rev, NULL);

	(void)generation_numbers_enabled(bt->rev.repo);
	if (bt->rev.repo->objects->commit_graph)
		bt->rev.bloom_filter_settings =
			bt->rev.repo->objects->commit_graph->bloom_filter_settings;

	if (add_from_revs(bt) < 0)
		die("unable to setup blame-tree");
}

void blame_tree_release(struct blame_tree *bt)
{
	hashmap_clear_and_free(&bt->paths, struct blame_tree_entry, hashent);
}

struct blame_tree_callback_data {
	struct commit *commit;
	struct hashmap *paths;
	int num_interesting;

	blame_tree_callback callback;
	void *callback_data;
};

static void mark_path(const char *path, const struct object_id *oid,
		      struct blame_tree_callback_data *data)
{
	struct blame_tree_entry *ent;

	/* Is it even a path that we are interested in? */
	ent = hashmap_get_entry_from_hash(data->paths, strhash(path), path,
					  struct blame_tree_entry, hashent);
	if (!ent)
		return;

	/* Have we already blamed a commit? */
	if (ent->commit)
		return;
	/*
	 * Is it arriving at a version of interest, or is it from a side branch
	 * which did not contribute to the final state?
	 */
	if (oidcmp(oid, &ent->oid))
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
			mark_path(p->two->path, &p->two->oid, data);
			break;
		}
	}
}

static int maybe_changed_path(struct blame_tree *bt, struct commit *origin)
{
	struct bloom_filter *filter;
	struct blame_tree_entry *e;
	struct hashmap_iter iter;

	if (!bt->rev.bloom_filter_settings)
		return 1;

	if (commit_graph_generation(origin) == GENERATION_NUMBER_INFINITY)
		return 1;

	filter = get_bloom_filter(bt->rev.repo, origin);
	if (!filter)
		return 1;

	hashmap_for_each_entry(&bt->paths, &iter, e, hashent) {
		if (bloom_filter_contains(filter, &e->key,
					  bt->rev.bloom_filter_settings))
			return 1;
	}
	return 0;
}

int blame_tree_run(struct blame_tree *bt, blame_tree_callback cb, void *cbdata)
{
	struct blame_tree_callback_data data;

	data.paths = &bt->paths;
	data.num_interesting = hashmap_get_size(&bt->paths);
	data.callback = cb;
	data.callback_data = cbdata;

	bt->rev.diffopt.output_format = DIFF_FORMAT_CALLBACK;
	bt->rev.diffopt.format_callback = blame_diff;
	bt->rev.diffopt.format_callback_data = &data;

	prepare_revision_walk(&bt->rev);

	while (data.num_interesting > 0) {
		data.commit = get_revision(&bt->rev);
		if (!data.commit)
			break;

		if (!maybe_changed_path(bt, data.commit))
			continue;

		if (data.commit->object.flags & BOUNDARY) {
			diff_tree_oid(the_hash_algo->empty_tree,
				      &data.commit->object.oid,
				      "", &bt->rev.diffopt);
			diff_flush(&bt->rev.diffopt);
		} else
			log_tree_commit(&bt->rev, data.commit);
	}

	return 0;
}
