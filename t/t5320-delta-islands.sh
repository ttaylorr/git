#!/bin/sh

test_description='exercise delta islands'

. ./test-lib.sh

# returns true iff $1 is a delta based on $2
is_delta_base () {
	delta_base=$(echo "$1" | git cat-file --batch-check='%(deltabase)') &&
	echo >&2 "$1 has base $delta_base" &&
	test "$delta_base" = "$2"
}

is_delta_base_in_pack () {
	object=$1 &&
	base=$2 &&
	pack=$3 &&
	git verify-pack -v "$pack" >pack.objects &&
	awk -v object="$object" -v base="$base" '
		$1 == object { found = ($7 == base) }
		END { exit !found }
	' pack.objects
}

objects_in_pack () {
	git show-index <"$1" >objects.raw &&
	cut -d" " -f2 objects.raw | sort &&
	rm -f objects.raw
}

# generate a commit on branch $1 with a single file, "file", whose
# content is mostly based on the seed $2, but with a unique bit
# of content $3 appended. This should allow us to see whether
# blobs of different refs delta against each other.
commit() {
	blob=$({ test-tool genrandom "$2" 10240 && echo "$3"; } |
	       git hash-object -w --stdin) &&
	tree=$(printf '100644 blob %s\tfile\n' "$blob" | git mktree) &&
	commit=$(echo "$2-$3" | git commit-tree "$tree" ${4:+-p "$4"}) &&
	git update-ref "refs/heads/$1" "$commit" &&
	eval "$1"'=$(git rev-parse $1:file)' &&
	eval "echo >&2 $1=\$$1"
}

test_expect_success 'setup commits' '
	commit one seed 1 &&
	commit two seed 12
'

# Note: This is heavily dependent on the "prefer larger objects as base"
# heuristic.
test_expect_success 'vanilla repack deltas one against two' '
	git repack -adf &&
	is_delta_base $one $two
'

test_expect_success 'island repack with no island definition is vanilla' '
	git repack -adfi &&
	is_delta_base $one $two
'

test_expect_success 'island repack with no matches is vanilla' '
	git -c "pack.island=refs/foo" repack -adfi &&
	is_delta_base $one $two
'

test_expect_success 'separate islands disallows delta' '
	git -c "pack.island=refs/heads/(.*)" repack -adfi &&
	! is_delta_base $one $two &&
	! is_delta_base $two $one
'

test_expect_success 'same island allows delta' '
	git -c "pack.island=refs/heads" repack -adfi &&
	is_delta_base $one $two
'

test_expect_success 'coalesce same-named islands' '
	git \
		-c "pack.island=refs/(.*)/one" \
		-c "pack.island=refs/(.*)/two" \
		repack -adfi &&
	is_delta_base $one $two
'

test_expect_success 'island restrictions drop reused deltas' '
	git repack -adfi &&
	is_delta_base $one $two &&
	git -c "pack.island=refs/heads/(.*)" repack -adi &&
	! is_delta_base $one $two &&
	! is_delta_base $two $one
'

test_expect_success 'island regexes are left-anchored' '
	git -c "pack.island=heads/(.*)" repack -adfi &&
	is_delta_base $one $two
'

test_expect_success 'island regexes follow last-one-wins scheme' '
	git \
		-c "pack.island=refs/heads/(.*)" \
		-c "pack.island=refs/heads/" \
		repack -adfi &&
	is_delta_base $one $two
'

test_expect_success 'setup shared history' '
	commit root shared root &&
	commit one shared 1 root &&
	commit two shared 12-long root
'

# We know that $two will be preferred as a base from $one,
# because we can transform it with a pure deletion.
#
# We also expect $root as a delta against $two by the "longest is base" rule.
test_expect_success 'vanilla delta goes between branches' '
	git repack -adf &&
	is_delta_base $one $two &&
	is_delta_base $root $two
'

# Here we should allow $one to base itself on $root; even though
# they are in different islands, the objects in $root are in a superset
# of islands compared to those in $one.
#
# Similarly, $two can delta against $root by our rules. And unlike $one,
# in which we are just allowing it, the island rules actually put $root
# as a possible base for $two, which it would not otherwise be (due to the size
# sorting).
test_expect_success 'deltas allowed against superset islands' '
	git -c "pack.island=refs/heads/(.*)" repack -adfi &&
	is_delta_base $one $root &&
	is_delta_base $two $root
'

# We are going to test the packfile order here, so we again have to make some
# assumptions. We assume that "$root", as part of our core "one", must come
# before "$two". This should be guaranteed by the island code. However, for
# this test to fail without islands, we are also assuming that it would not
# otherwise do so. This is true by the current write order, which will put
# commits (and their contents) before their parents.
test_expect_success 'island core places core objects first' '
	cat >expect <<-EOF &&
	$root
	$two
	EOF
	git -c "pack.island=refs/heads/(.*)" \
	    -c "pack.islandcore=one" \
	    repack -adfi &&
	git verify-pack -v .git/objects/pack/*.pack |
	cut -d" " -f1 |
	grep -E "$root|$two" >actual &&
	test_cmp expect actual
'

test_expect_success 'unmatched island core is not fatal' '
	git -c "pack.islandcore=one" repack -adfi
'

test_expect_success '--stdin-packs=follow respects delta islands' '
	test_when_finished "rm -fr stdin-follow" &&
	git init stdin-follow &&
	(
		cd stdin-follow &&
		packdir=.git/objects/pack &&

		commit one seed 1 &&
		commit two seed 12 &&
		one_parent=$(git rev-parse one) &&
		two_parent=$(git rev-parse two) &&

		one_tip=$(git commit-tree "$(git rev-parse one^{tree})" \
			-p "$one_parent" -m one-tip) &&
		two_tip=$(git commit-tree "$(git rev-parse two^{tree})" \
			-p "$two_parent" -m two-tip) &&
		git update-ref refs/heads/one "$one_tip" &&
		git update-ref refs/heads/two "$two_tip" &&

		parents=$(git pack-objects --revs $packdir/pack <<-EOF
		$one_parent
		$two_parent
		EOF
		) &&
		tips=$(printf "%s\n%s\n" "$one_tip" "$two_tip" |
			git pack-objects $packdir/pack) &&
		git prune-packed &&
		echo "pack-$parents.pack" >packs &&

		vanilla=$(git pack-objects --stdin-packs=follow \
			--no-reuse-delta $packdir/vanilla <packs) &&
		is_delta_base_in_pack $one $two \
			$packdir/vanilla-$vanilla.pack &&

		islands=$(git -c "pack.island=refs/heads/(.*)" \
			pack-objects --stdin-packs=follow --delta-islands \
			--no-reuse-delta $packdir/islands <packs) &&
		objects_in_pack $packdir/vanilla-$vanilla.idx >expect &&
		objects_in_pack $packdir/islands-$islands.idx >actual &&
		test_cmp expect actual &&
		! is_delta_base_in_pack $one $two \
			$packdir/islands-$islands.pack &&
		! is_delta_base_in_pack $two $one \
			$packdir/islands-$islands.pack
	)
'

test_done
