#ifndef PACK_GEOMETRY_H
#define PACK_GEOMETRY_H

struct packed_git;
struct existing_packs;
struct pack_objects_args;
struct string_list;
struct repack_config;

struct pack_geometry {
	struct packed_git **pack;
	uint32_t pack_nr, pack_alloc;
	uint32_t split;

	int split_factor;
	int midx_tip_rewritten;
};

void init_pack_geometry(struct pack_geometry *geometry,
			struct repack_config *cfg,
			struct existing_packs *existing);

void split_pack_geometry(struct pack_geometry *geometry);

void free_pack_geometry(struct pack_geometry *geometry);

struct packed_git *geometry_preferred_pack(struct pack_geometry *geometry);

void geometry_remove_redundant_packs(struct pack_geometry *geometry,
				     struct string_list *names,
				     struct existing_packs *existing,
				     const char *packdir);

#endif /* PACK_GEOMETRY_H */
