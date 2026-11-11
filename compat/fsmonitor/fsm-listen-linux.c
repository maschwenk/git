#include "git-compat-util.h"
#include "dir.h"
#include "fsmonitor-ll.h"
#include "fsm-listen.h"
#include "fsmonitor--daemon.h"
#include "fsmonitor-path-utils.h"
#include "gettext.h"
#include "simple-ipc.h"
#include "string-list.h"
#include "trace.h"

#include <sys/inotify.h>

enum shutdown_reason {
	SHUTDOWN_CONTINUE = 0,
	SHUTDOWN_STOP,
	SHUTDOWN_ERROR,
	SHUTDOWN_FORCE
};

struct watch_entry {
	struct hashmap_entry ent;
	int wd;
	uint32_t cookie;
	const char *dir;
	/*
	 * The value of watch_generation when add_watch() last added this
	 * watch, moved it or confirmed its path. Set only in the "watches"
	 * map, not in "revwatches".
	 */
	uint64_t generation;
};

struct rename_entry {
	struct hashmap_entry ent;
	time_t whence;
	uint32_t cookie;
	const char *dir;
};

struct stale_entry {
	struct hashmap_entry ent;
	const char *dir;
	uint64_t cutoff;
};

struct fsm_listen_data {
	int fd_inotify;
	enum shutdown_reason shutdown;
	struct hashmap watches;
	struct hashmap renames;
	struct hashmap revwatches;
	struct hashmap stale_dirs;
	uint64_t watch_generation;
};

static int watch_entry_cmp(const void *cmp_data UNUSED,
			   const struct hashmap_entry *eptr,
			   const struct hashmap_entry *entry_or_key,
			   const void *keydata UNUSED)
{
	const struct watch_entry *e1, *e2;

	e1 = container_of(eptr, const struct watch_entry, ent);
	e2 = container_of(entry_or_key, const struct watch_entry, ent);
	return e1->wd != e2->wd;
}

static int revwatches_entry_cmp(const void *cmp_data UNUSED,
				const struct hashmap_entry *eptr,
				const struct hashmap_entry *entry_or_key,
				const void *keydata UNUSED)
{
	const struct watch_entry *e1, *e2;

	e1 = container_of(eptr, const struct watch_entry, ent);
	e2 = container_of(entry_or_key, const struct watch_entry, ent);
	return strcmp(e1->dir, e2->dir);
}

static int rename_entry_cmp(const void *cmp_data UNUSED,
			    const struct hashmap_entry *eptr,
			    const struct hashmap_entry *entry_or_key,
			    const void *keydata UNUSED)
{
	const struct rename_entry *e1, *e2;

	e1 = container_of(eptr, const struct rename_entry, ent);
	e2 = container_of(entry_or_key, const struct rename_entry, ent);
	return e1->cookie != e2->cookie;
}

static int stale_entry_cmp(const void *cmp_data UNUSED,
			   const struct hashmap_entry *eptr,
			   const struct hashmap_entry *entry_or_key,
			   const void *keydata UNUSED)
{
	const struct stale_entry *e1, *e2;

	e1 = container_of(eptr, const struct stale_entry, ent);
	e2 = container_of(entry_or_key, const struct stale_entry, ent);
	return strcmp(e1->dir, e2->dir);
}

static struct watch_entry *find_watch_by_dir(const char *dir,
					     struct fsm_listen_data *data)
{
	struct watch_entry k;

	k.dir = dir;
	hashmap_entry_init(&k.ent, strhash(dir));
	return hashmap_get_entry(&data->revwatches, &k, ent, NULL);
}

/*
 * Forget the pending rename, if any, of the directory watched by "w",
 * which must be an entry of the reverse mapping.
 */
static void forget_dir_rename(struct watch_entry *w,
			      struct fsm_listen_data *data)
{
	struct rename_entry k;

	if (!w->cookie)
		return;
	k.cookie = w->cookie;
	hashmap_entry_init(&k.ent, memhash(&k.cookie, sizeof(uint32_t)));
	free(hashmap_remove_entry(&data->renames, &k, ent, NULL));
	w->cookie = 0;
}

static void remove_watch(struct watch_entry *w, struct fsm_listen_data *data);

/*
 * Mark "dir" stale: the next retire_stale_watches() removes every watch
 * at or below it that add_watch() has not added, moved or confirmed
 * since "dir" was last marked. The mark is on the path, not on a watch,
 * so it still covers the old subtree after the watch of "dir" itself
 * has moved to a new path.
 */
