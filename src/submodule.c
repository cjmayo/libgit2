/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */

#include "common.h"
#include "git2/config.h"
#include "git2/sys/config.h"
#include "git2/types.h"
#include "git2/index.h"
#include "buffer.h"
#include "buf_text.h"
#include "vector.h"
#include "posix.h"
#include "config_file.h"
#include "config.h"
#include "repository.h"
#include "submodule.h"
#include "tree.h"
#include "iterator.h"
#include "path.h"
#include "index.h"

#define GIT_MODULES_FILE ".gitmodules"

static git_cvar_map _sm_update_map[] = {
	{GIT_CVAR_STRING, "checkout", GIT_SUBMODULE_UPDATE_CHECKOUT},
	{GIT_CVAR_STRING, "rebase", GIT_SUBMODULE_UPDATE_REBASE},
	{GIT_CVAR_STRING, "merge", GIT_SUBMODULE_UPDATE_MERGE},
	{GIT_CVAR_STRING, "none", GIT_SUBMODULE_UPDATE_NONE},
	{GIT_CVAR_FALSE, NULL, GIT_SUBMODULE_UPDATE_NONE},
	{GIT_CVAR_TRUE, NULL, GIT_SUBMODULE_UPDATE_CHECKOUT},
};

static git_cvar_map _sm_ignore_map[] = {
	{GIT_CVAR_STRING, "none", GIT_SUBMODULE_IGNORE_NONE},
	{GIT_CVAR_STRING, "untracked", GIT_SUBMODULE_IGNORE_UNTRACKED},
	{GIT_CVAR_STRING, "dirty", GIT_SUBMODULE_IGNORE_DIRTY},
	{GIT_CVAR_STRING, "all", GIT_SUBMODULE_IGNORE_ALL},
	{GIT_CVAR_FALSE, NULL, GIT_SUBMODULE_IGNORE_NONE},
	{GIT_CVAR_TRUE, NULL, GIT_SUBMODULE_IGNORE_ALL},
};

static git_cvar_map _sm_recurse_map[] = {
	{GIT_CVAR_STRING, "on-demand", GIT_SUBMODULE_RECURSE_ONDEMAND},
	{GIT_CVAR_FALSE, NULL, GIT_SUBMODULE_RECURSE_NO},
	{GIT_CVAR_TRUE, NULL, GIT_SUBMODULE_RECURSE_YES},
};

enum {
	CACHE_OK = 0,
	CACHE_REFRESH = 1,
	CACHE_FLUSH = 2
};
enum {
	GITMODULES_EXISTING = 0,
	GITMODULES_CREATE = 1,
};

static kh_inline khint_t str_hash_no_trailing_slash(const char *s)
{
	khint_t h;

	for (h = 0; *s; ++s)
		if (s[1] != '\0' || *s != '/')
			h = (h << 5) - h + *s;

	return h;
}

static kh_inline int str_equal_no_trailing_slash(const char *a, const char *b)
{
	size_t alen = a ? strlen(a) : 0;
	size_t blen = b ? strlen(b) : 0;

	if (alen > 0 && a[alen - 1] == '/')
		alen--;
	if (blen > 0 && b[blen - 1] == '/')
		blen--;

	return (alen == 0 && blen == 0) ||
		(alen == blen && strncmp(a, b, alen) == 0);
}

__KHASH_IMPL(
	str, static kh_inline, const char *, void *, 1,
	str_hash_no_trailing_slash, str_equal_no_trailing_slash)

static int submodule_alloc(git_submodule **out, git_repository *repo, const char *name);
static git_config_backend *open_gitmodules(git_repository *repo, int gitmod);
static git_config *gitmodules_snapshot(git_repository *repo);
static int get_url_base(git_buf *url, git_repository *repo);
static int lookup_head_remote_key(git_buf *remote_key, git_repository *repo);
static int submodule_load_each(const git_config_entry *entry, void *payload);
static int submodule_read_config(git_submodule *sm, git_config *cfg);
static int submodule_load_from_wd_lite(git_submodule *);
static void submodule_get_index_status(unsigned int *, git_submodule *);
static void submodule_get_wd_status(unsigned int *, git_submodule *, git_repository *, git_submodule_ignore_t);
static void submodule_update_from_index_entry(git_submodule *sm, const git_index_entry *ie);
static void submodule_update_from_head_data(git_submodule *sm, mode_t mode, const git_oid *id);

static int submodule_cmp(const void *a, const void *b)
{
	return strcmp(((git_submodule *)a)->name, ((git_submodule *)b)->name);
}

static int submodule_config_key_trunc_puts(git_buf *key, const char *suffix)
{
	ssize_t idx = git_buf_rfind(key, '.');
	git_buf_truncate(key, (size_t)(idx + 1));
	return git_buf_puts(key, suffix);
}

/*
 * PUBLIC APIS
 */

static void submodule_set_lookup_error(int error, const char *name)
{
	if (!error)
		return;

	giterr_set(GITERR_SUBMODULE, (error == GIT_ENOTFOUND) ?
		"No submodule named '%s'" :
		"Submodule '%s' has not been added yet", name);
}

typedef struct {
	const char *path;
	char *name;
} fbp_data;

static int find_by_path(const git_config_entry *entry, void *payload)
{
	fbp_data *data = payload;

	if (!strcmp(entry->value, data->path)) {
		const char *fdot, *ldot;
		fdot = strchr(entry->name, '.');
		ldot = strrchr(entry->name, '.');
		data->name = git__strndup(fdot + 1, ldot - fdot - 1);
		GITERR_CHECK_ALLOC(data->name);
	}

	return 0;
}

/**
 * Find out the name of a submodule from its path
 */
static int name_from_path(git_buf *out, git_config *cfg, const char *path)
{
	const char *key = "submodule\\..*\\.path";
	git_config_iterator *iter;
	git_config_entry *entry;
	int error;

	if ((error = git_config_iterator_glob_new(&iter, cfg, key)) < 0)
		return error;

	while ((error = git_config_next(&entry, iter)) == 0) {
		const char *fdot, *ldot;
		/* TODO: this should maybe be strcasecmp on a case-insensitive fs */
		if (strcmp(path, entry->value) != 0)
			continue;

		fdot = strchr(entry->name, '.');
		ldot = strrchr(entry->name, '.');

		git_buf_clear(out);
		git_buf_put(out, fdot + 1, ldot - fdot - 1);
		goto cleanup;
	}

	if (error == GIT_ITEROVER) {
		giterr_set(GITERR_SUBMODULE, "could not find a submodule name for '%s'", path);
		error = GIT_ENOTFOUND;
	}

cleanup:
	git_config_iterator_free(iter);
	return error;
}

int git_submodule_lookup(
	git_submodule **out, /* NULL if user only wants to test existence */
	git_repository *repo,
	const char *name)    /* trailing slash is allowed */
{
	int error;
	unsigned int location;
	git_submodule *sm;

	assert(repo && name);

	if ((error = submodule_alloc(&sm, repo, name)) < 0)
		return error;

	if ((error = git_submodule_reload(sm, false)) < 0) {
		git_submodule_free(sm);
		return error;
	}

	if ((error = git_submodule_location(&location, sm)) < 0) {
		git_submodule_free(sm);
		return error;
	}

	/* If it's not configured or we're looking by path  */
	if (location == 0 || location == GIT_SUBMODULE_STATUS_IN_WD) {
		git_config_backend *mods;
		const char *pattern = "submodule\\..*\\.path";
		git_buf path = GIT_BUF_INIT;
		fbp_data data = { NULL, NULL };

		git_buf_puts(&path, name);
		while (path.ptr[path.size-1] == '/') {
			path.ptr[--path.size] = '\0';
		}
		data.path = path.ptr;

		mods = open_gitmodules(repo, GITMODULES_EXISTING);

		if (mods)
			error = git_config_file_foreach_match(mods, pattern, find_by_path, &data);

		git_config_file_free(mods);

		if (error < 0) {
			git_submodule_free(sm);
			git_buf_free(&path);
			return error;
		}

		if (data.name) {
			git__free(sm->name);
			sm->name = data.name;
			sm->path = git_buf_detach(&path);

			/* Try to load again with the right name */
			if ((error = git_submodule_reload(sm, false)) < 0) {
				git_submodule_free(sm);
				return error;
			}
		}

		git_buf_free(&path);
	}

	if ((error = git_submodule_location(&location, sm)) < 0) {
		git_submodule_free(sm);
		return error;
	}

	/* If we still haven't found it, do the WD check */
	if (location == 0 || location == GIT_SUBMODULE_STATUS_IN_WD) {
		git_submodule_free(sm);
		error = GIT_ENOTFOUND;

		/* If it's not configured, we still check if there's a repo at the path */
		if (git_repository_workdir(repo)) {
			git_buf path = GIT_BUF_INIT;
			if (git_buf_join3(&path,
					  '/', git_repository_workdir(repo), name, DOT_GIT) < 0)
				return -1;

			if (git_path_exists(path.ptr))
				error = GIT_EEXISTS;

			git_buf_free(&path);
		}

		submodule_set_lookup_error(error, name);
		return error;
	}

	if (out)
		*out = sm;
	else
		git_submodule_free(sm);

	return 0;
}

