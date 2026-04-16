#!/bin/sh

test_description='incremental multi-pack-index'

. ./test-lib.sh
. "$TEST_DIRECTORY"/lib-midx.sh
. "$TEST_DIRECTORY"/lib-chunk.sh

GIT_TEST_MULTI_PACK_INDEX=0
export GIT_TEST_MULTI_PACK_INDEX

objdir=.git/objects
packdir=$objdir/pack
midxdir=$packdir/multi-pack-index.d
midx_chain=$midxdir/multi-pack-index-chain

test_expect_success 'convert non-incremental MIDX to incremental' '
	test_commit base &&
	git config set maintenance.auto false &&
	git repack -ad &&
	git multi-pack-index write &&

	test_path_is_file $packdir/multi-pack-index &&
	old_hash="$(midx_checksum $objdir)" &&

	test_commit other &&
	git repack -d &&
	git multi-pack-index write --incremental &&

	test_path_is_missing $packdir/multi-pack-index &&
	test_path_is_file $midx_chain &&
	test_line_count = 2 $midx_chain &&
	grep $old_hash $midx_chain
'

compare_results_with_midx 'incremental MIDX'

test_expect_success 'convert incremental to non-incremental' '
	test_commit squash &&
	git repack -d &&
	git multi-pack-index write &&

	test_path_is_file $packdir/multi-pack-index &&
	test_dir_is_empty $midxdir
'

compare_results_with_midx 'non-incremental MIDX conversion'

write_midx_layer () {
	n=1
	if test -f $midx_chain
	then
		n="$(($(wc -l <$midx_chain) + 1))"
	fi

	for i in 1 2
	do
		test_commit $n.$i &&
		git repack -d || return 1
	done &&
	git multi-pack-index write --bitmap --incremental
}

test_expect_success 'write initial MIDX layer' '
	git repack -ad &&
	write_midx_layer
'

test_expect_success 'read bitmap from first MIDX layer' '
	git rev-list --test-bitmap 1.2
'

test_expect_success 'write another MIDX layer' '
	write_midx_layer
'

test_expect_success 'midx verify with multiple layers' '
	test_path_is_file "$midx_chain" &&
	test_line_count = 2 "$midx_chain" &&

	git multi-pack-index verify
'

test_expect_success 'read bitmap from second MIDX layer' '
	git rev-list --test-bitmap 2.2
'

test_expect_success 'read earlier bitmap from second MIDX layer' '
	git rev-list --test-bitmap 1.2
'

test_expect_success 'show object from first pack' '
	git cat-file -p 1.1
'

test_expect_success 'show object from second pack' '
	git cat-file -p 2.2
'

test_expect_success 'write MIDX layer with --no-write-chain-file' '
	test_commit no-write-chain-file &&
	git repack -d &&

	cp "$midx_chain" "$midx_chain.bak" &&
	layer="$(git multi-pack-index write --bitmap --incremental \
		--no-write-chain-file)" &&

	test_cmp "$midx_chain.bak" "$midx_chain" &&
	test_path_is_file "$midxdir/multi-pack-index-$layer.midx"
'

test_expect_success 'write non-incremental MIDX layer with --no-write-chain-file' '
	test_must_fail git multi-pack-index write --bitmap --no-write-chain-file 2>err &&
	test_grep "cannot use --no-write-chain-file without --incremental" err
'

test_expect_success 'write MIDX layer with --base without --no-write-chain-file' '
	test_must_fail git multi-pack-index write --bitmap --incremental \
		--base=none 2>err &&
	test_grep "cannot use --base without --no-write-chain-file" err
'

test_expect_success 'write MIDX layer with --base=none and --no-write-chain-file' '
	test_commit base-none &&
	git repack -d &&

	cp "$midx_chain" "$midx_chain.bak" &&
	layer="$(git multi-pack-index write --bitmap --incremental \
		--no-write-chain-file --base=none)" &&

	test_cmp "$midx_chain.bak" "$midx_chain" &&
	test_path_is_file "$midxdir/multi-pack-index-$layer.midx"
'

test_expect_success 'write MIDX layer with --base=<hash> and --no-write-chain-file' '
	test_commit base-hash &&
	git repack -d &&

	cp "$midx_chain" "$midx_chain.bak" &&
	layer="$(git multi-pack-index write --bitmap --incremental \
		--no-write-chain-file --base="$(nth_line 1 "$midx_chain")")" &&

	test_cmp "$midx_chain.bak" "$midx_chain" &&
	test_path_is_file "$midxdir/multi-pack-index-$layer.midx"
'

for reuse in false single multi
do
	test_expect_success "full clone (pack.allowPackReuse=$reuse)" '
		rm -fr clone.git &&

		git config pack.allowPackReuse $reuse &&
		git clone --no-local --bare . clone.git
	'
done

test_expect_success 'relink existing MIDX layer' '
	rm -fr "$midxdir" &&

	GIT_TEST_MIDX_WRITE_REV=1 git multi-pack-index write --bitmap &&

	midx_hash="$(test-tool read-midx --checksum $objdir)" &&

	test_path_is_file "$packdir/multi-pack-index" &&
	test_path_is_file "$packdir/multi-pack-index-$midx_hash.bitmap" &&
	test_path_is_file "$packdir/multi-pack-index-$midx_hash.rev" &&

	test_commit another &&
	git repack -d &&
	git multi-pack-index write --bitmap --incremental &&

	test_path_is_missing "$packdir/multi-pack-index" &&
	test_path_is_missing "$packdir/multi-pack-index-$midx_hash.bitmap" &&
	test_path_is_missing "$packdir/multi-pack-index-$midx_hash.rev" &&

	test_path_is_file "$midxdir/multi-pack-index-$midx_hash.midx" &&
	test_path_is_file "$midxdir/multi-pack-index-$midx_hash.bitmap" &&
	test_path_is_file "$midxdir/multi-pack-index-$midx_hash.rev" &&
	test_line_count = 2 "$midx_chain"

