#!/bin/sh

test_description='basic blame-tree tests'
. ./test-lib.sh

test_expect_success 'setup' '
	test_commit 1 file &&
	mkdir a &&
	test_commit 2 a/file &&
	mkdir a/b &&
	test_commit 3 a/b/file &&
	mkdir mult_of8 &&
	test_commit 4 mult_of8/file
'

test_expect_success 'cannot blame two trees' '
	test_must_fail git blame-tree HEAD HEAD~1
'

check_blame() {
	cat >expect &&
	git blame-tree "$@" >actual &&
	git name-rev --stdin --name-only --tags <actual >tmp &&
	mv tmp actual &&
	tr '\t' ' ' <actual >tmp &&
	mv tmp actual &&
	sort <actual >tmp &&
	mv tmp actual &&
	test_cmp expect actual
}

test_expect_success 'blame recursive' '
	check_blame <<-\EOF
	1 file
	2 a/file
	3 a/b/file
	4 mult_of8/file
	EOF
'

test_expect_success 'blame root' '
	check_blame --max-depth=0 <<-\EOF
	1 file
	3 a
	4 mult_of8
	EOF
'

test_expect_success 'blame subdir' '
	check_blame --max-depth=1 a <<-\EOF
	2 a/file
	3 a/b
	EOF
'

test_expect_success 'blame from non-HEAD commit' '
	check_blame --max-depth=0 HEAD^ <<-\EOF
	1 file
	3 a
	EOF
'

test_expect_success 'blame from subdir defaults to root' '(
	cd a &&
	check_blame --max-depth=0 <<-\EOF
	1 file
	3 a
	4 mult_of8
	EOF
)'

test_expect_success 'blame from subdir uses relative pathspecs' '(
	cd a &&
	check_blame --max-depth=1 b <<-\EOF
	3 a/b/file
	EOF
)'

test_expect_success 'limit blame traversal by count' '
	check_blame --max-depth=0 -1 <<-\EOF
	4 mult_of8
	^3 a
	^3 file
	EOF
'

test_expect_success 'limit blame traversal by commit' '
	check_blame --max-depth=0 HEAD~2..HEAD <<-\EOF
	3 a
	4 mult_of8
	^2 file
	EOF
'

test_expect_success 'only blame files in the current tree' '
	git rm -rf a &&
	git commit -m "remove a" &&
	check_blame <<-\EOF
	1 file
	4 mult_of8/file
	EOF
'

test_expect_success 'cross merge boundaries in blaming' '
	git checkout HEAD^0 &&
	git rm -rf . &&
	test_commit m1 &&
	git checkout HEAD^ &&
	git rm -rf . &&
	test_commit m2 &&
	git merge m1 &&
	check_blame <<-\EOF
	m1 m1.t
	m2 m2.t
	EOF
'

test_expect_success 'blame merge for resolved conflicts' '
	git checkout HEAD^0 &&
	git rm -rf . &&
	test_commit c1 conflict &&
	git checkout HEAD^ &&
	git rm -rf . &&
	test_commit c2 conflict &&
	test_must_fail git merge c1 &&
	test_commit resolved conflict &&
	check_blame conflict <<-\EOF
	resolved conflict
	EOF
'

test_expect_success 'blame-tree with merged cherry-pick' '
	git switch -c branchA &&
	echo stuff >file &&
	git add file &&
	test_commit A &&
	git switch -c branchB branchA~1 &&
	echo stuff >file &&
	git add file &&
	test_tick && # Ensure this commit is later
	test_commit B &&
	git switch -c mergeM branchA &&
	git merge branchB &&
	check_blame --max-depth=0 <<-\EOF
	A A.t
	A file
	B B.t
	resolved conflict
	EOF
'

test_expect_success 'blame-tree dereferences tags' '
	git tag -m lo lo HEAD~2 &&
	git tag -m hi hi HEAD &&
	cat >want <<-\EOF &&
		A A.t
		A file
		B B.t
		^resolved conflict
	EOF
	check_blame HEAD~2..HEAD <want &&
	check_blame lo..hi <want
'

test_expect_success 'blame-tree complains about non-commits' '
	test_must_fail git blame-tree HEAD^{tree}
'

test_expect_success 'blame-tree complains about unknown arguments' '
	test_must_fail git blame-tree --foo 2>err &&
	grep "unknown blame-tree argument: --foo" err
'

test_expect_success 'blame-tree complains about caching without --max-depth' '
	test_must_fail git blame-tree --cache HEAD 2>err &&
	grep "refusing to cache without --max-depth" err
'

