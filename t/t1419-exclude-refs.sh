#!/bin/sh

test_description='test exclude_patterns functionality in main ref store'

GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME=main
export GIT_TEST_DEFAULT_INITIAL_BRANCH_NAME

TEST_PASSES_SANITIZE_LEAK=true
. ./test-lib.sh

for_each_ref__exclude () {
	test-tool ref-store main for-each-ref--exclude "$@" >actual.raw
	cut -d ' ' -f 2 actual.raw
}

for_each_ref () {
	git for-each-ref --format='%(refname)' "$@"
}

test_expect_success 'setup' '
	test_commit --no-tag base &&
	base="$(git rev-parse HEAD)" &&

	for name in foo bar baz quux
	do
		for i in 1 2 3
		do
			echo "create refs/heads/$name/$i $base" || return 1
		done || return 1
	done >in &&
	echo "delete refs/heads/main" >>in &&

	git update-ref --stdin <in &&
	git pack-refs --all
'

test_expect_success 'for_each_ref__exclude(refs/heads/foo/)' '
	# region in middle
	for_each_ref__exclude refs/heads refs/heads/foo >actual &&
	for_each_ref refs/heads/bar refs/heads/baz refs/heads/quux >expect &&

	test_cmp expect actual
'

test_expect_success 'for_each_ref__exclude(refs/heads/bar/)' '
	# region at beginning
	for_each_ref__exclude refs/heads refs/heads/bar >actual &&
	for_each_ref refs/heads/baz refs/heads/foo refs/heads/quux >expect &&

	test_cmp expect actual
'

test_expect_success 'for_each_ref__exclude(refs/heads/quux/)' '
	# region at end
	for_each_ref__exclude refs/heads refs/heads/quux >actual &&
	for_each_ref refs/heads/foo refs/heads/bar refs/heads/baz >expect &&

	test_cmp expect actual
'

test_expect_success 'for_each_ref__exclude(refs/heads/bar/, refs/heads/quux/)' '
	# disjoint regions
	for_each_ref__exclude refs/heads refs/heads/bar refs/heads/quux >actual &&
	for_each_ref refs/heads/baz refs/heads/foo >expect &&

	test_cmp expect actual
'

test_expect_success 'for_each_ref__exclude(refs/heads/bar/, refs/heads/baz/)' '
	# adjacent, non-overlapping regions
	for_each_ref__exclude refs/heads refs/heads/bar refs/heads/baz >actual &&
	for_each_ref refs/heads/foo refs/heads/quux >expect &&

	test_cmp expect actual
'

test_expect_success 'for_each_ref__exclude(refs/heads/ba refs/heads/baz/)' '
	# overlapping region
	for_each_ref__exclude refs/heads refs/heads/ba refs/heads/baz >actual &&
	for_each_ref refs/heads/foo refs/heads/quux >expect &&

	test_cmp expect actual
'

test_expect_success 'for_each_ref__exclude(refs/heads/does/not/exist)' '
	# empty region
	for_each_ref__exclude refs/heads refs/heads/does/not/exist >actual &&
	for_each_ref >expect &&

	test_cmp expect actual
'

test_expect_success 'for_each_ref__exclude(refs/heads/ba*)' '
	# discards meta-characters
	for_each_ref__exclude refs/heads "refs/heads/ba*" >actual &&
	for_each_ref >expect &&

	test_cmp expect actual
'

test_done
