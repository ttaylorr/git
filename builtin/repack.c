#define USE_THE_REPOSITORY_VARIABLE
#define DISABLE_SIGN_COMPARE_WARNINGS

#include "builtin.h"
#include "config.h"
#include "dir.h"
#include "environment.h"
#include "gettext.h"
#include "hex.h"
#include "parse-options.h"
#include "path.h"
#include "run-command.h"
#include "server-info.h"
#include "strbuf.h"
#include "string-list.h"
#include "strvec.h"
#include "midx.h"
#include "packfile.h"
#include "prune-packed.h"
#include "odb.h"
#include "promisor-remote.h"
#include "repack.h"
#include "shallow.h"
#include "pack.h"
#include "pack-bitmap.h"
#include "pack-geometry.h"
#include "refs.h"
#include "list-objects-filter-options.h"

#define ALL_INTO_ONE 1
#define LOOSEN_UNREACHABLE 2
#define PACK_CRUFT 4

static int pack_everything;
static int write_bitmaps = -1;
static int use_delta_islands;
static int run_update_server_info = 1;
static char *packdir, *packtmp_name, *packtmp;
static int midx_must_contain_cruft = 1;

static const char *const git_repack_usage[] = {
	N_("git repack [-a] [-A] [-d] [-f] [-F] [-l] [-n] [-q] [-b] [-m]\n"
	   "[--window=<n>] [--depth=<n>] [--threads=<n>] [--keep-pack=<pack-name>]\n"
	   "[--write-midx] [--name-hash-version=<n>] [--path-walk]"),
	NULL
};

static const char incremental_bitmap_conflict_error[] = N_(
"Incremental repacks are incompatible with bitmap indexes.  Use\n"
"--no-write-bitmap-index or disable the pack.writeBitmaps configuration."
);

static int repack_config(const char *var, const char *value,
			 const struct config_context *ctx, void *_cb)
{
	struct repack_config *cb = _cb;
	if (!strcmp(var, "repack.usedeltabaseoffset")) {
		int delta_base_offset = git_config_bool(var, value);
		cb->po_args.delta_base_offset = delta_base_offset;
		cb->cruft_po_args.delta_base_offset = delta_base_offset;
		return 0;
	}
	if (!strcmp(var, "repack.packkeptobjects")) {
		cb->pack_kept_objects = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.writebitmaps") ||
	    !strcmp(var, "pack.writebitmaps")) {
		write_bitmaps = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.usedeltaislands")) {
		use_delta_islands = git_config_bool(var, value);
		return 0;
	}
	if (strcmp(var, "repack.updateserverinfo") == 0) {
		run_update_server_info = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.cruftwindow")) {
		free(cb->cruft_po_args.window);
		return git_config_string(&cb->cruft_po_args.window, var, value);
	}
	if (!strcmp(var, "repack.cruftwindowmemory")) {
		free(cb->cruft_po_args.window_memory);
		return git_config_string(&cb->cruft_po_args.window_memory, var, value);
	}
	if (!strcmp(var, "repack.cruftdepth")) {
		free(cb->cruft_po_args.depth);
		return git_config_string(&cb->cruft_po_args.depth, var, value);
	}
	if (!strcmp(var, "repack.cruftthreads")) {
		free(cb->cruft_po_args.threads);
		return git_config_string(&cb->cruft_po_args.threads, var, value);
	}
	if (!strcmp(var, "repack.midxmustcontaincruft")) {
		midx_must_contain_cruft = git_config_bool(var, value);
		return 0;
	}
	if (!strcmp(var, "repack.midxsplitfactor")) {
		cb->midx_split_factor = git_config_int(var, value, ctx->kvi);
		return 0;
	}
	if (!strcmp(var, "repack.midxnewlayerthreshold")) {
		cb->midx_new_layer_threshold = git_config_int(var, value,
							      ctx->kvi);
		return 0;
	}
	return git_default_config(var, value, ctx, cb);
}

