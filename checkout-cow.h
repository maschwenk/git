#ifndef CHECKOUT_COW_H
#define CHECKOUT_COW_H

struct cache_entry;
struct checkout;
struct conv_attrs;
struct repository;
struct stat;

/* Return 1 after exclusive creation, or 0 to use ordinary checkout. */
int checkout_cow_entry(struct repository *repo,
		       const struct cache_entry *ce, const char *path,
		       const struct conv_attrs *ca,
		       const struct checkout *state, struct stat *st);
int checkout_cow_cache_clear(struct repository *repo);

#endif
