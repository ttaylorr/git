#include "pack-mtimes.h"
#include "object-store.h"
#include "packfile.h"

static char *pack_mtimes_filename(struct packed_git *p)
{
	size_t len;
	if (!strip_suffix(p->pack_name, ".pack", &len))
		BUG("pack_name does not end in .pack");
	/* NEEDSWORK: this could reuse code from pack-revindex.c. */
	return xstrfmt("%.*s.mtimes", (int)len, p->pack_name);
}

int pack_has_mtimes(struct packed_git *p)
{
	struct stat st;
	char *fname = pack_mtimes_filename(p);

	if (stat(fname, &st) < 0) {
		if (errno == ENOENT)
			return 0;
		die_errno(_("could not stat %s"), fname);
	}

	free(fname);
	return 1;
}

#define MTIMES_HEADER_SIZE (12)
#define MTIMES_MIN_SIZE (MTIMES_HEADER_SIZE + (2 * the_hash_algo->rawsz))

struct mtimes_header {
	uint32_t signature;
	uint32_t version;
	uint32_t hash_id;
};

static int load_pack_mtimes_file(char *mtimes_file,
				 uint32_t num_objects,
				 const uint32_t **data_p, size_t *len_p)
{
	int fd, ret = 0;
	struct stat st;
	void *data = NULL;
	size_t mtimes_size;
	uint32_t *hdr;

	fd = git_open(mtimes_file);

	if (fd < 0) {
		ret = -1;
		goto cleanup;
	}
	if (fstat(fd, &st)) {
		ret = error_errno(_("failed to read %s"), mtimes_file);
		goto cleanup;
	}

	mtimes_size = xsize_t(st.st_size);

	if (mtimes_size < MTIMES_MIN_SIZE) {
		ret = error(_("mtimes file %s is too small"), mtimes_file);
		goto cleanup;
	}

	if (mtimes_size - MTIMES_MIN_SIZE != st_mult(sizeof(uint32_t), num_objects)) {
		ret = error(_("mtimes file %s is corrupt"), mtimes_file);
		goto cleanup;
	}

	data = hdr = xmmap(NULL, mtimes_size, PROT_READ, MAP_PRIVATE, fd, 0);

	if (ntohl(*hdr) != MTIMES_SIGNATURE) {
		ret = error(_("mtimes file %s has unknown signature"), mtimes_file);
		goto cleanup;
	}

	if (ntohl(*++hdr) != 1) {
		ret = error(_("mtimes file %s has unsupported version %"PRIu32),
			    mtimes_file, ntohl(*hdr));
		goto cleanup;
	}
	hdr++;
	if (!(ntohl(*hdr) == 1 || ntohl(*hdr) == 2)) {
		ret = error(_("mtimes file %s has unsupported hash id %"PRIu32),
			    mtimes_file, ntohl(*hdr));
		goto cleanup;
	}

cleanup:
	if (ret) {
		if (data)
			munmap(data, mtimes_size);
	} else {
		*len_p = mtimes_size;
		*data_p = (const uint32_t *)data;
	}

	close(fd);
	return ret;
}

int load_pack_mtimes(struct packed_git *p)
{
	char *mtimes_name = NULL;
	int ret = 0;

	if (!p->is_cruft)
		return ret; /* not a cruft pack */
	if (p->mtimes_map)
		return ret; /* already loaded */

	ret = open_pack_index(p);
	if (ret < 0)
		goto cleanup;

	mtimes_name = pack_mtimes_filename(p);
	ret = load_pack_mtimes_file(mtimes_name,
				    p->num_objects,
				    &p->mtimes_map,
				    &p->mtimes_size);
	if (ret)
		goto cleanup;

cleanup:
	free(mtimes_name);
	return ret;
}

uint32_t nth_packed_mtime(struct packed_git *p, uint32_t pos)
{
	if (!p->mtimes_map)
		BUG("pack .mtimes file not loaded for %s", p->pack_name);
	if (p->num_objects <= pos)
		BUG("pack .mtimes out-of-bounds (%"PRIu32" vs %"PRIu32")",
		    pos, p->num_objects);

	return get_be32(p->mtimes_map + pos + 3);
}
