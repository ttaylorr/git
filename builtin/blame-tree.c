#include "builtin.h"
#include "blame-tree.h"
#include "config.h"
#include "hex.h"
#include "parse-options.h"
#include "quote.h"
#include "strvec.h"

static const char blame_tree_usage[] =
"git blame-tree [diff/rev options]";

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

static int blame_tree_config(const char *var, const char *value,
			     const struct config_context *ctx, void *data)
{
	char **revopts = data;
	if (!strcmp(var, "blametree.revopts"))
		return git_config_string(revopts, var, value);
	return git_default_config(var, value, ctx, NULL);
}

int cmd_blame_tree(int argc, const char **argv, const char *prefix,
		   struct repository *repo)
{
	struct blame_tree bt;
	struct strvec new_argv = STRVEC_INIT;
	char *revopts = NULL;

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage(blame_tree_usage);

	repo_config(repo, blame_tree_config, NULL);
	if (revopts) {
		strvec_push(&new_argv, *argv++); /* "blame-tree" */
		strvec_push(&new_argv, revopts);
		while (*argv)
			strvec_push(&new_argv, *argv++);
		argv = new_argv.v;
		argc = new_argv.nr;
	}

	blame_tree_init(&bt, argc, argv, prefix);
	if (blame_tree_run(&bt, show_entry, &bt) < 0)
		die("error running blame-tree traversal");
	blame_tree_release(&bt);

	strvec_clear(&new_argv);
	return 0;
}
