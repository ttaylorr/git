#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "git-compat-util.h"
#include "dir.h"
#include "hex.h"
#include "midx.h"
#include "packfile.h"
#include "path.h"
#include "pack-bitmap.h"
#include "pack-geometry.h"
#include "refs.h"
#include "repack.h"
#include "run-command.h"
#include "tempfile.h"

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

int finish_pack_objects_cmd(struct child_process *cmd,
			    struct string_list *names,
			    const char *packtmp, int local)
{
	FILE *out;
	struct strbuf line = STRBUF_INIT;

	out = xfdopen(cmd->out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		struct string_list_item *item;

		if (line.len != the_hash_algo->hexsz)
			die(_("repack: Expecting full hex object ID lines only "
			      "from pack-objects."));
		/*
		 * Avoid putting packs written outside of the repository in the
		 * list of names.
		 */
		if (local) {
			item = string_list_append(names, line.buf);
			item->util = populate_pack_exts(line.buf, packtmp);
		}
	}
	fclose(out);

	strbuf_release(&line);

	return finish_command(cmd);
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

static struct {
	const char *name;
	unsigned optional:1;
} exts[] = {
	{".pack"},
	{".rev", 1},
	{".mtimes", 1},
	{".bitmap", 1},
	{".promisor", 1},
	{".idx"},
};

struct generated_pack_data {
	struct tempfile *tempfiles[ARRAY_SIZE(exts)];
};

struct generated_pack_data *populate_pack_exts(const char *name,
					       const char *packtmp)
{
	struct stat statbuf;
	struct strbuf path = STRBUF_INIT;
	struct generated_pack_data *data = xcalloc(1, sizeof(*data));
	int i;

	for (i = 0; i < ARRAY_SIZE(exts); i++) {
		strbuf_reset(&path);
		strbuf_addf(&path, "%s-%s%s", packtmp, name, exts[i].name);

		if (stat(path.buf, &statbuf))
			continue;

		data->tempfiles[i] = register_tempfile(path.buf);
	}

	strbuf_release(&path);
	return data;
}

int has_pack_ext(const struct generated_pack_data *data, const char *ext)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(exts); i++) {
		if (strcmp(exts[i].name, ext))
			continue;
		return !!data->tempfiles[i];
	}
	BUG("unknown pack extension: '%s'", ext);
}

void install_generated_pack(struct generated_pack_data *data,
			    const char *packdir, const char *packtmp,
			    const char *name)
{
	size_t ext;

	for (ext = 0; ext < ARRAY_SIZE(exts); ext++) {
		char *fname;

		fname = mkpathdup("%s/pack-%s%s",
				packdir, name, exts[ext].name);

		if (data->tempfiles[ext]) {
			const char *fname_old = get_tempfile_path(data->tempfiles[ext]);
			struct stat statbuffer;

			if (!stat(fname_old, &statbuffer)) {
				statbuffer.st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
				chmod(fname_old, statbuffer.st_mode);
			}

			if (rename_tempfile(&data->tempfiles[ext], fname))
				die_errno(_("renaming pack to '%s' failed"), fname);
		} else if (!exts[ext].optional)
			die(_("pack-objects did not write a '%s' file for pack %s-%s"),
			    exts[ext].name, packtmp, name);
		else if (unlink(fname) < 0 && errno != ENOENT)
			die_errno(_("could not unlink: %s"), fname);

		free(fname);
	}
}

struct midx_snapshot_ref_data {
	struct tempfile *f;
	struct oidset seen;
	int preferred;
};

static int midx_snapshot_ref_one(const char *refname UNUSED,
				 const char *referent UNUSED,
				 const struct object_id *oid,
				 int flag UNUSED, void *_data)
{
	struct midx_snapshot_ref_data *data = _data;
	struct object_id peeled;

	if (!peel_iterated_oid(the_repository, oid, &peeled))
		oid = &peeled;

	if (oidset_insert(&data->seen, oid))
		return 0; /* already seen */

	if (odb_read_object_info(the_repository->objects, oid, NULL) != OBJ_COMMIT)
		return 0;

	fprintf(data->f->fp, "%s%s\n", data->preferred ? "+" : "",
		oid_to_hex(oid));

	return 0;
}

