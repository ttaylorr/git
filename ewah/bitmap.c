/**
 * Copyright 2013, GitHub, Inc
 * Copyright 2009-2013, Daniel Lemire, Cliff Moon,
 *	David McIntosh, Robert Becho, Google Inc. and Veronika Zenz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include "git-compat-util.h"
#include "ewok.h"

#define EWAH_MASK(x) ((eword_t)1 << (x % BITS_IN_EWORD))
#define EWAH_BLOCK(x) (x / BITS_IN_EWORD)

struct bitmap *bitmap_word_alloc(size_t word_alloc)
{
	struct bitmap *bitmap = xmalloc(sizeof(struct bitmap));
	CALLOC_ARRAY(bitmap->words, word_alloc);
	bitmap->word_alloc = word_alloc;
	return bitmap;
}

struct bitmap *bitmap_new(void)
{
	return bitmap_word_alloc(32);
}

struct bitmap *bitmap_dup(const struct bitmap *src)
{
	struct bitmap *dst = bitmap_word_alloc(src->word_alloc);
	COPY_ARRAY(dst->words, src->words, src->word_alloc);
	return dst;
}

static void bitmap_grow(struct bitmap *self, size_t word_alloc)
{
	size_t old_size = self->word_alloc;
	ALLOC_GROW(self->words, word_alloc, self->word_alloc);
	MEMZERO_ARRAY(self->words + old_size, self->word_alloc - old_size);
}

void bitmap_set(struct bitmap *self, size_t pos)
{
	size_t block = EWAH_BLOCK(pos);

	bitmap_grow(self, block + 1);
	self->words[block] |= EWAH_MASK(pos);
}

void bitmap_unset(struct bitmap *self, size_t pos)
{
	size_t block = EWAH_BLOCK(pos);

	if (block < self->word_alloc)
		self->words[block] &= ~EWAH_MASK(pos);
}

int bitmap_get(struct bitmap *self, size_t pos)
{
	size_t block = EWAH_BLOCK(pos);
	return block < self->word_alloc &&
		(self->words[block] & EWAH_MASK(pos)) != 0;
}

struct ewah_bitmap *bitmap_to_ewah(struct bitmap *bitmap)
{
	struct ewah_bitmap *ewah = ewah_new();
	size_t i, running_empty_words = 0;
	eword_t last_word = 0;

	for (i = 0; i < bitmap->word_alloc; ++i) {
		if (bitmap->words[i] == 0) {
			running_empty_words++;
			continue;
		}

		if (last_word != 0)
			ewah_add(ewah, last_word);

		if (running_empty_words > 0) {
			ewah_add_empty_words(ewah, 0, running_empty_words);
			running_empty_words = 0;
		}

		last_word = bitmap->words[i];
	}

	ewah_add(ewah, last_word);
	return ewah;
}

struct bitmap *ewah_to_bitmap(struct ewah_bitmap *ewah)
{
	struct bitmap *bitmap;
	size_t final_size = (ewah->bit_size / BITS_IN_EWORD) + 1;
	size_t pos = 0;
	struct ewah_block_iterator it;
	struct ewah_block blk;

	bitmap = bitmap_word_alloc(final_size);

	ewah_block_iterator_init(&it, ewah);
	while (ewah_block_iterator_next(&blk, &it)) {
		switch (blk.type) {
		case EWAH_BLOCK_RUN:
			if (blk.u.run.bit)
				memset(bitmap->words + pos, 0xFF,
				       blk.u.run.len * sizeof(eword_t));
			pos += blk.u.run.len;
			break;
		case EWAH_BLOCK_LITERAL:
			memcpy(bitmap->words + pos, blk.u.literal.words,
			       blk.u.literal.nr * sizeof(eword_t));
			pos += blk.u.literal.nr;
			break;
		}
	}

	return bitmap;
}

void bitmap_and_not(struct bitmap *self, struct bitmap *other)
{
	const size_t count = (self->word_alloc < other->word_alloc) ?
		self->word_alloc : other->word_alloc;

	size_t i;

	for (i = 0; i < count; ++i)
		self->words[i] &= ~other->words[i];
}

void bitmap_or(struct bitmap *self, const struct bitmap *other)
{
	size_t i;

	bitmap_grow(self, other->word_alloc);
	for (i = 0; i < other->word_alloc; i++)
		self->words[i] |= other->words[i];
}

int ewah_bitmap_is_subset(struct ewah_bitmap *self, struct bitmap *other)
{
	size_t pos = 0;
	struct ewah_block_iterator it;
	struct ewah_block blk;

	ewah_block_iterator_init(&it, self);
	while (ewah_block_iterator_next(&blk, &it)) {
		size_t j;

		switch (blk.type) {
		case EWAH_BLOCK_RUN:
			if (blk.u.run.bit) {
				for (j = 0; j < blk.u.run.len; j++, pos++)
					if (pos >= other->word_alloc ||
					    ~other->words[pos])
						return 0;
			} else {
				pos += blk.u.run.len;
			}
			break;
		case EWAH_BLOCK_LITERAL:
			for (j = 0; j < blk.u.literal.nr; j++, pos++)
				if (blk.u.literal.words[j] &
				    ~(pos < other->word_alloc ?
				      other->words[pos] : 0))
					return 0;
			break;
		}
	}

	return 1;
}

void bitmap_or_ewah(struct bitmap *self, struct ewah_bitmap *other)
{
	size_t original_size = self->word_alloc;
	size_t other_final = (other->bit_size / BITS_IN_EWORD) + 1;
	size_t pos = 0;
	struct ewah_block_iterator it;
	struct ewah_block blk;

	if (self->word_alloc < other_final) {
		self->word_alloc = other_final;
		REALLOC_ARRAY(self->words, self->word_alloc);
		MEMZERO_ARRAY(self->words + original_size,
			      self->word_alloc - original_size);
	}

	ewah_block_iterator_init(&it, other);
	while (ewah_block_iterator_next(&blk, &it)) {
		size_t j;

		switch (blk.type) {
		case EWAH_BLOCK_RUN:
			if (blk.u.run.bit)
				memset(self->words + pos, 0xFF,
				       blk.u.run.len * sizeof(eword_t));
			pos += blk.u.run.len;
			break;
		case EWAH_BLOCK_LITERAL:
			for (j = 0; j < blk.u.literal.nr; j++)
				self->words[pos + j] |= blk.u.literal.words[j];
			pos += blk.u.literal.nr;
			break;
		}
	}
}

size_t bitmap_popcount(struct bitmap *self)
{
	size_t i, count = 0;

	for (i = 0; i < self->word_alloc; ++i)
		count += ewah_bit_popcount64(self->words[i]);

	return count;
}

size_t ewah_bitmap_popcount(struct ewah_bitmap *self)
{
	struct ewah_block_iterator it;
	struct ewah_block blk;
	size_t count = 0;

	ewah_block_iterator_init(&it, self);
	while (ewah_block_iterator_next(&blk, &it)) {
		switch (blk.type) {
		case EWAH_BLOCK_RUN:
			if (blk.u.run.bit)
				count += blk.u.run.len * BITS_IN_EWORD;
			break;
		case EWAH_BLOCK_LITERAL:
		{
			size_t j;
			for (j = 0; j < blk.u.literal.nr; j++)
				count += ewah_bit_popcount64(blk.u.literal.words[j]);
			break;
		}
		}
	}

	return count;
}

int bitmap_is_empty(struct bitmap *self)
{
	size_t i;
	for (i = 0; i < self->word_alloc; i++)
		if (self->words[i])
			return 0;
	return 1;
}

int bitmap_equals(struct bitmap *self, struct bitmap *other)
{
	struct bitmap *big, *small;
	size_t i;

	if (self->word_alloc < other->word_alloc) {
		small = self;
		big = other;
	} else {
		small = other;
		big = self;
	}

	for (i = 0; i < small->word_alloc; ++i) {
		if (small->words[i] != big->words[i])
			return 0;
	}

	for (; i < big->word_alloc; ++i) {
		if (big->words[i] != 0)
			return 0;
	}

	return 1;
}

int bitmap_equals_ewah(struct bitmap *self, struct ewah_bitmap *other)
{
	struct ewah_block_iterator it;
	struct ewah_block blk;
	size_t pos = 0;

	ewah_block_iterator_init(&it, other);
	while (ewah_block_iterator_next(&blk, &it)) {
		size_t j;

		switch (blk.type) {
		case EWAH_BLOCK_RUN:
		{
			eword_t expected = blk.u.run.bit ? (eword_t)(~0) : 0;
			for (j = 0; j < blk.u.run.len; j++, pos++)
				if ((pos < self->word_alloc ?
				     self->words[pos] : 0) != expected)
					return 0;
			break;
		}
		case EWAH_BLOCK_LITERAL:
			for (j = 0; j < blk.u.literal.nr; j++, pos++)
				if ((pos < self->word_alloc ?
				     self->words[pos] : 0) !=
				    blk.u.literal.words[j])
					return 0;
			break;
		}
	}

	for (; pos < self->word_alloc; pos++)
		if (self->words[pos])
			return 0;

	return 1;
}

int bitmap_is_subset(struct bitmap *self, struct bitmap *other)
{
	size_t common_size, i;

	if (self->word_alloc < other->word_alloc)
		common_size = self->word_alloc;
	else {
		common_size = other->word_alloc;
		for (i = common_size; i < self->word_alloc; i++) {
			if (self->words[i])
				return 1;
		}
	}

	for (i = 0; i < common_size; i++) {
		if (self->words[i] & ~other->words[i])
			return 1;
	}
	return 0;
}

void bitmap_free(struct bitmap *bitmap)
{
	if (!bitmap)
		return;

	free(bitmap->words);
	free(bitmap);
}