static void mark_stale_directory(const char *dir, struct fsm_listen_data *data)
{
	struct stale_entry k, *stale;

	k.dir = dir;
	hashmap_entry_init(&k.ent, strhash(dir));
	stale = hashmap_get_entry(&data->stale_dirs, &k, ent, NULL);
	if (!stale) {
		CALLOC_ARRAY(stale, 1);
		stale->dir = dir;
		hashmap_entry_init(&stale->ent, k.ent.hash);
		hashmap_add(&data->stale_dirs, &stale->ent);
	}
	stale->cutoff = data->watch_generation;
}

/*
 * Register an inotify watch, add watch descriptor to path mapping
 * and the reverse mapping.
 *
 * The directory may already be watched under another name, because it or
 * a directory above it was renamed. In that case, move the existing watch
 * to the new name so that its events are reported under the right path.
 */
static int add_watch(const char *path, struct fsm_listen_data *data)
{
	const char *interned = strintern(path);
	struct watch_entry k, *w1, *w2;

	/* returns the existing descriptor if the inode is already watched */
	int wd = inotify_add_watch(data->fd_inotify, interned,
				   (IN_ALL_EVENTS | IN_ONLYDIR)
				    ^ IN_ACCESS ^ IN_CLOSE ^ IN_OPEN);
	if (wd < 0) {
		if (errno == ENOENT || errno == ENOTDIR)
			return 0; /* directory was deleted or is not a directory */
		if (errno == ENOSPC)
			return error(_("inotify watch limit reached; "
				       "increase fs.inotify.max_user_watches"));
		return error_errno(_("inotify_add_watch('%s') failed"), interned);
	}

	k.wd = wd;
	hashmap_entry_init(&k.ent, memhash(&k.wd, sizeof(int)));
	w1 = hashmap_get_entry(&data->watches, &k, ent, NULL);
	if (w1 && w1->dir == interned) {
		/*
		 * It may have been renamed away and back, which leaves its
		 * rename pending. Mark the path stale in that case, since
		 * directories may have left it in the meantime.
		 */
		w2 = find_watch_by_dir(interned, data);
		if (w2->cookie)
			mark_stale_directory(interned, data);
		forget_dir_rename(w2, data);
		w1->generation = ++data->watch_generation;
		return 0;
	}

	/* whatever directory we watched under this name is gone from it */
	w2 = find_watch_by_dir(interned, data);
	if (w2) {
		mark_stale_directory(interned, data);
		remove_watch(w2, data);
	}

	if (w1) {
		trace_printf_key(&trace_fsmonitor, "watch moved from '%s' to '%s'",
				 w1->dir, interned);
		mark_stale_directory(w1->dir, data);
		w2 = find_watch_by_dir(w1->dir, data);
		hashmap_remove(&data->revwatches, &w2->ent, NULL);
		forget_dir_rename(w2, data);
		w1->dir = w2->dir = interned;
		w1->generation = ++data->watch_generation;
		hashmap_entry_init(&w2->ent, strhash(interned));
		hashmap_add(&data->revwatches, &w2->ent);
		return 0;
	}

	/* add watch descriptor -> directory mapping */
	CALLOC_ARRAY(w1, 1);
	w1->wd = wd;
	w1->dir = interned;
	w1->generation = ++data->watch_generation;
	hashmap_entry_init(&w1->ent, memhash(&w1->wd, sizeof(int)));
	hashmap_add(&data->watches, &w1->ent);

	/* add directory -> watch descriptor mapping */
	CALLOC_ARRAY(w2, 1);
	w2->wd = wd;
	w2->dir = interned;
	hashmap_entry_init(&w2->ent, strhash(w2->dir));
	hashmap_add(&data->revwatches, &w2->ent);

	return 0;
}

/*
 * Remove the inotify watch, the watch descriptor to path mapping
 * and the reverse mapping.
 */
