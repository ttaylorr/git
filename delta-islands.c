#define DISABLE_SIGN_COMPARE_WARNINGS

#include "git-compat-util.h"
#include "object.h"
#include "commit.h"
#include "gettext.h"
#include "hex.h"
#include "tag.h"
#include "tree.h"
#include "pack.h"
#include "tree-walk.h"
#include "diff.h"
#include "progress.h"
#include "refs.h"
#include "khash.h"
#include "pack-bitmap.h"
#include "pack-objects.h"
#include "delta-islands.h"
#include "oid-array.h"
#include "config.h"
#include "ewah/ewok.h"

KHASH_INIT(str, const char *, void *, 1, kh_str_hash_func, kh_str_hash_equal)

static kh_oid_map_t *island_marks;
static unsigned island_counter;
static unsigned island_counter_core;

struct remote_island {
	uint64_t hash;
	struct oid_array oids;
};

/*
 * Each island_bitmap wraps an EWAH-compressed bitmap with
 * `island_bitmap_size` logical bits, one per island. We never mutate
 * an interned bitmap; ORs are computed via a dense `struct bitmap`
 * intermediate and re-compressed.
 *
 * The wrapper caches the bitmap's popcount and EWAH content hash
 * alongside the pointer, so that callers can:
 *
 *   - reject `is_subset(a, b)` instantly when popcount(a) >= popcount(b)
 *     and (a, b) are distinct canonical pointers (a strict subset has
 *     strictly fewer bits, and equal popcount + distinct canonical
 *     pointer implies non-subset because canonicality means equal
 *     content collapses to equal pointer);
 *
 *   - short-circuit `or(a, b)` to whichever operand is the superset of
 *     the other, avoiding the dense decompress + re-compress + intern
 *     round-trip on the cache-miss path;
 *
 *   - hash a pool entry by its cached `checksum` rather than re-walking
 *     the EWAH buffer on every `kh_put` / rehash.
 */
struct island_bitmap {
	struct ewah_bitmap *ewah;
	uint32_t popcount;
	uint32_t checksum;
};

static uint32_t island_bitmap_size;	/* number of islands (logical bits) */

/*
 * Allocate a wrapper that takes ownership of `raw` and caches its
 * popcount and checksum. The returned wrapper is not yet interned;
 * callers must pass it through `island_bitmap_intern()` to get back
 * the canonical pool entry for its content.
 */
static struct island_bitmap *island_bitmap_wrap(struct ewah_bitmap *raw,
						uint32_t popcount)
{
	struct island_bitmap *b = xmalloc(sizeof(*b));
	b->ewah = raw;
	b->popcount = popcount;
	b->checksum = ewah_checksum(raw);
	return b;
}

static void island_bitmap_destroy(struct island_bitmap *b)
{
	if (!b)
		return;
	ewah_free(b->ewah);
	free(b);
}

/*
 * Hash-cons pool of canonical bitmap values. Two bitmaps with the same
 * set-bit content are represented by the same wrapper pointer, which
 * lets us answer "do these bitmaps have equal content?" with a pointer
 * comparison and lets us cache the result of pairwise unions by
 * pointer pair.
 */
static khint_t island_bitmap_pool_hash(const struct island_bitmap *b)
{
	/* `checksum` is the cached content hash from `ewah_checksum()`. */
	return (khint_t)b->checksum;
}

