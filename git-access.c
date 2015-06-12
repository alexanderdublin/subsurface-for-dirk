#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <git2.h>

#include "dive.h"
#include "membuffer.h"
#include "strndup.h"

/*
 * The libgit2 people are incompetent at making libraries. They randomly change
 * the interfaces, often just renaming things without any sane way to know which
 * version you should check for etc etc. It's a disgrace.
 */
#if !LIBGIT2_VER_MAJOR && LIBGIT2_VER_MINOR < 22
  #define git_remote_lookup(res, repo, name) git_remote_load(res, repo, name)
  #if LIBGIT2_VER_MINOR <= 20
    #define git_remote_fetch(remote, refspecs, signature, reflog) git_remote_fetch(remote)
  #else
    #define git_remote_fetch(remote, refspecs, signature, reflog) git_remote_fetch(remote, signature, reflog)
  #endif
#endif
/*
 * api break introduced in libgit2 master after 0.22 - let's guess this is the v0.23 API
 */
#if USE_LIBGIT23_API
  #define git_branch_create(out, repo, branch_name, target, force, signature, log_message) \
	git_branch_create(out, repo, branch_name, target, force)
#endif

enum remote_type { OTHER, HTTPS, SSH };

static char *get_local_dir(const char *remote, const char *branch)
{
	SHA_CTX ctx;
	unsigned char hash[20];

	// That zero-byte update is so that we don't get hash
	// collisions for "repo1 branch" vs "repo 1branch".
	SHA1_Init(&ctx);
	SHA1_Update(&ctx, remote, strlen(remote));
	SHA1_Update(&ctx, "", 1);
	SHA1_Update(&ctx, branch, strlen(branch));
	SHA1_Final(hash, &ctx);

	return format_string("%s/%02x%02x%02x%02x%02x%02x%02x%02x",
			system_default_directory(),
			hash[0], hash[1], hash[2], hash[3],
			hash[4], hash[5], hash[6], hash[7]);
}

static int check_clean(const char *path, unsigned int status, void *payload)
{
	status &= ~GIT_STATUS_CURRENT | GIT_STATUS_IGNORED;
	if (!status)
		return 0;
	report_error("WARNING: Git cache directory modified (path %s)", path);
	return 1;
}

/*
 * The remote is strictly newer than the local branch.
 */
static int reset_to_remote(git_repository *repo, git_reference *local, const git_oid *new_id)
{
	git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
	git_object *target;

	// If it's not checked out (bare or not HEAD), just update the reference */
	if (git_repository_is_bare(repo) || git_branch_is_head(local) != 1) {
		git_reference *out;

		if (git_reference_set_target(&out, local, new_id, "Update to remote"))
			return report_error("Could not update local ref to newer remote ref");

		git_reference_free(out);

		// Not really an error, just informational
		report_error("Updated local branch from remote");

		return 0;
	}

	if (git_object_lookup(&target, repo, new_id, GIT_OBJ_COMMIT))
		return report_error("Could not look up remote commit");

	opts.checkout_strategy = GIT_CHECKOUT_SAFE;
	if (git_reset(repo, target, GIT_RESET_HARD, &opts))
		return report_error("Local head checkout failed after update");

	// Not really an error, just informational
	report_error("Updated local information from remote");

	return 0;
}

#if USE_LIBGIT23_API
int credential_ssh_cb(git_cred **out,
		  const char *url,
		  const char *username_from_url,
		  unsigned int allowed_types,
		  void *payload)
{
	const char *priv_key = format_string("%s/%s", system_default_directory(), "ssrf_remote.key");
	const char *passphrase = prefs.cloud_storage_password ? strdup(prefs.cloud_storage_password) : strdup("");
	return git_cred_ssh_key_new(out, username_from_url, NULL, priv_key, passphrase);
}

int credential_https_cb(git_cred **out,
			const char *url,
			const char *username_from_url,
			unsigned int allowed_types,
			void *payload)
{
	const char *username = prefs.cloud_storage_email_encoded;
	const char *password = prefs.cloud_storage_password ? strdup(prefs.cloud_storage_password) : strdup("");
	return git_cred_userpass_plaintext_new(out, username, password);
}
#endif

static int update_remote(git_repository *repo, git_remote *origin, git_reference *local, git_reference *remote, enum remote_type rt)
{
	git_push_options opts = GIT_PUSH_OPTIONS_INIT;
	git_strarray refspec;
	const char *name = git_reference_name(local);

	refspec.count = 1;
	refspec.strings = (char **)&name;

#if USE_LIBGIT23_API
	if (rt == SSH)
		opts.callbacks.credentials = credential_ssh_cb;
	else if (rt == HTTPS)
		opts.callbacks.credentials = credential_https_cb;
#endif
	if (git_remote_push(origin, &refspec, &opts))
		return report_error("Unable to update remote with current local cache state (%s)", giterr_last()->message);

	// Not actually an error, just informational
	report_error("Local cache more recent than remote");
	return 0;
}