static void submodule_free_dup(void *sm)
{
	git_submodule_free(sm);
}

static int submodule_get_or_create(git_submodule **out, git_repository *repo, git_strmap *map, const char *name)
{
	int error = 0;
	khiter_t pos;
	git_submodule *sm = NULL;

	pos = git_strmap_lookup_index(map, name);
	if (git_strmap_valid_index(map, pos)) {
		sm = git_strmap_value_at(map, pos);
		goto done;
	}

	/* if the submodule doesn't exist yet in the map, create it */
	if ((error = submodule_alloc(&sm, repo, name)) < 0)
		return error;

	pos = kh_put(str, map, sm->name, &error);
	/* nobody can beat us to adding it */
	assert(error != 0);
	if (error < 0) {
		git_submodule_free(sm);
		return error;
	}

	git_strmap_set_value_at(map, pos, sm);

done:
	GIT_REFCOUNT_INC(sm);
	*out = sm;
	return 0;
}

static int submodules_from_index(git_strmap *map, git_index *idx, git_config *cfg)
{
       int error;
       git_iterator *i;
       const git_index_entry *entry;
       git_buf name = GIT_BUF_INIT;

       if ((error = git_iterator_for_index(&i, git_index_owner(idx), idx, NULL)) < 0)
               return error;

       while (!(error = git_iterator_advance(&entry, i))) {
               khiter_t pos = git_strmap_lookup_index(map, entry->path);
               git_submodule *sm;

	       git_buf_clear(&name);
	       if (!name_from_path(&name, cfg, entry->path)) {
		       git_strmap_lookup_index(map, name.ptr);
	       }

               if (git_strmap_valid_index(map, pos)) {
                       sm = git_strmap_value_at(map, pos);

                       if (S_ISGITLINK(entry->mode))
                               submodule_update_from_index_entry(sm, entry);
                       else
                               sm->flags |= GIT_SUBMODULE_STATUS__INDEX_NOT_SUBMODULE;
               } else if (S_ISGITLINK(entry->mode)) {
                       if (!submodule_get_or_create(&sm, git_index_owner(idx), map, name.ptr ? name.ptr : entry->path)) {
                               submodule_update_from_index_entry(sm, entry);
                               git_submodule_free(sm);
                       }
               }
       }

       if (error == GIT_ITEROVER)
               error = 0;

       git_buf_free(&name);
       git_iterator_free(i);

       return error;
}

static int submodules_from_head(git_strmap *map, git_tree *head, git_config *cfg)
{
       int error;
       git_iterator *i;
       const git_index_entry *entry;
       git_buf name = GIT_BUF_INIT;

       if ((error = git_iterator_for_tree(&i, head, NULL)) < 0)
               return error;

       while (!(error = git_iterator_advance(&entry, i))) {
               khiter_t pos = git_strmap_lookup_index(map, entry->path);
               git_submodule *sm;

	       git_buf_clear(&name);
	       if (!name_from_path(&name, cfg, entry->path)) {
		       git_strmap_lookup_index(map, name.ptr);
	       }

               if (git_strmap_valid_index(map, pos)) {
                       sm = git_strmap_value_at(map, pos);

                       if (S_ISGITLINK(entry->mode))
                               submodule_update_from_head_data(sm, entry->mode, &entry->id);
                       else
                               sm->flags |= GIT_SUBMODULE_STATUS__HEAD_NOT_SUBMODULE;
               } else if (S_ISGITLINK(entry->mode)) {
                       if (!submodule_get_or_create(&sm, git_tree_owner(head), map, name.ptr ? name.ptr : entry->path)) {
                               submodule_update_from_head_data(
                                       sm, entry->mode, &entry->id);
                               git_submodule_free(sm);
                       }
               }
       }

       if (error == GIT_ITEROVER)
               error = 0;

       git_buf_free(&name);
       git_iterator_free(i);

       return error;
}

/* If have_sm is true, sm is populated, otherwise map an repo are. */
typedef struct {
	git_config *mods;
	git_strmap *map;
	git_repository *repo;
} lfc_data;

static int all_submodules(git_repository *repo, git_strmap *map)
{
	int error = 0;
	git_index *idx = NULL;
	git_tree *head = NULL;
	const char *wd = NULL;
	git_buf path = GIT_BUF_INIT;
	git_submodule *sm;
	git_config *mods = NULL;
	uint32_t mask;

	assert(repo && map);

	/* get sources that we will need to check */
	if (git_repository_index(&idx, repo) < 0)
		giterr_clear();
	if (git_repository_head_tree(&head, repo) < 0)
		giterr_clear();

	wd = git_repository_workdir(repo);
	if (wd && (error = git_buf_joinpath(&path, wd, GIT_MODULES_FILE)) < 0)
		goto cleanup;

	/* clear submodule flags that are to be refreshed */
	mask = 0;
	mask |= GIT_SUBMODULE_STATUS_IN_INDEX |
		GIT_SUBMODULE_STATUS__INDEX_FLAGS |
		GIT_SUBMODULE_STATUS__INDEX_OID_VALID |
		GIT_SUBMODULE_STATUS__INDEX_MULTIPLE_ENTRIES;

	mask |= GIT_SUBMODULE_STATUS_IN_HEAD |
		GIT_SUBMODULE_STATUS__HEAD_OID_VALID;
	mask |= GIT_SUBMODULE_STATUS_IN_CONFIG;
	if (mask != 0)
		mask |= GIT_SUBMODULE_STATUS_IN_WD |
			GIT_SUBMODULE_STATUS__WD_SCANNED |
			GIT_SUBMODULE_STATUS__WD_FLAGS |
			GIT_SUBMODULE_STATUS__WD_OID_VALID;

	/* add submodule information from .gitmodules */
	if (wd) {
		lfc_data data = { 0 };
		data.map = map;
		data.repo = repo;

		if ((mods = gitmodules_snapshot(repo)) == NULL)
			goto cleanup;

		data.mods = mods;
		if ((error = git_config_foreach(
			    mods, submodule_load_each, &data)) < 0)
			goto cleanup;
	}
	/* add back submodule information from index */
	if (idx) {
		if ((error = submodules_from_index(map, idx, mods)) < 0)
			goto cleanup;
	}
	/* add submodule information from HEAD */
	if (head) {
		if ((error = submodules_from_head(map, head, mods)) < 0)
			goto cleanup;
	}
	/* shallow scan submodules in work tree as needed */
	if (wd && mask != 0) {
		git_strmap_foreach_value(map, sm, {
				submodule_load_from_wd_lite(sm);
			});
	}

cleanup:
	git_config_free(mods);
	/* TODO: if we got an error, mark submodule config as invalid? */
	git_index_free(idx);
	git_tree_free(head);
	git_buf_free(&path);
	return error;
}

