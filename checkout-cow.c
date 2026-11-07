#include "git-compat-util.h"
#include "checkout-cow.h"
#include "config.h"
#include "dir.h"
#include "entry.h"
#include "hex.h"
#include "object-file.h"
#include "odb/streaming.h"
#include "read-cache-ll.h"
#include "replace-object.h"
#include "repository.h"
#include "strbuf.h"
#include "sigchain.h"
#include "trace2.h"

/* Cache names are ASCII; fdopendir() needs the native directory API. */
#undef DIR
#undef dirent
#undef readdir
#undef closedir

#if !defined(NO_COW_CHECKOUT) && (defined(__linux__) || defined(__APPLE__))
#include <sys/file.h>
#ifdef __linux__
#include <sys/ioctl.h>
/* Stable Linux ioctl ABI; avoid a dependency on kernel development headers. */
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif
#endif
#ifdef __APPLE__
#include <sys/acl.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>
#endif

struct cow_cache {
	struct repository *repo;
	int dirfd, lockfd, writer_checked, unsupported;
	unsigned long bytes, entries, max_bytes, max_entries;
};

static struct cow_cache cache = { .dirfd = -1, .lockfd = -1 };

/* Two in-flight files: a cache population and a working-tree clone. */
struct cow_tempfile {
	int dirfd, fd;
	char name[64];
	volatile sig_atomic_t active;
	pid_t owner;
};

static struct cow_tempfile population_temp, checkout_temp;

static int close_cow_tempfile(struct cow_tempfile *temp)
{
	int fd = temp->fd;

	temp->fd = -1;
	return fd < 0 ? 0 : close(fd);
}

static void remove_cow_tempfile(struct cow_tempfile *temp)
{
	if (!temp->active || temp->owner != getpid())
		return;
	temp->active = 0;
	close_cow_tempfile(temp);
	unlinkat(temp->dirfd, temp->name, 0);
}

static void remove_cow_tempfiles(void)
{
	remove_cow_tempfile(&checkout_temp);
	remove_cow_tempfile(&population_temp);
}

static void remove_cow_tempfiles_on_signal(int signo)
{
	remove_cow_tempfiles();
	sigchain_pop(signo);
	raise(signo);
}

static int create_cow_tempfile(struct cow_tempfile *temp, int dirfd,
			       const char *prefix, mode_t mode)
{
	static int initialized;
	int attempts;

	if (temp->active)
		BUG("copy-on-write temporary file is already active");
	if (!initialized) {
		sigchain_push_common(remove_cow_tempfiles_on_signal);
		atexit(remove_cow_tempfiles);
		initialized = 1;
	}
	temp->dirfd = dirfd;
	temp->owner = getpid();
	for (attempts = 0; attempts < 16; attempts++) {
		uint64_t random;

		if (csprng_bytes(&random, sizeof(random), 0))
			return -1;
		xsnprintf(temp->name, sizeof(temp->name), "%s%016" PRIx64,
			  prefix, random);
		temp->fd = openat(dirfd, temp->name,
				  O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
		if (temp->fd >= 0) {
			temp->active = 1;
			return 0;
		}
		if (errno != EEXIST)
			return -1;
	}
	return -1;
}

static int has_acl(int fd)
{
#ifdef __APPLE__
	acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
	acl_entry_t entry;
	int ret;

	if (!acl)
		return errno != ENOENT;
	ret = !acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
	acl_free(acl);
	return ret;
#else
	(void)fd;
	return 0;
#endif
}

static int safe_directory(int fd, int private)
{
	struct stat st;

	return !fstat(fd, &st) && S_ISDIR(st.st_mode) &&
		st.st_uid == geteuid() && !(st.st_mode & (private ? 077 : 022)) &&
		!has_acl(fd);
}

static int open_cache_directory(int parent, const char *name, int create)
{
	int fd;

	if (create && mkdirat(parent, name, 0700) && errno != EEXIST)
		return -1;
	fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd >= 0 && !safe_directory(fd, 1)) {
		close(fd);
		fd = -1;
	}
	return fd;
}

