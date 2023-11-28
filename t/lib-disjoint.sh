# Helpers for scripts testing disjoint packs; see t5319 for example usage.

objdir=.git/objects

test_disjoint_1 () {
	local pack="$1"
	local want="$2"

	test-tool read-midx --bitmap $objdir >out &&
	grep -A 3 "$pack" out >found &&

	if ! test -s found
	then
		echo >&2 "could not find '$pack' in MIDX"
		return 1
	fi

	if ! grep -q "disjoint: $want" found
	then
		echo >&2 "incorrect disjoint state for pack '$pack'"
		return 1
	fi
	return 0
}

# test_must_be_disjoint <pack-$XYZ.pack>
#
# Ensures that the given pack is marked as disjoint.
test_must_be_disjoint () {
	test_disjoint_1 "$1" "yes"
}

# test_must_not_be_disjoint <pack-$XYZ.pack>
#
# Ensures that the given pack is not marked as disjoint.
test_must_not_be_disjoint () {
	test_disjoint_1 "$1" "no"
}

# packed_contents </path/to/pack-$XYZ.idx [...]>
#
# Prints the set of objects packed in the given pack indexes.
packed_contents () {
	for idx in "$@"
	do
		git show-index <$idx || return 1
	done >tmp &&
	cut -d" " -f2 <tmp | sort -u
}