int git_submodule_foreach(
	git_repository *repo,
	git_submodule_cb callback,
	void *payload)
{
	git_vector snapshot = GIT_VECTOR_INIT;
	git_strmap *submodules;
	git_submodule *sm;
	int error;
	size_t i;

	if ((error = git_strmap_alloc(&submodules)) < 0)
		return error;

	if ((error = all_submodules(repo, submodules)) < 0)
		goto done;

	if (!(error = git_vector_init(
			&snapshot, kh_size(submodules), submodule_cmp))) {

		git_strmap_foreach_value(submodules, sm, {
			if ((error = git_vector_insert(&snapshot, sm)) < 0)
				break;
			GIT_REFCOUNT_INC(sm);
		});
	}

	if (error < 0)
		goto done;

	git_vector_uniq(&snapshot, submodule_free_dup);

	git_vector_foreach(&snapshot, i, sm) {
		if ((error = callback(sm, sm->name, payload)) != 0) {
			giterr_set_after_callback(error);
			break;
		}
	}

done:
	git_vector_foreach(&snapshot, i, sm)
		git_submodule_free(sm);
	git_vector_free(&snapshot);

	git_strmap_foreach_value(submodules, sm, {
		git_submodule_free(sm);
	});
	git_strmap_free(submodules);

	return error;
}

static int submodule_repo_init(
	git_repository **out,
	git_repository *parent_repo,
	const char *path,
	const char *url,
	bool use_gitlink)
{
	int error = 0;
	git_buf workdir = GIT_BUF_INIT, repodir = GIT_BUF_INIT;
	git_repository_init_options initopt = GIT_REPOSITORY_INIT_OPTIONS_INIT;
	git_repository *subrepo = NULL;

	error = git_buf_joinpath(&workdir, git_repository_workdir(parent_repo), path);
	if (error < 0)
		goto cleanup;

	initopt.flags = GIT_REPOSITORY_INIT_MKPATH | GIT_REPOSITORY_INIT_NO_REINIT;
	initopt.origin_url = url;

	/* init submodule repository and add origin remote as needed */

	/* New style: sub-repo goes in <repo-dir>/modules/<name>/ with a
	 * gitlink in the sub-repo workdir directory to that repository
	 *
	 * Old style: sub-repo goes directly into repo/<name>/.git/
	 */
	 if (use_gitlink) {
		error = git_buf_join3(
			&repodir, '/', git_repository_path(parent_repo), "modules", path);
		if (error < 0)
			goto cleanup;

		initopt.workdir_path = workdir.ptr;
		initopt.flags |=
			GIT_REPOSITORY_INIT_NO_DOTGIT_DIR |
			GIT_REPOSITORY_INIT_RELATIVE_GITLINK;

		error = git_repository_init_ext(&subrepo, repodir.ptr, &initopt);
	} else
		error = git_repository_init_ext(&subrepo, workdir.ptr, &initopt);

cleanup:
	git_buf_free(&workdir);
	git_buf_free(&repodir);

	*out = subrepo;

	return error;
}

int git_submodule_add_setup(
	git_submodule **out,
	git_repository *repo,
	const char *url,
	const char *path,
	int use_gitlink)
{
	int error = 0;
	git_config_backend *mods = NULL;
	git_submodule *sm = NULL;
	git_buf name = GIT_BUF_INIT, real_url = GIT_BUF_INIT;
	git_repository *subrepo = NULL;

	assert(repo && url && path);

	/* see if there is already an entry for this submodule */

	if (git_submodule_lookup(NULL, repo, path) < 0)
		giterr_clear();
	else {
		giterr_set(GITERR_SUBMODULE,
			"Attempt to add submodule '%s' that already exists", path);
		return GIT_EEXISTS;
	}

	/* validate and normalize path */

	if (git__prefixcmp(path, git_repository_workdir(repo)) == 0)
		path += strlen(git_repository_workdir(repo));

	if (git_path_root(path) >= 0) {
		giterr_set(GITERR_SUBMODULE, "Submodule path must be a relative path");
		error = -1;
		goto cleanup;
	}

	/* update .gitmodules */

	if (!(mods = open_gitmodules(repo, GITMODULES_CREATE))) {
		giterr_set(GITERR_SUBMODULE,
			"Adding submodules to a bare repository is not supported");
		return -1;
	}

	if ((error = git_buf_printf(&name, "submodule.%s.path", path)) < 0 ||
		(error = git_config_file_set_string(mods, name.ptr, path)) < 0)
		goto cleanup;

	if ((error = submodule_config_key_trunc_puts(&name, "url")) < 0 ||
		(error = git_config_file_set_string(mods, name.ptr, url)) < 0)
		goto cleanup;

	git_buf_clear(&name);

	/* init submodule repository and add origin remote as needed */

	error = git_buf_joinpath(&name, git_repository_workdir(repo), path);
	if (error < 0)
		goto cleanup;

	/* if the repo does not already exist, then init a new repo and add it.
	 * Otherwise, just add the existing repo.
	 */
	if (!(git_path_exists(name.ptr) &&
		git_path_contains(&name, DOT_GIT))) {

		/* resolve the actual URL to use */
		if ((error = git_submodule_resolve_url(&real_url, repo, url)) < 0)
			goto cleanup;

		 if ((error = submodule_repo_init(&subrepo, repo, path, real_url.ptr, use_gitlink)) < 0)
			goto cleanup;
	}

	if ((error = git_submodule_lookup(&sm, repo, path)) < 0)
		goto cleanup;

	error = git_submodule_init(sm, false);

cleanup:
	if (error && sm) {
		git_submodule_free(sm);
		sm = NULL;
	}
	if (out != NULL)
		*out = sm;

	git_config_file_free(mods);
	git_repository_free(subrepo);
	git_buf_free(&real_url);
	git_buf_free(&name);

	return error;
}

int git_submodule_repo_init(
	git_repository **out,
	const git_submodule *sm,
	int use_gitlink)
{
	int error;
	git_repository *sub_repo = NULL;
	const char *configured_url;
	git_config *cfg = NULL;
	git_buf buf = GIT_BUF_INIT;

	assert(out && sm);

	/* get the configured remote url of the submodule */
	if ((error = git_buf_printf(&buf, "submodule.%s.url", sm->name)) < 0 ||
		(error = git_repository_config_snapshot(&cfg, sm->repo)) < 0 ||
		(error = git_config_get_string(&configured_url, cfg, buf.ptr)) < 0 ||
		(error = submodule_repo_init(&sub_repo, sm->repo, sm->path, configured_url, use_gitlink)) < 0)
		goto done;

	*out = sub_repo;

done:
	git_config_free(cfg);
	git_buf_free(&buf);
	return error;
}

int git_submodule_add_finalize(git_submodule *sm)
{
	int error;
	git_index *index;

	assert(sm);

	if ((error = git_repository_index__weakptr(&index, sm->repo)) < 0 ||
		(error = git_index_add_bypath(index, GIT_MODULES_FILE)) < 0)
		return error;

	return git_submodule_add_to_index(sm, true);
}

int git_submodule_add_to_index(git_submodule *sm, int write_index)
{
	int error;
	git_repository *sm_repo = NULL;
	git_index *index;
	git_buf path = GIT_BUF_INIT;
	git_commit *head;
	git_index_entry entry;
	struct stat st;

	assert(sm);

	/* force reload of wd OID by git_submodule_open */
	sm->flags = sm->flags & ~GIT_SUBMODULE_STATUS__WD_OID_VALID;

	if ((error = git_repository_index__weakptr(&index, sm->repo)) < 0 ||
		(error = git_buf_joinpath(
			&path, git_repository_workdir(sm->repo), sm->path)) < 0 ||
		(error = git_submodule_open(&sm_repo, sm)) < 0)
		goto cleanup;

	/* read stat information for submodule working directory */
	if (p_stat(path.ptr, &st) < 0) {
		giterr_set(GITERR_SUBMODULE,
			"Cannot add submodule without working directory");
		error = -1;
		goto cleanup;
	}

	memset(&entry, 0, sizeof(entry));
	entry.path = sm->path;
	git_index_entry__init_from_stat(
		&entry, &st, !(git_index_caps(index) & GIT_INDEXCAP_NO_FILEMODE));

	/* calling git_submodule_open will have set sm->wd_oid if possible */
	if ((sm->flags & GIT_SUBMODULE_STATUS__WD_OID_VALID) == 0) {
		giterr_set(GITERR_SUBMODULE,
			"Cannot add submodule without HEAD to index");
		error = -1;
		goto cleanup;
	}
	git_oid_cpy(&entry.id, &sm->wd_oid);

	if ((error = git_commit_lookup(&head, sm_repo, &sm->wd_oid)) < 0)
		goto cleanup;

	entry.ctime.seconds = (int32_t)git_commit_time(head);
	entry.ctime.nanoseconds = 0;
	entry.mtime.seconds = (int32_t)git_commit_time(head);
	entry.mtime.nanoseconds = 0;

	git_commit_free(head);

	/* add it */
	error = git_index_add(index, &entry);

	/* write it, if requested */
	if (!error && write_index) {
		error = git_index_write(index);

		if (!error)
			git_oid_cpy(&sm->index_oid, &sm->wd_oid);
	}

cleanup:
	git_repository_free(sm_repo);
	git_buf_free(&path);
	return error;
}

