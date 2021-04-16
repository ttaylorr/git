#include "builtin.h"
#include "blame-tree.h"
#include "config.h"
#include "hex.h"
#include "parse-options.h"
#include "quote.h"
#include "strvec.h"

static char const * const blame_tree_usage[] = {
	"git blame-tree [diff/rev options]",
	NULL
};

int cmd_blame_tree(int argc, const char **argv, const char *prefix,
		   struct repository *repo)
{
	struct blame_tree bt;
	int flags = 0;
	static int write_to_cache;
	static struct option blame_tree_options[] = {
		OPT_BOOL(0, "cache", &write_to_cache,
			N_("update the cache for this pathspec")),
		OPT_END(),
	};

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage(blame_tree_usage[0]);

	argc = parse_options(argc, argv, prefix,
			     blame_tree_options,
			     blame_tree_usage,
			     PARSE_OPT_KEEP_UNKNOWN_OPT | PARSE_OPT_KEEP_ARGV0);

	if (write_to_cache)
		flags |= BLAME_TREE_CACHE;

	repo_config(repo, git_default_config, NULL);

	blame_tree_init(&bt, flags, argc, argv, prefix);
	if (blame_tree_run(&bt) < 0)
		die("error running blame-tree traversal");
	blame_tree_release(&bt);

	return 0;
}
