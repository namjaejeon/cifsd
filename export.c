/*
 *   fs/cifsd/export.c
 *
 *   Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *   Copyright (C) 2016 Namjae Jeon <namjae.jeon@protocolfreedom.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <linux/types.h>
#include <linux/parser.h>
#include <linux/textsearch.h>
#include "glob.h"
#include "export.h"
#include "smb1pdu.h"

#include "transport_tcp.h"
#include "mgmt/user_session.h"

/* @FIXME: remove this code */

/* max string size for share and parameters */
#define SHARE_MAX_NAME_LEN	100
/* max string size data, ex- path, usernames, servernames etc */
#define SHARE_MAX_DATA_LEN	PATH_MAX
#define MAX_NT_PWD_LEN		128

LIST_HEAD(cifsd_share_list);
LIST_HEAD(cifsd_session_list);

__u16 tid = 1;
int cifsd_debug_enable = 0;
int cifsd_caseless_search;
static inline void free_share(struct cifsd_share *share);

/* Number of shares defined on server */
int cifsd_num_shares;

/* The parameters defined on configuration */
int maptoguest;
int server_signing;
char *guestAccountName;
char *server_string;
char *workgroup;
char *netbios_name;
int server_min_pr;
int server_max_pr;

struct cifsd_pipe_table cifsd_pipes[] = {
	{"\\srvsvc", SRVSVC},
	{"srvsvc", SRVSVC},
	{"\\wkssvc", SRVSVC},
	{"wkssvc", SRVSVC},
	{"\\winreg", WINREG},
	{"winreg", WINREG},
};
unsigned int npipes = ARRAY_SIZE(cifsd_pipes);

/**
 * get_pipe_type() - get the type of the pipe from the string name
 * @name:      string name for representation of pipe, need to be searched
 *             in the supported table
 * Return:     the pipe type number if found in the table,
 *             else invalid pipe type
 */
unsigned int get_pipe_type(char *pipename)
{
	int i;
	unsigned int pipetype = INVALID_PIPE;

	for (i = 0; i < npipes; i++) {
		if (!strcmp(cifsd_pipes[i].pipename, pipename)) {
			pipetype = cifsd_pipes[i].pipetype;
			break;
		}
	}
	return pipetype;
}

/**
 * get_pipe_desc() - get matching pipe descriptor from pipe id
 * @sess:	session info
 * @id:		lookup pipe id
 *
 * Return:	matching pipe descriptor from opened pipe id
 */
struct cifsd_pipe *get_pipe_desc(struct cifsd_session *sess,
		unsigned int id)
{
	struct cifsd_pipe *pipe_desc;
	int i;

	if (unlikely(!sess))
		return NULL;

	for (i = 0; i < MAX_PIPE; i++) {
		/* fid is not created for LANMAN */
		if (i == LANMAN)
			continue;

		pipe_desc = sess->pipe_desc[i];
		if (!pipe_desc)
			continue;

		if (pipe_desc->id == id)
			return pipe_desc;
	}

	return NULL;
}

/**
 * __add_share() - helper function to add a share in global exported share list
 * @share:	share instance to be added to global share list
 * @sharename:	name of share
 * @pathname:	path of share point
 *
 * Return:      true on success, false on error
 */
static bool __add_share(struct cifsd_share *share, char *sharename,
		       char *pathname)
{
	struct kstat stat;
	struct path share_path;
	int err;

	/* pathname will be NULL for IPC$ share */
	if (pathname != NULL) {
		err = kern_path(pathname, 0, &share_path);
		if (err) {
			cifsd_err("share add failed for %s\n", pathname);
			return false;
		} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
			err = vfs_getattr(&share_path, &stat, STATX_BASIC_STATS,
				AT_STATX_SYNC_AS_STAT);
#else
			err = vfs_getattr(&share_path, &stat);

#endif
			path_put(&share_path);
			if (err) {
				cifsd_err("share add failed for %s\n",
					    pathname);
				return false;
			}
		}
	}

	share->path = pathname;
	share->tcount = 0;
	share->tid = tid++;
	share->sharename = sharename;
	atomic_set(&share->num_conn, 0);
	INIT_LIST_HEAD(&share->list);
	list_add(&share->list, &cifsd_share_list);
	cifsd_num_shares++;
	return true;
}