const char *git_submodule_update_to_str(git_submodule_update_t update)
{
	int i;
	for (i = 0; i < (int)ARRAY_SIZE(_sm_update_map); ++i)
		if (_sm_update_map[i].map_value == (int)update)
			return _sm_update_map[i].str_match;
	return NULL;
}

git_repository *git_submodule_owner(git_submodule *submodule)
{
	assert(submodule);
	return submodule->repo;
}

const char *git_submodule_name(git_submodule *submodule)
{
	assert(submodule);
	return submodule->name;
}

const char *git_submodule_path(git_submodule *submodule)
{
	assert(submodule);
	return submodule->path;
}

const char *git_submodule_url(git_submodule *submodule)
{
	assert(submodule);
	return submodule->url;
}

int git_submodule_resolve_url(git_buf *out, git_repository *repo, const char *url)
{
	int error = 0;
	git_buf normalized = GIT_BUF_INIT;

	assert(out && repo && url);

	git_buf_sanitize(out);

	/* We do this in all platforms in case someone on Windows created the .gitmodules */
	if (strchr(url, '\\')) {
		if ((error = git_path_normalize_slashes(&normalized, url)) < 0)
			return error;

		url = normalized.ptr;
	}


	if (git_path_is_relative(url)) {
		if (!(error = get_url_base(out, repo)))
			error = git_path_apply_relative(out, url);
	} else if (strchr(url, ':') != NULL || url[0] == '/') {
		error = git_buf_sets(out, url);
	} else {
		giterr_set(GITERR_SUBMODULE, "Invalid format for submodule URL");
		error = -1;
	}

	git_buf_free(&normalized);
	return error;
}

static int write_var(git_repository *repo, const char *name, const char *var, const char *val)
{
	git_buf key = GIT_BUF_INIT;
	git_config_backend *mods;
	int error;

	mods = open_gitmodules(repo, GITMODULES_CREATE);
	if (!mods)
		return -1;

	if ((error = git_buf_printf(&key, "submodule.%s.%s", name, var)) < 0)
		goto cleanup;

	if (val)
		error = git_config_file_set_string(mods, key.ptr, val);
	else
		error = git_config_file_delete(mods, key.ptr);

	git_buf_free(&key);

cleanup:
	git_config_file_free(mods);
	return error;
}

static int write_mapped_var(git_repository *repo, const char *name, git_cvar_map *maps, size_t nmaps, const char *var, int ival)
{
	git_cvar_t type;
	const char *val;

	if (git_config_lookup_map_enum(&type, &val, maps, nmaps, ival) < 0) {
		giterr_set(GITERR_SUBMODULE, "invalid value for %s", var);
		return -1;
	}

	if (type == GIT_CVAR_TRUE)
		val = "true";

	return write_var(repo, name, var, val);
}

const char *git_submodule_branch(git_submodule *submodule)
{
	assert(submodule);
	return submodule->branch;
}

int git_submodule_set_branch(git_repository *repo, const char *name, const char *branch)
{

	assert(repo && name);

	return write_var(repo, name, "branch", branch);
}

int git_submodule_set_url(git_repository *repo, const char *name, const char *url)
{
	assert(repo && name && url);

	return write_var(repo, name, "url", url);
}

const git_oid *git_submodule_index_id(git_submodule *submodule)
{
	assert(submodule);

	if (submodule->flags & GIT_SUBMODULE_STATUS__INDEX_OID_VALID)
		return &submodule->index_oid;
	else
		return NULL;
}

const git_oid *git_submodule_head_id(git_submodule *submodule)
{
	assert(submodule);

	if (submodule->flags & GIT_SUBMODULE_STATUS__HEAD_OID_VALID)
		return &submodule->head_oid;
	else
		return NULL;
}

const git_oid *git_submodule_wd_id(git_submodule *submodule)
{
	assert(submodule);

	/* load unless we think we have a valid oid */
	if (!(submodule->flags & GIT_SUBMODULE_STATUS__WD_OID_VALID)) {
		git_repository *subrepo;

		/* calling submodule open grabs the HEAD OID if possible */
		if (!git_submodule_open_bare(&subrepo, submodule))
			git_repository_free(subrepo);
		else
			giterr_clear();
	}

	if (submodule->flags & GIT_SUBMODULE_STATUS__WD_OID_VALID)
		return &submodule->wd_oid;
	else
		return NULL;
}

git_submodule_ignore_t git_submodule_ignore(git_submodule *submodule)
{
	assert(submodule);
	return (submodule->ignore < GIT_SUBMODULE_IGNORE_NONE) ?
		GIT_SUBMODULE_IGNORE_NONE : submodule->ignore;
}

int git_submodule_set_ignore(git_repository *repo, const char *name, git_submodule_ignore_t ignore)
{
	assert(repo && name);

	return write_mapped_var(repo, name, _sm_ignore_map, ARRAY_SIZE(_sm_ignore_map), "ignore", ignore);
}

git_submodule_update_t git_submodule_update_strategy(git_submodule *submodule)
{
	assert(submodule);
	return (submodule->update < GIT_SUBMODULE_UPDATE_CHECKOUT) ?
		GIT_SUBMODULE_UPDATE_CHECKOUT : submodule->update;
}

int git_submodule_set_update(git_repository *repo, const char *name, git_submodule_update_t update)
{
	assert(repo && name);

	return write_mapped_var(repo, name, _sm_update_map, ARRAY_SIZE(_sm_update_map), "update", update);
}

git_submodule_recurse_t git_submodule_fetch_recurse_submodules(
	git_submodule *submodule)
{
	assert(submodule);
	return submodule->fetch_recurse;
}

int git_submodule_set_fetch_recurse_submodules(git_repository *repo, const char *name, git_submodule_recurse_t recurse)
{
	assert(repo && name);

	return write_mapped_var(repo, name, _sm_recurse_map, ARRAY_SIZE(_sm_recurse_map), "fetchRecurseSubmodules", recurse);
}

static int submodule_repo_create(
	git_repository **out,
	git_repository *parent_repo,
	const char *path)
{
	int error = 0;
	git_buf workdir = GIT_BUF_INIT, repodir = GIT_BUF_INIT;
	git_repository_init_options initopt = GIT_REPOSITORY_INIT_OPTIONS_INIT;
	git_repository *subrepo = NULL;

	initopt.flags =
		GIT_REPOSITORY_INIT_MKPATH |
		GIT_REPOSITORY_INIT_NO_REINIT |
		GIT_REPOSITORY_INIT_NO_DOTGIT_DIR |
		GIT_REPOSITORY_INIT_RELATIVE_GITLINK;

	/* Workdir: path to sub-repo working directory */
	error = git_buf_joinpath(&workdir, git_repository_workdir(parent_repo), path);
	if (error < 0)
		goto cleanup;

	initopt.workdir_path = workdir.ptr;

	/**
	 * Repodir: path to the sub-repo. sub-repo goes in:
	 * <repo-dir>/modules/<name>/ with a gitlink in the
	 * sub-repo workdir directory to that repository.
	 */
	error = git_buf_join3(
		&repodir, '/', git_repository_path(parent_repo), "modules", path);
	if (error < 0)
		goto cleanup;

	error = git_repository_init_ext(&subrepo, repodir.ptr, &initopt);

cleanup:
	git_buf_free(&workdir);
	git_buf_free(&repodir);

	*out = subrepo;

	return error;
}

/**
 * Callback to override sub-repository creation when
 * cloning a sub-repository.
 */