static int island_bitmap_pool_eq(const struct island_bitmap *a,
				 const struct island_bitmap *b)
{
	const struct ewah_bitmap *ea, *eb;

	if (a == b)
		return 1;
	if (a->checksum != b->checksum)
		return 0;
	if (a->popcount != b->popcount)
		return 0;

	ea = a->ewah;
	eb = b->ewah;

	/*
	 * Pool entries come from one of two encoders:
	 *   - `bitmap_to_ewah()`, which is canonical for any given logical
	 *     content (trailing zero words are dropped, runs are emitted
	 *     in a fixed schedule);
	 *   - `ewah_set()` on a freshly-allocated `ewah_new()`, used only
	 *     by `single_bit_bitmap()` for the singleton seeds.
	 *
	 * The two encoders produce different byte layouts in general, but
	 * their logical-content domains are disjoint: singletons only ever
	 * arrive via `single_bit_bitmap()`, and `bitmap_to_ewah()` is only
	 * fed multi-bit unions (`island_bitmap_or_intern()` early-returns
	 * when `a == b`, so a singleton OR'd with itself never reaches
	 * `bitmap_to_ewah()`). So two pool entries with equal content also
	 * have equal `(bit_size, buffer)` bytes and we can compare by
	 * `memcmp` rather than by walking the EWAH iterator.
	 */
	if (ea->bit_size != eb->bit_size)
		return 0;
	if (ea->buffer_size != eb->buffer_size)
		return 0;
	return !memcmp(ea->buffer, eb->buffer,
		       ea->buffer_size * sizeof(eword_t));
}

KHASH_INIT(bitmap_pool, struct island_bitmap *, struct island_bitmap *, 1,
	   island_bitmap_pool_hash, island_bitmap_pool_eq)
static kh_bitmap_pool_t *bitmap_pool;

/*
 * Memoize `intern(a | b)`. Each entry records a pair of canonical
 * bitmap pointers (a, b) and the canonical pointer for `a | b`.
 */
struct bitmap_or_key {
	struct island_bitmap *a;
	struct island_bitmap *b;
};

static khint_t bitmap_or_hash(struct bitmap_or_key k)
{
	return (khint_t)((uintptr_t)k.a * 0x9e3779b1u) ^
	       (khint_t)((uintptr_t)k.b * 0x85ebca6bu);
}

static int bitmap_or_eq(struct bitmap_or_key x, struct bitmap_or_key y)
{
	return x.a == y.a && x.b == y.b;
}

KHASH_INIT(bitmap_or, struct bitmap_or_key, struct island_bitmap *, 1,
	   bitmap_or_hash, bitmap_or_eq)
static kh_bitmap_or_t *bitmap_or_cache;

/*
 * Return the number of dense words required to hold `island_bitmap_size`
 * logical bits. Used to pre-size the dense-bitmap intermediate when
 * building a fresh EWAH via OR.
 */
static size_t island_bitmap_word_alloc(void)
{
	return (island_bitmap_size + BITS_IN_EWORD - 1) / BITS_IN_EWORD;
}

/*
 * Walk `super`'s EWAH and `self`'s EWAH in lock-step. Bail at the first
 * word where `self` has bits not present in `super`. Both arguments
 * must be canonical pool entries.
 */
static int island_bitmap_is_subset_uncached(struct island_bitmap *self,
					    struct island_bitmap *super)
{
	struct ewah_iterator it_self, it_super;
	eword_t w_self, w_super;
	int next_self, next_super;

	ewah_iterator_init(&it_self, self->ewah);
	ewah_iterator_init(&it_super, super->ewah);

	for (;;) {
		next_self = ewah_iterator_next(&w_self, &it_self);
		next_super = ewah_iterator_next(&w_super, &it_super);
		if (!next_self)
			return 1;
		if (!next_super)
			w_super = 0;
		if (w_self & ~w_super)
			return 0;
	}
}

/*
 * Intern `b` into the canonical pool. If a wrapper with equal content
 * already exists, destroy `b` (and its underlying EWAH) and return the
 * canonical one. Otherwise insert `b` and return it.
 */
static struct island_bitmap *island_bitmap_intern(struct island_bitmap *b)
{
	int hash_ret;
	khiter_t pos;

	if (!bitmap_pool)
		bitmap_pool = kh_init_bitmap_pool();

	pos = kh_put_bitmap_pool(bitmap_pool, b, &hash_ret);
	if (hash_ret) {
		kh_value(bitmap_pool, pos) = b;
		return b;
	} else {
		struct island_bitmap *canonical = kh_value(bitmap_pool, pos);
		if (canonical != b)
			island_bitmap_destroy(b);
		return canonical;
	}
}