static int try_to_update(git_repository *repo, git_remote *origin, git_reference *local, git_reference *remote, enum remote_type rt)
{
	git_oid base;
	const git_oid *local_id, *remote_id;

	if (!git_reference_cmp(local, remote))
		return 0;

	// Dirty modified state in the working tree? We're not going
	// to update either way
	if (git_status_foreach(repo, check_clean, NULL))
		return report_error("local cached copy is dirty, skipping update");

	local_id = git_reference_target(local);
	remote_id = git_reference_target(remote);

	if (!local_id || !remote_id)
		return report_error("Unable to get local or remote SHA1");

	if (git_merge_base(&base, repo, local_id, remote_id))
		return report_error("Unable to find common commit of local and remote branches");

	/* Is the remote strictly newer? Use it */
	if (git_oid_equal(&base, local_id))
		return reset_to_remote(repo, local, remote_id);

	/* Is the local repo the more recent one? See if we can update upstream */
	if (git_oid_equal(&base, remote_id))
		return update_remote(repo, origin, local, remote, rt);

	/* Merging a bare repository always needs user action */
	if (git_repository_is_bare(repo))
		return report_error("Local and remote have diverged, merge of bare branch needed");

	/* Merging will definitely need the head branch too */
	if (git_branch_is_head(local) != 1)
		return report_error("Local and remote do not match, local branch not HEAD - cannot update");

	/*
	 * Some day we migth try a clean merge here.
	 *
	 * But I couldn't find any good examples of this, so for now
	 * you'd need to merge divergent histories manually. But we've
	 * at least verified above that we have a working tree and the
	 * current branch is checked out and clean, so we *could* try
	 * to merge.
	 */
	return report_error("Local and remote have diverged, need to merge");
}

static int check_remote_status(git_repository *repo, git_remote *origin, const char *branch, enum remote_type rt)
{
	git_reference *local_ref, *remote_ref;

	if (git_branch_lookup(&local_ref, repo, branch, GIT_BRANCH_LOCAL))
		return report_error("Git cache branch %s no longer exists", branch);

	if (git_branch_upstream(&remote_ref, local_ref)) {
		git_reference_free(local_ref);
		return report_error("Git cache branch %s no longer has an upstream branch", branch);
	}

	try_to_update(repo, origin, local_ref, remote_ref, rt);
	git_reference_free(local_ref);
	git_reference_free(remote_ref);
}

/* from qthelper.cpp */
extern bool getProxyString(char **proxy_string);

static git_repository *update_local_repo(const char *localdir, const char *remote, const char *branch)
{
	int error;
	git_repository *repo = NULL;
	git_remote *origin;
	enum remote_type rt;
	char *proxy_string;
	git_config *conf;

	error = git_repository_open(&repo, localdir);
	if (error) {
		report_error("Unable to open git cache repository at %s: %s",
			localdir, giterr_last()->message);
		return NULL;
	}
	if (strncmp(remote, "ssh://", 6) == 0)
		rt = SSH;
	else if (strncmp(remote, "https://", 8) == 0)
		rt = HTTPS;
	else
		rt = OTHER;

	if (rt == HTTPS && getProxyString(&proxy_string)) {
		git_repository_config(&conf, repo);
		git_config_set_string(conf, "http.proxy", proxy_string);
		free(proxy_string);
	}
	/*
	 * NOTE! Remote errors are reported, but are nonfatal:
	 * we still successfully return the local repository.
	 */
	error = git_remote_lookup(&origin, repo, "origin");
	if (error) {
		report_error("Repository '%s' origin lookup failed (%s)",
			remote, giterr_last()->message);
		return repo;
	}

#if USE_LIBGIT23_API
	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	if (rt == SSH)
		opts.callbacks.credentials = credential_ssh_cb;
	else if (rt == HTTPS)
		opts.callbacks.credentials = credential_https_cb;
	error = git_remote_fetch(origin, NULL, &opts, NULL);
#else
	error = git_remote_fetch(origin, NULL, NULL, NULL);
#endif
	// NOTE! A fetch error is not fatal, we just report it
	if (error)
		report_error("Unable to fetch remote '%s'", remote);
	else
		check_remote_status(repo, origin, branch, rt);

	git_remote_free(origin);
	return repo;
}

static git_repository *create_local_repo(const char *localdir, const char *remote, const char *branch)
{
	int error;
	git_repository *cloned_repo = NULL;
	git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
#if USE_LIBGIT23_API
	if (strncmp(remote, "ssh://", 6) == 0)
		opts.fetch_opts.callbacks.credentials = credential_ssh_cb;
	else if (strncmp(remote, "https://", 8) == 0)
		opts.fetch_opts.callbacks.credentials = credential_https_cb;
#endif
	opts.checkout_branch = branch;
	error = git_clone(&cloned_repo, remote, localdir, &opts);
	if (error) {
		report_error("git clone of %s failed (%s)", remote, giterr_last()->message);
		return NULL;
	}
	return cloned_repo;
}

