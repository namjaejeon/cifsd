/*
 *   fs/cifsd/connect.c
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

#include "export.h"
#include "glob.h"
#include "smb1pdu.h"

struct task_struct *cifsd_forkerd;

static int deny_new_conn;
/**
 * kvec_array_init() - initialize a IO vector segment
 * @new:	IO vector to be intialized
 * @iov:	base IO vector
 * @nr_segs:	number of segments in base iov
 * @bytes:	total iovec length so far for read
 *
 * Return:	Number of IO segments
 */
static unsigned int kvec_array_init(struct kvec *new, struct kvec *iov,
				    unsigned int nr_segs, size_t bytes)
{
	size_t base = 0;

	while (bytes || !iov->iov_len) {
		int copy = min(bytes, iov->iov_len);

		bytes -= copy;
		base += copy;
		if (iov->iov_len == base) {
			iov++;
			nr_segs--;
			base = 0;
		}
	}

	memcpy(new, iov, sizeof(*iov) * nr_segs);
	new->iov_base += base;
	new->iov_len -= base;
	return nr_segs;
}

/**
 * get_conn_iovec() - get connection iovec for reading from socket
 * @conn:     TCP server instance of connection
 * @nr_segs:	number of segments in iov
 *
 * Return:	return existing or newly allocate iovec
 */
static struct kvec *get_conn_iovec(struct connection *conn,
				     unsigned int nr_segs)
{
	struct kvec *new_iov;

	if (conn->iov && nr_segs <= conn->nr_iov)
		return conn->iov;

	/* not big enough -- allocate a new one and release the old */
	new_iov = kmalloc(sizeof(*new_iov) * nr_segs, GFP_KERNEL);
	if (new_iov) {
		kfree(conn->iov);
		conn->iov = new_iov;
		conn->nr_iov = nr_segs;
	}
	return new_iov;
}

/**
 * conn_unresponsive() - check server is unresponsive or not
 * @conn:     TCP server instance of connection
 *
 * Return:	true if server unresponsive, otherwise  false
 */
bool conn_unresponsive(struct connection *conn)
{
	if (conn->stats.open_files_count > 0)
		return false;

#ifdef CONFIG_CIFS_SMB2_SERVER

	if (time_after(jiffies, conn->last_active + 2 * SMB_ECHO_INTERVAL)) {
		cifsd_debug("No response from client in 120 secs\n");
		return true;
	}
	return false;
#else
	return false;
#endif
}

/**
 * cifsd_readv_from_socket() - read data from socket in given iovec
 * @conn:     TCP server instance of connection
 * @iov_orig:	base IO vector
 * @nr_segs:	number of segments in base iov
 * @to_read:	number of bytes to read from socket
 *
 * Return:	on success return number of bytes read from socket,
 *		otherwise return error number
 */
int cifsd_readv_from_socket(struct connection *conn,
			      struct kvec *iov_orig, unsigned int nr_segs,
			      unsigned int to_read)
{
	int length = 0;
	int total_read;
	unsigned int segs;
	struct msghdr cifsd_msg;
	struct kvec *iov;

	iov = get_conn_iovec(conn, nr_segs);
	if (!iov)
		return -ENOMEM;

	cifsd_msg.msg_control = NULL;
	cifsd_msg.msg_controllen = 0;

	for (total_read = 0; to_read; total_read += length, to_read -= length) {
		try_to_freeze();

		if (conn_unresponsive(conn)) {
			total_read = -EAGAIN;
			break;
		}

		segs = kvec_array_init(iov, iov_orig, nr_segs, total_read);

		length = kernel_recvmsg(conn->sock, &cifsd_msg,
				iov, segs, to_read, 0);
		if (conn->tcp_status == CifsExiting) {
			total_read = -ESHUTDOWN;
			break;
		} else if (conn->tcp_status == CifsNeedReconnect) {
			total_read = -EAGAIN;
			break;
		} else if (length == -ERESTARTSYS ||
				length == -EAGAIN ||
				length == -EINTR) {
			usleep_range(1000, 2000);
			length = 0;
			continue;
		} else if (length <= 0) {
			usleep_range(1000, 2000);
			total_read = -EAGAIN;
			break;
		}
	}
	return total_read;
}

/**
 * cifsd_readv_from_socket() - read data from socket in given buffer
 * @conn:     TCP server instance of connection
 * @buf:	buffer to store read data from socket
 * @to_read:	number of bytes to read from socket
 *
 * Return:	on success return number of bytes read from socket,
 *		otherwise return error number
 */
int cifsd_read_from_socket(struct connection *conn, char *buf,
			     unsigned int to_read)
{
	struct kvec iov;

	iov.iov_base = buf;
	iov.iov_len = to_read;

	return cifsd_readv_from_socket(conn, &iov, 1, to_read);
}

/**
 * cifsd_create_socket - create socket for kcifsd/0
 *
 * Return:	Returns a task_struct or ERR_PTR
 */
