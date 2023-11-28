#!/bin/sh

test_description='git repack --extend-disjoint works correctly'

. ./test-lib.sh
. "${TEST_DIRECTORY}/lib-disjoint.sh"

packdir=.git/objects/pack

GIT_TEST_MULTI=0
GIT_TEST_MULTI_PACK_INDEX_WRITE_BITMAP=0

test_expect_success 'repack --extend-disjoint creates new disjoint packs' '
	git init repo &&
	(
		cd repo &&

		test_commit A &&
		test_commit B &&

		A="$(echo A | git pack-objects --revs $packdir/pack)" &&
		B="$(echo A..B | git pack-objects --revs $packdir/pack)" &&

		git prune-packed &&

		cat >in <<-EOF &&
		pack-$A.idx
		+pack-$B.idx
		EOF
		git multi-pack-index write --bitmap --stdin-packs <in &&

		test_must_not_be_disjoint "pack-$A.pack" &&
		test_must_be_disjoint "pack-$B.pack" &&

		test_commit C &&

		find $packdir -type f -name "*.idx" | sort >packs.before &&
		git repack --write-midx --write-bitmap-index --extend-disjoint &&
		find $packdir -type f -name "*.idx" | sort >packs.after &&

		comm -13 packs.before packs.after >packs.new &&

		test_line_count = 1 packs.new &&

		test_must_not_be_disjoint "pack-$A.pack" &&
		test_must_be_disjoint "pack-$B.pack" &&
		test_must_be_disjoint "$(basename $(cat packs.new) .idx).pack"
	)
'

test_expect_success 'repack --extend-disjoint combines existing disjoint packs' '
	(
		cd repo &&

		test_commit D &&

		git repack -a -d --write-midx --write-bitmap-index --extend-disjoint &&

		find $packdir -type f -name "*.pack" >packs &&
		test_line_count = 1 packs &&

		test_must_be_disjoint "$(basename $(cat packs))"

	)
'

test_expect_success 'repack --extend-disjoint with --geometric' '
	git init disjoint-geometric &&
	(
		cd disjoint-geometric &&

		test_commit_bulk 8 &&
		base="$(basename $(ls $packdir/pack-*.idx))" &&
		echo "+$base" >>in &&

		test_commit A &&
		A="$(echo HEAD^.. | git pack-objects --revs $packdir/pack)" &&
		test_commit B &&
		B="$(echo HEAD^.. | git pack-objects --revs $packdir/pack)" &&

		git prune-packed &&

		cat >>in <<-EOF &&
		+pack-$A.idx
		+pack-$B.idx
		EOF
		git multi-pack-index write --bitmap --stdin-packs <in &&

		test_must_be_disjoint "pack-$A.pack" &&
		test_must_be_disjoint "pack-$B.pack" &&
		test_must_be_disjoint "${base%.idx}.pack" &&

		test_commit C &&

		find $packdir -type f -name "*.pack" | sort >packs.before &&
		git repack --geometric=2 -d --write-midx --write-bitmap-index --extend-disjoint &&
		find $packdir -type f -name "*.pack" | sort >packs.after &&

		comm -12 packs.before packs.after >packs.unchanged &&
		comm -23 packs.before packs.after >packs.removed &&
		comm -13 packs.before packs.after >packs.new &&

		cat >expect <<-EOF &&
		$packdir/${base%.idx}.pack
		EOF
		test_cmp expect packs.unchanged &&

		sort >expect <<-EOF &&
		$packdir/pack-$A.pack
		$packdir/pack-$B.pack
		EOF
		test_cmp expect packs.removed &&

		test_line_count = 1 packs.new &&

		test_must_be_disjoint "$(basename $(cat packs.new))" &&
		test_must_be_disjoint "${base%.idx}.pack"
	)
'

for flag in "-A" "-a" "--cruft"
do
	test_expect_success "repack --extend-disjoint incompatible with $flag without -d" '
		test_must_fail git repack $flag --extend-disjoint \
			--write-midx --write-bitmap-index 2>actual &&
		cat >expect <<-EOF &&
		fatal: cannot use $SQ--extend-disjoint$SQ with $SQ$flag$SQ but not $SQ-d$SQ
		EOF
		test_cmp expect actual
	'
done

test_expect_success 'repack --extend-disjoint is incompatible with --filter-to' '
	test_must_fail git repack --extend-disjoint --filter-to=dir 2>actual &&

	cat >expect <<-EOF &&
	fatal: options $SQ--filter-to$SQ and $SQ--extend-disjoint$SQ cannot be used together
	EOF
	test_cmp expect actual
'

test_done