/*
 * Compute the canonical bitmap representing `a | b`, with memoization.
 * Both arguments must already be canonical pool entries.
 *
 * EWAH bitmaps are streaming-compressed and don't support random
 * mutation, so on a true cache miss we build the union via a dense
 * `struct bitmap` intermediate and re-compress with `bitmap_to_ewah()`.
 * Before paying that cost, try a popcount-guided subset check: if one
 * operand is a subset of the other, the OR result is just the larger
 * operand and we can skip the dense round-trip entirely.
 */
static struct island_bitmap *island_bitmap_or_intern(struct island_bitmap *a,
						     struct island_bitmap *b)
{
	struct bitmap_or_key key;
	struct island_bitmap *result;
	struct bitmap *tmp;
	khiter_t pos;
	int hash_ret;

	if (a == b)
		return a;

	/* Canonicalize order so (a, b) and (b, a) collapse to the same key. */
	if ((uintptr_t)a > (uintptr_t)b) {
		struct island_bitmap *t = a;
		a = b;
		b = t;
	}

	key.a = a;
	key.b = b;

	if (!bitmap_or_cache)
		bitmap_or_cache = kh_init_bitmap_or();

	pos = kh_put_bitmap_or(bitmap_or_cache, key, &hash_ret);
	if (!hash_ret)
		return kh_value(bitmap_or_cache, pos);

	/*
	 * Subset short-circuit: if one operand absorbs the other, the OR
	 * result is just the superset. Pick the candidate direction by
	 * popcount; equal popcount + distinct canonical pointers means
	 * neither is a subset of the other (canonicality folds equal
	 * content into equal pointers, caught above), so we skip the
	 * subset check entirely in that case.
	 */
	if (a->popcount > b->popcount) {
		if (island_bitmap_is_subset_uncached(b, a)) {
			kh_value(bitmap_or_cache, pos) = a;
			return a;
		}
	} else if (b->popcount > a->popcount) {
		if (island_bitmap_is_subset_uncached(a, b)) {
			kh_value(bitmap_or_cache, pos) = b;
			return b;
		}
	}

	tmp = bitmap_word_alloc(island_bitmap_word_alloc());
	bitmap_or_ewah(tmp, a->ewah);
	bitmap_or_ewah(tmp, b->ewah);
	/*
	 * Compute popcount from the dense intermediate so we don't pay
	 * an extra full EWAH walk inside `island_bitmap_wrap`. The dense
	 * buffer is already hot in cache from the two `bitmap_or_ewah`
	 * passes above.
	 */
	{
		uint32_t pc = (uint32_t)bitmap_popcount(tmp);
		result = island_bitmap_intern(
			island_bitmap_wrap(bitmap_to_ewah(tmp), pc));
	}
	bitmap_free(tmp);

	kh_value(bitmap_or_cache, pos) = result;
	return result;
}

/*
 * Memoize `subset(a, b)`. Both arguments are canonical pool entries,
 * so pointer equality means content equality and we can key by the
 * raw pointer pair. The relation is asymmetric, so don't canonicalize
 * the order.
 */
KHASH_INIT(bitmap_subset, struct bitmap_or_key, int, 1,
	   bitmap_or_hash, bitmap_or_eq)
static kh_bitmap_subset_t *bitmap_subset_cache;

static int island_bitmap_is_subset(struct island_bitmap *self,
				   struct island_bitmap *super)
{
	struct bitmap_or_key key;
	khiter_t pos;
	int hash_ret, result;

	if (self == super)
		return 1;
	/*
	 * Canonical pool entries fold equal content into equal pointers,
	 * so a strict subset has strictly fewer bits. Equal popcount on
	 * distinct pointers therefore implies non-subset.
	 */
	if (self->popcount >= super->popcount)
		return 0;

