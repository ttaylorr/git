#!/bin/sh

test_description='multi-pack-index compaction'

. ./test-lib.sh

GIT_TEST_MULTI_PACK_INDEX=0
GIT_TEST_MULTI_PACK_INDEX_WRITE_BITMAP=0
GIT_TEST_MULTI_PACK_INDEX_WRITE_INCREMENTAL=0

objdir=.git/objects
packdir=$objdir/pack
midxdir=$packdir/multi-pack-index.d
midx_chain=$midxdir/multi-pack-index-chain

nth_line() {
	local n="$1"
	shift
	awk "NR==$n" "$@"
}

write_packs () {
	for c in "$@"
	do
		test_commit "$c" &&

		git pack-objects --all --unpacked $packdir/pack-$c &&
		git prune-packed &&

		git multi-pack-index write --incremental --bitmap || return 1
	done
}

test_midx_layer_packs () {
	local checksum="$1" &&
	shift &&

	test-tool read-midx $objdir "$checksum" >out &&

	printf "%s\n" "$@" | sort >expect &&
	grep "^pack-" out | cut -d"-" -f2 | sort >actual &&

	test_cmp expect actual
}

test_expect_success 'MIDX compaction with lex-ordered pack names' '
	git init midx-compact-lex-order &&
	(
		cd midx-compact-lex-order &&

		write_packs A B C D E &&
		test_line_count = 5 $midx_chain &&

		git multi-pack-index compact --incremental \
			"$(nth_line 2 "$midx_chain")" \
			"$(nth_line 4 "$midx_chain")" &&
		test_line_count = 3 $midx_chain &&

		test_midx_layer_packs "$(nth_line 1 "$midx_chain")" A &&
		test_midx_layer_packs "$(nth_line 2 "$midx_chain")" B C D &&
		test_midx_layer_packs "$(nth_line 3 "$midx_chain")" E
	)
'

test_expect_success 'MIDX compaction with non-lex-ordered pack names' '
	git init midx-compact-non-lex-order &&
	(
		cd midx-compact-non-lex-order &&

		write_packs D C A B E &&
		test_line_count = 5 $midx_chain &&

		git multi-pack-index compact --incremental \
			"$(nth_line 2 "$midx_chain")" \
			"$(nth_line 4 "$midx_chain")" &&
		test_line_count = 3 $midx_chain &&

		test_midx_layer_packs "$(nth_line 1 "$midx_chain")" D &&
		test_midx_layer_packs "$(nth_line 2 "$midx_chain")" C A B &&
		test_midx_layer_packs "$(nth_line 3 "$midx_chain")" E
	)
'

test_done