static int write_filtered_pack(struct repack_config *cfg,
			       const char *pack_prefix,
			       struct existing_packs *existing,
			       struct string_list *names)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	FILE *in;
	int ret;
	const char *caret;
	const char *scratch;
	int local = skip_prefix(cfg->filter_to, packdir, &scratch);

	prepare_pack_objects(&cmd, &cfg->po_args, cfg->filter_to);

	strvec_push(&cmd.args, "--stdin-packs");

	if (!cfg->pack_kept_objects)
		strvec_push(&cmd.args, "--honor-pack-keep");
	for_each_string_list_item(item, &existing->kept_packs)
		strvec_pushf(&cmd.args, "--keep-pack=%s", item->string);

	cmd.in = -1;

	ret = start_command(&cmd);
	if (ret)
		return ret;

	/*
	 * Here 'names' contains only the pack(s) that were just
	 * written, which is exactly the packs we want to keep. Also
	 * 'existing_kept_packs' already contains the packs in
	 * 'cfg.keep_pack_list'.
	 */
	in = xfdopen(cmd.in, "w");
	for_each_string_list_item(item, names)
		fprintf(in, "^%s-%s.pack\n", pack_prefix, item->string);
	for_each_string_list_item(item, &existing->non_kept_packs)
		fprintf(in, "%s.pack\n", item->string);
	for_each_string_list_item(item, &existing->cruft_packs)
		fprintf(in, "%s.pack\n", item->string);
	caret = cfg->pack_kept_objects ? "" : "^";
	for_each_string_list_item(item, &existing->kept_packs)
		fprintf(in, "%s%s.pack\n", caret, item->string);
	fclose(in);

	return finish_pack_objects_cmd(&cmd, names, packtmp, local);
}

static void combine_small_cruft_packs(FILE *in, size_t combine_cruft_below_size,
				      struct existing_packs *existing)
{
	struct packed_git *p;
	struct strbuf buf = STRBUF_INIT;
	size_t i;

	for (p = get_all_packs(the_repository); p; p = p->next) {
		if (!(p->is_cruft && p->pack_local))
			continue;

		strbuf_reset(&buf);
		strbuf_addstr(&buf, pack_basename(p));
		strbuf_strip_suffix(&buf, ".pack");

		if (!string_list_has_string(&existing->cruft_packs, buf.buf))
			continue;

		if (p->pack_size < combine_cruft_below_size) {
			fprintf(in, "-%s\n", pack_basename(p));
		} else {
			retain_cruft_pack(existing, p);
			fprintf(in, "%s\n", pack_basename(p));
		}
	}

	for (i = 0; i < existing->non_kept_packs.nr; i++)
		fprintf(in, "-%s.pack\n",
			existing->non_kept_packs.items[i].string);

	strbuf_release(&buf);
}

static int write_cruft_pack(const struct pack_objects_args *args,
			    const char *destination,
			    const char *pack_prefix,
			    const char *cruft_expiration,
			    unsigned long combine_cruft_below_size,
			    struct string_list *names,
			    struct existing_packs *existing)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	FILE *in;
	int ret;
	const char *scratch;
	int local = skip_prefix(destination, packdir, &scratch);

	prepare_pack_objects(&cmd, args, destination);

	strvec_push(&cmd.args, "--cruft");
	if (cruft_expiration)
		strvec_pushf(&cmd.args, "--cruft-expiration=%s",
			     cruft_expiration);

	strvec_push(&cmd.args, "--honor-pack-keep");
	strvec_push(&cmd.args, "--non-empty");

	cmd.in = -1;

	ret = start_command(&cmd);
	if (ret)
		return ret;

	/*
	 * names has a confusing double use: it both provides the list
	 * of just-written new packs, and accepts the name of the cruft
	 * pack we are writing.
	 *
	 * By the time it is read here, it contains only the pack(s)
	 * that were just written, which is exactly the set of packs we
	 * want to consider kept.
	 *
	 * If `--expire-to` is given, the double-use served by `names`
	 * ensures that the pack written to `--expire-to` excludes any
	 * objects contained in the cruft pack.
	 */
	in = xfdopen(cmd.in, "w");
	for_each_string_list_item(item, names)
		fprintf(in, "%s-%s.pack\n", pack_prefix, item->string);
	if (combine_cruft_below_size && !cruft_expiration) {
		combine_small_cruft_packs(in, combine_cruft_below_size,
					  existing);
	} else {
		for_each_string_list_item(item, &existing->non_kept_packs)
			fprintf(in, "-%s.pack\n", item->string);
		for_each_string_list_item(item, &existing->cruft_packs)
			fprintf(in, "-%s.pack\n", item->string);
	}
	for_each_string_list_item(item, &existing->kept_packs)
		fprintf(in, "%s.pack\n", item->string);
	fclose(in);

	return finish_pack_objects_cmd(&cmd, names, packtmp, local);
}

