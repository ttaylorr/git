#include "builtin.h"
#include "blame-tree.h"
#include "config.h"
#include "hex.h"
#include "parse-options.h"
#include "quote.h"
#include "strvec.h"

static char const * const blame_tree_usage[] = {
	"git blame-tree [diff/rev options]",
	"git blame-tree --update-cache <revision>",
	NULL
};

int cmd_blame_tree(int argc, const char **argv, const char *prefix,
		   struct repository *repo)
{
	struct blame_tree bt;
	int flags = 0;
	int write_to_cache = 0;
	int update_cache = 0;
	int i, skip_read_cache = 0;

	if (argc == 2 && !strcmp(argv[1], "-h"))
		usage(blame_tree_usage[0]);

	for (i = 1; i < argc; i++) {
		int j;
		if (!strcmp(argv[i], "--cache"))
			write_to_cache = 1;
		else if (!strcmp(argv[i], "--update-cache"))
			update_cache = 1;
		else
			continue;

		for (j = i + 1; j <= argc; j++) {
			argv[j - 1] = argv[j];
		}
		argc--;
		i--;
	}

	if (update_cache) {
		if (argc != 2)
			usage(blame_tree_usage[1]);
		return update_blame_tree_caches(argv[1]);
	}

	if (write_to_cache)
		flags |= BLAME_TREE_WRITE_CACHE;

	repo_config(repo, git_default_config, NULL);

	if (!repo_config_get_bool(repo, "blametree.skipreadcache",
				  &skip_read_cache) && skip_read_cache)
		flags |= BLAME_TREE_SKIP_CACHE;

	blame_tree_init(&bt, flags, argc, argv, prefix);
	if (blame_tree_run(&bt) < 0)
		die("error running blame-tree traversal");
	if (blame_tree_release(&bt))
		die("error completing blame-tree operation");

	return 0;
}
