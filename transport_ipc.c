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

#include <linux/jhash.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/hashtable.h>
#include <net/net_namespace.h>
#include <net/genetlink.h>

#include "transport_ipc.h"
#include "buffer_pool.h"
#include "server.h"

#include "mgmt/user_config.h"
#include "mgmt/share_config.h"
#include "mgmt/user_session.h"
#include "mgmt/tree_connect.h"
#include "mgmt/cifsd_ida.h"

/* @FIXME fix this code */
extern int get_protocol_idx(char *str);

#define IPC_WAIT_TIMEOUT	(2 * HZ)

#define IPC_MSG_HASH_BITS	3
static DEFINE_HASHTABLE(ipc_msg_table, IPC_MSG_HASH_BITS);
static DECLARE_RWSEM(ipc_msg_table_lock);

struct cifsd_ida *ida;

static unsigned int cifsd_tools_pid;

#define CIFSD_IPC_MSG_HANDLE(m)	(*(unsigned int *)m)

#define CIFSD_INVALID_IPC_VERSION(m)					\
	({								\
		int ret = 0;						\
									\
		if (m->genlhdr->version != CIFSD_GENL_VERSION) {	\
			pr_err("IPC protocol version mismatch: %d\n",	\
				m->genlhdr->version);			\
			ret = 1;					\
		}							\
		ret;							\
	})

struct cifsd_ipc_msg {
	unsigned int		type;
	unsigned int		sz;
	unsigned char		____payload[0];
};

#define CIFSD_IPC_MSG_PAYLOAD(m)	\
	((void *)(m) + offsetof(struct cifsd_ipc_msg, ____payload))

struct ipc_msg_table_entry {
	unsigned int		handle;
	unsigned int		type;
	wait_queue_head_t	wait;
	struct hlist_node	ipc_table_hlist;

	void			*response;
};

static int handle_startup_event(struct sk_buff *skb, struct genl_info *info);
static int handle_unsupported_event(struct sk_buff *skb,
				    struct genl_info *info);
static int handle_generic_event(struct sk_buff *skb, struct genl_info *info);

static const struct nla_policy cifsd_nl_policy[CIFSD_EVENT_MAX] = {
	[CIFSD_EVENT_UNSPEC] = {
		.len = 0,
	},
	[CIFSD_EVENT_HEARTBEAT_REQUEST] = {
		.len = sizeof(struct cifsd_heartbeat),
	},
	[CIFSD_EVENT_HEARTBEAT_RESPONSE] = {
		.len = sizeof(struct cifsd_heartbeat),
	},
	[CIFSD_EVENT_STARTING_UP] = {
		.len = sizeof(struct cifsd_startup_request),
	},
	[CIFSD_EVENT_SHUTTING_DOWN] = {
		.len = sizeof(struct cifsd_shutdown_request),
	},
	[CIFSD_EVENT_LOGIN_REQUEST] = {
		.len = sizeof(struct cifsd_login_request),
	},
	[CIFSD_EVENT_LOGIN_RESPONSE] = {
		.len = sizeof(struct cifsd_login_response),
	},
	[CIFSD_EVENT_SHARE_CONFIG_REQUEST] = {
		.len = sizeof(struct cifsd_share_config_request),
	},
	[CIFSD_EVENT_SHARE_CONFIG_RESPONSE] = {
		.len = sizeof(struct cifsd_share_config_response),
	},
	[CIFSD_EVENT_TREE_CONNECT_REQUEST] = {
		.len = sizeof(struct cifsd_tree_connect_request),
	},
	[CIFSD_EVENT_TREE_CONNECT_RESPONSE] = {
		.len = sizeof(struct cifsd_tree_connect_response),
	},
	[CIFSD_EVENT_TREE_DISCONNECT_REQUEST] = {
		.len = sizeof(struct cifsd_tree_disconnect_request),
	},
	[CIFSD_EVENT_LOGOUT_REQUEST] = {
		.len = sizeof(struct cifsd_logout_request),
	},
	[CIFSD_EVENT_RPC_REQUEST] = {
	},
	[CIFSD_EVENT_RPC_RESPONSE] = {
	},
};