static const char *find_pack_prefix(const char *packdir, const char *packtmp)
{
	const char *pack_prefix;
	if (!skip_prefix(packtmp, packdir, &pack_prefix))
		die(_("pack prefix %s does not begin with objdir %s"),
		    packtmp, packdir);
	if (*pack_prefix == '/')
		pack_prefix++;
	return pack_prefix;
}

static int option_parse_write_midx(const struct option *opt, const char *arg,
				   int unset)
{
	struct repack_config *cfg = opt->value;

	if (unset) {
		cfg->write_midx = WRITE_MIDX_NONE;
		return 0;
	}

	if (!arg || !*arg)
		cfg->write_midx = WRITE_MIDX_DEFAULT;
	else if (!strcmp(arg, "default"))
		cfg->write_midx = WRITE_MIDX_DEFAULT;
	else if (!strcmp(arg, "geometric"))
		cfg->write_midx = WRITE_MIDX_GEOMETRIC;
	else
		return error(_("unknown value for %s: %s"), opt->long_name, arg);

	return 0;
}

int cmd_repack(int argc,
	       const char **argv,
	       const char *prefix,
	       struct repository *repo UNUSED)
{
	struct child_process cmd = CHILD_PROCESS_INIT;
	struct string_list_item *item;
	struct string_list names = STRING_LIST_INIT_DUP;
	struct existing_packs existing = EXISTING_PACKS_INIT;
	struct pack_geometry geometry = { 0 };
	struct tempfile *refs_snapshot = NULL;
	struct string_list midx_pack_names = STRING_LIST_INIT_DUP;
	int i, ret;
	int show_progress;

	/* variables to be filled by option parsing */
	struct repack_config cfg;

