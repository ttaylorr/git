#ifndef REPACK_H
#define REPACK_H

#include "list-objects-filter-options.h"
#include "string-list.h"

struct packed_git;
struct child_process;
struct tempfile;
struct pack_geometry;

struct pack_objects_args {
	char *window;
	char *window_memory;
	char *depth;
	char *threads;
	unsigned long max_pack_size;
	int no_reuse_delta;
	int no_reuse_object;
	int quiet;
	int local;
	int name_hash_version;
	int path_walk;
	int delta_base_offset;
	struct list_objects_filter_options filter_options;
};

#define PACK_OBJECTS_ARGS_INIT { .delta_base_offset = 1 }

void prepare_pack_objects(struct child_process *cmd,
			  const struct pack_objects_args *args,
			  const char *out);

int finish_pack_objects_cmd(struct child_process *cmd,
			    struct string_list *names,
			    const char *packtmp, int local);

void pack_objects_args_release(struct pack_objects_args *args);

void repack_promisor_objects(const struct pack_objects_args *args,
			     struct string_list *names,
			     char *packtmp);

struct existing_packs {
	struct string_list kept_packs;
	struct string_list non_kept_packs;
	struct string_list cruft_packs;
};

#define EXISTING_PACKS_INIT { \
	.kept_packs = STRING_LIST_INIT_DUP, \
	.non_kept_packs = STRING_LIST_INIT_DUP, \
	.cruft_packs = STRING_LIST_INIT_DUP, \
}

int has_existing_non_kept_packs(const struct existing_packs *existing);

void pack_mark_for_deletion(struct string_list_item *item);
void pack_unmark_for_deletion(struct string_list_item *item);
int pack_is_marked_for_deletion(struct string_list_item *item);

void pack_mark_retained(struct string_list_item *item);
int pack_is_retained(struct string_list_item *item);
void retain_cruft_pack(struct existing_packs *existing,
		       struct packed_git *cruft);

void mark_packs_for_deletion(struct existing_packs *existing,
			     struct string_list *names);

void remove_redundant_pack(const char *dir_name, const char *base_name);
void remove_redundant_existing_packs(struct existing_packs *existing,
				     const char *packdir);

/*
 * Adds all packs hex strings (pack-$HASH) to either packs->non_kept
 * or packs->kept based on whether each pack has a corresponding
 * .keep file or not.  Packs without a .keep file are not to be kept
 * if we are going to pack everything into one file.
 */
void collect_pack_filenames(struct existing_packs *existing,
			    const struct string_list *extra_keep);

void existing_packs_release(struct existing_packs *existing);

struct generated_pack_data;

struct generated_pack_data *populate_pack_exts(const char *name,
					       const char *packtmp);
void install_generated_pack(struct generated_pack_data *data,
			    const char *packdir, const char *packtmp,
			    const char *name);

void midx_snapshot_refs(struct tempfile *f);
int midx_has_unknown_packs(struct string_list *midx_pack_names,
			   struct string_list *include,
			   struct pack_geometry *geometry,
			   struct existing_packs *existing);
int write_midx_included_packs(struct existing_packs *existing,
			      struct pack_geometry *geometry,
			      struct string_list *names,
			      struct string_list *midx_pack_names,
			      const char *refs_snapshot,
			      const char *packdir,
			      int show_progress, int write_bitmaps,
			      int midx_must_contain_cruft);

#endif /* REPACK_H */