static const struct genl_ops cifsd_genl_ops[] = {
	{
		.cmd	= CIFSD_EVENT_UNSPEC,
		.doit	= handle_unsupported_event,
		.policy	= cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_HEARTBEAT_REQUEST,
		.doit	= handle_unsupported_event,
		.policy	= cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_HEARTBEAT_RESPONSE,
		.doit	= handle_generic_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_STARTING_UP,
		.doit	= handle_startup_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_SHUTTING_DOWN,
		.doit	= handle_unsupported_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_LOGIN_REQUEST,
		.doit	= handle_unsupported_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_LOGIN_RESPONSE,
		.doit	= handle_generic_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_SHARE_CONFIG_REQUEST,
		.doit	= handle_unsupported_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_SHARE_CONFIG_RESPONSE,
		.doit	= handle_generic_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_TREE_CONNECT_REQUEST,
		.doit	= handle_unsupported_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_TREE_CONNECT_RESPONSE,
		.doit	= handle_generic_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_TREE_DISCONNECT_REQUEST,
		.doit	= handle_unsupported_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_LOGOUT_REQUEST,
		.doit	= handle_unsupported_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_RPC_REQUEST,
		.doit	= handle_unsupported_event,
		.policy = cifsd_nl_policy,
	},
	{
		.cmd	= CIFSD_EVENT_RPC_RESPONSE,
		.doit	= handle_generic_event,
		.policy = cifsd_nl_policy,
	},
};

struct genl_family cifsd_genl_family = {
	.name		= CIFSD_GENL_NAME,
	.version	= CIFSD_GENL_VERSION,
	.hdrsize	= 0,
	.maxattr	= CIFSD_EVENT_MAX,
	.netnsok	= true,
	.module		= THIS_MODULE,
	.ops		= cifsd_genl_ops,
	.n_ops		= ARRAY_SIZE(cifsd_genl_ops),
};

static struct cifsd_ipc_msg *ipc_msg_alloc(size_t sz)
{
	struct cifsd_ipc_msg *msg;
	size_t msg_sz = sz + sizeof(struct cifsd_ipc_msg);

	msg = cifsd_alloc(msg_sz);
	if (msg)
		msg->sz = sz;
	return msg;
}

static void ipc_msg_free(struct cifsd_ipc_msg *msg)
{
	cifsd_free(msg);
}

static void ipc_msg_handle_free(int handle)
{
	if (handle >= 0)
		cifds_release_id(ida, handle);
}

static int handle_response(int type, void *payload, size_t sz)
{
	int handle = CIFSD_IPC_MSG_HANDLE(payload);
	struct ipc_msg_table_entry *entry;
	int ret = 0;

	down_read(&ipc_msg_table_lock);
	hash_for_each_possible(ipc_msg_table, entry, ipc_table_hlist, handle) {
		if (handle != entry->handle)
			continue;

		entry->response = NULL;
		/*
		 * Response message type value should be equal to
		 * request message type + 1.
		 */
		if (entry->type + 1 != type) {
			pr_err("Waiting for IPC type %d, got %d. Ignore.\n",
				entry->type + 1, type);
		}

		entry->response = cifsd_alloc(sz);
		if (!entry->response) {
			ret = -ENOMEM;
			break;
		}

		memcpy(entry->response, payload, sz);
		wake_up_interruptible(&entry->wait);
		ret = 0;
		break;
	}
	up_read(&ipc_msg_table_lock);

	return ret;
}

static int handle_startup_event(struct sk_buff *skb, struct genl_info *info)
{
	if (CIFSD_INVALID_IPC_VERSION(info))
		return -EINVAL;

	if (!info->attrs[CIFSD_EVENT_STARTING_UP])
		return -EINVAL;

	if (cifsd_tools_pid) {
		struct cifsd_heartbeat *beat;

		beat = cifsd_ipc_heartbeat_request();
		if (beat) {
			cifsd_free(beat);
			return -EINVAL;
		}

		pr_err("Reconnect to a new user space daemon");
	} else {
		struct cifsd_startup_request *req;
		int ret;

		req = nla_data(info->attrs[info->genlhdr->cmd]);

		server_conf.signing = req->signing;
		ret = cifsd_set_netbios_name(req->netbios_name);
		ret |= cifsd_set_server_string(req->server_string);
		ret |= cifsd_set_work_group(req->work_group);
		if (ret)
			return -EINVAL;

		if (req->min_prot[0]) {
			ret = get_protocol_idx(req->min_prot);
			if (ret >= 0)
				server_conf.min_protocol = ret;
		}
		if (req->max_prot[0]) {
			ret = get_protocol_idx(req->max_prot);
			if (ret >= 0)
				server_conf.max_protocol = ret;
		}
	}

	cifsd_tools_pid = info->snd_portid;
	cifsd_server_set_running();
	return 0;
}

static int handle_unsupported_event(struct sk_buff *skb,
				    struct genl_info *info)
{
	pr_err("Unknown IPC event: %d, ignore.\n", info->genlhdr->cmd);
	return -EINVAL;
}

static int handle_generic_event(struct sk_buff *skb, struct genl_info *info)
{
	void *payload;
	int sz;
	int type = info->genlhdr->cmd;

	if (type >= CIFSD_EVENT_MAX) {
		WARN_ON(1);
		return -EINVAL;
	}

	if (CIFSD_INVALID_IPC_VERSION(info))
		return -EINVAL;

	if (!info->attrs[type])
		return -EINVAL;

	payload = nla_data(info->attrs[info->genlhdr->cmd]);
	sz = nla_len(info->attrs[info->genlhdr->cmd]);
	return handle_response(type, payload, sz);
}

