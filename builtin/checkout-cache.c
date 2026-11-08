#include "builtin.h"
#include "checkout-cow.h"
#include "gettext.h"
#include "parse-options.h"

int cmd_checkout_cache(int argc, const char **argv, const char *prefix,
		       struct repository *repo)
{
	static const char * const usage[] = {
		N_("git checkout-cache --clear"),
		NULL
	};
	int clear = 0;
	struct option options[] = {
		OPT_BOOL(0, "clear", &clear, N_("remove cached checkout contents")),
		OPT_END()
	};

	argc = parse_options(argc, argv, prefix, options, usage, 0);
	if (argc || !clear)
		usage_with_options(usage, options);
	return checkout_cow_cache_clear(repo) ? 1 : 0;
}
