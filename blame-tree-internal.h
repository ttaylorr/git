#ifndef BLAME_TREE_INTERNAL_H
#define BLAME_TREE_INTERNAL_H

#include "revision.h"
#include "hashmap.h"
#include "blame-tree.h"

struct blame_tree_cache_writer;
struct blame_tree_cache_reader;

struct blame_tree {
	struct hashmap paths;
	struct rev_info rev;

	const char **all_paths;
	int all_paths_nr;

	struct blame_tree_cache_writer *writer;
	struct blame_tree_cache_reader *reader;

	clock_t goal_end_time;
};

#define BLAME_TREE_WRITE_CACHE	(1 << 0)
#define BLAME_TREE_SKIP_CACHE	(1 << 1)

#endif /* BLAME_TREE_INTERNAL_H */