test_expect_success 'blame-tree succeeds on commit with empty tree' '
	treehash=$(git mktree </dev/null) &&
	commithash=$(git commit-tree -m "empty commit" $treehash) &&
	mergehash=$(git commit-tree -p $commithash -p HEAD -m "merge empty on first-parent" HEAD^{tree}) &&
	git reset --hard $mergehash &&
	check_blame --max-depth=0 <<-\EOF
	A A.t
	A file
	B B.t
	resolved conflict
	EOF
'

test_expect_success '--cache writes to a btc file' '
	test_when_finished rm -rf .git/objects/info/blame-tree &&
	git blame-tree --cache --max-depth=0 >out 2>err &&
	test_path_is_dir .git/objects/info/blame-tree &&
	test_must_be_empty out &&
	test_must_be_empty err
'

test_expect_success '--cache writes to a btc file (extra details)' '
	test_when_finished rm -rf .git/objects/info/blame-tree &&
	git add a &&
	git commit -m "add dir a" &&
	echo new >a/new &&
	git add a/new &&
	git commit -m "add a/new" &&
	git blame-tree --cache --max-depth=1 -- a >out 2>err &&
	test_path_is_dir .git/objects/info/blame-tree &&
	test_must_be_empty out &&
	test_must_be_empty err
'

test_expect_success 'blame-tree cache is used during write' '
	test_when_finished rm -rf .git/objects/info/blame-tree blame-tree &&
	test_when_finished rm -f trace-* &&

	git blame-tree --cache --max-depth=0 HEAD~1 &&
	GIT_TRACE2_PERF="$(pwd)/trace-reads-cache" \
		git blame-tree --cache --max-depth=0 HEAD &&
	grep "cached-commit-true" trace-reads-cache &&

	GIT_TRACE2_PERF="$(pwd)/trace-skips-cache" \
		git -c blameTree.skipReadCache=true \
		blame-tree --cache --max-depth=0 HEAD &&
	grep "cached-commit-false" trace-skips-cache
'

test_expect_success 'blame-tree cache works across alternates' '
	test_when_finished rm -rf .git/objects/info/blame-tree blame-tree &&
	test_when_finished rm -f trace-* &&

	git clone --shared . fork &&
	git blame-tree --cache --max-depth=0 &&
	GIT_TRACE2_PERF="$(pwd)/trace-0-base" \
		git blame-tree --max-depth=0 >expect &&
	GIT_TRACE2_PERF="$(pwd)/trace-0-fork" \
		git -C fork blame-tree --max-depth=0 >actual &&
	test_cmp expect actual &&
	grep "cached-commit-true" trace-0-base &&
	grep "cached-commit-true" trace-0-fork &&

	git blame-tree --cache --max-depth=1 -- a &&
	GIT_TRACE2_PERF="$(pwd)/trace-1-base" \
		git blame-tree --max-depth=1 -- a >expect &&
	GIT_TRACE2_PERF="$(pwd)/trace-1-fork" \
		git -C fork blame-tree --max-depth=1 -- a >actual &&
	test_cmp expect actual &&
	grep "cached-commit-true" trace-1-base &&
	grep "cached-commit-true" trace-1-fork
'

test_expect_success 'blame-tree cache --max-depth=0 collision' '
	test_when_finished rm -rf .git/objects/info/blame-tree &&
	test_when_finished rm -f trace-* &&

	git blame-tree HEAD --max-depth=0 >expect-0 &&
	git blame-tree HEAD >expect-full &&

	# Cache the root directory.
	git blame-tree --cache HEAD --max-depth=0 &&

	GIT_TRACE2_PERF="$(pwd)/trace-depth-0" \
		git blame-tree HEAD --max-depth=0 >actual-0 &&
	GIT_TRACE2_PERF="$(pwd)/trace-depth-none" \
		git blame-tree HEAD >actual-full &&

	test_cmp expect-0 actual-0 &&
	test_cmp expect-full actual-full &&

	grep "cached-commit-true.count:1" trace-depth-0 &&
	grep "cached-commit-false.count:1" trace-depth-none
'

test_expect_success 'test cache with unicode paths' '
	test_when_finished rm -rf .git/objects/info/blame-tree blame-tree &&

	# The first unicode character Ċ is U+010A, so the second
	# byte looks like a newline.
	# The second unicode character Ā is U+0100, so the second
	# byte looks like a nul terminator.
	echo >_Ċ_Ā_ &&
	git add _Ċ_Ā_ &&
	git commit -m "add unicode pathname" &&

	git blame-tree --max-depth=0 >expect &&
	git blame-tree --cache --max-depth=0 &&
	git blame-tree --max-depth=0 >actual &&
	test_cmp expect actual
