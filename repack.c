#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#define PLAN_VERBOSE

#include "git-compat-util.h"
#include "dir.h"
#include "hex.h"
#include "lockfile.h"
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

static struct generated_pack_data *populate_pack_exts(const char *name,
						      const char *packtmp);

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

/*
 * Write oid to the given struct child_process's stdin, starting it first if
 * necessary.
 */
static int write_oid(const struct object_id *oid,
		     struct packed_git *pack UNUSED,
		     uint32_t pos UNUSED, void *data)
{
	struct child_process *cmd = data;

	if (cmd->in == -1) {
		if (start_command(cmd))
			die(_("could not start pack-objects to repack promisor objects"));
	}

	if (write_in_full(cmd->in, oid_to_hex(oid), the_hash_algo->hexsz) < 0 ||
	    write_in_full(cmd->in, "\n", 1) < 0)
		die(_("failed to feed promisor objects to pack-objects"));
	return 0;
}

void repack_promisor_objects(const struct pack_objects_args *args,
			     struct string_list *names,
			     char *packtmp)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	FILE *out;
	struct strbuf line = STRBUF_INIT;

	prepare_pack_objects(&cmd, args, packtmp);
	cmd.in = -1;

	/*
	 * NEEDSWORK: Giving pack-objects only the OIDs without any ordering
	 * hints may result in suboptimal deltas in the resulting pack. See if
	 * the OIDs can be sent with fake paths such that pack-objects can use a
	 * {type -> existing pack order} ordering when computing deltas instead
	 * of a {type -> size} ordering, which may produce better deltas.
	 */
	for_each_packed_object(the_repository, write_oid, &cmd,
			       FOR_EACH_OBJECT_PROMISOR_ONLY);

	if (cmd.in == -1) {
		/* No packed objects; cmd was never started */
		child_process_clear(&cmd);
		return;
	}

	close(cmd.in);

	out = xfdopen(cmd.out, "r");
	while (strbuf_getline_lf(&line, out) != EOF) {
		struct string_list_item *item;
		char *promisor_name;

		if (line.len != the_hash_algo->hexsz)
			die(_("repack: Expecting full hex object ID lines only from pack-objects."));
		item = string_list_append(names, line.buf);

		/*
		 * pack-objects creates the .pack and .idx files, but not the
		 * .promisor file. Create the .promisor file, which is empty.
		 *
		 * NEEDSWORK: fetch-pack sometimes generates non-empty
		 * .promisor files containing the ref names and associated
		 * hashes at the point of generation of the corresponding
		 * packfile, but this would not preserve their contents. Maybe
		 * concatenate the contents of all .promisor files instead of
		 * just creating a new empty file.
		 */
		promisor_name = mkpathdup("%s-%s.promisor", packtmp,
					  line.buf);
		write_promisor_file(promisor_name, NULL, 0);

		item->util = populate_pack_exts(item->string, packtmp);

		free(promisor_name);
	}

	fclose(out);
	if (finish_command(&cmd))
		die(_("could not finish pack-objects to repack promisor objects"));
	strbuf_release(&line);
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

static struct generated_pack_data *populate_pack_exts(const char *name,
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

static int generated_pack_has_ext(const struct generated_pack_data *data,
				  const char *ext)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(exts); i++) {
		if (strcmp(exts[i].name, ext))
			continue;
		return !!data->tempfiles[i];
	}
	BUG("unknown pack extension: '%s'", ext);
}