	struct option builtin_repack_options[] = {
		OPT_BIT('a', NULL, &pack_everything,
				N_("pack everything in a single pack"), ALL_INTO_ONE),
		OPT_BIT('A', NULL, &pack_everything,
				N_("same as -a, and turn unreachable objects loose"),
				   LOOSEN_UNREACHABLE | ALL_INTO_ONE),
		OPT_BIT(0, "cruft", &pack_everything,
				N_("same as -a, pack unreachable cruft objects separately"),
				   PACK_CRUFT),
		OPT_STRING(0, "cruft-expiration", &cfg.cruft_expiration, N_("approxidate"),
				N_("with --cruft, expire objects older than this")),
		OPT_UNSIGNED(0, "combine-cruft-below-size",
			     &cfg.combine_cruft_below_size,
			     N_("with --cruft, only repack cruft packs smaller than this")),
		OPT_UNSIGNED(0, "max-cruft-size", &cfg.cruft_po_args.max_pack_size,
			     N_("with --cruft, limit the size of new cruft packs")),
		OPT_BOOL('d', NULL, &cfg.delete_redundant,
				N_("remove redundant packs, and run git-prune-packed")),
		OPT_BOOL('f', NULL, &cfg.po_args.no_reuse_delta,
				N_("pass --no-reuse-delta to git-pack-objects")),
		OPT_BOOL('F', NULL, &cfg.po_args.no_reuse_object,
				N_("pass --no-reuse-object to git-pack-objects")),
		OPT_INTEGER(0, "name-hash-version", &cfg.po_args.name_hash_version,
				N_("specify the name hash version to use for grouping similar objects by path")),
		OPT_BOOL(0, "path-walk", &cfg.po_args.path_walk,
				N_("pass --path-walk to git-pack-objects")),
		OPT_NEGBIT('n', NULL, &run_update_server_info,
				N_("do not run git-update-server-info"), 1),
		OPT__QUIET(&cfg.po_args.quiet, N_("be quiet")),
		OPT_BOOL('l', "local", &cfg.po_args.local,
				N_("pass --local to git-pack-objects")),
		OPT_BOOL('b', "write-bitmap-index", &write_bitmaps,
				N_("write bitmap index")),
		OPT_BOOL('i', "delta-islands", &use_delta_islands,
				N_("pass --delta-islands to git-pack-objects")),
		OPT_STRING(0, "unpack-unreachable", &cfg.unpack_unreachable, N_("approxidate"),
				N_("with -A, do not loosen objects older than this")),
		OPT_BOOL('k', "keep-unreachable", &cfg.keep_unreachable,
				N_("with -a, repack unreachable objects")),
		OPT_STRING(0, "window", &cfg.opt_window, N_("n"),
				N_("size of the window used for delta compression")),
		OPT_STRING(0, "window-memory", &cfg.opt_window_memory, N_("bytes"),
				N_("same as the above, but limit memory size instead of entries count")),
		OPT_STRING(0, "depth", &cfg.opt_depth, N_("n"),
				N_("limits the maximum delta depth")),
		OPT_STRING(0, "threads", &cfg.opt_threads, N_("n"),
				N_("limits the maximum number of threads")),
		OPT_UNSIGNED(0, "max-pack-size", &cfg.po_args.max_pack_size,
			     N_("maximum size of each packfile")),
		OPT_PARSE_LIST_OBJECTS_FILTER(&cfg.po_args.filter_options),
		OPT_BOOL(0, "pack-kept-objects", &cfg.pack_kept_objects,
				N_("repack objects in packs marked with .keep")),
		OPT_STRING_LIST(0, "keep-pack", &cfg.keep_pack_list, N_("name"),
				N_("do not repack this pack")),
		OPT_INTEGER('g', "geometric", &geometry.split_factor,
			    N_("find a geometric progression with factor <N>")),
		OPT_CALLBACK('m', "write-midx", &cfg.write_midx,
			   N_("mode"),
			   N_("write a multi-pack index of the resulting packs"),
			   option_parse_write_midx),
		OPT_STRING(0, "expire-to", &cfg.expire_to, N_("dir"),
			   N_("pack prefix to store a pack containing pruned objects")),
		OPT_STRING(0, "filter-to", &cfg.filter_to, N_("dir"),
			   N_("pack prefix to store a pack containing filtered out objects")),
		OPT_END()
	};

	list_objects_filter_init(&cfg.po_args.filter_options);

	git_config(repack_config, &cfg);

	argc = parse_options(argc, argv, prefix, builtin_repack_options,
				git_repack_usage, 0);

	cfg.po_args.window = xstrdup_or_null(cfg.opt_window);
	cfg.po_args.window_memory = xstrdup_or_null(cfg.opt_window_memory);
	cfg.po_args.depth = xstrdup_or_null(cfg.opt_depth);
	cfg.po_args.threads = xstrdup_or_null(cfg.opt_threads);

	if (cfg.delete_redundant && the_repository->repository_format_precious_objects)
		die(_("cannot delete packs in a precious-objects repo"));

	die_for_incompatible_opt3(cfg.unpack_unreachable || (pack_everything & LOOSEN_UNREACHABLE), "-A",
				  cfg.keep_unreachable, "-k/--keep-unreachable",
				  pack_everything & PACK_CRUFT, "--cruft");

	if (pack_everything & PACK_CRUFT)
		pack_everything |= ALL_INTO_ONE;

	if (write_bitmaps < 0) {
		if (!cfg.write_midx &&
		    (!(pack_everything & ALL_INTO_ONE) || !is_bare_repository()))
			write_bitmaps = 0;
	}
	if (cfg.pack_kept_objects < 0)
		cfg.pack_kept_objects = write_bitmaps > 0 && !cfg.write_midx;

	if (write_bitmaps && !(pack_everything & ALL_INTO_ONE) && !cfg.write_midx)
		die(_(incremental_bitmap_conflict_error));