void midx_snapshot_refs(struct tempfile *f)
{
	struct midx_snapshot_ref_data data;
	const struct string_list *preferred = bitmap_preferred_tips(the_repository);

	data.f = f;
	data.preferred = 0;
	oidset_init(&data.seen, 0);

	if (!fdopen_tempfile(f, "w"))
		 die(_("could not open tempfile %s for writing"),
		     get_tempfile_path(f));

	if (preferred) {
		struct string_list_item *item;

		data.preferred = 1;
		for_each_string_list_item(item, preferred)
			refs_for_each_ref_in(get_main_ref_store(the_repository),
					     item->string,
					     midx_snapshot_ref_one, &data);
		data.preferred = 0;
	}

	refs_for_each_ref(get_main_ref_store(the_repository),
			  midx_snapshot_ref_one, &data);

	if (close_tempfile_gently(f)) {
		int save_errno = errno;
		delete_tempfile(&f);
		errno = save_errno;
		die_errno(_("could not close refs snapshot tempfile"));
	}

	oidset_clear(&data.seen);
}

int midx_has_unknown_packs(struct string_list *midx_pack_names,
			   struct string_list *include,
			   struct pack_geometry *geometry,
			   struct existing_packs *existing)
{
	size_t i;

	string_list_sort(include);

	for (i = 0; i < midx_pack_names->nr; i++) {
		const char *pack_name = midx_pack_names->items[i].string;

		/*
		 * Determine whether or not each MIDX'd pack from the existing
		 * MIDX (if any) is represented in the new MIDX. For each pack
		 * in the MIDX, it must either be:
		 *
		 *  - In the "include" list of packs to be included in the new
		 *    MIDX. Note this function is called before the include
		 *    list is populated with any cruft pack(s).
		 *
		 *  - Below the geometric split line (if using pack geometry),
		 *    indicating that the pack won't be included in the new
		 *    MIDX, but its contents were rolled up as part of the
		 *    geometric repack.
		 *
		 *  - In the existing non-kept packs list (if not using pack
		 *    geometry), and marked as non-deleted.
		 */
		if (string_list_has_string(include, pack_name)) {
			continue;
		} else if (geometry) {
			struct strbuf buf = STRBUF_INIT;
			uint32_t j;

			for (j = 0; j < geometry->split; j++) {
				strbuf_reset(&buf);
				strbuf_addstr(&buf, pack_basename(geometry->pack[j]));
				strbuf_strip_suffix(&buf, ".pack");
				strbuf_addstr(&buf, ".idx");

				if (!strcmp(pack_name, buf.buf)) {
					strbuf_release(&buf);
					break;
				}
			}

			strbuf_release(&buf);

			if (j < geometry->split)
				continue;
		} else {
			struct string_list_item *item;

			item = string_list_lookup(&existing->non_kept_packs,
						  pack_name);
			if (item && !pack_is_marked_for_deletion(item))
				continue;
		}

		/*
		 * If we got to this point, the MIDX includes some pack that we
		 * don't know about.
		 */
		return 1;
	}

	return 0;
}