	if (!bitmap_subset_cache)
		bitmap_subset_cache = kh_init_bitmap_subset();

	key.a = self;
	key.b = super;
	pos = kh_put_bitmap_subset(bitmap_subset_cache, key, &hash_ret);
	if (!hash_ret)
		return kh_value(bitmap_subset_cache, pos);

	result = island_bitmap_is_subset_uncached(self, super);
	kh_value(bitmap_subset_cache, pos) = result;
	return result;
}

static int island_bitmap_get(struct island_bitmap *self, uint32_t i)
{
	struct ewah_iterator it;
	eword_t word;
	uint32_t pos = 0;

	ewah_iterator_init(&it, self->ewah);
	while (ewah_iterator_next(&word, &it)) {
		if (i < pos + BITS_IN_EWORD)
			return (word >> (i - pos)) & 1;
		pos += BITS_IN_EWORD;
	}
	return 0;
}

static struct island_bitmap *single_bit_bitmap(uint32_t i)
{
	struct ewah_bitmap *fresh = ewah_new();
	ewah_set(fresh, i);
	/* A single-bit seed has popcount = 1 by construction. */
	return island_bitmap_intern(island_bitmap_wrap(fresh, 1));
}

int in_same_island(const struct object_id *trg_oid, const struct object_id *src_oid)
{
	khiter_t trg_pos, src_pos;

	/* If we aren't using islands, assume everything goes together. */
	if (!island_marks)
		return 1;

	/*
	 * If we don't have a bitmap for the target, we can delta it
	 * against anything -- it's not an important object
	 */
	trg_pos = kh_get_oid_map(island_marks, *trg_oid);
	if (trg_pos >= kh_end(island_marks))
		return 1;

	/*
	 * if the source (our delta base) doesn't have a bitmap,
	 * we don't want to base any deltas on it!
	 */
	src_pos = kh_get_oid_map(island_marks, *src_oid);
	if (src_pos >= kh_end(island_marks))
		return 0;

	return island_bitmap_is_subset(kh_value(island_marks, trg_pos),
				kh_value(island_marks, src_pos));
}

int island_delta_cmp(const struct object_id *a, const struct object_id *b)
{
	khiter_t a_pos, b_pos;
	struct island_bitmap *a_bitmap = NULL, *b_bitmap = NULL;

	if (!island_marks)
		return 0;

	a_pos = kh_get_oid_map(island_marks, *a);
	if (a_pos < kh_end(island_marks))
		a_bitmap = kh_value(island_marks, a_pos);

	b_pos = kh_get_oid_map(island_marks, *b);
	if (b_pos < kh_end(island_marks))
		b_bitmap = kh_value(island_marks, b_pos);

	if (a_bitmap) {
		if (!b_bitmap || !island_bitmap_is_subset(a_bitmap, b_bitmap))
			return -1;
	}
	if (b_bitmap) {
		if (!a_bitmap || !island_bitmap_is_subset(b_bitmap, a_bitmap))
			return 1;
	}

	return 0;
}

static void set_island_marks(struct object *obj, struct island_bitmap *marks)
{
	struct island_bitmap *b;
	khiter_t pos;
	int hash_ret;

	pos = kh_put_oid_map(island_marks, obj->oid, &hash_ret);
	if (hash_ret) {
		kh_value(island_marks, pos) = marks;
		return;
	}

	b = kh_value(island_marks, pos);
	if (b == marks)
		return;
	kh_value(island_marks, pos) = island_bitmap_or_intern(b, marks);
}

static void mark_remote_island_1(struct repository *r,
				 struct remote_island *rl,
				 int is_core_island)
{
	struct island_bitmap *seed = single_bit_bitmap(island_counter);
	uint32_t i;

	for (i = 0; i < rl->oids.nr; ++i) {
		struct object *obj = parse_object(r, &rl->oids.oid[i]);

		if (!obj)
			continue;

		set_island_marks(obj, seed);

		if (is_core_island && obj->type == OBJ_COMMIT)
			obj->flags |= NEEDS_BITMAP;

		/* If it was a tag, also make sure we hit the underlying object. */
		while (obj && obj->type == OBJ_TAG) {
			obj = ((struct tag *)obj)->tagged;
			if (obj) {
				parse_object(r, &obj->oid);
				set_island_marks(obj, seed);
			}
		}
	}

	if (is_core_island)
		island_counter_core = island_counter;

	island_counter++;
}

