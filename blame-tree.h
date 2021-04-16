#ifndef BLAME_TREE_H
#define BLAME_TREE_H

#include "commit.h"
#include "diff.h"
#include "revision.h"
#include "hashmap.h"
#include "bloom.h"

struct blame_tree {
	struct hashmap paths;
	struct rev_info rev;

	const char **all_paths;
	int all_paths_nr;
};

#define BLAME_TREE_CACHE (1 << 0)

void blame_tree_init(struct blame_tree *, int flags,
		     int argc, const char **argv, const char *prefix);
void blame_tree_release(struct blame_tree *);

typedef void (*blame_tree_callback)(const char *path,
				    const struct commit *commit,
				    void *data);
int blame_tree_run(struct blame_tree *);

#endif /* BLAME_TREE_H */
