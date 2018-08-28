/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
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

#ifndef __USER_SESSION_MANAGEMENT_H__
#define __USER_SESSION_MANAGEMENT_H__

#include <linux/hashtable.h>

#include "../glob.h"  /* FIXME */

#define CIFDS_SESSION_FLAG_SMB1		(1 << 0)
#define CIFDS_SESSION_FLAG_SMB2		(1 << 1)

#define PREAUTH_HASHVALUE_SIZE		64

struct cifsd_ida;

struct channel {
	__u8			smb3signingkey[SMB3_SIGN_KEY_SIZE];
	struct cifsd_tcp_conn	*conn;
	struct list_head	chann_list;
};

struct preauth_session {
	__u8			Preauth_HashValue[PREAUTH_HASHVALUE_SIZE];
	uint64_t		sess_id;
	struct list_head	list_entry;
};

struct cifsd_session {
	uint64_t			id;

	struct cifsd_user		*user;
	struct cifsd_tcp_conn		*conn;
	unsigned int			sequence_number;
	unsigned int			flags;

	int				valid;
	bool				sign;
	bool				enc;
	bool				is_anonymous;
	bool				is_guest;

	int				state;
	__u8				*Preauth_HashValue;

	struct ntlmssp_auth		ntlmssp;
	char				sess_key[CIFS_KEY_SIZE];

	struct hlist_node		hlist;
	struct list_head		cifsd_chann_list;
	struct list_head		tree_conn_list;
	struct cifsd_ida		*tree_conn_ida;
	struct list_head		rpc_handle_list;

	/* should be under CONFIG_CIFS_SMB2_SERVER */
	struct fidtable_desc		fidtable;
	__u8				smb3encryptionkey[SMB3_SIGN_KEY_SIZE];
	__u8				smb3decryptionkey[SMB3_SIGN_KEY_SIZE];
	__u8				smb3signingkey[SMB3_SIGN_KEY_SIZE];

	/* REMOVE */
	struct list_head		cifsd_ses_list;
	struct list_head		cifsd_ses_global_list;
};

static inline int test_session_flag(struct cifsd_session *sess, int bit)
{
	return sess->flags & bit;
}

static inline void set_session_flag(struct cifsd_session *sess, int bit)
{
	sess->flags |= bit;
}

static inline void clear_session_flag(struct cifsd_session *sess, int bit)
{
	sess->flags &= ~bit;
}

struct cifsd_session *cifsd_smb1_session_create(void);
struct cifsd_session *cifsd_smb2_session_create(void);

void cifsd_session_destroy(struct cifsd_session *sess);

struct cifsd_session *cifsd_session_lookup(unsigned long long id);

int cifsd_acquire_tree_conn_id(struct cifsd_session *sess);
void cifsd_release_tree_conn_id(struct cifsd_session *sess, int id);

int cifsd_session_rpc_open(struct cifsd_session *sess, char *rpc_name);
void cifsd_session_rpc_close(struct cifsd_session *sess, int id);
int cifsd_session_rpc_method(struct cifsd_session *sess, int id);

int cifsd_init_session_table(void);
void cifsd_free_session_table(void);

#endif /* __USER_SESSION_MANAGEMENT_H__ */