'

test_expect_success 'non-incremental write with existing incremental chain' '
	git init non-incremental-write-with-existing &&
	test_when_finished "rm -fr non-incremental-write-with-existing" &&

	(
		cd non-incremental-write-with-existing &&

		git config set maintenance.auto false &&

		write_midx_layer &&
		write_midx_layer &&

		git multi-pack-index write
	)
'

test_expect_success 'verify detects corrupted base layer checksum' '
	git init verify-base-checksum &&
	test_when_finished "rm -fr verify-base-checksum" &&
	(
		cd verify-base-checksum &&
		git config set maintenance.auto false &&

		for i in 1 2 3
		do
			test_commit c$i &&
			git repack -d &&
			git multi-pack-index write --incremental || return 1
		done &&

		base_hash=$(sed -n 1p "$midx_chain") &&
		base_midx="$midxdir/multi-pack-index-$base_hash.midx" &&
		chmod u+w "$base_midx" &&
		size=$(test_file_size "$base_midx") &&
		seek=$((size - 1)) &&
		printf "\\xff" |
			dd of="$base_midx" bs=1 count=1 conv=notrunc seek=$seek &&

		test_must_fail git multi-pack-index verify 2>err &&
		grep "incorrect checksum" err
	)
'

test_expect_success 'verify detects out-of-order OIDs in base layer' '
	git init verify-base-oid-order &&
	test_when_finished "rm -fr verify-base-oid-order" &&
	(
		cd verify-base-oid-order &&
		git config set maintenance.auto false &&

		for i in 1 2 3
		do
			test_commit c$i &&
			git repack -d &&
			git multi-pack-index write --incremental || return 1
		done &&

		# Corrupt the first byte of the OID lookup chunk of the
		# base layer. Since the base layer has >= 2 OIDs in
		# sorted order, flipping the high bits of oid[0] makes
		# it sort above oid[1].
		base_hash=$(sed -n 1p "$midx_chain") &&
		base_midx="$midxdir/multi-pack-index-$base_hash.midx" &&
		chmod u+w "$base_midx" &&
		corrupt_chunk_file "$base_midx" OIDL 0 ff &&

		test_must_fail git multi-pack-index verify 2>err &&
		grep "oid lookup out of order" err
	)
'

test_expect_success 'bitmapped rev-list survives missing base-layer bitmap' '
	git init missing-base-bitmap &&
	test_when_finished "rm -fr missing-base-bitmap" &&
	(
		cd missing-base-bitmap &&
		git config maintenance.auto false &&

		test_commit base &&
		git repack -adq &&
		git multi-pack-index write --bitmap --incremental &&

		test_commit tip &&
		git repack -dq &&
		git multi-pack-index write --bitmap --incremental &&

		base_hash=$(sed -n 1p "$midx_chain") &&
		rm -f "$midxdir/multi-pack-index-$base_hash.bitmap" &&

		git rev-list --use-bitmap-index --count --all >out 2>err &&
		test_line_count = 1 out
	)
'

test_expect_success 'cat-file --batch-all-objects --filter covers full chain' '
	git init batch-filter &&
	test_when_finished "rm -fr batch-filter" &&
	(
		cd batch-filter &&
		git config maintenance.auto false &&

		for i in 1 2 3
		do
			test_commit "base-$i" || exit 1
		done &&
		git repack -adq &&
		git multi-pack-index write --bitmap --incremental &&

		for i in 1 2
		do
			test_commit "tip-$i" || exit 1
		done &&
		git repack -dq &&
		git multi-pack-index write --bitmap --incremental &&

		git cat-file --batch-all-objects --batch-check >expect &&
		git cat-file --batch-all-objects --batch-check \
			--filter=blob:limit=1m >actual &&
		test_cmp expect actual
	)
'

test_expect_success 'open_midx_bitmap_1 reports correct pack name on failure' '
	git init missing-base-pack &&
	test_when_finished "rm -fr missing-base-pack" &&
	(
		cd missing-base-pack &&
		git config maintenance.auto false &&

		for i in 1 2 3
		do
			test_commit "c$i" &&
			git repack -dq || exit 1
		done &&
		git multi-pack-index write --bitmap --incremental &&

		test_commit tip &&
		git repack -dq &&
		git multi-pack-index write --bitmap --incremental &&

		# Create an additional pack containing all reachable
		# objects (without -d, so the MIDX-referenced packs
		# stay on disk). After we delete one of the
		# MIDX-referenced base packs, objects remain available
		# through this "spare" pack -- exercising the bitmap
		# open path far enough to hit prepare_midx_pack().
		git repack -aq &&

		tip_pack=$(ls -t $packdir/pack-*.pack | head -1) &&
		# Prefer a pack that is not the tip pack nor the spare
		# we just created.
		spare_pack=$(ls -t $packdir/pack-*.pack | sed -n 1p) &&
		doomed=$(ls $packdir/pack-*.pack | grep -v "$spare_pack" | head -1) &&
		doomed_hash=$(echo "$doomed" | sed -e "s,.*/pack-,," -e "s,\\.pack$,,") &&
		rm -f "$doomed" &&

		git rev-list --use-bitmap-index --count --all 2>err &&
		grep "pack-$doomed_hash" err
	)
'

test_done
