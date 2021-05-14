#ifndef PACK_MTIMES_H
#define PACK_MTIMES_H

#include "git-compat-util.h"

#define MTIMES_SIGNATURE 0x4d544d45 /* "MTME" */
#define MTIMES_VERSION 1

struct packed_git;

int pack_has_mtimes(struct packed_git *p);
int load_pack_mtimes(struct packed_git *p);

uint32_t nth_packed_mtime(struct packed_git *p, uint32_t pos);

#endif
