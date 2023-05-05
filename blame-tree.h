#ifndef BLAME_TREE_H
#define BLAME_TREE_H

struct commit;
struct blame_tree;

#define BLAME_TREE_WRITE_CACHE	(1 << 0)
#define BLAME_TREE_SKIP_CACHE	(1 << 1)

void blame_tree_init(struct blame_tree *, int flags,
		     int argc, const char **argv, const char *prefix);
int blame_tree_release(struct blame_tree *);
int blame_tree_run(struct blame_tree *);

/*
 * Iterate through all blame-tree cache files and
 * recompute them starting at the given commit.
 *
 * The command-line arguments are supplied as the
 * revision to use.
 */
int update_blame_tree_caches(const char *revision);

/*
 * Iterate through all blame-tree cache files and
 * verify if they are valid.
 */
int verify_blame_tree_caches(struct repository *);

#endif /* BLAME_TREE_H */