/**
 * init_share() - initialize config parameters of a share
 * @share:	share instance to be initialized
 */
static void init_share(struct cifsd_share *share)
{
	set_attr_available(&share->config.attr);
	set_attr_browsable(&share->config.attr);
	clear_attr_guestok(&share->config.attr);
	clear_attr_guestonly(&share->config.attr);
	set_attr_oplocks(&share->config.attr);
	set_attr_readonly(&share->config.attr);
	set_attr_writeok(&share->config.attr);
	share->config.max_connections = 0;
	INIT_LIST_HEAD(&share->config.filter_list);
}

/**
 * add_share() - allocate and add a share in global exported share list
 * @sharename:	name of share
 * @pathname:	path of share point
 *
 * Return:      0 on success, error number on error
 */
static int add_share(char *sharename, char *pathname)
{
	struct cifsd_share *share;
	int ret;

	share = kzalloc(sizeof(struct cifsd_share), GFP_KERNEL);
	if (!share)
		return -ENOMEM;

	init_share(share);

	ret = __add_share(share, sharename, pathname);
	if (!ret) {
		free_share(share);
		kfree(share);
	}

	return 0;
}

/**
 * free_share() - free a share point release associated memory
 * @share:	share instance to be freed
 */
static inline void free_share(struct cifsd_share *share)
{
	kfree(share->sharename);

	if (share->path)
		kfree(share->path);

	kfree(share->config.comment);
	kfree(share->config.allow_hosts);
	kfree(share->config.deny_hosts);
	kfree(share->config.invalid_users);
	kfree(share->config.read_list);
	kfree(share->config.write_list);
	kfree(share->config.valid_users);
}

static void destroy_filters(struct cifsd_share *share)
{
	struct list_head *tmp, *t;
	struct cifsd_filter *cf;

	list_for_each_safe(tmp, t, &share->config.filter_list) {
		cf = list_entry(tmp, struct cifsd_filter, entry);
		list_del(&cf->entry);
		textsearch_destroy(cf->config);
		kfree(cf);
	}
}

/**
 * cifsd_share_free() - delete all shares from global exported share list
 */
static void cifsd_share_free(void)
{
	struct cifsd_share *share;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		destroy_filters(share);
		list_del(&share->list);
		cifsd_num_shares--;
		free_share(share);
		kfree(share);
	}
}

/**
 * cleanup_bad_share() - remove a bad share, added while config parse error
 * @badshare:	share name to be removed
 */
static void cleanup_bad_share(struct cifsd_share *badshare)
{
	struct cifsd_share *share;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		if (share != badshare)
			continue;
		list_del(&share->list);
		cifsd_num_shares--;
	}
	free_share(badshare);
	kfree(badshare);
}

static int parse_user_strings(const char *src, char **str, int exp_num,
	ssize_t src_len)
{
	int s_num = 0, pos = 0;
	ssize_t len;

	while (s_num < exp_num) {
		len = strcspn(&src[pos], (const char *)":") + 1;
		if (!len)
			break;

		str[s_num] = kmalloc(len, GFP_KERNEL);
		if (!str[s_num])
			break;

		strlcpy(str[s_num], &src[pos], len);
		s_num++;
		pos += len;
		if (pos >= src_len)
			break;
	}

	return s_num;
}

static char *strim_conflist_entry(char *p, char *limit)
{
	while ((p < limit) && (*p == ',' || *p == '\t' || *p == ' '))
		p++;
	return p < limit ? p : NULL;
}

/*
 * conflist_search() - looks up for a key in smb.conf config list
 * @list:	config string (must not be NULL)
 * @key:	key to lookup for
 *
 * Return:	0 when entry found, -ENOENT when not found
 */
static int conflist_search(char *list, char *key)
{
	char *begin = list, *end;

	/*
	 * From smb.conf
	 *    This parameter is a comma, space, or tab delimited set of
	 *    hosts which are permitted to access a service
	 */
	while ((end = strpbrk(begin, "\t, "))) {
		char e;

		if (end - begin < 2) {
			begin++;
			continue;
		}

		begin = strim_conflist_entry(begin, end);
		if (!begin)
			return -ENOENT;

		e = *end;
		*end = '\0';

		if (!strcmp(begin, key))
			return 0;
		*end = e;
		begin = end + 1;
	}

	begin = strim_conflist_entry(begin, list + strlen(list) - 1);
	if (!begin)
		return -ENOENT;
	if (!strcmp(begin, key))
		return 0;
	return -ENOENT;
}