'

test_expect_success '--update-cache populates all cache files' '
	test_when_finished rm -rf .git/objects/info/blame-tree blame-tree &&
	# Compute expected cache files
	git blame-tree --cache --max-depth=1 HEAD -- a &&
	git blame-tree --cache --max-depth=0 HEAD &&
	git blame-tree --cache --max-depth=1 HEAD -- mult_of8 &&

	mv .git/objects/info/blame-tree blame-tree &&
	git blame-tree --cache --max-depth=1 HEAD~1 -- a &&
	git blame-tree --cache --max-depth=0 HEAD~1 &&
	git blame-tree --cache --max-depth=1 HEAD~1 -- mult_of8 &&

	for filename in $(ls blame-tree/*.btc)
	do
		test_path_is_file .git/objects/info/$filename || return 1
	done &&
	git blame-tree --update-cache HEAD &&
	for filename in $(ls blame-tree/*.btc)
	do
		test_path_is_file .git/objects/info/$filename &&
		test_cmp $filename .git/objects/info/$filename || return 1
	done
'

test_expect_success 'timeout creates an empty cache file' '
	test_when_finished rm -rf .git/objects/info/blame-tree blame-tree &&

	# Root directory skips the auto-cache.
	git -c blameTree.limitMilliseconds=0 \
		blame-tree --max-depth=0 HEAD &&
	test_path_is_missing .git/objects/info/blame-tree &&

	git -c blameTree.limitMilliseconds=0 \
		blame-tree --max-depth=1 HEAD -- a &&
	ls .git/objects/info/blame-tree/*.btc >actual &&
	test_line_count = 1 actual
'

test_expect_success 'modify multiple caches, with limit' '
	test_when_finished rm -rf .git/objects/info/blame-tree &&
	test_when_finished git config --unset blameTree.limitMilliseconds &&

	git blame-tree --cache --max-depth=0 &&
	file="$(ls .git/objects/info/blame-tree/*.btc)" &&
	test-tool chmtime =-10 "$file" &&

	# populate with empty cache files
	git config blameTree.limitMilliseconds 0 &&
	for i in $(test_seq 1 5)
	do
		cp -r a $i &&
		git add $i &&
		git commit -m "add $i" &&
		git blame-tree --max-depth=1 -- $i &&
		file="$(ls -t .git/objects/info/blame-tree/*.btc | head -n 1)" &&
		backtime=$((10 - i)) &&
		test-tool chmtime =-$backtime "$file" || return 1
	done &&

	ls -t .git/objects/info/blame-tree/ >before-all &&
	tail -n 3 before-all | sort >before-oldest &&
	head -n 3 before-all >before-newest &&

	git -c blameTree.maxWrites=2 blame-tree --update-cache HEAD &&

	ls -t .git/objects/info/blame-tree/ >after-all &&
	tail -n 3 after-all >after-oldest &&
	head -n 3 after-all | sort >after-newest &&

	test_cmp before-oldest after-newest &&
	test_cmp before-newest after-oldest &&

	# Double-check which caches were hit
	for i in $(test_seq 1 2)
	do
		GIT_TRACE2_PERF="$(pwd)/trace-$i" \
			git blame-tree --max-depth=1 -- $i &&
		grep cached-commit-true trace-$i || return 1
	done &&
	for i in $(test_seq 3 5)
	do
		GIT_TRACE2_PERF="$(pwd)/trace-$i" \
			git blame-tree --max-depth=1 -- $i &&
		grep cached-commit-false trace-$i || return 1
	done
'

test_expect_success 'cache with strange pathnames' '
	mkdir dir\*name &&
	echo >dir\*name/file &&
	git add dir\*name &&
	git commit -m "add strange path" &&
	git blame-tree --max-depth=1 --cache HEAD -- dir\*name  &&

	git blame-tree --update-cache HEAD 2>err &&
	test_must_be_empty err
'

test_expect_success '--update-cache removes non-existent paths' '
	rm -rf .git/objects/info/blame-tree &&

	# Make sure there is a cache file for "a"
	git blame-tree --max-depth=1 --cache HEAD -- a &&
	git blame-tree --update-cache HEAD &&
	ls .git/objects/info/blame-tree/*.btc >before &&

	# Contains cache for root and "a"
	test_line_count = 2 before &&

	git rm -r a &&
	git commit -m "remove a" &&
	git blame-tree --update-cache HEAD 2>err &&
	test_must_be_empty err &&

	ls .git/objects/info/blame-tree/*.btc >after &&
	test_line_count = 1 after
'

test_done