static int git_submodule_update_repo_init_cb(
	git_repository **out,
	const char *path,
	int bare,
	void *payload)
{
	git_submodule *sm;

	GIT_UNUSED(bare);

	sm = payload;

	return submodule_repo_create(out, sm->repo, path);
}

int git_submodule_update_init_options(git_submodule_update_options *opts, unsigned int version)
{
	GIT_INIT_STRUCTURE_FROM_TEMPLATE(
		opts, version, git_submodule_update_options, GIT_SUBMODULE_UPDATE_OPTIONS_INIT);
	return 0;
}

int git_submodule_update(git_submodule *sm, int init, git_submodule_update_options *_update_options)
{
	int error;
	unsigned int submodule_status;
	git_config *config = NULL;
	const char *submodule_url;
	git_repository *sub_repo = NULL;
	git_remote *remote = NULL;
	git_object *target_commit = NULL;
	git_buf buf = GIT_BUF_INIT;
	git_submodule_update_options update_options = GIT_SUBMODULE_UPDATE_OPTIONS_INIT;
	git_clone_options clone_options = GIT_CLONE_OPTIONS_INIT;

	assert(sm);

	if (_update_options)
		memcpy(&update_options, _update_options, sizeof(git_submodule_update_options));

	GITERR_CHECK_VERSION(&update_options, GIT_SUBMODULE_UPDATE_OPTIONS_VERSION, "git_submodule_update_options");

	/* Copy over the remote callbacks */
	memcpy(&clone_options.fetch_opts, &update_options.fetch_opts, sizeof(git_fetch_options));

	/* Get the status of the submodule to determine if it is already initialized  */
	if ((error = git_submodule_status(&submodule_status, sm->repo, sm->name, GIT_SUBMODULE_IGNORE_UNSPECIFIED)) < 0)
		goto done;

	/*
	 * If submodule work dir is not already initialized, check to see
	 * what we need to do (initialize, clone, return error...)
	 */
	if (submodule_status & GIT_SUBMODULE_STATUS_WD_UNINITIALIZED) {
		/*
		 * Work dir is not initialized, check to see if the submodule
		 * info has been copied into .git/config
		 */
		if ((error = git_repository_config_snapshot(&config, sm->repo)) < 0 ||
			(error = git_buf_printf(&buf, "submodule.%s.url", git_submodule_name(sm))) < 0)
			goto done;

		if ((error = git_config_get_string(&submodule_url, config, git_buf_cstr(&buf))) < 0) {
			/*
			 * If the error is not "not found" or if it is "not found" and we are not
			 * initializing the submodule, then return error.
			 */
			if (error != GIT_ENOTFOUND)
				goto done;

			if (error == GIT_ENOTFOUND && !init) {
				giterr_set(GITERR_SUBMODULE, "Submodule is not initialized.");
				error = GIT_ERROR;
				goto done;
			}

			/* The submodule has not been initialized yet - initialize it now.*/
			if ((error = git_submodule_init(sm, 0)) < 0)
				goto done;

			git_config_free(config);
			config = NULL;

			if ((error = git_repository_config_snapshot(&config, sm->repo)) < 0 ||
				(error = git_config_get_string(&submodule_url, config, git_buf_cstr(&buf))) < 0)
				goto done;
		}

		/** submodule is initialized - now clone it **/
		/* override repo creation */
		clone_options.repository_cb = git_submodule_update_repo_init_cb;
		clone_options.repository_cb_payload = sm;

		/*
		 * Do not perform checkout as part of clone, instead we
		 * will checkout the specific commit manually.
		 */
		clone_options.checkout_opts.checkout_strategy = GIT_CHECKOUT_NONE;
		update_options.checkout_opts.checkout_strategy = update_options.clone_checkout_strategy;

		if ((error = git_clone(&sub_repo, submodule_url, sm->path, &clone_options)) < 0 ||
			(error = git_repository_set_head_detached(sub_repo, git_submodule_index_id(sm))) < 0 ||
			(error = git_checkout_head(sub_repo, &update_options.checkout_opts)) != 0)
			goto done;
	} else {
		/**
		 * Work dir is initialized - look up the commit in the parent repository's index,
		 * update the workdir contents of the subrepository, and set the subrepository's
		 * head to the new commit.
		 */
		if ((error = git_submodule_open(&sub_repo, sm)) < 0 ||
			(error = git_object_lookup(&target_commit, sub_repo, git_submodule_index_id(sm), GIT_OBJ_COMMIT)) < 0 ||
			(error = git_checkout_tree(sub_repo, target_commit, &update_options.checkout_opts)) != 0 ||
			(error = git_repository_set_head_detached(sub_repo, git_submodule_index_id(sm))) < 0)
			goto done;

		/* Invalidate the wd flags as the workdir has been updated. */
		sm->flags = sm->flags &
			~(GIT_SUBMODULE_STATUS_IN_WD |
		  	GIT_SUBMODULE_STATUS__WD_OID_VALID |
		  	GIT_SUBMODULE_STATUS__WD_SCANNED);
	}

done:
	git_buf_free(&buf);
	git_config_free(config);
	git_object_free(target_commit);
	git_remote_free(remote);
	git_repository_free(sub_repo);

	return error;
}

int git_submodule_init(git_submodule *sm, int overwrite)
{
	int error;
	const char *val;
	git_buf key = GIT_BUF_INIT, effective_submodule_url = GIT_BUF_INIT;
	git_config *cfg = NULL;

	if (!sm->url) {
		giterr_set(GITERR_SUBMODULE,
			"No URL configured for submodule '%s'", sm->name);
		return -1;
	}

	if ((error = git_repository_config(&cfg, sm->repo)) < 0)
		return error;

	/* write "submodule.NAME.url" */

	if ((error = git_submodule_resolve_url(&effective_submodule_url, sm->repo, sm->url)) < 0 ||
		(error = git_buf_printf(&key, "submodule.%s.url", sm->name)) < 0 ||
		(error = git_config__update_entry(
			cfg, key.ptr, effective_submodule_url.ptr, overwrite != 0, false)) < 0)
		goto cleanup;

	/* write "submodule.NAME.update" if not default */

	val = (sm->update == GIT_SUBMODULE_UPDATE_CHECKOUT) ?
		NULL : git_submodule_update_to_str(sm->update);

	if ((error = git_buf_printf(&key, "submodule.%s.update", sm->name)) < 0 ||
		(error = git_config__update_entry(
			cfg, key.ptr, val, overwrite != 0, false)) < 0)
		goto cleanup;

	/* success */

cleanup:
	git_config_free(cfg);
	git_buf_free(&key);
	git_buf_free(&effective_submodule_url);

	return error;
}

int git_submodule_sync(git_submodule *sm)
{
	int error = 0;
	git_config *cfg = NULL;
	git_buf key = GIT_BUF_INIT;
	git_repository *smrepo = NULL;

	if (!sm->url) {
		giterr_set(GITERR_SUBMODULE,
			"No URL configured for submodule '%s'", sm->name);
		return -1;
	}

	/* copy URL over to config only if it already exists */

	if (!(error = git_repository_config__weakptr(&cfg, sm->repo)) &&
		!(error = git_buf_printf(&key, "submodule.%s.url", sm->name)))
		error = git_config__update_entry(cfg, key.ptr, sm->url, true, true);

	/* if submodule exists in the working directory, update remote url */

	if (!error &&
		(sm->flags & GIT_SUBMODULE_STATUS_IN_WD) != 0 &&
		!(error = git_submodule_open(&smrepo, sm)))
	{
		git_buf remote_name = GIT_BUF_INIT;

		if ((error = git_repository_config__weakptr(&cfg, smrepo)) < 0)
			/* return error from reading submodule config */;
		else if ((error = lookup_head_remote_key(&remote_name, smrepo)) < 0) {
			giterr_clear();
			error = git_buf_sets(&key, "remote.origin.url");
		} else {
			error = git_buf_join3(
				&key, '.', "remote", remote_name.ptr, "url");
			git_buf_free(&remote_name);
		}

		if (!error)
			error = git_config__update_entry(cfg, key.ptr, sm->url, true, false);

		git_repository_free(smrepo);
	}

	git_buf_free(&key);

	return error;
}