	if (write_bitmaps && cfg.po_args.local &&
	    odb_has_alternates(the_repository->objects)) {
		/*
		 * When asked to do a local repack, but we have
		 * packfiles that are inherited from an alternate, then
		 * we cannot guarantee that the multi-pack-index would
		 * have full coverage of all objects. We thus disable
		 * writing bitmaps in that case.
		 */
		warning(_("disabling bitmap writing, as some objects are not being packed"));
		write_bitmaps = 0;
	}

	if (cfg.write_midx && write_bitmaps) {
		struct strbuf path = STRBUF_INIT;

		strbuf_addf(&path, "%s/%s_XXXXXX", repo_get_object_directory(the_repository),
			    "bitmap-ref-tips");

		refs_snapshot = xmks_tempfile(path.buf);
		midx_snapshot_refs(refs_snapshot);

		strbuf_release(&path);
	}

	packdir = mkpathdup("%s/pack", repo_get_object_directory(the_repository));
	packtmp_name = xstrfmt(".tmp-%d-pack", (int)getpid());
	packtmp = mkpathdup("%s/%s", packdir, packtmp_name);

	collect_pack_filenames(&existing, &cfg.keep_pack_list);

	if (geometry.split_factor) {
		if (pack_everything)
			die(_("options '%s' and '%s' cannot be used together"), "--geometric", "-A/-a");
		init_pack_geometry(&geometry, &cfg, &existing);
		split_pack_geometry(&geometry);
	}

	prepare_pack_objects(&cmd, &cfg.po_args, packtmp);

	show_progress = !cfg.po_args.quiet && isatty(2);

	strvec_push(&cmd.args, "--keep-true-parents");
	if (!cfg.pack_kept_objects)
		strvec_push(&cmd.args, "--honor-pack-keep");
	for (i = 0; i < cfg.keep_pack_list.nr; i++)
		strvec_pushf(&cmd.args, "--keep-pack=%s",
			     cfg.keep_pack_list.items[i].string);
	strvec_push(&cmd.args, "--non-empty");
	if (!geometry.split_factor) {
		/*
		 * We need to grab all reachable objects, including those that
		 * are reachable from reflogs and the index.
		 *
		 * When repacking into a geometric progression of packs,
		 * however, we ask 'git pack-objects --stdin-packs', and it is
		 * not about packing objects based on reachability but about
		 * repacking all the objects in specified packs and loose ones
		 * (indeed, --stdin-packs is incompatible with these options).
		 */
		strvec_push(&cmd.args, "--all");
		strvec_push(&cmd.args, "--reflog");
		strvec_push(&cmd.args, "--indexed-objects");
	}
	if (repo_has_promisor_remote(the_repository))
		strvec_push(&cmd.args, "--exclude-promisor-objects");
	if (!cfg.write_midx) {
		if (write_bitmaps > 0)
			strvec_push(&cmd.args, "--write-bitmap-index");
		else if (write_bitmaps < 0)
			strvec_push(&cmd.args, "--write-bitmap-index-quiet");
	}
	if (use_delta_islands)
		strvec_push(&cmd.args, "--delta-islands");

	if (pack_everything & ALL_INTO_ONE) {
		repack_promisor_objects(&cfg.po_args, &names, packtmp);

		if (has_existing_non_kept_packs(&existing) &&
		    cfg.delete_redundant &&
		    !(pack_everything & PACK_CRUFT)) {
			for_each_string_list_item(item, &names) {
				strvec_pushf(&cmd.args, "--keep-pack=%s-%s.pack",
					     packtmp_name, item->string);
			}
			if (cfg.unpack_unreachable) {
				strvec_pushf(&cmd.args,
					     "--unpack-unreachable=%s",
					     cfg.unpack_unreachable);
			} else if (pack_everything & LOOSEN_UNREACHABLE) {
				strvec_push(&cmd.args,
					    "--unpack-unreachable");
			} else if (cfg.keep_unreachable) {
				strvec_push(&cmd.args, "--keep-unreachable");
			}
		}

		if (cfg.keep_unreachable && cfg.delete_redundant &&
		    !(pack_everything & PACK_CRUFT))
			strvec_push(&cmd.args, "--pack-loose-unreachable");
	} else if (geometry.split_factor) {
		if (midx_must_contain_cruft)
			strvec_push(&cmd.args, "--stdin-packs");
		else
			strvec_push(&cmd.args, "--stdin-packs=follow");
		strvec_push(&cmd.args, "--unpacked");
	} else {
		strvec_push(&cmd.args, "--unpacked");
		strvec_push(&cmd.args, "--incremental");
	}