struct tree_islands_todo {
	struct object_entry *entry;
	unsigned int depth;
};

static int tree_depth_compare(const void *a, const void *b)
{
	const struct tree_islands_todo *todo_a = a;
	const struct tree_islands_todo *todo_b = b;

	return todo_a->depth - todo_b->depth;
}

void resolve_tree_islands(struct repository *r,
			  int progress,
			  struct packing_data *to_pack)
{
	struct progress *progress_state = NULL;
	struct tree_islands_todo *todo;
	int nr = 0;
	int i;

	if (!island_marks)
		return;

	/*
	 * We process only trees, as commits and tags have already been handled
	 * (and passed their marks on to root trees, as well. We must make sure
	 * to process them in descending tree-depth order so that marks
	 * propagate down the tree properly, even if a sub-tree is found in
	 * multiple parent trees.
	 */
	ALLOC_ARRAY(todo, to_pack->nr_objects);
	for (i = 0; i < to_pack->nr_objects; i++) {
		if (oe_type(&to_pack->objects[i]) == OBJ_TREE) {
			todo[nr].entry = &to_pack->objects[i];
			todo[nr].depth = oe_tree_depth(to_pack, &to_pack->objects[i]);
			nr++;
		}
	}
	QSORT(todo, nr, tree_depth_compare);

	if (progress)
		progress_state = start_progress(r, _("Propagating island marks"), nr);

	/*
	 * Project `island_marks` onto a flat array indexed by `to_pack`
	 * position. The inner loop then translates each tree entry's OID
	 * to a `to_pack` position via `packlist_find()` and reads/writes
	 * `bm[]` directly, avoiding the `kh_put_oid_map()` cost on every
	 * tree-entry visit. After the loop, the array is written back
	 * into `island_marks` so `in_same_island()` and friends see the
	 * final state.
	 */
	{
	struct island_bitmap **bm;

	CALLOC_ARRAY(bm, to_pack->nr_objects);
	for (i = 0; i < to_pack->nr_objects; i++) {
		khiter_t kpos = kh_get_oid_map(island_marks,
					       to_pack->objects[i].idx.oid);
		if (kpos < kh_end(island_marks))
			bm[i] = kh_value(island_marks, kpos);
	}

	for (i = 0; i < nr; i++) {
		struct object_entry *ent = todo[i].entry;
		struct island_bitmap *root_marks;
		struct tree *tree;
		struct tree_desc desc;
		struct name_entry entry;
		uint32_t idx = ent - to_pack->objects;

		root_marks = bm[idx];
		if (!root_marks)
			continue;

		tree = lookup_tree(r, &ent->idx.oid);
		if (!tree || repo_parse_tree(r, tree) < 0)
			die(_("bad tree object %s"), oid_to_hex(&ent->idx.oid));

		init_tree_desc(&desc, &tree->object.oid, tree->buffer, tree->size);
		while (tree_entry(&desc, &entry)) {
			struct object_entry *child;
			uint32_t cidx;
			struct island_bitmap *b;

			if (S_ISGITLINK(entry.mode))
				continue;

			child = packlist_find(to_pack, &entry.oid);
			if (!child)
				continue;
			cidx = child - to_pack->objects;

			b = bm[cidx];
			if (!b) {
				bm[cidx] = root_marks;
				continue;
			}
			if (b == root_marks)
				continue;
			bm[cidx] = island_bitmap_or_intern(b, root_marks);
		}

		free_tree_buffer(tree);

		display_progress(progress_state, i+1);
	}

	for (i = 0; i < to_pack->nr_objects; i++) {
		khiter_t kpos;
		int hash_ret;
		if (!bm[i])
			continue;
		kpos = kh_put_oid_map(island_marks,
				      to_pack->objects[i].idx.oid, &hash_ret);
		kh_value(island_marks, kpos) = bm[i];
	}
	free(bm);
	}