static int git_submodule__open(
	git_repository **subrepo, git_submodule *sm, bool bare)
{
	int error;
	git_buf path = GIT_BUF_INIT;
	unsigned int flags = GIT_REPOSITORY_OPEN_NO_SEARCH;
	const char *wd;

	assert(sm && subrepo);

	if (git_repository__ensure_not_bare(
			sm->repo, "open submodule repository") < 0)
		return GIT_EBAREREPO;

	wd = git_repository_workdir(sm->repo);

	if (git_buf_joinpath(&path, wd, sm->path) < 0 ||
		git_buf_joinpath(&path, path.ptr, DOT_GIT) < 0)
		return -1;

	sm->flags = sm->flags &
		~(GIT_SUBMODULE_STATUS_IN_WD |
		  GIT_SUBMODULE_STATUS__WD_OID_VALID |
		  GIT_SUBMODULE_STATUS__WD_SCANNED);

	if (bare)
		flags |= GIT_REPOSITORY_OPEN_BARE;

	error = git_repository_open_ext(subrepo, path.ptr, flags, wd);

	/* if we opened the submodule successfully, grab HEAD OID, etc. */
	if (!error) {
		sm->flags |= GIT_SUBMODULE_STATUS_IN_WD |
			GIT_SUBMODULE_STATUS__WD_SCANNED;

		if (!git_reference_name_to_id(&sm->wd_oid, *subrepo, GIT_HEAD_FILE))
			sm->flags |= GIT_SUBMODULE_STATUS__WD_OID_VALID;
		else
			giterr_clear();
	} else if (git_path_exists(path.ptr)) {
		sm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED |
			GIT_SUBMODULE_STATUS_IN_WD;
	} else {
		git_buf_rtruncate_at_char(&path, '/'); /* remove "/.git" */

		if (git_path_isdir(path.ptr))
			sm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED;
	}

	git_buf_free(&path);

	return error;
}

int git_submodule_open_bare(git_repository **subrepo, git_submodule *sm)
{
	return git_submodule__open(subrepo, sm, true);
}

int git_submodule_open(git_repository **subrepo, git_submodule *sm)
{
	return git_submodule__open(subrepo, sm, false);
}

static void submodule_update_from_index_entry(
	git_submodule *sm, const git_index_entry *ie)
{
	bool already_found = (sm->flags & GIT_SUBMODULE_STATUS_IN_INDEX) != 0;

	if (!S_ISGITLINK(ie->mode)) {
		if (!already_found)
			sm->flags |= GIT_SUBMODULE_STATUS__INDEX_NOT_SUBMODULE;
	} else {
		if (already_found)
			sm->flags |= GIT_SUBMODULE_STATUS__INDEX_MULTIPLE_ENTRIES;
		else
			git_oid_cpy(&sm->index_oid, &ie->id);

		sm->flags |= GIT_SUBMODULE_STATUS_IN_INDEX |
			GIT_SUBMODULE_STATUS__INDEX_OID_VALID;
	}
}

static int submodule_update_index(git_submodule *sm)
{
	git_index *index;
	const git_index_entry *ie;

	if (git_repository_index__weakptr(&index, sm->repo) < 0)
		return -1;

	sm->flags = sm->flags &
		~(GIT_SUBMODULE_STATUS_IN_INDEX |
		  GIT_SUBMODULE_STATUS__INDEX_OID_VALID);

	if (!(ie = git_index_get_bypath(index, sm->path, 0)))
		return 0;

	submodule_update_from_index_entry(sm, ie);

	return 0;
}

static void submodule_update_from_head_data(
	git_submodule *sm, mode_t mode, const git_oid *id)
{
	if (!S_ISGITLINK(mode))
		sm->flags |= GIT_SUBMODULE_STATUS__HEAD_NOT_SUBMODULE;
	else {
		git_oid_cpy(&sm->head_oid, id);

		sm->flags |= GIT_SUBMODULE_STATUS_IN_HEAD |
			GIT_SUBMODULE_STATUS__HEAD_OID_VALID;
	}
}

static int submodule_update_head(git_submodule *submodule)
{
	git_tree *head = NULL;
	git_tree_entry *te = NULL;

	submodule->flags = submodule->flags &
		~(GIT_SUBMODULE_STATUS_IN_HEAD |
		  GIT_SUBMODULE_STATUS__HEAD_OID_VALID);

	/* if we can't look up file in current head, then done */
	if (git_repository_head_tree(&head, submodule->repo) < 0 ||
		git_tree_entry_bypath(&te, head, submodule->path) < 0)
		giterr_clear();
	else
		submodule_update_from_head_data(submodule, te->attr, git_tree_entry_id(te));

	git_tree_entry_free(te);
	git_tree_free(head);
	return 0;
}

int git_submodule_reload(git_submodule *sm, int force)
{
	int error = 0;
	git_config *mods;

	GIT_UNUSED(force);

	assert(sm);

	if (!git_repository_is_bare(sm->repo)) {
		/* refresh config data */
		mods = gitmodules_snapshot(sm->repo);
		if (mods != NULL) {
			error = submodule_read_config(sm, mods);
			git_config_free(mods);

			if (error < 0)
				return error;
		}

		/* refresh wd data */
		sm->flags &=
			~(GIT_SUBMODULE_STATUS_IN_WD |
			  GIT_SUBMODULE_STATUS__WD_OID_VALID |
			  GIT_SUBMODULE_STATUS__WD_FLAGS);

		error = submodule_load_from_wd_lite(sm);
	}

	if (error == 0 && (error = submodule_update_index(sm)) == 0)
		error = submodule_update_head(sm);

	return error;
}

static void submodule_copy_oid_maybe(
	git_oid *tgt, const git_oid *src, bool valid)
{
	if (tgt) {
		if (valid)
			memcpy(tgt, src, sizeof(*tgt));
		else
			memset(tgt, 0, sizeof(*tgt));
	}
}

int git_submodule__status(
	unsigned int *out_status,
	git_oid *out_head_id,
	git_oid *out_index_id,
	git_oid *out_wd_id,
	git_submodule *sm,
	git_submodule_ignore_t ign)
{
	unsigned int status;
	git_repository *smrepo = NULL;

	if (ign == GIT_SUBMODULE_IGNORE_UNSPECIFIED)
		ign = sm->ignore;

	/* only return location info if ignore == all */
	if (ign == GIT_SUBMODULE_IGNORE_ALL) {
		*out_status = (sm->flags & GIT_SUBMODULE_STATUS__IN_FLAGS);
		return 0;
	}

	/* refresh the index OID */
	if (submodule_update_index(sm) < 0)
		return -1;

	/* refresh the HEAD OID */
	if (submodule_update_head(sm) < 0)
		return -1;

	/* for ignore == dirty, don't scan the working directory */
	if (ign == GIT_SUBMODULE_IGNORE_DIRTY) {
		/* git_submodule_open_bare will load WD OID data */
		if (git_submodule_open_bare(&smrepo, sm) < 0)
			giterr_clear();
		else
			git_repository_free(smrepo);
		smrepo = NULL;
	} else if (git_submodule_open(&smrepo, sm) < 0) {
		giterr_clear();
		smrepo = NULL;
	}

	status = GIT_SUBMODULE_STATUS__CLEAR_INTERNAL(sm->flags);

	submodule_get_index_status(&status, sm);
	submodule_get_wd_status(&status, sm, smrepo, ign);

	git_repository_free(smrepo);

	*out_status = status;

	submodule_copy_oid_maybe(out_head_id, &sm->head_oid,
		(sm->flags & GIT_SUBMODULE_STATUS__HEAD_OID_VALID) != 0);
	submodule_copy_oid_maybe(out_index_id, &sm->index_oid,
		(sm->flags & GIT_SUBMODULE_STATUS__INDEX_OID_VALID) != 0);
	submodule_copy_oid_maybe(out_wd_id, &sm->wd_oid,
		(sm->flags & GIT_SUBMODULE_STATUS__WD_OID_VALID) != 0);

	return 0;
}

int git_submodule_status(unsigned int *status, git_repository *repo, const char *name, git_submodule_ignore_t ignore)
{
	git_submodule *sm;
	int error;

	assert(status && repo && name);

	if ((error = git_submodule_lookup(&sm, repo, name)) < 0)
		return error;

	error = git_submodule__status(status, NULL, NULL, NULL, sm, ignore);
	git_submodule_free(sm);

	return error;
}