/**
 * validate_host() - check if a client is allowed or denied access of a share
 * @cip:	host ip to be checked
 * @share:	share config containing allowed and denied list of client ip
 *
 * Return:      0 if cip is allowed access to share, otherwise
 */
static int validate_host(char *cip, struct cifsd_share *share)
{
	char *allow_list = share->config.allow_hosts;
	char *deny_list = share->config.deny_hosts;

	if (!allow_list && !deny_list)
		return 0;

	if (allow_list) {
		if (conflist_search(allow_list, cip) == -ENOENT)
			return -EACCES;
		/*
		 * "allow hosts" list takes precedence over "deny hosts" list,
		 *  No further checking needed
		 */
		return 0;
	}

	if (deny_list && conflist_search(deny_list, cip) == 0)
		return -EACCES;

	/*
	 * Default is always allowed - So, when there is no allowed list
	 * and no entry in Deny, then switch to default behaviour
	 */
	return 0;
}

/**
 * validate_user() - check if a user is allowed or denied access to a share
 * @usr:	user to be checked
 * @share:	share config containing allowed and denied list of users
 *
 * Return:      0 if usr is allowed access to share, otherwise error
 */
static int validate_user(struct cifsd_session *sess,
			  struct cifsd_share *share,
			  bool *can_write)
{
	char *vlist = share->config.valid_users;
	char *ilist = share->config.invalid_users;
	char *wlist = share->config.write_list;
	char *rlist = share->config.read_list;

	/* for share IPC$, does not support smb.conf share parameters*/
	if (!share->path)
		return 0;

	/* if "guest = ok, no checking of users required "*/
	/*
	 * if guest ok not set, but guestAccountname
	 * mapped with valid share path
	 */
	if (get_attr_guestok(&share->config.attr)) {
		cifsd_debug("guest login on to share %s\n",
				share->sharename);
		return 0;
	}

	/* name should not be present in "invalid users" */
	if (ilist && conflist_search(ilist, user_name(sess->user)) == 0)
		return -EACCES;

	*can_write = (share->writeable == 1) ? true : false;
	/* if user present in read list, sess will be readable */
	if (rlist && conflist_search(rlist, user_name(sess->user)) == 0)
		*can_write = false;

	/* if user present in write list, make user session writeable */
	if (wlist && conflist_search(wlist, user_name(sess->user)) == 0)
		*can_write = true;

	/* if "valid users" list is empty then any user can login */
	if (!vlist)
		return 0;

	/* user exists in "valid users" list? */
	return conflist_search(vlist, user_name(sess->user));
}

struct cifsd_share *get_cifsd_share(struct cifsd_tcp_conn *conn,
				    struct cifsd_session *sess,
				    char *sharename,
				    bool *can_write)
{
	struct list_head *tmp;
	struct cifsd_share *share;
	int rc;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);

		cifsd_debug("comparing(%s) with treename %s\n",
				sharename, share->sharename);

		if (strcasecmp(share->sharename, sharename))
			continue;

		rc = validate_host(conn->peeraddr, share);
		if (rc != 0) {
			cifsd_err("[host:%s] not allowed for [share:%s]\n",
				  conn->peeraddr, share->sharename);
			return ERR_PTR(rc);
		}

		rc = validate_user(sess, share, can_write);
		if (rc != 0) {
			cifsd_err("[user:%s] not authorised for [share:%s]\n",
				  user_name(sess->user), share->sharename);
			return ERR_PTR(rc);
		}
		return share;
	}
	cifsd_debug("Tree(%s) not exported on connection\n", sharename);
	return ERR_PTR(-ENOENT);
}

/**
 * find_matching_share() - get a share instance from tree id
 * @tid:	tree id for share instance lookup
 *
 * Return:      share if there is matching share tid, otherwise NULL
 */