static void install_generated_pack(struct generated_pack_data *data,
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

void install_generated_packs(struct string_list *names, const char *packdir,
			     const char *packtmp)
{
	struct string_list_item *item;

	for_each_string_list_item(item, names) {
		struct generated_pack_data *data = item->util;

		install_generated_pack(data, packdir, packtmp, item->string);
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

static int midx_has_unknown_packs(struct string_list *midx_pack_names,
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

static void midx_included_packs(struct repack_midx_opts *opts,
				struct string_list *include)
{
	struct string_list_item *item;
	struct strbuf buf = STRBUF_INIT;

	for_each_string_list_item(item, &opts->existing->kept_packs) {
		strbuf_reset(&buf);
		strbuf_addf(&buf, "%s.idx", item->string);
		string_list_insert(include, buf.buf);
	}

	for_each_string_list_item(item, opts->names) {
		strbuf_reset(&buf);
		strbuf_addf(&buf, "pack-%s.idx", item->string);
		string_list_insert(include, buf.buf);
	}

	if (opts->geometry->split_factor) {
		uint32_t i;

		for (i = opts->geometry->split; i < opts->geometry->pack_nr; i++) {
			struct packed_git *p = opts->geometry->pack[i];

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
		for_each_string_list_item(item,
					  &opts->existing->non_kept_packs) {
			if (pack_is_marked_for_deletion(item))
				continue;

			strbuf_reset(&buf);
			strbuf_addf(&buf, "%s.idx", item->string);
			string_list_insert(include, buf.buf);
		}
	}

	if (opts->midx_must_contain_cruft ||
	    midx_has_unknown_packs(opts->midx_pack_names, include, opts->geometry,
				   opts->existing)) {
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
		for_each_string_list_item(item, &opts->existing->cruft_packs) {
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

static void remove_redundant_bitmaps(struct string_list *include,
				     const char *packdir)
{
	struct strbuf path = STRBUF_INIT;
	struct string_list_item *item;
	size_t packdir_len;

	strbuf_addstr(&path, packdir);
	strbuf_addch(&path, '/');
	packdir_len = path.len;

	/*
	 * Remove any pack bitmaps corresponding to packs which are now
	 * included in the MIDX.
	 */
	for_each_string_list_item(item, include) {
		strbuf_addstr(&path, item->string);
		strbuf_strip_suffix(&path, ".idx");
		strbuf_addstr(&path, ".bitmap");

		if (unlink(path.buf) && errno != ENOENT)
			warning_errno(_("could not remove stale bitmap: %s"),
				      path.buf);

		strbuf_setlen(&path, packdir_len);
	}
	strbuf_release(&path);
}

static void prepare_midx_command(struct child_process *cmd,
				 struct repack_midx_opts *opts,
				 const char *verb)
{
	cmd->git_cmd = 1;

	strvec_pushl(&cmd->args, "multi-pack-index", verb, NULL);

	if (opts->show_progress)
		strvec_push(&cmd->args, "--progress");
	else
		strvec_push(&cmd->args, "--no-progress");

	if (opts->write_bitmaps > 0)
		strvec_push(&cmd->args, "--bitmap");

	/*
	 * TODO(@ttaylorr): compaction should understand how to deal
	 * with refs-snapshot
	 */
	if (strcmp(verb, "compact")) {
		if (opts->refs_snapshot)
			strvec_pushf(&cmd->args, "--refs-snapshot=%s",
				     get_tempfile_path(opts->refs_snapshot));
	}
}

static int fill_midx_stdin_packs(struct child_process *cmd,
				 struct string_list *include,
				 struct string_list *out)
{
	struct string_list_item *item;
	FILE *in;
	int ret;

	cmd->in = -1;
	if (out)
		cmd->out = -1;

	ret = start_command(cmd);
	if (ret)
		return ret;

	in = xfdopen(cmd->in, "w");
	for_each_string_list_item(item, include)
		fprintf(in, "%s\n", item->string);
	fclose(in);

	if (out) {
		struct strbuf buf = STRBUF_INIT;
		FILE *outf = xfdopen(cmd->out, "r");

		while (strbuf_getline_lf(&buf, outf) != EOF)
			string_list_append(out, buf.buf);
		strbuf_release(&buf);

		fclose(outf);
	}

	return finish_command(cmd);
}

int write_midx_included_packs(struct repack_midx_opts *opts)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	struct packed_git *preferred = geometry_preferred_pack(opts->geometry);
	struct string_list include = STRING_LIST_INIT_DUP;
	int ret = 0;

	midx_included_packs(opts, &include);

	if (!include.nr)
		goto out;

	prepare_midx_command(&cmd, opts, "write");
	strvec_push(&cmd.args, "--stdin-packs");

	if (preferred)
		strvec_pushf(&cmd.args, "--preferred-pack=%s",
			     pack_basename(preferred));
	else if (opts->names->nr) {
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
		for_each_string_list_item(item, opts->names) {
			struct generated_pack_data *data = item->util;
			if (generated_pack_has_ext(data, ".mtimes"))
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

	ret = fill_midx_stdin_packs(&cmd, &include, NULL);
out:
	if (!ret && opts->write_bitmaps)
		remove_redundant_bitmaps(&include, opts->packdir);

	string_list_clear(&include, 0);

	return ret;
}

struct midx_compaction_step {
	union {
		struct multi_pack_index *midx;
		struct {
			struct multi_pack_index *from;
			struct multi_pack_index *to;
		} compact;
		struct string_list packs;
	} u;
	uint32_t num_objects;
	const char *result;

	enum {
		MIDX_UNKNOWN,
		MIDX_KEEP_AS_IS,
		MIDX_WRITE_PACKS,
		MIDX_COMPACT_MIDXS,
	} type;
};

static const char *midx_compaction_step_base(struct midx_compaction_step *step)
{
	switch (step->type) {
	case MIDX_WRITE_PACKS:
		BUG("cannot use a MIDX_WRTIE_PACKS step as a base");
	case MIDX_KEEP_AS_IS:
		return hash_to_hex(get_midx_checksum(step->u.midx));
	case MIDX_COMPACT_MIDXS:
		return hash_to_hex(get_midx_checksum(step->u.compact.to));
	default:
		BUG("unknown MIDX compaction step type: %d", step->type);
	}
}

static int midx_compaction_step_exec(struct midx_compaction_step *step,
				     struct repack_midx_opts *opts,
				     const char *base)
{
	FILE *out;
	struct strbuf buf = STRBUF_INIT;
	const unsigned char *from, *to;
	const char *preferred_pack = NULL;
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list hash = STRING_LIST_INIT_DUP;
	int ret = 0;

	switch (step->type) {
	case MIDX_KEEP_AS_IS:
		step->result = xstrdup(hash_to_hex(get_midx_checksum(step->u.midx)));
#ifdef PLAN_VERBOSE
		warning("%s:%d: [EVAL] keeping existing MIDX %s as-is", __FILE__, __LINE__, step->result);
#endif
		break;
	case MIDX_WRITE_PACKS:
		if (!step->u.packs.nr) {
			ret = error(_("no packs to write MIDX from"));
			goto out;
		}

		warning("%s:%d: [EVAL] writing new MIDX (base=%s)", __FILE__, __LINE__, base ? base : "<none>");
		for (size_t i = 0; i < step->u.packs.nr; i++) {
			if (step->u.packs.items[i].util) {
				preferred_pack = step->u.packs.items[i].string;
#ifdef PLAN_VERBOSE
				warning("  including pack %s <- ",
					step->u.packs.items[i].string);
#endif
			} else {
#ifdef PLAN_VERBOSE
				warning("  including pack %s",
					step->u.packs.items[i].string);
#endif
				;
			}
		}

		prepare_midx_command(&cmd, opts, "write");
		strvec_pushl(&cmd.args, "--stdin-packs", "--incremental",
			     "--print-checksum", NULL);

		if (preferred_pack)
			strvec_pushf(&cmd.args, "--preferred-pack=%s",
				     preferred_pack);

		strvec_pushl(&cmd.args, "--base", base ? base : "none", NULL);

		ret = fill_midx_stdin_packs(&cmd, &step->u.packs, &hash);

		if (hash.nr != 1) {
			ret = error(_("expected exactly one line, got: %"PRIuMAX),
				    (uintmax_t)hash.nr);
			goto out;
		}
		step->result = xstrdup(hash.items[0].string);

		break;
	case MIDX_COMPACT_MIDXS:
		prepare_midx_command(&cmd, opts, "compact");

		strvec_pushl(&cmd.args, "--incremental", "--print-checksum",
			     NULL);

		from = get_midx_checksum(step->u.compact.from);
		to = get_midx_checksum(step->u.compact.to);

		strvec_push(&cmd.args, hash_to_hex(from));
		strvec_push(&cmd.args, hash_to_hex(to));

#ifdef PLAN_VERBOSE
		warning("%s:%d: [EVAL] compacting MIDX", __FILE__, __LINE__);
		warning("  from=%s", hash_to_hex(from));
		warning("  to  =%s", hash_to_hex(to));
#endif

		cmd.out = -1;

		ret = start_command(&cmd);
		if (ret)
			return ret;

		out = xfdopen(cmd.out, "r");
		while (strbuf_getline_lf(&buf, out) != EOF) {
			if (step->result)
				BUG("unexpected output: %s", buf.buf);
			step->result = strbuf_detach(&buf, NULL);
		}
		fclose(out);

		ret = finish_command(&cmd);
		strbuf_release(&buf);

		break;
	default:
		BUG("unknown MIDX compaction step type: %d", step->type);
	}

out:
	return ret;
}

static int make_compaction_plan(struct repack_midx_opts *opts,
				struct midx_compaction_step **steps_p,
				size_t *steps_nr_p)
{
	struct midx_compaction_step *steps = NULL;
	struct midx_compaction_step step = { 0 };
	size_t steps_alloc = 0, steps_nr = 0;

	struct multi_pack_index *m;
	struct strbuf buf = STRBUF_INIT;
	uint32_t i;

	reprepare_packed_git(the_repository);
	m = get_multi_pack_index(the_repository);

	for (i = 0; m && i < m->num_packs + m->num_packs_in_base; i++) {
		if (prepare_midx_pack(the_repository, m, i))
			return error(_("could not load pack %u from MIDX"), i);
	}

	/*
	 * The first MIDX in the resulting chain is always going to be
	 * new.
	 *
	 * At a minimum, it will include all of the newly rewritten
	 * packs. If the tip MIDX was rewritten, it will also include
	 * any of its packs which were *not* rolled up as part of the
	 * geometric repack.
	 *
	 * It may grow to include the packs from zero or more MIDXs
	 * from the old chain, beginning either at the old tip (if the
	 * MIDX tip was *not* rewritten) or the old tip's base
	 * (otherwise).
	 */
	step.type = MIDX_WRITE_PACKS;
	string_list_init_nodup(&step.u.packs);

	/* First include all of the newly written packs. */
	for (i = 0; i < opts->names->nr; i++) {
#ifdef PLAN_VERBOSE
		warning("%s:%d adding new pack: %s", __FILE__, __LINE__, opts->names->items[i].string);
#endif
		strbuf_addf(&buf, "pack-%s.idx", opts->names->items[i].string);
		string_list_append(&step.u.packs, strbuf_detach(&buf, NULL));
	}
	for (i = 0; i < opts->geometry->split; i++)
		step.num_objects = u32_add(step.num_objects,
					   opts->geometry->pack[i]->num_objects);

	/*
	 * Then handle existing packs which were not rewritten.
	 *
	 * The list of packs in opts->geometry only contains MIDX'd
	 * packs from the youngest layer when that layer has more than
	 * repack.midxNewLayerThreshold packs.
	 *
	 * If the MIDX tip was rewritten (i.e. one or more of those
	 * packs appear below the split line), then add all packs above
	 * the split line must be added to the new MIDX layer, since the
	 * old one is no longer usable.
	 *
	 * If the MIDX tip was not rewritten (i.e. all MIDX'd packs from
	 * the youngest layer appear above the split line), ignore them
	 * since we will retain the existing MIDX layer as-is.
	 */
	for (i = opts->geometry->split; i < opts->geometry->pack_nr; i++) {
		struct packed_git *p = opts->geometry->pack[i];
		struct string_list_item *item;

		strbuf_reset(&buf);
		strbuf_addstr(&buf, pack_basename(p));
		strbuf_strip_suffix(&buf, ".pack");
		strbuf_addstr(&buf, ".idx");

#ifdef PLAN_VERBOSE
		warning("p->multi_pack_index=%d, opts->geometry->midx_tip_rewritten=%d",
			p->multi_pack_index,
			opts->geometry->midx_tip_rewritten);
#endif
		if (p->multi_pack_index && !opts->geometry->midx_tip_rewritten) {
#ifdef PLAN_VERBOSE
			warning("%s:%d skipping old pack: %s", __FILE__, __LINE__, buf.buf);
#endif
			continue;
		}

#ifdef PLAN_VERBOSE
		warning("%s:%d adding old pack: %s", __FILE__, __LINE__, buf.buf);
#endif

		item = string_list_append(&step.u.packs,
					  strbuf_detach(&buf, NULL));
		if (p->multi_pack_index || i == opts->geometry->pack_nr - 1)
			item->util = (void *)1; /* mark as preferred */

		step.num_objects = u32_add(step.num_objects, p->num_objects);
	}

	/*
	 * If the MIDX tip was rewritten, then we no longer consider
	 * it a candidate for compaction, since it will not exist in
	 * the resultant MIDX chain.
	 */
	if (opts->geometry->midx_tip_rewritten) {
#ifdef PLAN_VERBOSE
		warning("%s:%d: MIDX tip was rewritten (%s -> %s)", __FILE__, __LINE__,
			hash_to_hex(get_midx_checksum(m)),
			m->base_midx ?  hash_to_hex(get_midx_checksum(m->base_midx)) : "<none>");
#endif
		m = m->base_midx;
	} else {
#ifdef PLAN_VERBOSE
		warning("%s:%d: MIDX tip kept as-is (%s)", __FILE__, __LINE__,
			m ? hash_to_hex(get_midx_checksum(m)) : "<none>");
#endif
		;
	}

	/*
	 * Compact additional MIDX layers into this proposed one until
	 * the merging condition is violated.
	 */
	while (m) {
		uint32_t preferred_pack_idx;
#ifdef PLAN_VERBOSE
		warning("evaluating existing MIDX: %s",
			hash_to_hex(get_midx_checksum(m)));
#endif
		if (step.num_objects < m->num_objects / opts->midx_split_factor) {
			/*
			 * Stop compacting MIDXs as soon as the merged
			 * size is less than half of the size of the
			 * next MIDX in the chain.
			 */
#ifdef PLAN_VERBOSE
			warning(" STOP step_nr: %"PRIuMAX", m_nr: %"PRIuMAX,
				(uintmax_t)step.num_objects, (uintmax_t)m->num_objects);
#endif
			break;
		}
#ifdef PLAN_VERBOSE
		warning(" GO step_nr: %"PRIuMAX", m_nr: %"PRIuMAX,
			(uintmax_t)step.num_objects, (uintmax_t)m->num_objects);
#endif

		if (midx_preferred_pack(m, &preferred_pack_idx) < 0)
			return error(_("could not determine preferred pack for %s"),
				     hash_to_hex(get_midx_checksum(m)));

		for (i = 0; i < m->num_packs; i++) {
			struct string_list_item *item;
			uint32_t pack_int_id = i + m->num_packs_in_base;
			struct packed_git *p = nth_midxed_pack(m, pack_int_id);

			strbuf_reset(&buf);
			strbuf_addstr(&buf, pack_basename(p));
			strbuf_strip_suffix(&buf, ".pack");
			strbuf_addstr(&buf, ".idx");

			item = string_list_append(&step.u.packs,
						  strbuf_detach(&buf, NULL));

			if (i + m->num_packs_in_base == preferred_pack_idx)
				item->util = (void *)1;
		}

		step.num_objects = u32_add(step.num_objects, m->num_objects);
		m = m->base_midx;
#ifdef PLAN_VERBOSE
		warning("backing up");
#endif
	}

	/*
	 * In the special case where no new packs were written, avoid
	 * writing a bogus step into the plan.
	 */
	if (step.u.packs.nr > 0) {
		ALLOC_GROW(steps, steps_nr + 1, steps_alloc);
		steps[steps_nr++] = step;
#ifdef PLAN_VERBOSE
		warning("%s:%d: adding first step of type %d", __FILE__, __LINE__, step.type);
#endif
	}


	/*
	 * Then start over, repeat, and either compact or keep as-is
	 * until we have exhausted the chain.
	 *
	 * Finally, evaluate the remainder of the MIDX chain (if any)
	 * and either compact a sequence of adjacent layers or keep
	 * individual layers as-is according to the same merging
	 * condition as above.
	 */
#ifdef PLAN_VERBOSE
	warning("considering remaining MIDXs: %s", m ?
		hash_to_hex(get_midx_checksum(m)) : "<none>");
#endif
	while (m) {
		struct multi_pack_index *next = m;

		ALLOC_GROW(steps, steps_nr + 1, steps_alloc);

		memset(&step, 0, sizeof(step));
		step.type = MIDX_UNKNOWN;
		step.num_objects = 0;

		while (next) {
			struct multi_pack_index *base = next->base_midx;
			uint32_t proposed = u32_add(step.num_objects, next->num_objects);

			if (!base) {
				/*
				 * If we are at the end of the MIDX
				 * chain, there is nothing to comapct
				 * into this MIDX, so mark it for
				 * inclusion and then stop.
				 */
				step.num_objects = proposed;
				break;
			}
			if (proposed < base->num_objects / opts->midx_split_factor) {
				/*
				 * If there is a MIDX following this
				 * one, but our accumulated size is less
				 * than half of its size, so compacting
				 * them would violate the merging
				 * condition.
				 */
				break;
			}

			/*
			 * Otherwise, it is OK to compact the next layer
			 * into this one, so do so and then continue
			 * down the remainder of the MIDX chain.
			 */
			step.num_objects = proposed;
			next = base;
		}

		if (m == next) {
			step.type = MIDX_KEEP_AS_IS;
			step.u.midx = m;

#ifdef PLAN_VERBOSE
			warning("%s:%d: keeping MIDX %s as-is", __FILE__, __LINE__,
				hash_to_hex(get_midx_checksum(m)));
#endif
		} else {
			step.type = MIDX_COMPACT_MIDXS;
			step.u.compact.from = next;
			step.u.compact.to = m;

#ifdef PLAN_VERBOSE
			warning("%s:%d: compacting MIDX from=%s to=%s", __FILE__, __LINE__,
				hash_to_hex(get_midx_checksum(next)),
				hash_to_hex(get_midx_checksum(m)));
#endif
		}

		m = next->base_midx;

		steps[steps_nr++] = step;
#ifdef PLAN_VERBOSE
		warning("%s:%d: adding step of type %d", __FILE__, __LINE__, step.type);
#endif
	}

	*steps_p = steps;
	*steps_nr_p = steps_nr;

#ifdef PLAN_VERBOSE
	warning("TOTAL STEPS: %"PRIuMAX, (uintmax_t)*steps_nr_p);
#endif
	return 0;
}

int write_midx_incremental(struct repack_midx_opts *opts)
{
	struct midx_compaction_step *steps = NULL;
	struct strbuf lock_name = STRBUF_INIT;
	struct lock_file lf;
	size_t steps_nr = 0;
	size_t i;
	int ret = 0;

	get_midx_chain_filename(&lock_name,
				repo_get_object_directory(the_repository));
	if (safe_create_leading_directories(the_repository, lock_name.buf))
		die_errno(_("unable to create leading directories of %s"),
			  lock_name.buf);
	hold_lock_file_for_update(&lf, lock_name.buf, LOCK_DIE_ON_ERROR);

	if (!fdopen_lock_file(&lf, "w")) {
		ret = error_errno(_("unable to open multi-pack-index chain file"));
		goto done;
	}

	if (make_compaction_plan(opts, &steps, &steps_nr) < 0)
		return error(_("unable to generate compaction plan"));

	for (size_t i = 0; i < steps_nr; i++) {
		struct midx_compaction_step *step = &steps[i];
		const char *base = NULL;

		if (i + 1 < steps_nr)
			base = midx_compaction_step_base(&steps[i + 1]);

		if (midx_compaction_step_exec(step, opts, base) < 0) {
			ret = error(_("unable to execute compaction step %"PRIuMAX),
				    (uintmax_t)i);
			goto done;
		}
	}

	i = steps_nr;
	while (i--) {
		struct midx_compaction_step *step = &steps[i];
		if (!step->result)
			BUG("missing result for compaction step %"PRIuMAX,
			    (uintmax_t)i);
		fprintf(get_lock_file_fp(&lf), "%s\n", step->result);
	}

	commit_lock_file(&lf);

done:
	free(steps);
	return ret;
}
