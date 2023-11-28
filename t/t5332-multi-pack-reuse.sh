#!/bin/sh

test_description='pack-objects multi-pack reuse'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-bitmap.sh
. "$TEST_DIRECTORY"/lib-disjoint.sh

objdir=.git/objects
packdir=$objdir/pack

all_packs () {
	find $packdir -type f -name "*.idx" | sed -e 's/^.*\/\([^\/]\)/\1/g'
}

all_disjoint () {
	all_packs | sed -e 's/^/+/g'
}

test_pack_reused () {
	test_trace2_data pack-objects pack-reused "$1"
}

test_packs_reused () {
	test_trace2_data pack-objects packs-reused "$1"
}


# pack_position <object> </path/to/pack.idx
pack_position () {
	git show-index >objects &&
	grep "$1" objects | cut -d" " -f1
}

test_expect_success 'setup' '
	git config pack.allowPackReuse multi
'

test_expect_success 'preferred pack is reused without packs marked disjoint' '
	test_commit A &&
	test_commit B &&

	A="$(echo A | git pack-objects --unpacked --delta-base-offset $packdir/pack)" &&
	B="$(echo B | git pack-objects --unpacked --delta-base-offset $packdir/pack)" &&

	git prune-packed &&

	git multi-pack-index write --bitmap &&

	test_must_not_be_disjoint "pack-$A.pack" &&
	test_must_not_be_disjoint "pack-$B.pack" &&

	: >trace2.txt &&
	GIT_TRACE2_EVENT="$PWD/trace2.txt" \
		git pack-objects --stdout --revs --all >/dev/null &&

	test_pack_reused 3 <trace2.txt &&
	test_packs_reused 1 <trace2.txt
'

test_expect_success 'reuse all objects from subset of disjoint packs' '
	test_commit C &&

	C="$(echo C | git pack-objects --unpacked --delta-base-offset $packdir/pack)" &&

	git prune-packed &&

	cat >in <<-EOF &&
	pack-$A.idx
	+pack-$B.idx
	+pack-$C.idx
	EOF
	git multi-pack-index write --bitmap --stdin-packs <in &&

	test_must_not_be_disjoint "pack-$A.pack" &&
	test_must_be_disjoint "pack-$B.pack" &&
	test_must_be_disjoint "pack-$C.pack" &&

	: >trace2.txt &&
	GIT_TRACE2_EVENT="$PWD/trace2.txt" \
		git pack-objects --stdout --revs --all >/dev/null &&

	test_pack_reused 6 <trace2.txt &&
	test_packs_reused 2 <trace2.txt
'

test_expect_success 'reuse all objects from all disjoint packs' '
	rm -fr $packdir/multi-pack-index* &&

	all_disjoint >in &&
	git multi-pack-index write --bitmap --stdin-packs <in &&

	test_must_be_disjoint "pack-$A.pack" &&
	test_must_be_disjoint "pack-$B.pack" &&
	test_must_be_disjoint "pack-$C.pack" &&

	: >trace2.txt &&
	GIT_TRACE2_EVENT="$PWD/trace2.txt" \
		git pack-objects --stdout --revs --all >/dev/null &&

	test_pack_reused 9 <trace2.txt &&
	test_packs_reused 3 <trace2.txt
'

test_expect_success 'reuse objects from first disjoint pack with middle gap' '
	test_commit D &&
	test_commit E &&
	test_commit F &&

	# Set "pack.window" to zero to ensure that we do not create any
	# deltas, which could alter the amount of pack reuse we perform
	# (if, for e.g., we are not sending one or more bases).
	D="$(git -c pack.window=0 pack-objects --all --unpacked $packdir/pack)" &&

	d_pos="$(pack_position $(git rev-parse D) <$packdir/pack-$D.idx)" &&
	e_pos="$(pack_position $(git rev-parse E) <$packdir/pack-$D.idx)" &&
	f_pos="$(pack_position $(git rev-parse F) <$packdir/pack-$D.idx)" &&

	# commits F, E, and D, should appear in that order at the
	# beginning of the pack
	test $f_pos -lt $e_pos &&
	test $e_pos -lt $d_pos &&

	# Ensure that the pack we are constructing sorts ahead of any
	# other packs in lexical/bitmap order by choosing it as the
	# preferred pack.
	all_disjoint >in &&
	git multi-pack-index write --bitmap --preferred-pack="pack-$D.idx" \
		--stdin-packs <in &&

	test_must_be_disjoint pack-$A.pack &&
	test_must_be_disjoint pack-$B.pack &&
	test_must_be_disjoint pack-$C.pack &&
	test_must_be_disjoint pack-$D.pack &&

	cat >in <<-EOF &&
	$(git rev-parse E)
	^$(git rev-parse D)
	EOF

	: >trace2.txt &&
	GIT_TRACE2_EVENT="$PWD/trace2.txt" \
		git pack-objects --stdout --delta-base-offset --revs <in >/dev/null &&

	test_pack_reused 3 <trace2.txt &&
	test_packs_reused 1 <trace2.txt
'

test_expect_success 'reuse objects from middle disjoint pack with middle gap' '
	rm -fr $packdir/multi-pack-index* &&

	# Ensure that the pack we are constructing sort into any
	# position *but* the first one, by choosing a different pack as
	# the preferred one.
	all_disjoint >in &&
	git multi-pack-index write --bitmap --preferred-pack="pack-$A.idx" \
		--stdin-packs <in &&

	cat >in <<-EOF &&
	$(git rev-parse E)
	^$(git rev-parse D)
	EOF

	: >trace2.txt &&
	GIT_TRACE2_EVENT="$PWD/trace2.txt" \
		git pack-objects --stdout --delta-base-offset --revs <in >/dev/null &&

	test_pack_reused 3 <trace2.txt &&
	test_packs_reused 1 <trace2.txt
'

test_expect_success 'omit delta with uninteresting base' '
	git repack -adk &&

	test_seq 32 >f &&
	git add f &&
	test_tick &&
	git commit -m "delta" &&
	delta="$(git rev-parse HEAD)" &&

	test_seq 64 >f &&
	test_tick &&
	git commit -a -m "base" &&
	base="$(git rev-parse HEAD)" &&

	test_commit other &&

	git repack -d &&

	have_delta "$(git rev-parse $delta:f)" "$(git rev-parse $base:f)" &&

	all_disjoint >in &&
	git multi-pack-index write --bitmap --stdin-packs <in &&

	cat >in <<-EOF &&
	$(git rev-parse other)
	^$base
	EOF

	: >trace2.txt &&
	GIT_TRACE2_EVENT="$PWD/trace2.txt" \
		git pack-objects --stdout --delta-base-offset --revs <in >/dev/null &&

	# Even though all packs are marked disjoint, we can only reuse
	# the 3 objects corresponding to "other" from the latest pack.
	#
	# This is because even though we want "delta", we do not want
	# "base", meaning that we have to inflate the delta/base-pair
	# corresponding to the blob in commit "delta", which bypasses
	# the pack-reuse mechanism.
	#
	# The remaining objects from the other pack are similarly not
	# reused because their objects are on the uninteresting side of
	# the query.
	test_pack_reused 3 <trace2.txt &&
	test_packs_reused 1 <trace2.txt
'

test_done