static void remove_watch(struct watch_entry *w, struct fsm_listen_data *data)
{
	struct watch_entry k1, k2, *w1, *w2;

	/* remove watch, ignore error if kernel already did it */
	if (inotify_rm_watch(data->fd_inotify, w->wd) && errno != EINVAL)
		error_errno(_("inotify_rm_watch() failed"));

	k1.wd = w->wd;
	hashmap_entry_init(&k1.ent, memhash(&k1.wd, sizeof(int)));
	w1 = hashmap_remove_entry(&data->watches, &k1, ent, NULL);
	if (!w1)
		BUG("double remove of watch for '%s'", w->dir);

	k2.dir = w->dir;
	hashmap_entry_init(&k2.ent, strhash(k2.dir));
	w2 = hashmap_remove_entry(&data->revwatches, &k2, ent, NULL);
	if (!w2)
		BUG("double remove of reverse watch for '%s'", w->dir);

	/*
	 * The directory may still have a pending rename, for example if it
	 * was moved out of the worktree and then deleted.
	 */
	forget_dir_rename(w2, data);

	/* w1->dir and w2->dir are interned strings, we don't own them */
	free(w1);
	free(w2);
}

/*
 * Called after each batch of events, once the directories moved in it
 * have been walked. A directory that left a moved directory before that
 * walk is not found by it and keeps its old path, even when the rename
 * of its parent is no longer pending. Remove every watch at or below a
 * stale path that add_watch() has not added, moved or confirmed since
 * the path was marked. A new directory created at that path in the
 * meantime keeps its watch.
 */
static void retire_stale_watches(struct fsm_listen_data *data)
{
	struct hashmap_iter iter;
	struct watch_entry *w, **to_remove = NULL;
	struct strbuf path = STRBUF_INIT;
	size_t nr = 0, alloc = 0;

	if (!hashmap_get_size(&data->stale_dirs))
		return;

	/* Collect first: remove_watch() deletes entries from "watches". */
	hashmap_for_each_entry(&data->watches, &iter, w, ent) {
		strbuf_reset(&path);
		strbuf_addstr(&path, w->dir);
		for (;;) {
			struct stale_entry k, *stale;
			char *slash;

			k.dir = path.buf;
			hashmap_entry_init(&k.ent, strhash(path.buf));
			stale = hashmap_get_entry(&data->stale_dirs, &k, ent,
						  NULL);
			if (stale && w->generation <= stale->cutoff) {
				ALLOC_GROW(to_remove, nr + 1, alloc);
				to_remove[nr++] = w;
				break;
			}
			slash = strrchr(path.buf, '/');
			if (!slash)
				break;
			strbuf_setlen(&path, slash - path.buf);
		}
	}

	for (size_t i = 0; i < nr; i++)
		remove_watch(to_remove[i], data);
	free(to_remove);
	strbuf_release(&path);
	hashmap_partial_clear_and_free(&data->stale_dirs,
				       struct stale_entry, ent);
}

/*
 * Check for stale directory renames.
 *
 * https://man7.org/linux/man-pages/man7/inotify.7.html
 *
 * Allow for some small timeout to account for the fact that insertion of the
 * IN_MOVED_FROM+IN_MOVED_TO event pair is not atomic, and the possibility that
 * there may not be any IN_MOVED_TO event.
 *
 * If the IN_MOVED_TO event is not received within the timeout then events have
 * been missed and the monitor is in an inconsistent state with respect to the
 * filesystem.
 */
static int check_stale_dir_renames(struct hashmap *renames, time_t max_age)
{
	struct rename_entry *re;
	struct hashmap_iter iter;

	hashmap_for_each_entry(renames, &iter, re, ent) {
		if (re->whence <= max_age)
			return -1;
	}
	return 0;
}

/*
 * Track pending renames.
 *
 * Tracking is done via an event cookie to source path mapping.
 *
 * A rename is not complete until matching an IN_MOVED_TO event is received
 * for a corresponding IN_MOVED_FROM event.
 */
static void add_dir_rename(uint32_t cookie, const char *path,
			   struct fsm_listen_data *data)
{
	struct watch_entry *w;
	struct rename_entry *re;

	w = find_watch_by_dir(path, data);
	if (!w) {
		/*
		 * This can happen in rare cases where the directory was
		 * moved before we had a chance to add a watch on it.
		 * Just ignore this rename.
		 */
		trace_printf_key(&trace_fsmonitor,
				 "no watch found for rename from '%s'", path);
		return;
	}
	forget_dir_rename(w, data);
	w->cookie = cookie;

	/* add the pending rename to match against later */
	CALLOC_ARRAY(re, 1);
	re->dir = w->dir;
	re->cookie = w->cookie;
	re->whence = time(NULL);
	hashmap_entry_init(&re->ent, memhash(&re->cookie, sizeof(uint32_t)));
	hashmap_add(&data->renames, &re->ent);
}