struct cifsd_share *find_matching_share(__u16 tid)
{
	struct cifsd_share *share;
	struct list_head *tmp;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		if (share->tid == tid)
			return share;
	}

	return NULL;
}

struct cifsd_user *cifsd_is_user_present(char *name)
{
	struct cifsd_user *user = um_user_search(name);

	if (user)
		return user;
	if (maptoguest)
		return um_user_search_guest();
	return NULL;
}

/**
 * get_smb_session_user() - get logged in user information for a session
 * @sess:    session information
 *
 * Return:      matching user for a session on success, otherwise NULL
 */
struct cifsd_user *get_smb_session_user(struct cifsd_session *sess)
{
	/*
	 * FIXME I don't understand why did we perform user list lookup
	 * here. The session-user mapping seems to be 1:1. Anyway, this
	 * probably will be reworked anyway.
	 */
	return sess->user;
}

/**
 * check_share() - check if a share name is already exported,if not
 *		allocate a new empty share
 * @share_buf:	buffer containing share name
 * @share_sz:	share name length
 *
 * Return:      share name if already exported, otherwise NULL
 */
static struct cifsd_share *check_share(char *share_name, int *alloc_share)
{
	struct cifsd_share *share;
	struct list_head *tmp;
	int srclen;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		srclen = strlen(share->sharename);
		if (srclen == strlen(share_name)) {
			if (strncasecmp(share->sharename,
					share_name, srclen) == 0) {
				return share;
			}
		}
	}

	share = kzalloc(sizeof(struct cifsd_share), GFP_KERNEL);
	if (!share)
		return ERR_PTR(-ENOMEM);

	init_share(share);
	*alloc_share = 1;
	return share;
}

/**
 * cifsadmin_user_query() - search a user from user list
 * @username:	buffer containing user name to search
 *
 * Return:	0: for username found
 *	  -EINVAL: if not found from cifsd user list
 */
int cifsadmin_user_query(char *name)
{
	int ret = -EINVAL;
	struct cifsd_user *user = um_user_search(name);

	if (user) {
		put_cifsd_user(user);
		ret = 0;
	}

	return ret;
}

/**
 * cifsadmin_user_del() - delete a user from user list
 * @username:	buffer containing user name to delete
 *
 * Return:      0: for username found and deleted
 *	  -EINVAL: if not found from cifsd user list
 */
int cifsadmin_user_del(char *name)
{
	return um_delete_user(name);
}

/**
 * cifsd_user_store() - add a user in valid user list
 * @buf:	buffer containing user name to be added
 * @len:	user name buf length
 *
 * Return:      user name buf length on success, otherwise error
 */
int cifsd_user_store(const char *buf, size_t len)
{
	enum {
		CONF_USER,
		CONF_PASSWD,
		CONF_UID,
		CONF_GID,
	};
	char *conf[CONF_GID + 1] = {0};
	kuid_t uid;
	kgid_t gid;
	int ret;

	uid.val = 0;
	gid.val = 0;

	ret = parse_user_strings(buf, conf, ARRAY_SIZE(conf), len);
	if (ret < 2) {
		cifsd_err("[%s] <usr:pass> format err\n", __func__);
		ret = -EINVAL;
		goto out;
	}

	if (ret > 2) {
		ret = -EINVAL;
		if (kstrtouint(conf[CONF_UID], 10, &uid.val))
			goto out;
		if (kstrtouint(conf[CONF_GID], 10, &gid.val))
			goto out;
		cifsd_debug("uid : %u, gid %u\n", uid.val, gid.val);
	}

	ret = um_add_new_user(conf[CONF_USER], conf[CONF_PASSWD], uid, gid);
	if (ret == -EEXIST) {
		ret = len;
		goto out;
	}
	if (ret != 0)
		goto out;

	/*
	 * Success. cifsd_usr keeps pointers to conf[CONF_USER] and
	 * conf[CONF_PASSWD]. So we free all of conf[] entries on error,
	 * but we need to keep CONF_USER and CONF_PASSWD alive on success.
	 */
	ret = len;
out:
	kfree(conf[CONF_GID]);
	kfree(conf[CONF_UID]);
	if (ret != len) {
		kfree(conf[CONF_PASSWD]);
		kfree(conf[CONF_USER]);
	}
	return ret;
}