int cifsd_create_socket(__u32 cifsd_pid)
{
	int ret;
	struct socket *socket = NULL;
	struct sockaddr_in sin;
	int opt = 1;
	struct cifsd_pid_info *cifsd_pid_info = NULL;

	ret = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &socket);
	if (ret)
		return ret;

	cifsd_debug("socket created\n");
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_family = PF_INET;
	sin.sin_port = htons(SMB_PORT);

	ret = kernel_setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
			(char *)&opt, sizeof(opt));
	if (ret < 0) {
		cifsd_err("failed to set socket options(%d)\n", ret);
		goto release;
	}

	ret = kernel_setsockopt(socket, SOL_TCP, TCP_NODELAY,
			(char *)&opt, sizeof(opt));
	if (ret < 0) {
		cifsd_err("set TCP_NODELAY socket option error %d\n", ret);
		goto release;
	}

	ret = kernel_bind(socket, (struct sockaddr *)&sin, sizeof(sin));
	if (ret) {
		cifsd_err("failed to bind socket err = %d\n", ret);
		goto release;
	}

	socket->sk->sk_rcvtimeo = 7 * HZ;
	socket->sk->sk_sndtimeo = 5 * HZ;

	ret = socket->ops->listen(socket, 64);
	if (ret) {
		cifsd_err("port listen failure(%d)\n", ret);
		goto release;
	}

	cifsd_pid_info = kmalloc(sizeof(struct cifsd_pid_info), GFP_KERNEL);
	if (!cifsd_pid_info)
		goto release;

	cifsd_pid_info->socket = socket;
	cifsd_pid_info->cifsd_pid = cifsd_pid;

	ret = cifsd_start_forker_thread(cifsd_pid_info);
	if (ret) {
		cifsd_err("failed to run forker thread(%d)\n", ret);
		goto release;
	}

	return 0;

release:
	cifsd_debug("releasing socket\n");
	ret = kernel_sock_shutdown(socket, SHUT_RDWR);
	if (ret)
		cifsd_err("failed to shutdown socket cleanly\n");

	sock_release(socket);

	return ret;
}

/**
 * cifsd_do_fork() - forker thread to listen new SMB connection
 * @p:		arguments to forker thread
 *
 * Return:	Returns a task_struct or ERR_PTR
 */
static int cifsd_do_fork(void *p)
{
	struct cifsd_pid_info *cifsd_pid_info = (struct cifsd_pid_info *)p;
	struct socket *socket = cifsd_pid_info->socket;
	__u32 cifsd_pid = cifsd_pid_info->cifsd_pid;
	struct task_struct *cifsd_task;
	int ret;
	struct socket *newsock = NULL;

	while (!kthread_should_stop()) {
		if (deny_new_conn)
			continue;

		rcu_read_lock();
		cifsd_task = pid_task(find_vpid(cifsd_pid), PIDTYPE_PID);
		rcu_read_unlock();
		if (cifsd_task) {
			if (strncmp(cifsd_task->comm, "cifsd", 5)) {
				cifsd_err("cifsd is not alive\n");
				break;
			}
		} else {
			cifsd_err("cifsd is not alive\n");
			break;
		}

		ret = kernel_accept(socket, &newsock, O_NONBLOCK);
		if (ret) {
			if (ret == -EAGAIN)
				/* check for new connections every 100 msecs */
				schedule_timeout_interruptible(HZ/10);
		} else {
			cifsd_debug("connect success: accepted new connection\n");
			newsock->sk->sk_rcvtimeo = 7 * HZ;
			newsock->sk->sk_sndtimeo = 5 * HZ;
			/* request for new connection */
			connect_tcp_sess(newsock);
		}
	}
	cifsd_debug("releasing socket\n");
	ret = kernel_sock_shutdown(socket, SHUT_RDWR);
	if (ret)
		cifsd_err("failed to shutdown socket cleanly\n");

	sock_release(socket);
	kfree(cifsd_pid_info);
	cifsd_forkerd = NULL;

	return 0;
}

void terminate_old_forker_thread(void)
{
	if (cifsd_forkerd)
		cifsd_stop_forker_thread();
}

/**
 * cifsd_start_forker_thread() - start forker thread
 *
 * start forker thread(kcifsd/0) at module init time to listen
 * on port 445 for new SMB connection requests. It creates per connection
 * server threads(kcifsd/x)
 *
 * @cifsd_pid_info:	struct pointer which has cifsd's pid and
 *	socket pointer members
 * Return:	0 on success or error number
 */
int cifsd_start_forker_thread(struct cifsd_pid_info *cifsd_pid_info)
{
	int rc;

	deny_new_conn = 0;
	cifsd_forkerd = kthread_run(cifsd_do_fork, cifsd_pid_info, "kcifsd/0");
	if (IS_ERR(cifsd_forkerd)) {
		rc = PTR_ERR(cifsd_forkerd);
		cifsd_forkerd = NULL;
		return rc;
	}

	return 0;
}

/**
 * cifsd_stop_forker_thread() - stop forker thread
 *
 * stop forker thread(cifsd_forkerd) at module exit time
 */
void cifsd_stop_forker_thread(void)
{
	int ret;

	if (cifsd_forkerd) {
		ret = kthread_stop(cifsd_forkerd);
		if (ret)
			cifsd_err("failed to stop forker thread\n");
	}

	cifsd_forkerd = NULL;
}

void cifsd_close_socket(void)
{
	int ret;

	cifsd_debug("closing SMB PORT and releasing socket\n");
	deny_new_conn = 1;
	ret = cifsd_stop_tcp_sess();
	if (!ret) {
		cifsd_stop_forker_thread();
		cifsd_debug("SMB PORT closed\n");
	}
}
