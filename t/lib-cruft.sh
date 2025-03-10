# generate_random_blob <seed-string> [<size>]
generate_random_blob () {
	test-tool genrandom "$@" >blob &&
	git hash-object -w -t blob blob &&
	rm blob
}

# pack_random_blob <seed-string> [<size>]
pack_random_blob () {
	generate_random_blob "$@" &&
	git repack -d -q >/dev/null
}

# generate_cruft_pack <seed-string> [<size>]
generate_cruft_pack () {
	pack_random_blob "$@" >/dev/null &&

	ls $packdir/pack-*.pack | xargs -n 1 basename >in &&
	pack="$(git pack-objects --cruft $packdir/pack <in)" &&
	git prune-packed &&

	echo "$packdir/pack-$pack.mtimes"
}