/**
 * cifsd_debug_store() - enable debug prints
 * @buf:	buffer containing debug enable disable setting
 *
 * Return:	0: on success, -EINVAL on fail
 */
int cifsd_debug_store(const char *buf)
{
	long int value;

	if (kstrtol(buf, 10, &value))
		return -EINVAL;

	if (value > 0)
		cifsd_debug_enable = value;
	else if (value == 0)
		cifsd_debug_enable = 0;

	return 0;
}

/**
 * cifsd_caseless_search_store() - enable disable case insensitive
 search of files
 * @buf:	buffer containing case setting
 *
 * Return:      0: success, -EINVAL: on fail
 */
int cifsd_caseless_search_store(const char *buf)
{
	long int value;

	if (kstrtol(buf, 10, &value))
		return -EINVAL;

	if (value > 0)
		cifsd_caseless_search = 1;
	else if (value == 0)
		cifsd_caseless_search = 0;

	return 0;
}

enum {
	Opt_guest,
	Opt_servern,
	Opt_domain,
	Opt_netbiosname,
	Opt_signing,
	Opt_maptoguest,
	Opt_server_min_protocol,
	Opt_server_max_protocol,

	Opt_global_err
};

static const match_table_t cifsd_global_tokens = {
	{ Opt_guest, "guest account = %s" },
	{ Opt_servern, "server string = %s" },
	{ Opt_domain, "workgroup = %s" },
	{ Opt_netbiosname, "netbios name = %s" },
	{ Opt_signing, "server signing = %s" },
	{ Opt_maptoguest, "map to guest = %s" },
	{ Opt_server_min_protocol, "server min protocol = %s" },
	{ Opt_server_max_protocol, "server max protocol = %s" },

	{ Opt_global_err, NULL }
};

enum {
	Opt_sharename,
	Opt_available,
	Opt_browsable,
	Opt_writeable,
	Opt_guestok,
	Opt_guestonly,
	Opt_oplocks,
	Opt_maxcon,
	Opt_comment,
	Opt_allowhost,
	Opt_denyhost,
	Opt_validusers,
	Opt_invalidusers,
	Opt_path,
	Opt_readlist,
	Opt_readonly,
	Opt_writeok,
	Opt_writelist,
	Opt_hostallow,
	Opt_hostdeny,
	Opt_store_dos_attr,
	Opt_veto_files,

	Opt_share_err
};

static const match_table_t cifsd_share_tokens = {
	{ Opt_sharename, "sharename = %s" },
	{ Opt_available, "available = %s" },
	{ Opt_browsable, "browsable = %s" },
	{ Opt_writeable, "writeable = %s" },
	{ Opt_guestok, "guest ok = %s" },
	{ Opt_guestonly, "guest only = %s" },
	{ Opt_oplocks, "oplocks = %s" },
	{ Opt_maxcon, "max connections = %s" },
	{ Opt_comment, "comment = %s" },
	{ Opt_allowhost, "allow hosts = %s" },
	{ Opt_denyhost, "deny hosts = %s" },
	{ Opt_validusers, "valid users = %s" },
	{ Opt_invalidusers, "invalid users = %s" },
	{ Opt_path, "path = %s" },
	{ Opt_readlist, "read list = %s" },
	{ Opt_readonly, "read only = %s" },
	{ Opt_writeok, "write ok = %s" },
	{ Opt_writelist, "write list = %s" },
	{ Opt_hostallow, "hosts allow = %s" },
	{ Opt_hostdeny, "hosts deny = %s" },
	{ Opt_store_dos_attr, "store dos attributes = %s" },
	{ Opt_veto_files, "veto files = %s" },

	{ Opt_share_err, NULL }
};

/*
 * cifsd_get_config_str() - get a configuration string
 * @arg:	configuration argument list
 * @config:	destination to store output config string
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
static int cifsd_get_config_str(substring_t args[], char **config)
{
	kfree(*config);
	*config = match_strdup(args);
	if (!*config)
		return -ENOMEM;

	return 0;
}

/*
 * cifsd_get_config_val() - get a configuration string value
 * @arg:	configuration argument list
 * @val:	destination to store output val
 *
 * Return:      0 on success, otherwise error
 */