/*
 * Handle directory renames
 *
 * Once an IN_MOVED_TO event is received, lookup the rename tracking information
 * via the event cookie and mark the source path stale. The add_watch()
 * and the walk that follow move the watches of the renamed directory
 * and of the directories still inside it to their new paths, and
 * retire_stale_watches() removes those left at or below the old path.
 */
static void rename_dir(uint32_t cookie, const char *path,
		       struct fsm_listen_data *data)
{
	struct rename_entry rek, *re;
	struct watch_entry *w;

	/* lookup a pending rename to match */
	rek.cookie = cookie;
	hashmap_entry_init(&rek.ent, memhash(&rek.cookie, sizeof(uint32_t)));
	re = hashmap_remove_entry(&data->renames, &rek, ent, NULL);
	if (re) {
		mark_stale_directory(re->dir, data);
		w = find_watch_by_dir(re->dir, data);
		if (w)
			w->cookie = 0; /* rename handled */
		free(re);
	} else {
		/* The source may have been outside the watched tree. */
		trace_printf_key(&trace_fsmonitor,
				 "no matching cookie for rename to '%s'", path);
	}
}

/*
 * Recursively add watches to every directory under path
 */
static int register_inotify(const char *path,
			    struct fsmonitor_daemon_state *state,
			    struct fsmonitor_batch *batch)
{
	DIR *dir;
	const char *rel;
	struct strbuf current = STRBUF_INIT;
	struct dirent *de;
	struct stat fs;
	int ret = -1;

	dir = opendir(path);
	if (!dir) {
		if (errno == ENOENT || errno == ENOTDIR)
			return 0; /* directory was deleted */
		return error_errno(_("opendir('%s') failed"), path);
	}

	while ((de = readdir_skip_dot_and_dotdot(dir)) != NULL) {
		strbuf_reset(&current);
		strbuf_addf(&current, "%s/%s", path, de->d_name);
		if (lstat(current.buf, &fs)) {
			if (errno == ENOENT)
				continue; /* file was deleted */
			error_errno(_("lstat('%s') failed"), current.buf);
			goto failed;
		}

		/* recurse into directory */
		if (S_ISDIR(fs.st_mode)) {
			if (add_watch(current.buf, state->listen_data))
				goto failed;
			if (register_inotify(current.buf, state, batch))
				goto failed;
		} else if (batch) {
			rel = current.buf + state->path_worktree_watch.len + 1;
			trace_printf_key(&trace_fsmonitor, "explicitly adding '%s'", rel);
			fsmonitor_batch__add_path(batch, rel);
		}
	}
	ret = 0;

failed:
	strbuf_release(&current);
	if (closedir(dir) < 0)
		return error_errno(_("closedir('%s') failed"), path);
	return ret;
}

static int em_rename_dir_from(uint32_t mask)
{
	return ((mask & IN_ISDIR) && (mask & IN_MOVED_FROM));
}

static int em_rename_dir_to(uint32_t mask)
{
	return ((mask & IN_ISDIR) && (mask & IN_MOVED_TO));
}

static int em_remove_watch(uint32_t mask)
{
	return (mask & IN_DELETE_SELF);
}

static int em_dir_renamed(uint32_t mask)
{
	return ((mask & IN_ISDIR) && (mask & IN_MOVE));
}

static int em_dir_created(uint32_t mask)
{
	return ((mask & IN_ISDIR) && (mask & IN_CREATE));
}

static int em_dir_deleted(uint32_t mask)
{
	return ((mask & IN_ISDIR) && (mask & IN_DELETE));
}

static int em_force_shutdown(uint32_t mask)
{
	return (mask & IN_UNMOUNT) || (mask & IN_Q_OVERFLOW);
}

static int em_ignore(uint32_t mask)
{
	return (mask & IN_IGNORED) || (mask & IN_MOVE_SELF);
}

