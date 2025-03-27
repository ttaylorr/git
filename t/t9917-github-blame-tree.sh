#!/bin/sh

test_description='basic blame-tree tests'
. ./test-lib.sh

test_expect_success 'setup' '
	test_commit 1 file &&
	mkdir a &&
	test_commit 2 a/file &&
	mkdir a/b &&
	test_commit 3 a/b/file
'

test_expect_success 'cannot blame two trees' '
	test_must_fail git blame-tree HEAD HEAD~1
'

check_blame() {
	cat >expect &&
	for arg in "" --go-faster
	do
		git blame-tree $arg "$@" >actual &&
		git name-rev --stdin --name-only --tags <actual >tmp &&
		mv tmp actual &&
		tr '\t' ' ' <actual >tmp &&
		mv tmp actual &&
		sort <actual >tmp &&
		mv tmp actual &&
		test_cmp expect actual
	done
}

test_expect_success 'blame recursive' '
	check_blame <<-\EOF
	1 file
	2 a/file
	3 a/b/file
	EOF
'

test_expect_success 'blame root' '
	check_blame --max-depth=0 <<-\EOF
	1 file
	3 a
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
	2 a
	EOF
'

test_expect_success 'blame from subdir defaults to root' '(
	cd a &&
	check_blame --max-depth=0 <<-\EOF
	1 file
	3 a
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
	3 a
	^2 file
	EOF
'

test_expect_success 'limit blame traversal by commit' '
	check_blame --max-depth=0 HEAD~2..HEAD <<-\EOF
	3 a
	^1 file
	EOF
'

test_expect_success 'limit blame traversal using config' '
	test_config blametree.revopts -1 &&
	check_blame --max-depth=0 <<-\EOF
	3 a
	^2 file
	EOF
'

test_expect_success 'only blame files in the current tree' '
	git rm -rf a &&
	git commit -m "remove a" &&
	check_blame <<-\EOF
	1 file
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
	git blame-tree --max-depth=0 >old &&
	git blame-tree --go-faster --max-depth=0 >new &&
	grep "$(git rev-parse branchB)	file" old &&
	grep "$(git rev-parse branchA)	file" new
'

test_expect_success 'blame-tree dereferences tags (old)' '
	git tag -m lo lo HEAD~2 &&
	git tag -m hi hi HEAD &&
	git blame-tree HEAD~2..HEAD >by-commit &&
	git blame-tree lo..hi >by-tag &&
	test_cmp by-commit by-tag
'

test_expect_success 'blame-tree dereferences tags (go-faster)' '
	git blame-tree --go-faster HEAD~2..HEAD >by-commit &&
	git blame-tree --go-faster lo..hi >by-tag &&
	test_cmp by-commit by-tag
'

test_expect_failure 'blame-tree complains about non-commits (old)' '
	test_must_fail git blame-tree HEAD^{tree}
'

test_expect_success 'blame-tree complains about non-commits (go-faster)' '
	test_must_fail git blame-tree --go-faster HEAD^{tree}
'

test_done