static int cifsd_get_config_val(substring_t args[], unsigned int *val)
{
	char *str;
	int ret = 0;

	str = match_strdup(args);
	if (str == NULL)
		return -ENOMEM;

	if (!strcasecmp(str, "yes") ||
	    !strcasecmp(str, "true") ||
	    !strcasecmp(str, "enable") ||
	    !strcasecmp(str, "Bad User") ||
	    !strcmp(str, "1"))
		*val = ENABLE;
	else if (!strcasecmp(str, "no") ||
		 !strcasecmp(str, "false") ||
		 !strcasecmp(str, "disable") ||
		 !strcasecmp(str, "Never") ||
		 !strcmp(str, "0"))
		*val = DISABLE;
	else if (!strcasecmp(str, "auto"))
		*val = AUTO;
	else if (!strcasecmp(str, "mandatory"))
		*val = MANDATORY;
	else {
		cifsd_err("bad option value %s\n", str);
		ret = -EINVAL;
	}

	kfree(str);
	return ret;
}

static int cifsd_parse_global_options(char *configdata)
{
	char *data;
	char *options;
	char separator[2];
	char *string = NULL;

	separator[0] = '<';
	separator[1] = 0;

	if (!configdata)
		goto config_err;

	options = configdata;

	while ((data = strsep(&options, separator)) != NULL) {
		substring_t args[MAX_OPT_ARGS];
		int token;

		if (!*data)
			continue;

		token = match_token(data, cifsd_global_tokens, args);
		switch (token) {
		case Opt_guest:
		{
			char *user_name;
			struct cifsd_user *user;
			kuid_t uid;
			kgid_t gid;

			if (cifsd_get_config_str(args, &guestAccountName))
				goto out_nomem;

			user_name = kstrdup(guestAccountName, GFP_KERNEL);
			if (!user_name)
				goto out_nomem;

			uid.val = 9999;
			gid.val = 9999;
			if (um_add_new_user(user_name, NULL, uid, gid)) {
				kfree(user_name);
				goto config_err;
			}
			user = um_user_search(user_name);
			if (!user) {
				kfree(user_name);
				goto config_err;
			}
			set_user_guest(user);

			break;
		}
		case Opt_servern:
			if (cifsd_get_config_str(args, &server_string))
				goto out_nomem;
			break;
		case Opt_domain:
			if (cifsd_get_config_str(args, &workgroup))
				goto out_nomem;
			break;
		case Opt_netbiosname:
			if (cifsd_get_config_str(args, &netbios_name))
				goto out_nomem;
			break;
		case Opt_signing:
			if (cifsd_get_config_val(args, &server_signing) < 0)
				goto out_nomem;
			break;
		case Opt_maptoguest:
			if (cifsd_get_config_val(args, &maptoguest) < 0)
				goto out_nomem;
			break;
		case Opt_server_min_protocol:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			server_min_pr = get_protocol_idx(string);
			if (server_min_pr < 0)
				server_min_pr = cifsd_min_protocol();
			kfree(string);
			break;
		case Opt_server_max_protocol:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			server_max_pr = get_protocol_idx(string);
			if (server_max_pr < 0)
				server_max_pr = cifsd_max_protocol();
			kfree(string);
			break;
		default:
			cifsd_err("[%s] not supported\n", data);
			break;
		}
	}

	return 0;

out_nomem:
	cifsd_err("Could not allocate buffer\n");

config_err:
	return 1;
}

static struct cifsd_filter *parse_veto_file(char *string)
{
	struct cifsd_filter *cf;
	char *pattern;
	int len;

	cf = kzalloc(sizeof(struct cifsd_filter), GFP_KERNEL);
	if (!cf)
		return NULL;

	INIT_LIST_HEAD(&cf->entry);

	if (string[0] == '*') {
		if (string[1] == '.') {
			pattern = &string[1];
			len = strlen(pattern);
			cf->type = FILTER_FILE_EXTENSION;
		} else {
			pattern = &string[1];
			len = strlen(pattern) - 1;
			cf->type = FILTER_WILDCARD;
		}
	} else {
		pattern = string;
		len = strlen(pattern);
		cf->type = FILTER_NON_TYPE;
	}

