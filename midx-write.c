#include "git-compat-util.h"
#include "config.h"
#include "hex.h"
#include "packfile.h"
#include "midx.h"
#include "progress.h"
#include "run-command.h"
#include "pack-bitmap.h"
#include "revision.h"

extern int write_midx_internal(const char *object_dir,
			       struct string_list *packs_to_include,
			       struct string_list *packs_to_drop,
			       const char *preferred_pack_name,
			       const char *refs_snapshot,
			       unsigned flags);

extern struct multi_pack_index *lookup_multi_pack_index(struct repository *r,
							const char *object_dir);

int expire_midx_packs(struct repository *r, const char *object_dir, unsigned flags)
{
	uint32_t i, *count, result = 0;
	struct string_list packs_to_drop = STRING_LIST_INIT_DUP;
	struct multi_pack_index *m = lookup_multi_pack_index(r, object_dir);
	struct progress *progress = NULL;

	if (!m)
		return 0;

	CALLOC_ARRAY(count, m->num_packs);

	if (flags & MIDX_PROGRESS)
		progress = start_delayed_progress(_("Counting referenced objects"),
					  m->num_objects);
	for (i = 0; i < m->num_objects; i++) {
		int pack_int_id = nth_midxed_pack_int_id(m, i);
		count[pack_int_id]++;
		display_progress(progress, i + 1);
	}
	stop_progress(&progress);

	if (flags & MIDX_PROGRESS)
		progress = start_delayed_progress(_("Finding and deleting unreferenced packfiles"),
					  m->num_packs);
	for (i = 0; i < m->num_packs; i++) {
		char *pack_name;
		display_progress(progress, i + 1);

		if (count[i])
			continue;

		if (prepare_midx_pack(r, m, i))
			continue;

		if (m->packs[i]->pack_keep || m->packs[i]->is_cruft)
			continue;

		pack_name = xstrdup(m->packs[i]->pack_name);
		close_pack(m->packs[i]);

		string_list_insert(&packs_to_drop, m->pack_names[i]);
		unlink_pack_path(pack_name, 0);
		free(pack_name);
	}
	stop_progress(&progress);

	free(count);

	if (packs_to_drop.nr)
		result = write_midx_internal(object_dir, NULL, &packs_to_drop, NULL, NULL, flags);

	string_list_clear(&packs_to_drop, 0);

	return result;
}

struct repack_info {
	timestamp_t mtime;
	uint32_t referenced_objects;
	uint32_t pack_int_id;
};

static int compare_by_mtime(const void *a_, const void *b_)
{
	const struct repack_info *a, *b;

	a = (const struct repack_info *)a_;
	b = (const struct repack_info *)b_;

	if (a->mtime < b->mtime)
		return -1;
	if (a->mtime > b->mtime)
		return 1;
	return 0;
}

static int fill_included_packs_all(struct repository *r,
				   struct multi_pack_index *m,
				   unsigned char *include_pack)
{
	uint32_t i, count = 0;
	int pack_kept_objects = 0;

	repo_config_get_bool(r, "repack.packkeptobjects", &pack_kept_objects);

	for (i = 0; i < m->num_packs; i++) {
		if (prepare_midx_pack(r, m, i))
			continue;
		if (!pack_kept_objects && m->packs[i]->pack_keep)
			continue;
		if (m->packs[i]->is_cruft)
			continue;

		include_pack[i] = 1;
		count++;
	}

	return count < 2;
}