	if (cfg.po_args.filter_options.choice)
		strvec_pushf(&cmd.args, "--filter=%s",
			     expand_list_objects_filter_spec(&cfg.po_args.filter_options));
	else if (cfg.filter_to)
		die(_("option '%s' can only be used along with '%s'"), "--filter-to", "--filter");

	if (geometry.split_factor)
		cmd.in = -1;
	else
		cmd.no_stdin = 1;

	ret = start_command(&cmd);
	if (ret)
		goto cleanup;

	if (geometry.split_factor) {
		FILE *in = xfdopen(cmd.in, "w");
		/*
		 * The resulting pack should contain all objects in packs that
		 * are going to be rolled up, but exclude objects in packs which
		 * are being left alone.
		 */
		for (i = 0; i < geometry.split; i++)
			fprintf(in, "%s\n", pack_basename(geometry.pack[i]));
		for (i = geometry.split; i < geometry.pack_nr; i++)
			fprintf(in, "^%s\n", pack_basename(geometry.pack[i]));
		fclose(in);
	}

	ret = finish_pack_objects_cmd(&cmd, &names, packtmp, 1);
	if (ret)
		goto cleanup;

	if (!names.nr) {
		if (!cfg.po_args.quiet)
			printf_ln(_("Nothing new to pack."));
		/*
		 * If we didn't write any new packs, the non-cruft packs
		 * may refer to once-unreachable objects in the cruft
		 * pack(s).
		 *
		 * If there isn't already a MIDX, the one we write
		 * must include the cruft pack(s), in case the
		 * non-cruft pack(s) refer to once-cruft objects.
		 *
		 * If there is already a MIDX, we can punt here, since
		 * midx_has_unknown_packs() will make the decision for
		 * us.
		 */
		if (!get_local_multi_pack_index(the_repository))
			midx_must_contain_cruft = 1;
	}

	if (pack_everything & PACK_CRUFT) {
		const char *pack_prefix = find_pack_prefix(packdir, packtmp);

		if (!cfg.cruft_po_args.window)
			cfg.cruft_po_args.window = xstrdup_or_null(cfg.po_args.window);
		if (!cfg.cruft_po_args.window_memory)
			cfg.cruft_po_args.window_memory = xstrdup_or_null(cfg.po_args.window_memory);
		if (!cfg.cruft_po_args.depth)
			cfg.cruft_po_args.depth = xstrdup_or_null(cfg.po_args.depth);
		if (!cfg.cruft_po_args.threads)
			cfg.cruft_po_args.threads = xstrdup_or_null(cfg.po_args.threads);
		if (!cfg.cruft_po_args.max_pack_size)
			cfg.cruft_po_args.max_pack_size = cfg.po_args.max_pack_size;

		cfg.cruft_po_args.local = cfg.po_args.local;
		cfg.cruft_po_args.quiet = cfg.po_args.quiet;

		ret = write_cruft_pack(&cfg.cruft_po_args, packtmp, pack_prefix,
				       cfg.cruft_expiration,
				       cfg.combine_cruft_below_size, &names,
				       &existing);
		if (ret)
			goto cleanup;

		if (cfg.delete_redundant && cfg.expire_to) {
			/*
			 * If `--expire-to` is given with `-d`, it's possible
			 * that we're about to prune some objects. With cruft
			 * packs, pruning is implicit: any objects from existing
			 * packs that weren't picked up by new packs are removed
			 * when their packs are deleted.
			 *
			 * Generate an additional cruft pack, with one twist:
			 * `names` now includes the name of the cruft pack
			 * written in the previous step. So the contents of
			 * _this_ cruft pack exclude everything contained in the
			 * existing cruft pack (that is, all of the unreachable
			 * objects which are no older than
			 * `--cruft-expiration`).
			 *
			 * To make this work, cfg.cruft_expiration must become NULL
			 * so that this cruft pack doesn't actually prune any
			 * objects. If it were non-NULL, this call would always
			 * generate an empty pack (since every object not in the
			 * cruft pack generated above will have an mtime older
			 * than the expiration).
			 *
			 * Pretend we don't have a `--combine-cruft-below-size`
			 * argument, since we're not selectively combining
			 * anything based on size to generate the limbo cruft
			 * pack, but rather removing all cruft packs from the
			 * main repository regardless of size.
			 */
			ret = write_cruft_pack(&cfg.cruft_po_args, cfg.expire_to,
					       pack_prefix,
					       NULL,
					       0ul,
					       &names,
					       &existing);
			if (ret)
				goto cleanup;
		}
	}