static int prepare_cache(struct repository *repo, int create)
{
	int fd, next;

	if (cache.repo == repo && cache.dirfd >= 0)
		return 0;
	if (cache.dirfd >= 0)
		close(cache.dirfd);
	if (cache.lockfd >= 0)
		close(cache.lockfd);
	memset(&cache, 0, sizeof(cache));
	cache.repo = repo;
	cache.dirfd = cache.lockfd = -1;
	cache.max_bytes = 1024UL * 1024 * 1024;
	cache.max_entries = 100000;
	repo_config_get_ulong(repo, "checkout.cowcachemaxbytes", &cache.max_bytes);
	repo_config_get_ulong(repo, "checkout.cowcachemaxentries", &cache.max_entries);
	fd = open(repo_get_common_dir(repo), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	next = open_cache_directory(fd, "checkout-cache", create);
	close(fd);
	if (next < 0)
		return -1;
	fd = open_cache_directory(next, "v1", create);
	close(next);
	if (fd < 0)
		return -1;
	next = open_cache_directory(fd, repo->hash_algo->name, create);
	close(fd);
	if (next < 0)
		return -1;
	cache.dirfd = next;
	return 0;
}

/* Count every retained file, including temporaries left by a killed writer. */
static int scan_cache(int clear)
{
	int scanfd = openat(cache.dirfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	DIR *dirs = scanfd < 0 ? NULL : fdopendir(scanfd);
	struct dirent *de;
	int ret = 0;

	if (!dirs) {
		if (scanfd >= 0)
			close(scanfd);
		return -1;
	}
	for (;;) {
		DIR *files;
		struct dirent *file;
		int fd;

		errno = 0;
		de = readdir(dirs);
		if (!de) {
			if (errno)
				ret = -1;
			break;
		}
		if (is_dot_or_dotdot(de->d_name) || !strcmp(de->d_name, "writer.lock"))
			continue;
		if (strlen(de->d_name) != 2 || !isxdigit(de->d_name[0]) ||
		    !isxdigit(de->d_name[1])) {
			ret = -1;
			break;
		}
		fd = open_cache_directory(cache.dirfd, de->d_name, 0);
		if (fd < 0 || !(files = fdopendir(fd))) {
			if (fd >= 0)
				close(fd);
			ret = -1;
			break;
		}
		for (;;) {
			struct stat st;

			errno = 0;
			file = readdir(files);
			if (!file) {
				if (errno)
					ret = -1;
				break;
			}

			if (is_dot_or_dotdot(file->d_name))
				continue;
			if (fstatat(fd, file->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
			    (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) ||
			    st.st_size < 0 || (uintmax_t)st.st_size > ULONG_MAX - cache.bytes ||
			    cache.entries == ULONG_MAX) {
				ret = -1;
				break;
			}
			cache.bytes += st.st_size;
			cache.entries++;
			if (clear && unlinkat(fd, file->d_name, 0)) {
				ret = -1;
				break;
			}
		}
		closedir(files);
		if (ret)
			break;
	}
	closedir(dirs);
	return ret;
}

static int acquire_writer(void)
{
	struct stat st;
	int fd;

	if (cache.writer_checked)
		return cache.lockfd >= 0;
	cache.writer_checked = 1;
	fd = openat(cache.dirfd, "writer.lock",
		    O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
	    st.st_nlink != 1 || (st.st_mode & 077) || flock(fd, LOCK_EX | LOCK_NB)) {
		close(fd);
		return 0;
	}
	cache.bytes = cache.entries = 0;
	if (scan_cache(0)) {
		close(fd);
		return 0;
	}
	cache.lockfd = fd;
	return 1;
}

static int verify_blob(int fd, size_t size, const struct object_id *expected)
{
	struct git_hash_ctx ctx;
	struct object_id actual;
	char buf[32768];
	size_t total = 0;
	int header = format_object_header(buf, sizeof(buf), OBJ_BLOB, size);

	if (lseek(fd, 0, SEEK_SET))
		return -1;
	git_hash_init(&ctx, cache.repo->hash_algo);
	git_hash_update(&ctx, buf, header);
	for (;;) {
		ssize_t n = xread(fd, buf, sizeof(buf));
		if (n < 0 || (size_t)n > size - total)
			return -1;
		if (!n)
			break;
		git_hash_update(&ctx, buf, n);
		total += n;
	}
	git_hash_final_oid(&actual, &ctx);
	return total != size || !oideq(&actual, expected) ? -1 : 0;
}

static int clone_to_tempfile(int source, struct cow_tempfile *temp, mode_t mode)
{
	int fd;
#ifdef __APPLE__
	if (close_cow_tempfile(temp) || unlinkat(temp->dirfd, temp->name, 0) ||
	    fclonefileat(source, temp->dirfd, temp->name, CLONE_NOOWNERCOPY))
		return -1;
	fd = openat(temp->dirfd, temp->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0) {
		struct stat st;
		char names[256];
		ssize_t names_len = flistxattr(fd, names, sizeof(names), 0);
		/* macOS adds provenance even to freshly created ordinary files. */
		int unexpected_attrs = names_len != 0 &&
			(names_len != sizeof("com.apple.provenance") ||
			 memcmp(names, "com.apple.provenance", sizeof("com.apple.provenance")));
		if (fstat(fd, &st) || st.st_flags || unexpected_attrs || has_acl(fd)) {
			/* Flags belong to this private clone, never the cache inode. */
			fchflags(fd, 0);
			close(fd);
			errno = EINVAL;
			return -1;
		}
	}
#elif defined(FICLONE)
	if (ioctl(temp->fd, FICLONE, source))
		return -1;
	fd = dup(temp->fd);
#else
	errno = ENOTSUP;
	return -1;
#endif
	if (fd >= 0 && fchmod(fd, mode)) {
		close(fd);
		return -1;
	}
	return fd;
}

int checkout_cow_entry(struct repository *repo,
		       const struct cache_entry *ce, const char *path,
		       const struct conv_attrs *ca,
		       const struct checkout *state, struct stat *st)
{
	struct strbuf dir = STRBUF_INIT;
	struct cow_tempfile *temp = &checkout_temp, *population = &population_temp;
	struct stat before, source_st;
	const char *slash;
	char hex[GIT_MAX_HEXSZ + 1];
	char shard[3];
	size_t size;
	int enabled = 0, source = -1, target = -1, dirfd = -1, shardfd = -1;
	int ret = 0, populate = 0;
	unsigned int mode = (ce->ce_mode & 0100) ? 0777 : 0666;
	struct timeval times[2];

	if (!repo || state->base_dir_len)
		return 0;
	if (!S_ISREG(ce->ce_mode) || ca->drv || ca->ident || ca->working_tree_encoding ||
	    (ca->crlf_action != CRLF_BINARY && ca->crlf_action != CRLF_TEXT_INPUT &&
	     ca->crlf_action != CRLF_AUTO_INPUT) ||
	    repo_config_get_bool(repo, "checkout.cowcache", &enabled) || !enabled ||
	    !oideq(lookup_replace_object(repo, &ce->oid), &ce->oid) ||
	    prepare_cache(repo, 1) || cache.unsupported ||
	    odb_read_object_info(repo->objects, &ce->oid, &size) != OBJ_BLOB)
		return 0;

	slash = find_last_dir_sep(path);
	strbuf_add(&dir, path, slash ? (size_t)(slash - path) : 0);
	if (!dir.len)
		strbuf_addstr(&dir, slash ? "/" : ".");
	dirfd = open(dir.buf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0 || !safe_directory(dirfd, 0))
		goto out;
	oid_to_hex_r(hex, &ce->oid);
	memcpy(shard, hex, 2);
	shard[2] = 0;
	shardfd = open_cache_directory(cache.dirfd, shard, 0);
	if (shardfd >= 0) {
		source = openat(shardfd, hex + 2, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (source < 0 && errno != ENOENT)
			goto out;
	}
	if (source < 0) {
		if (!acquire_writer())
			goto out;
		if (shardfd < 0)
			shardfd = open_cache_directory(cache.dirfd, shard, 1);
		if (shardfd < 0)
			goto out;
		/* A previous writer may have filled this miss before we locked. */
		source = openat(shardfd, hex + 2, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (source < 0) {
			if (errno != ENOENT || !cache.max_bytes ||
			    cache.entries >= cache.max_entries || size > cache.max_bytes ||
			    cache.bytes > cache.max_bytes - size ||
			    create_cow_tempfile(population, shardfd, "tmp-", 0600))
				goto out;
			source = dup(population->fd);
			if (source < 0 || odb_stream_blob_to_fd(repo->objects, source, &ce->oid, NULL, 1) ||
			    fchmod(source, 0444))
				goto out;
			populate = 1;
		}
	}
	if (fstat(source, &source_st) || !S_ISREG(source_st.st_mode) ||
	    source_st.st_size < 0 || (uintmax_t)source_st.st_size != size)
		goto out;
	if (create_cow_tempfile(temp, dirfd, ".git-cow-", mode) || fstat(temp->fd, &before))
		goto out;
	target = clone_to_tempfile(source, temp, before.st_mode & 0777);
	if (target < 0) {
		if (errno == ENOTSUP || errno == EXDEV || errno == ENOSYS)
			cache.unsupported = 1;
		goto out;
	}
	if (verify_blob(target, size, &ce->oid) || gettimeofday(&times[0], NULL))
		goto out;
	times[1] = times[0];
	if (futimes(target, times))
		goto out;
	if (populate && !close_cow_tempfile(population) &&
	    !renameat(shardfd, population->name, shardfd, hex + 2)) {
		population->active = 0;
		cache.bytes += size;
		cache.entries++;
	}
	if (close_cow_tempfile(temp) ||
	    linkat(dirfd, temp->name, dirfd, slash ? slash + 1 : path, 0))
		goto out;
	remove_cow_tempfile(temp);
	if (fstat(target, st))
		die_errno("unable to stat copy-on-write checkout '%s'", path);
	trace2_data_string("checkout", repo, "cow_cache/clone", hex);
	ret = 1;
out:
	remove_cow_tempfile(temp);
	remove_cow_tempfile(population);
	if (target >= 0)
		close(target);
	if (source >= 0)
		close(source);
	if (shardfd >= 0)
		close(shardfd);
	if (dirfd >= 0)
		close(dirfd);
	strbuf_release(&dir);
	return ret;
}

int checkout_cow_cache_clear(struct repository *repo)
{
	if (prepare_cache(repo, 0))
		return error("copy-on-write checkout cache is absent or inaccessible");
	if (!acquire_writer())
		return error("copy-on-write checkout cache is busy or unsafe");
	cache.bytes = cache.entries = 0;
	if (scan_cache(1))
		return error_errno("unable to clear copy-on-write checkout cache");
	return 0;
}

#else
int checkout_cow_entry(struct repository *repo UNUSED,
		       const struct cache_entry *ce UNUSED, const char *path UNUSED,
		       const struct conv_attrs *ca UNUSED,
		       const struct checkout *state UNUSED, struct stat *st UNUSED)
{
	return 0;
}

int checkout_cow_cache_clear(struct repository *repo UNUSED)
{
	return error("copy-on-write checkout is not supported by this build");
}
#endif