static int fill_included_packs_batch(struct repository *r,
				     struct multi_pack_index *m,
				     unsigned char *include_pack,
				     size_t batch_size)
{
	uint32_t i, packs_to_repack;
	size_t total_size;
	struct repack_info *pack_info;
	int pack_kept_objects = 0;

	CALLOC_ARRAY(pack_info, m->num_packs);

	repo_config_get_bool(r, "repack.packkeptobjects", &pack_kept_objects);

	for (i = 0; i < m->num_packs; i++) {
		pack_info[i].pack_int_id = i;

		if (prepare_midx_pack(r, m, i))
			continue;

		pack_info[i].mtime = m->packs[i]->mtime;
	}

	for (i = 0; i < m->num_objects; i++) {
		uint32_t pack_int_id = nth_midxed_pack_int_id(m, i);
		pack_info[pack_int_id].referenced_objects++;
	}

	QSORT(pack_info, m->num_packs, compare_by_mtime);

	total_size = 0;
	packs_to_repack = 0;
	for (i = 0; total_size < batch_size && i < m->num_packs; i++) {
		int pack_int_id = pack_info[i].pack_int_id;
		struct packed_git *p = m->packs[pack_int_id];
		size_t expected_size;

		if (!p)
			continue;
		if (!pack_kept_objects && p->pack_keep)
			continue;
		if (p->is_cruft)
			continue;
		if (open_pack_index(p) || !p->num_objects)
			continue;

		expected_size = st_mult(p->pack_size,
					pack_info[i].referenced_objects);
		expected_size /= p->num_objects;

		if (expected_size >= batch_size)
			continue;

		packs_to_repack++;
		total_size += expected_size;
		include_pack[pack_int_id] = 1;
	}

	free(pack_info);

	if (packs_to_repack < 2)
		return 1;

	return 0;
}

int midx_repack(struct repository *r, const char *object_dir, size_t batch_size, unsigned flags)
{
	int result = 0;
	uint32_t i;
	unsigned char *include_pack;
	struct child_process cmd = CHILD_PROCESS_INIT;
	FILE *cmd_in;
	struct strbuf base_name = STRBUF_INIT;
	struct multi_pack_index *m = lookup_multi_pack_index(r, object_dir);

	/*
	 * When updating the default for these configuration
	 * variables in builtin/repack.c, these must be adjusted
	 * to match.
	 */
	int delta_base_offset = 1;
	int use_delta_islands = 0;

	if (!m)
		return 0;

	CALLOC_ARRAY(include_pack, m->num_packs);

	if (batch_size) {
		if (fill_included_packs_batch(r, m, include_pack, batch_size))
			goto cleanup;
	} else if (fill_included_packs_all(r, m, include_pack))
		goto cleanup;

	repo_config_get_bool(r, "repack.usedeltabaseoffset", &delta_base_offset);
	repo_config_get_bool(r, "repack.usedeltaislands", &use_delta_islands);

	strvec_push(&cmd.args, "pack-objects");

	strbuf_addstr(&base_name, object_dir);
	strbuf_addstr(&base_name, "/pack/pack");
	strvec_push(&cmd.args, base_name.buf);

	if (delta_base_offset)
		strvec_push(&cmd.args, "--delta-base-offset");
	if (use_delta_islands)
		strvec_push(&cmd.args, "--delta-islands");

	if (flags & MIDX_PROGRESS)
		strvec_push(&cmd.args, "--progress");
	else
		strvec_push(&cmd.args, "-q");

	strbuf_release(&base_name);

	cmd.git_cmd = 1;
	cmd.in = cmd.out = -1;

	if (start_command(&cmd)) {
		error(_("could not start pack-objects"));
		result = 1;
		goto cleanup;
	}

	cmd_in = xfdopen(cmd.in, "w");

	for (i = 0; i < m->num_objects; i++) {
		struct object_id oid;
		uint32_t pack_int_id = nth_midxed_pack_int_id(m, i);

		if (!include_pack[pack_int_id])
			continue;

		nth_midxed_object_oid(&oid, m, i);
		fprintf(cmd_in, "%s\n", oid_to_hex(&oid));
	}
	fclose(cmd_in);

	if (finish_command(&cmd)) {
		error(_("could not finish pack-objects"));
		result = 1;
		goto cleanup;
	}

	result = write_midx_internal(object_dir, NULL, NULL, NULL, NULL, flags);

cleanup:
	free(include_pack);
	return result;
}
