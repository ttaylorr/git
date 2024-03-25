#include "git-compat-util.h"
#include "midx.h"

extern int write_midx_internal(const char *object_dir,
			       struct string_list *packs_to_include,
			       struct string_list *packs_to_drop,
			       const char *preferred_pack_name,
			       const char *refs_snapshot,
			       unsigned flags);

extern struct multi_pack_index *lookup_multi_pack_index(struct repository *r,
							const char *object_dir);