static int ipc_msg_send(struct cifsd_ipc_msg *msg)
{
	struct genlmsghdr *nlh;
	struct sk_buff *skb;
	int ret = -EINVAL;

	if (!cifsd_tools_pid)
		return ret;

	skb = genlmsg_new(msg->sz, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	nlh = genlmsg_put(skb, 0, 0, &cifsd_genl_family, 0, msg->type);
	if (!nlh)
		goto out;

	ret = nla_put(skb, msg->type, msg->sz, CIFSD_IPC_MSG_PAYLOAD(msg));
	if (ret) {
		genlmsg_cancel(skb, nlh);
		goto out;
	}

	genlmsg_end(skb, nlh);
	ret = genlmsg_unicast(&init_net, skb, cifsd_tools_pid);
	return ret;

out:
	nlmsg_free(skb);
	return ret;
}

static void *ipc_msg_send_request(struct cifsd_ipc_msg *msg,
				  unsigned int handle)
{
	struct ipc_msg_table_entry entry;
	int ret;

	if ((int)handle < 0)
		return NULL;

	entry.type = msg->type;
	entry.response = NULL;
	init_waitqueue_head(&entry.wait);

	down_write(&ipc_msg_table_lock);
	entry.handle = handle;
	hash_add(ipc_msg_table, &entry.ipc_table_hlist, entry.handle);
	up_write(&ipc_msg_table_lock);

	ret = ipc_msg_send(msg);
	if (ret)
		goto out;

	ret = wait_event_interruptible_timeout(entry.wait,
					       entry.response != NULL,
					       IPC_WAIT_TIMEOUT);
out:
	down_write(&ipc_msg_table_lock);
	hash_del(&entry.ipc_table_hlist);
	up_write(&ipc_msg_table_lock);
	return entry.response;
}

struct cifsd_login_response *cifsd_ipc_login_request(const char *account)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_login_request *req;
	struct cifsd_login_response *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_login_request));
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_LOGIN_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = cifds_acquire_id(ida);
	strncpy(req->account, account, sizeof(req->account) - 1);

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_handle_free(req->handle);
	ipc_msg_free(msg);
	return resp;
}

struct cifsd_tree_connect_response *
cifsd_ipc_tree_connect_request(struct cifsd_session *sess,
			       struct cifsd_share_config *share,
			       struct cifsd_tree_connect *tree_conn,
			       struct sockaddr *peer_addr)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_tree_connect_request *req;
	struct cifsd_tree_connect_response *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_tree_connect_request));
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_TREE_CONNECT_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);

	req->handle = cifds_acquire_id(ida);
	req->account_flags = sess->user->flags;
	req->session_id = sess->id;
	req->connect_id = tree_conn->id;
	strncpy(req->account, user_name(sess->user), sizeof(req->account) - 1);
	strncpy(req->share, share->name, sizeof(req->share) - 1);
	snprintf(req->peer_addr, sizeof(req->peer_addr), "%pIS", peer_addr);

	if (peer_addr->sa_family == AF_INET6)
		req->flags |= CIFSD_TREE_CONN_FLAG_REQUEST_IPV6;
	if (test_session_flag(sess, CIFDS_SESSION_FLAG_SMB2))
		req->flags |= CIFSD_TREE_CONN_FLAG_REQUEST_SMB2;

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_handle_free(req->handle);
	ipc_msg_free(msg);
	return resp;
}

int cifsd_ipc_tree_disconnect_request(unsigned long long session_id,
				      unsigned long long connect_id)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_tree_disconnect_request *req;
	int ret;

	msg = ipc_msg_alloc(sizeof(struct cifsd_tree_disconnect_request));
	if (!msg)
		return -ENOMEM;

	msg->type = CIFSD_EVENT_TREE_DISCONNECT_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->session_id = session_id;
	req->connect_id = connect_id;

	ret = ipc_msg_send(msg);
	ipc_msg_free(msg);
	return ret;
}

int cifsd_ipc_logout_request(const char *account)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_logout_request *req;
	int ret;

	msg = ipc_msg_alloc(sizeof(struct cifsd_logout_request));
	if (!msg)
		return -ENOMEM;

	msg->type = CIFSD_EVENT_LOGOUT_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	strcpy(req->account, account);

	ret = ipc_msg_send(msg);
	ipc_msg_free(msg);
	return ret;
}