int git_submodule_location(unsigned int *location, git_submodule *sm)
{
	assert(location && sm);

	return git_submodule__status(
		location, NULL, NULL, NULL, sm, GIT_SUBMODULE_IGNORE_ALL);
}


/*
 * INTERNAL FUNCTIONS
 */

static int submodule_alloc(
	git_submodule **out, git_repository *repo, const char *name)
{
	size_t namelen;
	git_submodule *sm;

	if (!name || !(namelen = strlen(name))) {
		giterr_set(GITERR_SUBMODULE, "Invalid submodule name");
		return -1;
	}

	sm = git__calloc(1, sizeof(git_submodule));
	GITERR_CHECK_ALLOC(sm);

	sm->name = sm->path = git__strdup(name);
	if (!sm->name) {
		git__free(sm);
		return -1;
	}

	GIT_REFCOUNT_INC(sm);
	sm->ignore = sm->ignore_default = GIT_SUBMODULE_IGNORE_NONE;
	sm->update = sm->update_default = GIT_SUBMODULE_UPDATE_CHECKOUT;
	sm->fetch_recurse = sm->fetch_recurse_default = GIT_SUBMODULE_RECURSE_NO;
	sm->repo   = repo;
	sm->branch = NULL;

	*out = sm;
	return 0;
}

static void submodule_release(git_submodule *sm)
{
	if (!sm)
		return;

	if (sm->repo) {
		sm->repo = NULL;
	}

	if (sm->path != sm->name)
		git__free(sm->path);
	git__free(sm->name);
	git__free(sm->url);
	git__free(sm->branch);
	git__memzero(sm, sizeof(*sm));
	git__free(sm);
}

void git_submodule_free(git_submodule *sm)
{
	if (!sm)
		return;
	GIT_REFCOUNT_DEC(sm, submodule_release);
}

static int submodule_config_error(const char *property, const char *value)
{
	giterr_set(GITERR_INVALID,
		"Invalid value for submodule '%s' property: '%s'", property, value);
	return -1;
}

int git_submodule_parse_ignore(git_submodule_ignore_t *out, const char *value)
{
	int val;

	if (git_config_lookup_map_value(
			&val, _sm_ignore_map, ARRAY_SIZE(_sm_ignore_map), value) < 0) {
		*out = GIT_SUBMODULE_IGNORE_NONE;
		return submodule_config_error("ignore", value);
	}

	*out = (git_submodule_ignore_t)val;
	return 0;
}

int git_submodule_parse_update(git_submodule_update_t *out, const char *value)
{
	int val;

	if (git_config_lookup_map_value(
			&val, _sm_update_map, ARRAY_SIZE(_sm_update_map), value) < 0) {
		*out = GIT_SUBMODULE_UPDATE_CHECKOUT;
		return submodule_config_error("update", value);
	}

	*out = (git_submodule_update_t)val;
	return 0;
}

int git_submodule_parse_recurse(git_submodule_recurse_t *out, const char *value)
{
	int val;

	if (git_config_lookup_map_value(
			&val, _sm_recurse_map, ARRAY_SIZE(_sm_recurse_map), value) < 0) {
		*out = GIT_SUBMODULE_RECURSE_YES;
		return submodule_config_error("recurse", value);
	}

	*out = (git_submodule_recurse_t)val;
	return 0;
}

static int get_value(const char **out, git_config *cfg, git_buf *buf, const char *name, const char *field)
{
	int error;

	git_buf_clear(buf);

	if ((error = git_buf_printf(buf, "submodule.%s.%s", name, field)) < 0 ||
	    (error = git_config_get_string(out, cfg, buf->ptr)) < 0)
		return error;

	return error;
}

static int submodule_read_config(git_submodule *sm, git_config *cfg)
{
	git_buf key = GIT_BUF_INIT;
	const char *value;
	int error, in_config = 0;

	/*
	 * TODO: Look up path in index and if it is present but not a GITLINK
	 * then this should be deleted (at least to match git's behavior)
	 */

	if ((error = get_value(&value, cfg, &key, sm->name, "path")) == 0) {
		in_config = 1;
	/*
	 * TODO: if case insensitive filesystem, then the following strcmp
	 * should be strcasecmp
	 */
		if (strcmp(sm->name, value) != 0) {
			if (sm->path != sm->name)
				git__free(sm->path);
			sm->path = git__strdup(value);
			GITERR_CHECK_ALLOC(sm->path);
		}
	} else if (error != GIT_ENOTFOUND) {
		goto cleanup;
	}

	if ((error = get_value(&value, cfg, &key, sm->name, "url")) == 0) {
		in_config = 1;
		sm->url = git__strdup(value);
		GITERR_CHECK_ALLOC(sm->url);
	} else if (error != GIT_ENOTFOUND) {
		goto cleanup;
	}

	if ((error = get_value(&value, cfg, &key, sm->name, "branch")) == 0) {
		in_config = 1;
		sm->branch = git__strdup(value);
		GITERR_CHECK_ALLOC(sm->branch);
	} else if (error != GIT_ENOTFOUND) {
		goto cleanup;
	}

	if ((error = get_value(&value, cfg, &key, sm->name, "update")) == 0) {
		in_config = 1;
		if ((error = git_submodule_parse_update(&sm->update, value)) < 0)
			goto cleanup;
		sm->update_default = sm->update;
	} else if (error != GIT_ENOTFOUND) {
		goto cleanup;
	}

	if ((error = get_value(&value, cfg, &key, sm->name, "fetchRecurseSubmodules")) == 0) {
		in_config = 1;
		if ((error = git_submodule_parse_recurse(&sm->fetch_recurse, value)) < 0)
			goto cleanup;
		sm->fetch_recurse_default = sm->fetch_recurse;
	} else if (error != GIT_ENOTFOUND) {
		goto cleanup;
	}

	if ((error = get_value(&value, cfg, &key, sm->name, "ignore")) == 0) {
		in_config = 1;
		if ((error = git_submodule_parse_ignore(&sm->ignore, value)) < 0)
			goto cleanup;
		sm->ignore_default = sm->ignore;
	} else if (error != GIT_ENOTFOUND) {
		goto cleanup;
	}

	if (in_config)
		sm->flags |= GIT_SUBMODULE_STATUS_IN_CONFIG;

	error = 0;

cleanup:
	git_buf_free(&key);
	return error;
}

static int submodule_load_each(const git_config_entry *entry, void *payload)
{
	lfc_data *data = payload;
	const char *namestart, *property;
	git_strmap_iter pos;
	git_strmap *map = data->map;
	git_buf name = GIT_BUF_INIT;
	git_submodule *sm;
	int error;

	if (git__prefixcmp(entry->name, "submodule.") != 0)
		return 0;

	namestart = entry->name + strlen("submodule.");
	property  = strrchr(namestart, '.');

	if (!property || (property == namestart))
		return 0;

	property++;

	if ((error = git_buf_set(&name, namestart, property - namestart -1)) < 0)
		return error;

	/*
	 * Now that we have the submodule's name, we can use that to
	 * figure out whether it's in the map. If it's not, we create
	 * a new submodule, load the config and insert it. If it's
	 * already inserted, we've already loaded it, so we skip.
	 */
	pos = git_strmap_lookup_index(map, name.ptr);
	if (git_strmap_valid_index(map, pos)) {
		error = 0;
		goto done;
	}

	if ((error = submodule_alloc(&sm, data->repo, name.ptr)) < 0)
		goto done;

	if ((error = submodule_read_config(sm, data->mods)) < 0) {
		git_submodule_free(sm);
		goto done;
	}

	git_strmap_insert(map, sm->name, sm, error);
	assert(error != 0);
	if (error < 0)
		goto done;

	error = 0;

done:
	git_buf_free(&name);
	return error;
}

static int submodule_load_from_wd_lite(git_submodule *sm)
{
	git_buf path = GIT_BUF_INIT;

	if (git_buf_joinpath(&path, git_repository_workdir(sm->repo), sm->path) < 0)
		return -1;

	if (git_path_isdir(path.ptr))
		sm->flags |= GIT_SUBMODULE_STATUS__WD_SCANNED;

	if (git_path_contains(&path, DOT_GIT))
		sm->flags |= GIT_SUBMODULE_STATUS_IN_WD;

	git_buf_free(&path);
	return 0;
}