	cifsd_debug("add pattern(%s) entry to veto file list\n", pattern);
	cf->config = textsearch_prepare("kmp", pattern,
			len, GFP_KERNEL, TS_AUTOLOAD);
	if (IS_ERR(cf->config)) {
		kfree(cf);
		cf = NULL;
	}

	return cf;
}

static void add_filter_share(struct cifsd_share *share, char *veto_strings)
{
	char *parse_str = veto_strings;
	char *str;
	struct cifsd_filter *cf;

	strsep(&parse_str, "/");
	do {
		str = strsep(&parse_str, "/");
		if (!str)
			break;
		cf = parse_veto_file(str);
		if (!cf)
			continue;
		list_add(&cf->entry, &share->config.filter_list);

	} while (str != NULL);
}

static int varify_veto_file(char *veto_strings)
{
	if (veto_strings[0] != '/')
		return -EINVAL;

	if (!strchr(&veto_strings[1], '/'))
		return -EINVAL;


	return 0;
}

static int cifsd_parse_share_options(const char *configdata)
{
	struct cifsd_share *share = NULL;
	char *data, *end;
	char *configdata_copy = NULL, *options;
	char separator[2];
	char *string = NULL;
	unsigned int val;
	unsigned int new_share = 0;

	separator[0] = '<';
	separator[1] = 0;

	if (!configdata)
		goto config_err;

	configdata_copy = kstrndup(configdata, PAGE_SIZE, GFP_KERNEL);
	if (!configdata_copy)
		goto config_err;

	options = configdata_copy;
	end = options + strlen(options);

	while ((data = strsep(&options, separator)) != NULL) {
		substring_t args[MAX_OPT_ARGS];
		int token;

		if (!*data)
			continue;

		token = match_token(data, cifsd_share_tokens, args);
		switch (token) {
		case Opt_sharename:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			if (!strncmp(string, "global", 6)) {
				kfree(string);
				if (cifsd_parse_global_options(options) != 0)
					goto config_err;
				options = end;
			} else {
				share = check_share(string, &new_share);
				if (IS_ERR(share)) {
					kfree(string);
					share = NULL;
					goto config_err;
				}
				share->sharename = string;
			}
			break;
		case Opt_available:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;

			if (val == 0)
				clear_attr_available(&share->config.attr);
			else
				set_attr_available(&share->config.attr);
			break;
		case Opt_browsable:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 0)
				clear_attr_browsable(&share->config.attr);
			else
				set_attr_browsable(&share->config.attr);
			break;
		case Opt_writeable:
			if (!share ||
				cifsd_get_config_val(args, &share->writeable))
				goto config_err;
			break;
		case Opt_guestok:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_guestok(&share->config.attr);
			else
				clear_attr_guestok(&share->config.attr);
			break;
		case Opt_guestonly:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_guestonly(&share->config.attr);
			else
				clear_attr_guestonly(&share->config.attr);
			break;
		case Opt_oplocks:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 0)
				clear_attr_oplocks(&share->config.attr);
			else
				set_attr_oplocks(&share->config.attr);
			break;
		case Opt_maxcon:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			if (!share || kstrtouint(string, 10, &val)) {
				kfree(string);
				goto config_err;
			}
			share->config.max_connections = val;
			kfree(string);
			break;
		case Opt_comment:
			if (!share || cifsd_get_config_str(args,
						&share->config.comment))
				goto out_nomem;
			break;
		case Opt_allowhost:
		case Opt_hostallow:
			if (!share || cifsd_get_config_str(args,
						&share->config.allow_hosts))
				goto out_nomem;
			break;
		case Opt_denyhost:
		case Opt_hostdeny:
			if (!share || cifsd_get_config_str(args,
						&share->config.deny_hosts))
				goto out_nomem;
			break;
		case Opt_validusers:
			if (!share || cifsd_get_config_str(args,
						&share->config.valid_users))
				goto out_nomem;
			break;
		case Opt_invalidusers:
			if (!share || cifsd_get_config_str(args,
						&share->config.invalid_users))
				goto out_nomem;
			break;
		case Opt_path:
			if (!share || cifsd_get_config_str(args,
						&share->path))
				goto out_nomem;
			if (new_share && !__add_share(share, share->sharename,
						share->path))
				cifsd_err("share add error %s:%s\n",
						share->sharename, share->path);
			break;
		case Opt_readlist:
			if (!share || cifsd_get_config_str(args,
						&share->config.read_list))
				goto out_nomem;
			break;
		case Opt_readonly:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_readonly(&share->config.attr);
			else
				clear_attr_readonly(&share->config.attr);
			break;
		case Opt_writeok:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_writeok(&share->config.attr);
			else
				clear_attr_writeok(&share->config.attr);
			break;
		case Opt_writelist:
			if (!share || cifsd_get_config_str(args,
						&share->config.write_list))
				goto out_nomem;
			break;
		case Opt_store_dos_attr:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_store_dos(&share->config.attr);
			else
				clear_attr_store_dos(&share->config.attr);
			break;
		case Opt_veto_files:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			if (varify_veto_file(string) < 0)
				return -EINVAL;
			add_filter_share(share, string);
			kfree(string);
			break;
		default:
			cifsd_err("[%s] not supported\n", data);
			break;
		}
	}

	kfree(configdata_copy);
	return 0;