struct cifsd_heartbeat *cifsd_ipc_heartbeat_request(void)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_heartbeat *out, *in;

	msg = ipc_msg_alloc(sizeof(struct cifsd_heartbeat));
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_HEARTBEAT_REQUEST;
	out = CIFSD_IPC_MSG_PAYLOAD(msg);
	out->handle = cifds_acquire_id(ida);

	in = ipc_msg_send_request(msg, out->handle);
	ipc_msg_handle_free(out->handle);
	ipc_msg_free(msg);
	return in;
}

struct cifsd_share_config_response *
cifsd_ipc_share_config_request(const char *name)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_share_config_request *req;
	struct cifsd_share_config_response *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_share_config_request));
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_SHARE_CONFIG_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = cifds_acquire_id(ida);
	strncpy(req->share_name, name, sizeof(req->share_name) - 1);

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_handle_free(req->handle);
	ipc_msg_free(msg);
	return resp;
}

struct cifsd_rpc_command *cifsd_rpc_open(struct cifsd_session *sess,
					 int handle)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_rpc_command *req;
	struct cifsd_rpc_command *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_rpc_command));
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_RPC_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = handle;
	req->flags = cifsd_session_rpc_method(sess, handle);
	req->flags |= CIFSD_RPC_OPEN_METHOD;
	req->payload_sz = 0;

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_free(msg);
	return resp;
}

struct cifsd_rpc_command *cifsd_rpc_close(struct cifsd_session *sess,
					  int handle)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_rpc_command *req;
	struct cifsd_rpc_command *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_rpc_command));
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_RPC_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = handle;
	req->flags = cifsd_session_rpc_method(sess, handle);
	req->flags |= CIFSD_RPC_CLOSE_METHOD;
	req->payload_sz = 0;

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_free(msg);
	return resp;
}

struct cifsd_rpc_command *cifsd_rpc_write(struct cifsd_session *sess,
					  int handle,
					  void *payload,
					  size_t payload_sz)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_rpc_command *req;
	struct cifsd_rpc_command *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_rpc_command) + payload_sz + 1);
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_RPC_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = handle;
	req->flags = cifsd_session_rpc_method(sess, handle);
	req->flags |= CIFSD_RPC_WRITE_METHOD;
	req->payload_sz = payload_sz;
	memcpy(req->payload, payload, payload_sz);

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_free(msg);
	return resp;
}

struct cifsd_rpc_command *cifsd_rpc_read(struct cifsd_session *sess,
					 int handle)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_rpc_command *req;
	struct cifsd_rpc_command *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_rpc_command));
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_RPC_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = handle;
	req->flags = cifsd_session_rpc_method(sess, handle);
	req->flags |= CIFSD_RPC_READ_METHOD;
	req->payload_sz = 0;

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_free(msg);
	return resp;
}

struct cifsd_rpc_command *cifsd_rpc_ioctl(struct cifsd_session *sess,
					  int handle,
					  void *payload,
					  size_t payload_sz)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_rpc_command *req;
	struct cifsd_rpc_command *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_rpc_command) + payload_sz + 1);
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_RPC_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = handle;
	req->flags = cifsd_session_rpc_method(sess, handle);
	req->flags |= CIFSD_RPC_IOCTL_METHOD;
	req->payload_sz = payload_sz;
	memcpy(req->payload, payload, payload_sz);

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_free(msg);
	return resp;
}

struct cifsd_rpc_command *cifsd_rpc_rap(struct cifsd_session *sess,
					void *payload,
					size_t payload_sz)
{
	struct cifsd_ipc_msg *msg;
	struct cifsd_rpc_command *req;
	struct cifsd_rpc_command *resp;

	msg = ipc_msg_alloc(sizeof(struct cifsd_rpc_command) + payload_sz + 1);
	if (!msg)
		return NULL;

	msg->type = CIFSD_EVENT_RPC_REQUEST;
	req = CIFSD_IPC_MSG_PAYLOAD(msg);
	req->handle = cifds_acquire_id(ida);
	req->flags = CIFSD_RPC_RAP_METHOD;
	req->payload_sz = payload_sz;
	memcpy(req->payload, payload, payload_sz);

	resp = ipc_msg_send_request(msg, req->handle);
	ipc_msg_handle_free(req->handle);
	ipc_msg_free(msg);
	return resp;
}

int cifsd_ipc_id_alloc(void)
{
	return cifds_acquire_id(ida);
}

void cifsd_rpc_id_free(int handle)
{
	cifds_release_id(ida, handle);
}

void cifsd_ipc_release(void)
{
	cifsd_ida_free(ida);
	genl_unregister_family(&cifsd_genl_family);
}

int cifsd_ipc_init(void)
{
	int ret = genl_register_family(&cifsd_genl_family);

	if (ret) {
		pr_err("Failed to register CIFSD netlink interface %d\n", ret);
		return ret;
	}

	ida = cifsd_ida_alloc();
	if (!ida)
		return -ENOMEM;
	return 0;
}