/**
 * Returns a snapshot of $WORK_TREE/.gitmodules.
 *
 * We ignore any errors and just pretend the file isn't there.
 */
static git_config *gitmodules_snapshot(git_repository *repo)
{
	const char *workdir = git_repository_workdir(repo);
	git_config *mods = NULL, *snap = NULL;
	git_buf path = GIT_BUF_INIT;

	if (workdir != NULL) {
		if (git_buf_joinpath(&path, workdir, GIT_MODULES_FILE) != 0)
			return NULL;

		if (git_config_open_ondisk(&mods, path.ptr) < 0)
			mods = NULL;
	}

	git_buf_free(&path);

	if (mods) {
		git_config_snapshot(&snap, mods);
		git_config_free(mods);
	}

	return snap;
}

static git_config_backend *open_gitmodules(
	git_repository *repo,
	int okay_to_create)
{
	const char *workdir = git_repository_workdir(repo);
	git_buf path = GIT_BUF_INIT;
	git_config_backend *mods = NULL;

	if (workdir != NULL) {
		if (git_buf_joinpath(&path, workdir, GIT_MODULES_FILE) != 0)
			return NULL;

		if (okay_to_create || git_path_isfile(path.ptr)) {
			/* git_config_file__ondisk should only fail if OOM */
			if (git_config_file__ondisk(&mods, path.ptr) < 0)
				mods = NULL;
			/* open should only fail here if the file is malformed */
			else if (git_config_file_open(mods, GIT_CONFIG_LEVEL_LOCAL) < 0) {
				git_config_file_free(mods);
				mods = NULL;
			}
		}
	}

	git_buf_free(&path);

	return mods;
}

/* Lookup name of remote of the local tracking branch HEAD points to */
static int lookup_head_remote_key(git_buf *remote_name, git_repository *repo)
{
	int error;
	git_reference *head = NULL;
	git_buf upstream_name = GIT_BUF_INIT;

	/* lookup and dereference HEAD */
	if ((error = git_repository_head(&head, repo)) < 0)
		return error;

	/**
	 * If head does not refer to a branch, then return
	 * GIT_ENOTFOUND to indicate that we could not find
	 * a remote key for the local tracking branch HEAD points to.
	 **/
	if (!git_reference_is_branch(head)) {
		giterr_set(GITERR_INVALID,
			"HEAD does not refer to a branch.");
		error = GIT_ENOTFOUND;
		goto done;
	}

	/* lookup remote tracking branch of HEAD */
	if ((error = git_branch_upstream_name(
		&upstream_name,
		repo,
		git_reference_name(head))) < 0)
		goto done;

	/* lookup remote of remote tracking branch */
	if ((error = git_branch_remote_name(remote_name, repo, upstream_name.ptr)) < 0)
		goto done;

done:
	git_buf_free(&upstream_name);
	git_reference_free(head);

	return error;
}

/* Lookup the remote of the local tracking branch HEAD points to */
static int lookup_head_remote(git_remote **remote, git_repository *repo)
{
	int error;
	git_buf remote_name = GIT_BUF_INIT;

	/* lookup remote of remote tracking branch name */
	if (!(error = lookup_head_remote_key(&remote_name, repo)))
		error = git_remote_lookup(remote, repo, remote_name.ptr);

	git_buf_free(&remote_name);

	return error;
}

/* Lookup remote, either from HEAD or fall back on origin */
static int lookup_default_remote(git_remote **remote, git_repository *repo)
{
	int error = lookup_head_remote(remote, repo);

	/* if that failed, use 'origin' instead */
	if (error == GIT_ENOTFOUND)
		error = git_remote_lookup(remote, repo, "origin");

	if (error == GIT_ENOTFOUND)
		giterr_set(
			GITERR_SUBMODULE,
			"Cannot get default remote for submodule - no local tracking "
			"branch for HEAD and origin does not exist");

	return error;
}

static int get_url_base(git_buf *url, git_repository *repo)
{
	int error;
	git_remote *remote = NULL;

	if (!(error = lookup_default_remote(&remote, repo))) {
		error = git_buf_sets(url, git_remote_url(remote));
		git_remote_free(remote);
	}
	else if (error == GIT_ENOTFOUND) {
		/* if repository does not have a default remote, use workdir instead */
		giterr_clear();
		error = git_buf_sets(url, git_repository_workdir(repo));
	}

	return error;
}

static void submodule_get_index_status(unsigned int *status, git_submodule *sm)
{
	const git_oid *head_oid  = git_submodule_head_id(sm);
	const git_oid *index_oid = git_submodule_index_id(sm);

	*status = *status & ~GIT_SUBMODULE_STATUS__INDEX_FLAGS;

	if (!head_oid) {
		if (index_oid)
			*status |= GIT_SUBMODULE_STATUS_INDEX_ADDED;
	}
	else if (!index_oid)
		*status |= GIT_SUBMODULE_STATUS_INDEX_DELETED;
	else if (!git_oid_equal(head_oid, index_oid))
		*status |= GIT_SUBMODULE_STATUS_INDEX_MODIFIED;
}


static void submodule_get_wd_status(
	unsigned int *status,
	git_submodule *sm,
	git_repository *sm_repo,
	git_submodule_ignore_t ign)
{
	const git_oid *index_oid = git_submodule_index_id(sm);
	const git_oid *wd_oid =
		(sm->flags & GIT_SUBMODULE_STATUS__WD_OID_VALID) ? &sm->wd_oid : NULL;
	git_tree *sm_head = NULL;
	git_index *index = NULL;
	git_diff_options opt = GIT_DIFF_OPTIONS_INIT;
	git_diff *diff;

	*status = *status & ~GIT_SUBMODULE_STATUS__WD_FLAGS;

	if (!index_oid) {
		if (wd_oid)
			*status |= GIT_SUBMODULE_STATUS_WD_ADDED;
	}
	else if (!wd_oid) {
		if ((sm->flags & GIT_SUBMODULE_STATUS__WD_SCANNED) != 0 &&
			(sm->flags & GIT_SUBMODULE_STATUS_IN_WD) == 0)
			*status |= GIT_SUBMODULE_STATUS_WD_UNINITIALIZED;
		else
			*status |= GIT_SUBMODULE_STATUS_WD_DELETED;
	}
	else if (!git_oid_equal(index_oid, wd_oid))
		*status |= GIT_SUBMODULE_STATUS_WD_MODIFIED;

	/* if we have no repo, then we're done */
	if (!sm_repo)
		return;

	/* the diffs below could be optimized with an early termination
	 * option to the git_diff functions, but for now this is sufficient
	 * (and certainly no worse that what core git does).
	 */

	if (ign == GIT_SUBMODULE_IGNORE_NONE)
		opt.flags |= GIT_DIFF_INCLUDE_UNTRACKED;

	(void)git_repository_index__weakptr(&index, sm_repo);

	/* if we don't have an unborn head, check diff with index */
	if (git_repository_head_tree(&sm_head, sm_repo) < 0)
		giterr_clear();
	else {
		/* perform head to index diff on submodule */
		if (git_diff_tree_to_index(&diff, sm_repo, sm_head, index, &opt) < 0)
			giterr_clear();
		else {
			if (git_diff_num_deltas(diff) > 0)
				*status |= GIT_SUBMODULE_STATUS_WD_INDEX_MODIFIED;
			git_diff_free(diff);
			diff = NULL;
		}

		git_tree_free(sm_head);
	}

	/* perform index-to-workdir diff on submodule */
	if (git_diff_index_to_workdir(&diff, sm_repo, index, &opt) < 0)
		giterr_clear();
	else {
		size_t untracked =
			git_diff_num_deltas_of_type(diff, GIT_DELTA_UNTRACKED);

		if (untracked > 0)
			*status |= GIT_SUBMODULE_STATUS_WD_UNTRACKED;

		if (git_diff_num_deltas(diff) != untracked)
			*status |= GIT_SUBMODULE_STATUS_WD_WD_MODIFIED;

		git_diff_free(diff);
		diff = NULL;
	}
}
