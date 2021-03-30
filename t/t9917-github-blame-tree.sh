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

test_done