static void log_mask_set(const char *path, uint32_t mask)
{
	struct strbuf msg = STRBUF_INIT;

	if (mask & IN_ACCESS)
		strbuf_addstr(&msg, "IN_ACCESS|");
	if (mask & IN_MODIFY)
		strbuf_addstr(&msg, "IN_MODIFY|");
	if (mask & IN_ATTRIB)
		strbuf_addstr(&msg, "IN_ATTRIB|");
	if (mask & IN_CLOSE_WRITE)
		strbuf_addstr(&msg, "IN_CLOSE_WRITE|");
	if (mask & IN_CLOSE_NOWRITE)
		strbuf_addstr(&msg, "IN_CLOSE_NOWRITE|");
	if (mask & IN_OPEN)
		strbuf_addstr(&msg, "IN_OPEN|");
	if (mask & IN_MOVED_FROM)
		strbuf_addstr(&msg, "IN_MOVED_FROM|");
	if (mask & IN_MOVED_TO)
		strbuf_addstr(&msg, "IN_MOVED_TO|");
	if (mask & IN_CREATE)
		strbuf_addstr(&msg, "IN_CREATE|");
	if (mask & IN_DELETE)
		strbuf_addstr(&msg, "IN_DELETE|");
	if (mask & IN_DELETE_SELF)
		strbuf_addstr(&msg, "IN_DELETE_SELF|");
	if (mask & IN_MOVE_SELF)
		strbuf_addstr(&msg, "IN_MOVE_SELF|");
	if (mask & IN_UNMOUNT)
		strbuf_addstr(&msg, "IN_UNMOUNT|");
	if (mask & IN_Q_OVERFLOW)
		strbuf_addstr(&msg, "IN_Q_OVERFLOW|");
	if (mask & IN_IGNORED)
		strbuf_addstr(&msg, "IN_IGNORED|");
	if (mask & IN_ISDIR)
		strbuf_addstr(&msg, "IN_ISDIR|");

	strbuf_strip_suffix(&msg, "|");

	trace_printf_key(&trace_fsmonitor, "inotify_event: '%s', mask=%#8.8x %s",
			 path, mask, msg.buf);

	strbuf_release(&msg);
}

int fsm_listen__ctor(struct fsmonitor_daemon_state *state)
{
	int fd;
	int ret = 0;
	struct fsm_listen_data *data;

	CALLOC_ARRAY(data, 1);
	state->listen_data = data;
	state->listen_error_code = -1;
	data->fd_inotify = -1;
	data->shutdown = SHUTDOWN_ERROR;

	fd = inotify_init1(O_NONBLOCK);
	if (fd < 0) {
		FREE_AND_NULL(state->listen_data);
		return error_errno(_("inotify_init1() failed"));
	}

	data->fd_inotify = fd;

	hashmap_init(&data->watches, watch_entry_cmp, NULL, 0);
	hashmap_init(&data->renames, rename_entry_cmp, NULL, 0);
	hashmap_init(&data->revwatches, revwatches_entry_cmp, NULL, 0);
	hashmap_init(&data->stale_dirs, stale_entry_cmp, NULL, 0);

	if (add_watch(state->path_worktree_watch.buf, data))
		ret = -1;
	else if (register_inotify(state->path_worktree_watch.buf, state, NULL))
		ret = -1;
	else if (state->nr_paths_watching > 1) {
		if (add_watch(state->path_gitdir_watch.buf, data))
			ret = -1;
		else if (register_inotify(state->path_gitdir_watch.buf, state, NULL))
			ret = -1;
	}

	if (!ret) {
		state->listen_error_code = 0;
		data->shutdown = SHUTDOWN_CONTINUE;
	}

	return ret;
}

void fsm_listen__dtor(struct fsmonitor_daemon_state *state)
{
	struct fsm_listen_data *data;
	struct hashmap_iter iter;
	struct watch_entry *w;
	struct watch_entry **to_remove;
	size_t nr_to_remove = 0, alloc_to_remove = 0;
	size_t i;
	int fd;

	if (!state || !state->listen_data)
		return;

	data = state->listen_data;
	fd = data->fd_inotify;

	/*
	 * Collect all entries first, then remove them.
	 * We can't modify the hashmap while iterating over it.
	 */
	to_remove = NULL;
	hashmap_for_each_entry(&data->watches, &iter, w, ent) {
		ALLOC_GROW(to_remove, nr_to_remove + 1, alloc_to_remove);
		to_remove[nr_to_remove++] = w;
	}

	for (i = 0; i < nr_to_remove; i++) {
		to_remove[i]->cookie = 0; /* ignore any pending renames */
		remove_watch(to_remove[i], data);
	}
	free(to_remove);

	hashmap_clear(&data->watches);

	hashmap_clear(&data->revwatches); /* remove_watch freed the entries */

	hashmap_clear_and_free(&data->renames, struct rename_entry, ent);
	hashmap_clear_and_free(&data->stale_dirs, struct stale_entry, ent);

	FREE_AND_NULL(state->listen_data);

	if (fd >= 0 && (close(fd) < 0))
		error_errno(_("closing inotify file descriptor failed"));
}

