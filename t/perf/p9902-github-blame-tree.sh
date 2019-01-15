#!/bin/sh

test_description='blame-tree perf tests'
. ./perf-lib.sh

test_perf_default_repo

test_perf 'top-level blame-tree' '
	git blame-tree --max-depth=0 HEAD
'

test_perf 'subdir blame-tree' '
	path=$(git ls-tree HEAD | grep ^040000 | head -n 1 | cut -f2)
	git blame-tree --max-depth=1 HEAD -- "$path"
'

test_done