static void midx_included_packs(struct string_list *include,
				struct existing_packs *existing,
				struct string_list *midx_pack_names,
				struct string_list *names,
				struct pack_geometry *geometry,
				int midx_must_contain_cruft)
{
	struct string_list_item *item;
	struct strbuf buf = STRBUF_INIT;

	for_each_string_list_item(item, &existing->kept_packs) {
		strbuf_reset(&buf);
		strbuf_addf(&buf, "%s.idx", item->string);
		string_list_insert(include, buf.buf);
	}

	for_each_string_list_item(item, names) {
		strbuf_reset(&buf);
		strbuf_addf(&buf, "pack-%s.idx", item->string);
		string_list_insert(include, buf.buf);
	}

	if (geometry->split_factor) {
		uint32_t i;

		for (i = geometry->split; i < geometry->pack_nr; i++) {
			struct packed_git *p = geometry->pack[i];

			/*
			 * The multi-pack index never refers to packfiles part
			 * of an alternate object database, so we skip these.
			 * While git-multi-pack-index(1) would silently ignore
			 * them anyway, this allows us to skip executing the
			 * command completely when we have only non-local
			 * packfiles.
			 */
			if (!p->pack_local)
				continue;

			strbuf_reset(&buf);
			strbuf_addstr(&buf, pack_basename(p));
			strbuf_strip_suffix(&buf, ".pack");
			strbuf_addstr(&buf, ".idx");

			string_list_insert(include, buf.buf);
		}
	} else {
		for_each_string_list_item(item, &existing->non_kept_packs) {
			if (pack_is_marked_for_deletion(item))
				continue;

			strbuf_reset(&buf);
			strbuf_addf(&buf, "%s.idx", item->string);
			string_list_insert(include, buf.buf);
		}
	}

	if (midx_must_contain_cruft ||
	    midx_has_unknown_packs(midx_pack_names, include, geometry,
				   existing)) {
		/*
		 * If there are one or more unknown pack(s) present (see
		 * midx_has_unknown_packs() for what makes a pack
		 * "unknown") in the MIDX before the repack, keep them
		 * as they may be required to form a reachability
		 * closure if the MIDX is bitmapped.
		 *
		 * For example, a cruft pack can be required to form a
		 * reachability closure if the MIDX is bitmapped and one
		 * or more of the bitmap's selected commits reaches a
		 * once-cruft object that was later made reachable.
		 */
		for_each_string_list_item(item, &existing->cruft_packs) {
			/*
			 * When doing a --geometric repack, there is no
			 * need to check for deleted packs, since we're
			 * by definition not doing an ALL_INTO_ONE
			 * repack (hence no packs will be deleted).
			 * Otherwise we must check for and exclude any
			 * packs which are enqueued for deletion.
			 *
			 * So we could omit the conditional below in the
			 * --geometric case, but doing so is unnecessary
			 *  since no packs are marked as pending
			 *  deletion (since we only call
			 *  `mark_packs_for_deletion()` when doing an
			 *  all-into-one repack).
			 */
			if (pack_is_marked_for_deletion(item))
				continue;

			strbuf_reset(&buf);
			strbuf_addf(&buf, "%s.idx", item->string);
			string_list_insert(include, buf.buf);
		}
	} else {
		/*
		 * Modern versions of Git (with the appropriate
		 * configuration setting) will write new copies of
		 * once-cruft objects when doing a --geometric repack.
		 *
		 * If the MIDX has no cruft pack, new packs written
		 * during a --geometric repack will not rely on the
		 * cruft pack to form a reachability closure, so we can
		 * avoid including them in the MIDX in that case.
		 */
		;
	}

	strbuf_release(&buf);
}

int write_midx_included_packs(struct string_list *included,
			      struct existing_packs *existing,
			      struct pack_geometry *geometry,
			      struct string_list *names,
			      struct string_list *midx_pack_names,
			      const char *refs_snapshot,
			      int show_progress, int write_bitmaps,
			      int midx_must_contain_cruft)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	struct packed_git *preferred = geometry_preferred_pack(geometry);
	FILE *in;
	int ret;

	midx_included_packs(included, existing, midx_pack_names, names,
			    geometry, midx_must_contain_cruft);

	if (!included->nr)
		return 0;

	cmd.in = -1;
	cmd.git_cmd = 1;

	strvec_push(&cmd.args, "multi-pack-index");
	strvec_pushl(&cmd.args, "write", "--stdin-packs", NULL);

	if (show_progress)
		strvec_push(&cmd.args, "--progress");
	else
		strvec_push(&cmd.args, "--no-progress");

	if (write_bitmaps)
		strvec_push(&cmd.args, "--bitmap");

	if (preferred)
		strvec_pushf(&cmd.args, "--preferred-pack=%s",
			     pack_basename(preferred));
	else if (names->nr) {
		/* The largest pack was repacked, meaning that either
		 * one or two packs exist depending on whether the
		 * repository has a cruft pack or not.
		 *
		 * Select the non-cruft one as preferred to encourage
		 * pack-reuse among packs containing reachable objects
		 * over unreachable ones.
		 *
		 * (Note we could write multiple packs here if
		 * `--max-pack-size` was given, but any one of them
		 * will suffice, so pick the first one.)
		 */
		for_each_string_list_item(item, names) {
			struct generated_pack_data *data = item->util;
			if (has_pack_ext(data, ".mtimes"))
				continue;

			strvec_pushf(&cmd.args, "--preferred-pack=pack-%s.pack",
				     item->string);
			break;
		}
	} else {
		/*
		 * No packs were kept, and no packs were written. The
		 * only thing remaining are .keep packs (unless
		 * --pack-kept-objects was given).
		 *
		 * Set the `--preferred-pack` arbitrarily here.
		 */
		;
	}

	if (refs_snapshot)
		strvec_pushf(&cmd.args, "--refs-snapshot=%s", refs_snapshot);

	ret = start_command(&cmd);
	if (ret)
		return ret;

	in = xfdopen(cmd.in, "w");
	for_each_string_list_item(item, included)
		fprintf(in, "%s\n", item->string);
	fclose(in);

	return finish_command(&cmd);
}
