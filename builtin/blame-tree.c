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

int cmd_blame_tree(int argc, const char **argv, const char *prefix,
		   struct repository *repo)
{
	struct blame_tree bt;
	struct strvec new_argv = STRVEC_INIT;

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage(blame_tree_usage);

	repo_config(repo, git_default_config, NULL);

	blame_tree_init(&bt, argc, argv, prefix);
	if (blame_tree_run(&bt, show_entry, &bt) < 0)
		die("error running blame-tree traversal");
	blame_tree_release(&bt);

	strvec_clear(&new_argv);
	return 0;
}