	stop_progress(&progress_state);
	free(todo);
}

struct island_load_data {
	kh_str_t *remote_islands;
	regex_t *rx;
	size_t nr;
	size_t alloc;
};
static char *core_island_name;

static void free_config_regexes(struct island_load_data *ild)
{
	for (size_t i = 0; i < ild->nr; i++)
		regfree(&ild->rx[i]);
	free(ild->rx);
}

static void free_remote_islands(kh_str_t *remote_islands)
{
	const char *island_name;
	struct remote_island *rl;

	kh_foreach(remote_islands, island_name, rl, {
		free((void *)island_name);
		oid_array_clear(&rl->oids);
		free(rl);
	});
	kh_destroy_str(remote_islands);
}

static int island_config_callback(const char *k, const char *v,
				  const struct config_context *ctx UNUSED,
				  void *cb)
{
	struct island_load_data *ild = cb;

	if (!strcmp(k, "pack.island")) {
		struct strbuf re = STRBUF_INIT;

		if (!v)
			return config_error_nonbool(k);

		ALLOC_GROW(ild->rx, ild->nr + 1, ild->alloc);

		if (*v != '^')
			strbuf_addch(&re, '^');
		strbuf_addstr(&re, v);

		if (regcomp(&ild->rx[ild->nr], re.buf, REG_EXTENDED))
			die(_("failed to load island regex for '%s': %s"), k, re.buf);

		strbuf_release(&re);
		ild->nr++;
		return 0;
	}

	if (!strcmp(k, "pack.islandcore"))
		return git_config_string(&core_island_name, k, v);

	return 0;
}

static void add_ref_to_island(kh_str_t *remote_islands, const char *island_name,
				const struct object_id *oid)
{
	uint64_t sha_core;
	struct remote_island *rl = NULL;

	int hash_ret;
	khiter_t pos = kh_put_str(remote_islands, island_name, &hash_ret);

	if (hash_ret) {
		kh_key(remote_islands, pos) = xstrdup(island_name);
		kh_value(remote_islands, pos) = xcalloc(1, sizeof(struct remote_island));
	}

	rl = kh_value(remote_islands, pos);
	oid_array_append(&rl->oids, oid);

	memcpy(&sha_core, oid->hash, sizeof(uint64_t));
	rl->hash += sha_core;
}

static int find_island_for_ref(const struct reference *ref, void *cb)
{
	struct island_load_data *ild = cb;

	/*
	 * We should advertise 'ARRAY_SIZE(matches) - 2' as the max,
	 * so we can diagnose below a config with more capture groups
	 * than we support.
	 */
	regmatch_t matches[16];
	int i, m;
	struct strbuf island_name = STRBUF_INIT;

	/* walk backwards to get last-one-wins ordering */
	for (i = ild->nr - 1; i >= 0; i--) {
		if (!regexec(&ild->rx[i], ref->name,
			     ARRAY_SIZE(matches), matches, 0))
			break;
	}

	if (i < 0)
		return 0;

	if (matches[ARRAY_SIZE(matches) - 1].rm_so != -1)
		warning(_("island regex from config has "
			  "too many capture groups (max=%d)"),
			(int)ARRAY_SIZE(matches) - 2);

	for (m = 1; m < ARRAY_SIZE(matches); m++) {
		regmatch_t *match = &matches[m];

		if (match->rm_so == -1)
			continue;

		if (island_name.len)
			strbuf_addch(&island_name, '-');

		strbuf_add(&island_name, ref->name + match->rm_so, match->rm_eo - match->rm_so);
	}

	add_ref_to_island(ild->remote_islands, island_name.buf, ref->oid);
	strbuf_release(&island_name);
	return 0;
}