	if (cfg.po_args.filter_options.choice) {
		if (!cfg.filter_to)
			cfg.filter_to = packtmp;

		ret = write_filtered_pack(&cfg,
					  find_pack_prefix(packdir, packtmp),
					  &existing,
					  &names);
		if (ret)
			goto cleanup;
	}

	string_list_sort(&names);

	if (get_local_multi_pack_index(the_repository)) {
		struct multi_pack_index *m =
			get_local_multi_pack_index(the_repository);

		for (; m; m = m->base_midx)
			for (uint32_t i = 0; i < m->num_packs; i++)
				string_list_insert(&midx_pack_names,
						   m->pack_names[i]);

		string_list_sort(&midx_pack_names);
	}

	close_object_store(the_repository->objects);

	/*
	 * Ok we have prepared all new packfiles.
	 */
	install_generated_packs(&names, packdir, packtmp);
	/* End of pack replacement. */

	if (cfg.delete_redundant && pack_everything & ALL_INTO_ONE)
		mark_packs_for_deletion(&existing, &names);

	if (cfg.write_midx) {
		struct repack_midx_opts opts = {
			.existing = &existing,
			.geometry = &geometry,
			.names = &names,
			.midx_pack_names = &midx_pack_names,
			.refs_snapshot = refs_snapshot,
			.packdir = packdir,
			.show_progress = show_progress,
			.write_bitmaps = write_bitmaps,
			.midx_must_contain_cruft = midx_must_contain_cruft,
			.midx_split_factor = cfg.midx_split_factor,
			.midx_new_layer_threshold = cfg.midx_new_layer_threshold,
		};

		if (cfg.write_midx == WRITE_MIDX_DEFAULT)
			ret = write_midx_included_packs(&opts);
		else
			ret = write_midx_incremental(&opts);

		if (ret)
			goto cleanup;
	}

	reprepare_packed_git(the_repository);

	if (cfg.delete_redundant) {
		int opts = 0;
		remove_redundant_existing_packs(&existing, packdir);

		if (geometry.split_factor)
			geometry_remove_redundant_packs(&geometry, &names,
							&existing, packdir);
		if (show_progress)
			opts |= PRUNE_PACKED_VERBOSE;
		prune_packed_objects(opts);

		if (!cfg.keep_unreachable &&
		    (!(pack_everything & LOOSEN_UNREACHABLE) ||
		     cfg.unpack_unreachable) &&
		    is_repository_shallow(the_repository))
			prune_shallow(PRUNE_QUICK);
	}

	if (run_update_server_info)
		update_server_info(the_repository, 0);

	if (git_env_bool(GIT_TEST_MULTI_PACK_INDEX, 0)) {
		unsigned flags = 0;
		if (git_env_bool(GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL, 0))
			flags |= MIDX_WRITE_INCREMENTAL;
		write_midx_file(the_repository, repo_get_object_directory(the_repository),
				NULL, NULL, flags);
	}

cleanup:
	string_list_clear(&cfg.keep_pack_list, 0);
	string_list_clear(&names, 1);
	existing_packs_release(&existing);
	free_pack_geometry(&geometry);
	string_list_clear(&midx_pack_names, 0);
	pack_objects_args_release(&cfg.po_args);
	pack_objects_args_release(&cfg.cruft_po_args);

	return ret;
}