void fsm_listen__stop_async(struct fsmonitor_daemon_state *state)
{
	if (state && state->listen_data &&
	    state->listen_data->shutdown == SHUTDOWN_CONTINUE)
		state->listen_data->shutdown = SHUTDOWN_STOP;
}

/*
 * Process a single inotify event and queue for publication.
 */
static int process_event(const char *path,
			 const struct inotify_event *event,
			 struct fsmonitor_batch **batch,
			 struct string_list *cookie_list,
			 struct fsmonitor_daemon_state *state)
{
	const char *rel;
	const char *last_sep;

	switch (fsmonitor_classify_path_absolute(state, path)) {
	case IS_INSIDE_DOT_GIT_WITH_COOKIE_PREFIX:
	case IS_INSIDE_GITDIR_WITH_COOKIE_PREFIX:
		/* Use just the filename of the cookie file. */
		last_sep = find_last_dir_sep(path);
		string_list_append(cookie_list,
				   last_sep ? last_sep + 1 : path);
		break;
	case IS_INSIDE_DOT_GIT:
	case IS_INSIDE_GITDIR:
		break;
	case IS_DOT_GIT:
	case IS_GITDIR:
		/*
		 * If .git directory is deleted or renamed away,
		 * we have to quit.
		 */
		if (em_dir_deleted(event->mask)) {
			trace_printf_key(&trace_fsmonitor,
					 "event: gitdir removed");
			state->listen_data->shutdown = SHUTDOWN_FORCE;
			goto done;
		}

		if (em_dir_renamed(event->mask)) {
			trace_printf_key(&trace_fsmonitor,
					 "event: gitdir renamed");
			state->listen_data->shutdown = SHUTDOWN_FORCE;
			goto done;
		}
		break;
	case IS_WORKDIR_PATH:
		/* normal events in the working directory */
		if (trace_pass_fl(&trace_fsmonitor))
			log_mask_set(path, event->mask);

		if (!*batch)
			*batch = fsmonitor_batch__new();

		rel = path + state->path_worktree_watch.len + 1;
		fsmonitor_batch__add_path(*batch, rel);

		if (em_dir_deleted(event->mask))
			break;

		/* received IN_MOVE_FROM, add tracking for expected IN_MOVE_TO */
		if (em_rename_dir_from(event->mask))
			add_dir_rename(event->cookie, path, state->listen_data);

		/* received IN_MOVE_TO, update watch to reflect new path */
		if (em_rename_dir_to(event->mask)) {
			rename_dir(event->cookie, path, state->listen_data);
			/* moves a renamed directory's watch, or adds one */
			if (add_watch(path, state->listen_data) ||
			    register_inotify(path, state, *batch)) {
				state->listen_data->shutdown = SHUTDOWN_ERROR;
				goto done;
			}
		}

		if (em_dir_created(event->mask)) {
			if (add_watch(path, state->listen_data)) {
				state->listen_data->shutdown = SHUTDOWN_ERROR;
				goto done;
			}
			if (register_inotify(path, state, *batch)) {
				state->listen_data->shutdown = SHUTDOWN_ERROR;
				goto done;
			}
		}
		break;
	case IS_OUTSIDE_CONE:
	default:
		trace_printf_key(&trace_fsmonitor,
				 "ignoring '%s'", path);
		break;
	}
	return 0;
done:
	return -1;
}

/*
 * Read the inotify event stream and pre-process events before further
 * processing and eventual publishing.
 */
