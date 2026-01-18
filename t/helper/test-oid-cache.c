#include "test-tool.h"
#include "git-compat-util.h"
#include "parse-options.h"
#include "strbuf.h"
#include "string-list.h"
#include "hash.h"
#include "quote.h"
#include "usage.h"

static void warn_test(const char *warn, va_list params)
{
	vfreportf(fcntl(7, F_GETFD) == -1 ? 2 : 7, _("warning: "), err, params);
}

static const char *const oid_cache_usage[] = {
	"test-tool oid-cache",
	NULL
};

#define IFS " \t"

int cmd__oid_cache(int argc, const char **argv)
{
	struct string_list parts = STRING_LIST_INIT_NODUP;
	struct strbuf buf = STRBUF_INIT;
	struct strbuf out = STRBUF_INIT;
	struct option options[] = {
		OPT_END()
	};

	set_warn_routine(warn_test);

	argc = parse_options(argc, argv, "test-tools", options, oid_cache_usage, 0);
	if (argc > 0)
		usage_with_options(oid_cache_usage, options);

	while (strbuf_getline(&buf, stdin) != EOF) {
		const struct git_hash_algo *algop;
		char *colon;
		int algo;

		if (!buf.len || *buf.buf == '#')
			continue;

		string_list_setlen(&parts, 0);
		string_list_split_in_place_f(&parts, buf.buf, IFS, 1,
					     STRING_LIST_SPLIT_TRIM);

		if (parts.nr != 2) {
			warning(_("malformed oid-cache line: %s"), buf.buf);
			continue;
		}

		colon = strchr(parts.items[1].string, ':');
		if (!colon) {
			warning(_("malformed oid-cache line: %s"), buf.buf);
			continue;
		}
		*colon = '\0';

		algo = hash_algo_by_name(parts.items[1].string);
		if (algo == GIT_HASH_UNKNOWN)
			die(_("unknown hash algorithm: %s"),
			    parts.items[1].string);
		algop = &hash_algos[algo];

		strbuf_reset(&out);
		strbuf_addf(&out, "test_oid_%s_%s=", algop->name, parts.items[0].string);
		sq_quote_buf(&out, colon + 1);

		printf("%s\n", out.buf);
	}

	strbuf_release(&buf);
	strbuf_release(&out);
	string_list_clear(&parts, 0);

	return 0;
}