out_nomem:
	cifsd_err("Could not allocate buffer\n");

config_err:
	if (new_share && share)
		cleanup_bad_share(share);
	kfree(configdata_copy);
	return 1;
}

/**
 * cifsd_config_store() - update config settings
 * @buf:	buffer containing config setting
 * @len:	buf length of config setting
 *
 * Return:      config setting buf length
 */

int cifsd_config_store(const char *buf, size_t len)
{
	if (cifsd_parse_share_options(buf))
		return -EINVAL;

	return len;
}

/**
 * cifsd_add_IPC_share() - add share entry for IPC$ pipe with tid = 1
 *
 * Return:      0 on success, otherwise error
 */
static int cifsd_add_IPC_share(void)
{
	char *ipc;
	int len, rc;

	len = strlen(STR_IPC) + 1;

	ipc = kmalloc(len, GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;

	memcpy(ipc, STR_IPC, len - 1);
	ipc[len - 1] = '\0';

	rc = add_share(ipc, NULL);
	if (rc)
		kfree(ipc);

	return rc;
}

/**
 * cifsd_init_global_params() - initialize default values of Server name
 *		Domain name
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
int cifsd_init_global_params(void)
{
	int len;

	len = strlen(STR_SRV_NAME);
	server_string = kzalloc(len + 1, GFP_KERNEL);
	if (!server_string)
		return -ENOMEM;
	memcpy(server_string, STR_SRV_NAME, len);

	len = strlen(STR_WRKGRP);
	workgroup = kzalloc(len + 1, GFP_KERNEL);
	if (!workgroup) {
		kfree(server_string);
		return -ENOMEM;
	}
	memcpy(workgroup, STR_WRKGRP, len);

	len = strlen(TGT_Name);
	netbios_name = kzalloc(len + 1, GFP_KERNEL);
	if (!netbios_name) {
		kfree(server_string);
		kfree(workgroup);
		return -ENOMEM;
	}
	memcpy(netbios_name, TGT_Name, len);

	server_signing = 0;
	maptoguest = 0;
	server_min_pr = cifsd_min_protocol();
	server_max_pr = cifsd_max_protocol();
	return 0;
}

/**
 * cifsd_free_global_params() - free global parameters
 */
void cifsd_free_global_params(void)
{
	kfree(server_string);
	kfree(workgroup);
	kfree(guestAccountName);
	kfree(netbios_name);
}

/**
 * cifsd_export_init() - perform export related setup at module
 *			load time
 *
 * Return:      0 on success, otherwise error
 */
int cifsd_export_init(void)
{
	int rc;

	/* IPC share */
	rc = cifsd_add_IPC_share();
	if (rc)
		return rc;

	rc = cifsd_init_global_params();
	if (rc) {
		cifsd_share_free();
		return rc;
	}

	return 0;
}

/**
 * cifsd_export_exit() - perform export related cleanup at module
 *			exit time
 */
void cifsd_export_exit(void)
{
	cifsd_free_global_params();
	um_cleanup_users();
	cifsd_share_free();
}
