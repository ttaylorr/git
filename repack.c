#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "git-compat-util.h"
#include "dir.h"
#include "midx.h"
#include "packfile.h"
#include "repack.h"
#include "run-command.h"

#define DELETE_PACK 1
#define RETAIN_PACK 2

void prepare_pack_objects(struct child_process *cmd,
			  const struct pack_objects_args *args,
			  const char *out)
{
	strvec_push(&cmd->args, "pack-objects");
	if (args->window)
		strvec_pushf(&cmd->args, "--window=%s", args->window);
	if (args->window_memory)
		strvec_pushf(&cmd->args, "--window-memory=%s", args->window_memory);
	if (args->depth)
		strvec_pushf(&cmd->args, "--depth=%s", args->depth);
	if (args->threads)
		strvec_pushf(&cmd->args, "--threads=%s", args->threads);
	if (args->max_pack_size)
		strvec_pushf(&cmd->args, "--max-pack-size=%lu", args->max_pack_size);
	if (args->no_reuse_delta)
		strvec_pushf(&cmd->args, "--no-reuse-delta");
	if (args->no_reuse_object)
		strvec_pushf(&cmd->args, "--no-reuse-object");
	if (args->name_hash_version)
		strvec_pushf(&cmd->args, "--name-hash-version=%d", args->name_hash_version);
	if (args->path_walk)
		strvec_pushf(&cmd->args, "--path-walk");
	if (args->local)
		strvec_push(&cmd->args,  "--local");
	if (args->quiet)
		strvec_push(&cmd->args,  "--quiet");
	if (args->delta_base_offset)
		strvec_push(&cmd->args,  "--delta-base-offset");
	strvec_push(&cmd->args, out);
	cmd->git_cmd = 1;
	cmd->out = -1;
}

void pack_objects_args_release(struct pack_objects_args *args)
{
	free(args->window);
	free(args->window_memory);
	free(args->depth);
	free(args->threads);
	list_objects_filter_release(&args->filter_options);
}

int has_existing_non_kept_packs(const struct existing_packs *existing)
{
	return existing->non_kept_packs.nr || existing->cruft_packs.nr;
}

void pack_mark_for_deletion(struct string_list_item *item)
{
	item->util = (void*)((uintptr_t)item->util | DELETE_PACK);
}

void pack_unmark_for_deletion(struct string_list_item *item)
{
	item->util = (void*)((uintptr_t)item->util & ~DELETE_PACK);
}

int pack_is_marked_for_deletion(struct string_list_item *item)
{
	return (uintptr_t)item->util & DELETE_PACK;
}

void pack_mark_retained(struct string_list_item *item)
{
	item->util = (void*)((uintptr_t)item->util | RETAIN_PACK);
}

int pack_is_retained(struct string_list_item *item)
{
	return (uintptr_t)item->util & RETAIN_PACK;
}

static void mark_packs_for_deletion_1(struct string_list *names,
				      struct string_list *list)
{
	struct string_list_item *item;
	const int hexsz = the_hash_algo->hexsz;

	for_each_string_list_item(item, list) {
		char *sha1;
		size_t len = strlen(item->string);
		if (len < hexsz)
			continue;
		sha1 = item->string + len - hexsz;

		if (pack_is_retained(item)) {
			pack_unmark_for_deletion(item);
		} else if (!string_list_has_string(names, sha1)) {
			/*
			 * Mark this pack for deletion, which ensures
			 * that this pack won't be included in a MIDX
			 * (if `--write-midx` was given) and that we
			 * will actually delete this pack (if `-d` was
			 * given).
			 */
			pack_mark_for_deletion(item);
		}
	}
}

void retain_cruft_pack(struct existing_packs *existing,
		       struct packed_git *cruft)
{
	struct strbuf buf = STRBUF_INIT;
	struct string_list_item *item;

	strbuf_addstr(&buf, pack_basename(cruft));
	strbuf_strip_suffix(&buf, ".pack");

	item = string_list_lookup(&existing->cruft_packs, buf.buf);
	if (!item)
		BUG("could not find cruft pack '%s'", pack_basename(cruft));

	pack_mark_retained(item);
	strbuf_release(&buf);
}

void mark_packs_for_deletion(struct existing_packs *existing,
			     struct string_list *names)

{
	mark_packs_for_deletion_1(names, &existing->non_kept_packs);
	mark_packs_for_deletion_1(names, &existing->cruft_packs);
}

void remove_redundant_pack(const char *dir_name, const char *base_name)
{
	struct strbuf buf = STRBUF_INIT;
	struct multi_pack_index *m = get_local_multi_pack_index(the_repository);
	strbuf_addf(&buf, "%s.pack", base_name);
	if (m && midx_contains_pack(m, buf.buf))
		clear_midx_file(the_repository);
	strbuf_insertf(&buf, 0, "%s/", dir_name);
	unlink_pack_path(buf.buf, 1);
	strbuf_release(&buf);
}

static void remove_redundant_packs_1(struct string_list *packs,
				     const char *packdir)
{
	struct string_list_item *item;
	for_each_string_list_item(item, packs) {
		if (!pack_is_marked_for_deletion(item))
			continue;
		remove_redundant_pack(packdir, item->string);
	}
}

void remove_redundant_existing_packs(struct existing_packs *existing,
				     const char *packdir)
{
	remove_redundant_packs_1(&existing->non_kept_packs, packdir);
	remove_redundant_packs_1(&existing->cruft_packs, packdir);
}

void existing_packs_release(struct existing_packs *existing)
{
	string_list_clear(&existing->kept_packs, 0);
	string_list_clear(&existing->non_kept_packs, 0);
	string_list_clear(&existing->cruft_packs, 0);
}

void collect_pack_filenames(struct existing_packs *existing,
			    const struct string_list *extra_keep)
{
	struct packed_git *p;
	struct strbuf buf = STRBUF_INIT;

	for (p = get_all_packs(the_repository); p; p = p->next) {
		int i;
		const char *base;

		if (!p->pack_local)
			continue;

		base = pack_basename(p);

		for (i = 0; i < extra_keep->nr; i++)
			if (!fspathcmp(base, extra_keep->items[i].string))
				break;

		strbuf_reset(&buf);
		strbuf_addstr(&buf, base);
		strbuf_strip_suffix(&buf, ".pack");

		if ((extra_keep->nr > 0 && i < extra_keep->nr) || p->pack_keep)
			string_list_append(&existing->kept_packs, buf.buf);
		else if (p->is_cruft)
			string_list_append(&existing->cruft_packs, buf.buf);
		else
			string_list_append(&existing->non_kept_packs, buf.buf);
	}

	string_list_sort(&existing->kept_packs);
	string_list_sort(&existing->non_kept_packs);
	string_list_sort(&existing->cruft_packs);
	strbuf_release(&buf);
}