static struct git_repository *get_remote_repo(const char *localdir, const char *remote, const char *branch)
{
	struct stat st;

	/* Do we already have a local cache? */
	if (!stat(localdir, &st)) {
		if (!S_ISDIR(st.st_mode)) {
			report_error("local git cache at '%s' is corrupt");
			return NULL;
		}
		return update_local_repo(localdir, remote, branch);
	}
	return create_local_repo(localdir, remote, branch);
}

/*
 * This turns a remote repository into a local one if possible.
 *
 * The recognized formats are
 *    git://host/repo[branch]
 *    ssh://host/repo[branch]
 *    http://host/repo[branch]
 *    https://host/repo[branch]
 *    file://repo[branch]
 */
static struct git_repository *is_remote_git_repository(char *remote, const char *branch)
{
	char c, *localdir;
	const char *p = remote;

	while ((c = *p++) >= 'a' && c <= 'z')
		/* nothing */;
	if (c != ':')
		return NULL;
	if (*p++ != '/' || *p++ != '/')
		return NULL;

	/* Special-case "file://", since it's already local */
	if (!strncmp(remote, "file://", 7))
		remote += 7;

	/*
	 * Ok, we found "[a-z]*://", we've simplified the
	 * local repo case (because libgit2 is insanely slow
	 * for that), and we think we have a real "remote
	 * git" format.
	 *
	 * We now create the SHA1 hash of the whole thing,
	 * including the branch name. That will be our unique
	 * unique local repository name.
	 *
	 * NOTE! We will create a local repository per branch,
	 * because
	 *
	 *  (a) libgit2 remote tracking branch support seems to
	 *      be a bit lacking
	 *  (b) we'll actually check the branch out so that we
	 *      can do merges etc too.
	 *
	 * so even if you have a single remote git repo with
	 * multiple branches for different people, the local
	 * caches will sadly force that to split into multiple
	 * individual repositories.
	 */

	/*
	 * next we need to make sure that any encoded username
	 * has been extracted from an https:// based URL
	 */
	if  (!strncmp(remote, "https://", 8)) {
		char *at = strchr(remote, '@');
		if (at) {
			/* was this the @ that denotes an account? that means it was before the
			 * first '/' after the https:// - so let's find a '/' after that and compare */
			char *slash = strchr(remote + 8, '/');
			if (slash && slash > at) {
				/* grab the part between "https://" and "@" as encoded email address
				 * (that's our username) and move the rest of the URL forward, remembering
				 * to copy the closing NUL as well */
				prefs.cloud_storage_email_encoded = strndup(remote + 8, at - remote - 8);
				memmove(remote + 8, at + 1, strlen(at + 1) + 1);
			}
		}
	}
	localdir = get_local_dir(remote, branch);
	if (!localdir)
		return NULL;

	return get_remote_repo(localdir, remote, branch);
}

/*
 * If it's not a git repo, return NULL. Be very conservative.
 */
struct git_repository *is_git_repository(const char *filename, const char **branchp)
{
	int flen, blen, ret;
	int offset = 1;
	struct stat st;
	git_repository *repo;
	char *loc, *branch;

	flen = strlen(filename);
	if (!flen || filename[--flen] != ']')
		return NULL;

	/* Find the matching '[' */
	blen = 0;
	while (flen && filename[--flen] != '[')
		blen++;

	/* Ignore slashes at the end of the repo name */
	while (flen && filename[flen-1] == '/') {
		flen--;
		offset++;
	}

	if (!flen)
		return NULL;

	/*
	 * This is the "point of no return": the name matches
	 * the git repository name rules, and we will no longer
	 * return NULL.
	 *
	 * We will either return "dummy_git_repository" and the
	 * branch pointer will have the _whole_ filename in it,
	 * or we will return a real git repository with the
	 * branch pointer being filled in with just the branch
	 * name.
	 *
	 * The actual git reading/writing routines can use this
	 * to generate proper error messages.
	 */
	*branchp = filename;
	loc = format_string("%.*s", flen, filename);
	if (!loc)
		return dummy_git_repository;

	branch = format_string("%.*s", blen, filename + flen + offset);
	if (!branch) {
		free(loc);
		return dummy_git_repository;
	}

	repo = is_remote_git_repository(loc, branch);
	if (repo) {
		free(loc);
		*branchp = branch;
		return repo;
	}

	if (stat(loc, &st) < 0 || !S_ISDIR(st.st_mode)) {
		free(loc);
		free(branch);
		return dummy_git_repository;
	}

	ret = git_repository_open(&repo, loc);
	free(loc);
	if (ret < 0) {
		free(branch);
		return dummy_git_repository;
	}
	*branchp = branch;
	return repo;
}