static void handle_events(struct fsmonitor_daemon_state *state)
{
	/* See https://man7.org/linux/man-pages/man7/inotify.7.html */
	char buf[4096]
		__attribute__ ((aligned(__alignof__(struct inotify_event))));

	struct hashmap *watches = &state->listen_data->watches;
	struct fsmonitor_batch *batch = NULL;
	struct string_list cookie_list = STRING_LIST_INIT_DUP;
	struct watch_entry k, *w;
	struct strbuf path = STRBUF_INIT;
	const struct inotify_event *event;
	int fd = state->listen_data->fd_inotify;
	ssize_t len;
	char *ptr, *p;

	for (;;) {
		len = read(fd, buf, sizeof(buf));
		if (len == -1) {
			if (errno == EAGAIN || errno == EINTR)
				goto done;
			error_errno(_("reading inotify message stream failed"));
			state->listen_data->shutdown = SHUTDOWN_ERROR;
			goto done;
		}

		/* nothing to read */
		if (len == 0)
			goto done;

		/* Loop over all events in the buffer. */
		for (ptr = buf; ptr < buf + len;
		     ptr += sizeof(struct inotify_event) + event->len) {

			event = (const struct inotify_event *)ptr;

			if (em_ignore(event->mask))
				continue;

			/* File system was unmounted or event queue overflowed */
			if (em_force_shutdown(event->mask)) {
				if (trace_pass_fl(&trace_fsmonitor))
					log_mask_set("forcing shutdown", event->mask);
				state->listen_data->shutdown = SHUTDOWN_FORCE;
				goto done;
			}

			k.wd = event->wd;
			hashmap_entry_init(&k.ent, memhash(&k.wd, sizeof(int)));

			w = hashmap_get_entry(watches, &k, ent, NULL);
			if (!w) {
				/* Watch was removed, skip event */
				continue;
			}

			/* directory watch was removed */
			if (em_remove_watch(event->mask)) {
				remove_watch(w, state->listen_data);
				continue;
			}

			strbuf_reset(&path);
			strbuf_addf(&path, "%s/%s", w->dir, event->name);

			p = fsmonitor__resolve_alias(path.buf, &state->alias);
			if (!p)
				p = strbuf_detach(&path, NULL);

			if (process_event(p, event, &batch, &cookie_list, state)) {
				free(p);
				goto done;
			}
			free(p);
		}
		strbuf_reset(&path);
		retire_stale_watches(state->listen_data);
		fsmonitor_publish(state, batch, &cookie_list);
		string_list_clear(&cookie_list, 0);
		batch = NULL;
	}
done:
	strbuf_release(&path);
	fsmonitor_batch__free_list(batch);
	string_list_clear(&cookie_list, 0);
}

/*
 * Non-blocking read of the inotify events stream. The inotify fd is polled
 * frequently to help minimize the number of queue overflows.
 */
void fsm_listen__loop(struct fsmonitor_daemon_state *state)
{
	int poll_num;
	/*
	 * Interval in seconds between checks for stale directory renames.
	 * A directory rename that is not completed within this window
	 * (i.e. no matching IN_MOVED_TO for an IN_MOVED_FROM) indicates
	 * missed events, forcing a shutdown.
	 */
	const int interval = 1;
	time_t checked = time(NULL);
	struct pollfd fds[1];

	fds[0].fd = state->listen_data->fd_inotify;
	fds[0].events = POLLIN;

	/*
	 * Our fs event listener is now running, so it's safe to start
	 * serving client requests.
	 */
	ipc_server_start_async(state->ipc_server_data);

	for (;;) {
		switch (state->listen_data->shutdown) {
		case SHUTDOWN_CONTINUE:
			poll_num = poll(fds, 1, 50);
			if (poll_num == -1) {
				if (errno == EINTR)
					continue;
				error_errno(_("polling inotify message stream failed"));
				state->listen_data->shutdown = SHUTDOWN_ERROR;
				continue;
			}

			if ((time(NULL) - checked) >= interval) {
				checked = time(NULL);
				if (check_stale_dir_renames(&state->listen_data->renames,
							    checked - interval)) {
					trace_printf_key(&trace_fsmonitor,
							 "missed IN_MOVED_TO events, forcing shutdown");
					state->listen_data->shutdown = SHUTDOWN_FORCE;
					continue;
				}
			}

			if (poll_num > 0 && (fds[0].revents & POLLIN))
				handle_events(state);

			continue;
		case SHUTDOWN_ERROR:
			state->listen_error_code = -1;
			ipc_server_stop_async(state->ipc_server_data);
			break;
		case SHUTDOWN_FORCE:
			state->listen_error_code = 0;
			ipc_server_stop_async(state->ipc_server_data);
			break;
		case SHUTDOWN_STOP:
		default:
			state->listen_error_code = 0;
			break;
		}
		return;
	}
}