static struct remote_island *get_core_island(kh_str_t *remote_islands)
{
	if (core_island_name) {
		khiter_t pos = kh_get_str(remote_islands, core_island_name);
		if (pos < kh_end(remote_islands))
			return kh_value(remote_islands, pos);
	}

	return NULL;
}

static void deduplicate_islands(kh_str_t *remote_islands, struct repository *r)
{
	struct remote_island *island, *core = NULL, **list;
	unsigned int island_count, dst, src, ref, i = 0;

	island_count = kh_size(remote_islands);
	ALLOC_ARRAY(list, island_count);

	kh_foreach_value(remote_islands, island, {
		list[i++] = island;
	});

	for (ref = 0; ref + 1 < island_count; ref++) {
		for (src = ref + 1, dst = src; src < island_count; src++) {
			if (list[ref]->hash == list[src]->hash)
				continue;

			if (src != dst)
				list[dst] = list[src];

			dst++;
		}
		island_count = dst;
	}

	island_bitmap_size = island_count;
	core = get_core_island(remote_islands);

	for (i = 0; i < island_count; ++i) {
		mark_remote_island_1(r, list[i], core && list[i]->hash == core->hash);
	}

	free(list);
}

void load_delta_islands(struct repository *r, int progress)
{
	struct island_load_data ild = { 0 };

	island_marks = kh_init_oid_map();

	repo_config(r, island_config_callback, &ild);
	ild.remote_islands = kh_init_str();
	refs_for_each_ref(get_main_ref_store(r),
			  find_island_for_ref, &ild);
	free_config_regexes(&ild);
	deduplicate_islands(ild.remote_islands, r);
	free_remote_islands(ild.remote_islands);

	if (progress)
		fprintf(stderr, _("Marked %d islands, done.\n"), island_counter);
}

void propagate_island_marks(struct repository *r, struct commit *commit)
{
	khiter_t pos = kh_get_oid_map(island_marks, commit->object.oid);

	if (pos < kh_end(island_marks)) {
		struct commit_list *p;
		struct island_bitmap *root_marks = kh_value(island_marks, pos);

		repo_parse_commit(r, commit);
		set_island_marks(&repo_get_commit_tree(r, commit)->object,
				 root_marks);
		for (p = commit->parents; p; p = p->next)
			set_island_marks(&p->item->object, root_marks);
	}
}

void free_island_marks(void)
{
	struct island_bitmap *bitmap;

	if (island_marks)
		kh_destroy_oid_map(island_marks);

	if (bitmap_pool) {
		kh_foreach_value(bitmap_pool, bitmap, {
			island_bitmap_destroy(bitmap);
		});
		kh_destroy_bitmap_pool(bitmap_pool);
		bitmap_pool = NULL;
	}

	if (bitmap_or_cache) {
		kh_destroy_bitmap_or(bitmap_or_cache);
		bitmap_or_cache = NULL;
	}

	if (bitmap_subset_cache) {
		kh_destroy_bitmap_subset(bitmap_subset_cache);
		bitmap_subset_cache = NULL;
	}

	/* detect use-after-free with a an address which is never valid: */
	island_marks = (void *)-1;
}

int compute_pack_layers(struct packing_data *to_pack)
{
	uint32_t i;

	if (!core_island_name || !island_marks)
		return 1;

	for (i = 0; i < to_pack->nr_objects; ++i) {
		struct object_entry *entry = &to_pack->objects[i];
		khiter_t pos = kh_get_oid_map(island_marks, entry->idx.oid);

		oe_set_layer(to_pack, entry, 1);

		if (pos < kh_end(island_marks)) {
			struct island_bitmap *bitmap = kh_value(island_marks, pos);

			if (island_bitmap_get(bitmap, island_counter_core))
				oe_set_layer(to_pack, entry, 0);
		}
	}

	return 2;
}
