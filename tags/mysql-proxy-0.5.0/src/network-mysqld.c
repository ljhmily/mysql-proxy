/* Copyright (C) 2007 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */ 

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>

#ifdef HAVE_SYS_FILIO_H
/**
 * required for FIONREAD on solaris
 */
#include <sys/filio.h>
#endif

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <netdb.h>

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include <glib.h>

#include <mysql.h>

#include "network-mysqld.h"

#ifndef _WIN32
/**
 * use closesocket() to close sockets to be compatible with win32
 */
#define closesocket(x) close(x)
#endif

#ifdef _WIN32
extern volatile int agent_shutdown;
#else
extern volatile sig_atomic_t agent_shutdown;
#endif

#define C(x) x, sizeof(x) - 1

gboolean g_hash_table_true(gpointer UNUSED_PARAM(key), gpointer UNUSED_PARAM(value), gpointer UNUSED_PARAM(u)) {
	return TRUE;
}	

void g_list_string_free(gpointer data, gpointer UNUSED_PARAM(user_data)) {
	g_string_free(data, TRUE);
}

network_mysqld_index_status *network_mysqld_index_status_init() {
	network_mysqld_index_status *s;

	s = g_new0(network_mysqld_index_status, 1);

	return s;
}

void network_mysqld_index_status_free(network_mysqld_index_status *s) {
	if (!s) return;

	g_free(s);
}

retval_t plugin_call_cleanup(network_mysqld *srv, network_mysqld_con *con) {
	NETWORK_MYSQLD_PLUGIN_FUNC(func) = NULL;

	func = con->plugins.con_cleanup;
	
	if (!func) return RET_SUCCESS;

	return (*func)(srv, con);
}

network_queue *network_queue_init() {
	network_queue *queue;

	queue = g_new0(network_queue, 1);

	queue->chunks = g_queue_new();
	
	return queue;
}

void network_queue_free(network_queue *queue) {
	GString *packet;

	if (!queue) return;

	while ((packet = g_queue_pop_head(queue->chunks))) g_string_free(packet, TRUE);

	g_queue_free(queue->chunks);

	g_free(queue);
}

network_socket *network_socket_init() {
	network_socket *s;
	
	s = g_new0(network_socket, 1);

	s->send_queue = network_queue_init();
	s->recv_queue = network_queue_init();
	s->recv_raw_queue = network_queue_init();

	s->mysqld_version = 50112;

	s->packet_len = PACKET_LEN_UNSET;
	
	return s;
}

void network_socket_free(network_socket *s) {
	if (!s) return;

	network_queue_free(s->send_queue);
	network_queue_free(s->recv_queue);
	network_queue_free(s->recv_raw_queue);
#if 0
	/* */
	if (s->addr.str) {
		g_free(s->addr.str);
	}
#endif

	if (s->fd != -1) {
		closesocket(s->fd);
	}

	g_free(s);
}


network_mysqld_con *network_mysqld_con_init(network_mysqld *srv) {
	network_mysqld_con *con;

	con = g_new0(network_mysqld_con, 1);

	con->default_db = g_string_new(NULL);
	con->srv = srv;

	g_ptr_array_add(srv->cons, con);

	return con;
}

void network_mysqld_con_free(network_mysqld_con *con) {
	if (!con) return;

	g_string_free(con->default_db, TRUE);

	if (con->filename) {
		g_free(con->filename);
	}

	if (con->server) network_socket_free(con->server);
	if (con->client) network_socket_free(con->client);

	/* we are still in the conns-array */

	g_ptr_array_remove_fast(con->srv->cons, con);

	g_free(con);
}



/**
 * the free functions used by g_hash_table_new_full()
 */
static void network_mysqld_index_status_free_void(void *s) {
	network_mysqld_index_status_free(s);
}

static void network_mysqld_tables_free_void(void *s) {
	network_mysqld_table_free(s);
}

network_mysqld *network_mysqld_init() {
	network_mysqld *m;

	m = g_new0(network_mysqld, 1);

	m->event_base  = event_init();

	m->index_usage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, network_mysqld_index_status_free_void);
	m->tables      = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, network_mysqld_tables_free_void);
	
	m->cons        = g_ptr_array_new();
	
	return m;
}

void network_mysqld_free(network_mysqld *m) {
	guint i;

	if (!m) return;

	for (i = 0; i < m->cons->len; i++) {
		network_mysqld_con *con = m->cons->pdata[i];

		plugin_call_cleanup(m, con);
		network_mysqld_con_free(con);
	}

	g_ptr_array_free(m->cons, TRUE);

	g_hash_table_destroy(m->tables);
	g_hash_table_destroy(m->index_usage);

	if (m->config.proxy.backend_addresses) {
		for (i = 0; m->config.proxy.backend_addresses[i]; i++) {
			g_free(m->config.proxy.backend_addresses[i]);
		}
		g_free(m->config.proxy.backend_addresses);
	}

	if (m->config.proxy.address) g_free(m->config.proxy.address);
	if (m->config.admin.address) g_free(m->config.admin.address);
#if 0
	/* only recent versions have this call */
	event_base_free(m->event_base);
#endif
	
	g_free(m);
}



/**
 * connect to the proxy backend */
int network_mysqld_con_set_address(network_address *addr, gchar *address) {
	gchar *s;
	uint   port;

	/* split the address:port */
	if (NULL != (s = strchr(address, ':'))) {
		port = strtoul(s + 1, NULL, 10);

		if (port == 0) {
			g_critical("<ip>:<port>, port is invalid or 0, has to be > 0, got '%s'", address);
			return -1;
		}
		if (port > 65535) {
			g_critical("<ip>:<port>, port is too large, has to be < 65536, got '%s'", address);

			return -1;
		}

		memset(&addr->addr.ipv4, 0, sizeof(struct sockaddr_in));

		if (address == s || 
		    0 == strcmp("0.0.0.0", address)) {
			/* no ip */
			addr->addr.ipv4.sin_addr.s_addr = htonl(INADDR_ANY);
		} else {
			struct hostent *he;

			*s = '\0';
			he = gethostbyname(address);
			*s = ':';

			if (NULL == he)  {
				g_error("resolving proxy-address '%s' failed: ", address);
			}

			g_assert(he->h_addrtype == AF_INET);
			g_assert(he->h_length == sizeof(struct in_addr));

			memcpy(&(addr->addr.ipv4.sin_addr.s_addr), he->h_addr_list[0], he->h_length);
		}

		addr->addr.ipv4.sin_family = AF_INET;
		addr->addr.ipv4.sin_port = htons(port);
		addr->len = sizeof(struct sockaddr_in);

		addr->str = g_strdup(address);
	} else {
		g_critical("address has to contain a <ip>:<port>, got '%s'", address);
		return -1;
	}

	return 0;
}

int network_mysqld_con_connect(network_mysqld *UNUSED_PARAM(srv), network_socket * con) {
	int val = 1;

	g_assert(con->addr.len);

	if (-1 == (con->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))) {
		g_critical("socket() failed");
		return -1;
	}

	setsockopt(con->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val) );

	if (-1 == connect(con->fd, (struct sockaddr *) &(con->addr.addr), con->addr.len)) {
		g_critical("%s.%d: connect(%s) failed: %s", 
				__FILE__, __LINE__,
				con->addr.str,
				strerror(errno));
		return -1;
	}

	return 0;
}

int network_mysqld_con_bind(network_mysqld *UNUSED_PARAM(srv), network_socket * con) {
	int val = 1;

	g_assert(con->addr.len);

	if (-1 == (con->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP))) {
		g_critical("socket() failed");
		return -1;
	}

	setsockopt(con->fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
	setsockopt(con->fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

	if (-1 == bind(con->fd, (struct sockaddr *) &(con->addr.addr), con->addr.len)) {
		g_critical("%s.%d: bind(%s) failed: %s", 
				__FILE__, __LINE__,
				con->addr.str,
				strerror(errno));
		return -1;
	}

	if (-1 == listen(con->fd, 8)) {
		g_critical("%s.%d: listen() failed: %s",
				__FILE__, __LINE__,
				strerror(errno));
		return -1;
	}

	return 0;
}


static void dump_str(const char *msg, const unsigned char *s, size_t len) {
	GString *hex;
	size_t i;
		
       	hex = g_string_new(NULL);

	for (i = 0; i < len; i++) {
		g_string_append_printf(hex, "%02x", s[i]);

		if ((i + 1) % 16 == 0) {
			g_string_append(hex, "\n");
		} else {
			g_string_append_c(hex, ' ');
		}

	}

	g_message("(%s): %s", msg, hex->str);

	g_string_free(hex, TRUE);

}

int network_mysqld_packet_set_header(unsigned char *header, size_t len, unsigned char id) {
	g_assert(len <= PACKET_LEN_MAX);

	header[0] = (len >>  0) & 0xFF;
	header[1] = (len >>  8) & 0xFF;
	header[2] = (len >> 16) & 0xFF;
	header[3] = id;

	return 0;
}

size_t network_mysqld_packet_get_header(unsigned char *header) {
	return header[0] | header[1] << 8 | header[2] << 16;
}

int network_queue_append(network_queue *queue, const char *data, size_t len, int packet_id) {
	unsigned char header[4];
	GString *s;

	network_mysqld_packet_set_header(header, len, packet_id);

	s = g_string_sized_new(len + 4);

	g_string_append_len(s, (gchar *)header, 4);
	g_string_append_len(s, data, len);

	g_queue_push_tail(queue->chunks, s);

	return 0;
}

int network_queue_append_chunk(network_queue *queue, GString *chunk) {
	g_queue_push_tail(queue->chunks, chunk);

	return 0;
}

int network_mysqld_con_send_ok(network_socket *con) {
	const unsigned char packet_ok[] = 
		"\x00" /* field-count */
		"\x00" /* affected rows */
		"\x00" /* insert-id */
		"\x02\x00" /* server-status */
		"\x00\x00" /* warnings */
		;

	network_queue_append(con->send_queue, (gchar *)packet_ok, (sizeof(packet_ok) - 1), con->packet_id);

	return 0;
}

int network_mysqld_con_send_error(network_socket *con, const char *errmsg, gsize errmsg_len) {
	GString *packet;
	
	packet = g_string_sized_new(10 + errmsg_len);
	
	g_string_append_len(packet, 
			C("\xff"     /* type: error */
			  "\x00\x00" /* errno */
			  "#"        /* insert-id */
			  "00S00"    /* SQLSTATE */
			 ));

	if (errmsg_len < 250) {
		g_string_append_c(packet, (guchar)errmsg_len);
		g_string_append_len(packet, errmsg, errmsg_len);
	} else {
		g_string_append_c(packet, 0);
	}

	network_queue_append(con->send_queue, packet->str, packet->len, con->packet_id);

	g_string_free(packet, TRUE);

	return 0;
}


retval_t network_mysqld_read_raw(network_mysqld *UNUSED_PARAM(srv), network_socket *con, char *dest, size_t we_want) {
	GList *chunk;
	ssize_t len;
	network_queue *queue = con->recv_raw_queue;
	size_t we_have;

	/**
	 * 1. we read all we can get into a local buffer,
	 * 2. we split it into header + data
	 * 
	 * */

	if (con->to_read) {
		GString *s;

		s = g_string_sized_new(con->to_read + 1);

		if (-1 == (len = read(con->fd, s->str, s->allocated_len - 1))) {
			g_string_free(s, TRUE);
			switch (errno) {
			case EAGAIN:
				return RET_WAIT_FOR_EVENT;
			default:
				return RET_ERROR;
			}
		} else if (len == 0) {
			g_string_free(s, TRUE);
			return RET_ERROR;
		}

		s->len = len;
		s->str[s->len] = '\0';
		if (len > con->to_read) {
			/* between ioctl() and read() might be a cap and we might read more than we expected */
			con->to_read = 0;
		} else {
			con->to_read -= len;
		}

		network_queue_append_chunk(queue, s);
	}


	/* check if we have enough data */
	for (chunk = queue->chunks->head, we_have = 0; chunk; chunk = chunk->next) {
		GString *s = chunk->data;

		if (chunk == queue->chunks->head) {
			g_assert(queue->offset < s->len);
		
			we_have += (s->len - queue->offset);
		} else {
			we_have += s->len;
		}

		if (we_have >= we_want) break;
	}

	if (we_have < we_want) {
		/* we don't have enough */

		return RET_WAIT_FOR_EVENT;
	}

	for (chunk = queue->chunks->head, we_have = 0; chunk && we_want; ) {
		GString *s = chunk->data;

		size_t chunk_has = s->len - queue->offset;
		size_t to_read   = we_want > chunk_has ? chunk_has : we_want;
				
		memcpy(dest + we_have, s->str + queue->offset, to_read);
		we_have += to_read;
		we_want -= to_read;

		queue->offset += to_read;

		if (queue->offset == s->len) {
			/* this chunk is empty now */
			g_string_free(s, TRUE);

			g_queue_delete_link(queue->chunks, chunk);
			queue->offset = 0;

			chunk = queue->chunks->head;
		}
	}

	return RET_SUCCESS;
}


retval_t network_mysqld_read(network_mysqld *srv, network_socket *con) {
	GString *packet = NULL;

	if (con->packet_len == PACKET_LEN_UNSET) {
		switch (network_mysqld_read_raw(srv, con, (gchar *)con->header, NET_HEADER_SIZE)) {
		case RET_WAIT_FOR_EVENT:
			return RET_WAIT_FOR_EVENT;
		case RET_ERROR:
			return RET_ERROR;
		case RET_SUCCESS:
			break;
		case RET_ERROR_RETRY:
			g_error("RET_ERROR_RETRY wasn't expected");
			break;
		}

		con->packet_len = network_mysqld_packet_get_header(con->header);
		con->packet_id  = con->header[3]; /* packet-id if the next packet */

		packet = g_string_sized_new(con->packet_len + NET_HEADER_SIZE);
		g_string_append_len(packet, (gchar *)con->header, NET_HEADER_SIZE); /* copy the header */

		network_queue_append_chunk(con->recv_queue, packet);
	} else {
		packet = con->recv_queue->chunks->tail->data;
	}

	g_assert(packet->allocated_len >= con->packet_len + NET_HEADER_SIZE);

	switch (network_mysqld_read_raw(srv, con, packet->str + NET_HEADER_SIZE, con->packet_len)) {
	case RET_WAIT_FOR_EVENT:
		return RET_WAIT_FOR_EVENT;
	case RET_ERROR:
		return RET_ERROR;
	case RET_SUCCESS:
		break;
	case RET_ERROR_RETRY:
		g_error("RET_ERROR_RETRY wasn't expected");
		break;

	}

	packet->len += con->packet_len;

	return RET_SUCCESS;
}

retval_t network_mysqld_write_len(network_mysqld *UNUSED_PARAM(srv), network_socket *con, int send_chunks) {
	/* send the whole queue */
	GList *chunk;

	if (send_chunks == 0) return RET_SUCCESS;

	for (chunk = con->send_queue->chunks->head; chunk; ) {
		GString *s = chunk->data;
		ssize_t len;

		g_assert(con->send_queue->offset < s->len);

		if (-1 == (len = write(con->fd, s->str + con->send_queue->offset, s->len - con->send_queue->offset))) {
			switch (errno) {
			case EAGAIN:
				return RET_WAIT_FOR_EVENT;
			default:
				return RET_ERROR;
			}
		} else if (len == 0) {
			return RET_ERROR;
		}

		con->send_queue->offset += len;

		if (con->send_queue->offset == s->len) {
			g_string_free(s, TRUE);
			
			g_queue_delete_link(con->send_queue->chunks, chunk);
			con->send_queue->offset = 0;

			if (send_chunks > 0 && --send_chunks == 0) break;

			chunk = con->send_queue->chunks->head;
		} else {
			return RET_WAIT_FOR_EVENT;
		}
	}

	return RET_SUCCESS;
}

retval_t network_mysqld_write(network_mysqld *srv, network_socket *con) {
	retval_t ret;
	int corked;

#ifdef TCP_CORK
	corked = 1;
	setsockopt(con->fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
#endif
	ret = network_mysqld_write_len(srv, con, -1);
#ifdef TCP_CORK
	corked = 0;
	setsockopt(con->fd, IPPROTO_TCP, TCP_CORK, &corked, sizeof(corked));
#endif

	return ret;
}

int g_string_lenenc_append_len(GString *dest, const char *s, size_t len) {
	g_string_append_c(dest, len);
	if (len) g_string_append_len(dest, s, len);

	return 0;
}

int g_string_lenenc_append(GString *dest, const char *s) {
	return g_string_lenenc_append_len(dest, s, s ? strlen(s) : 0);
}

/**
 * call the hooks of the plugins for each state
 *
 * if the plugin doesn't implement a hook, we provide a default operation
 */
retval_t plugin_call(network_mysqld *srv, network_mysqld_con *con, int state) {
	NETWORK_MYSQLD_PLUGIN_FUNC(func) = NULL;

	switch (state) {
	case CON_STATE_INIT:
		func = con->plugins.con_init;

		if (!func) { /* default implementation */
			con->state = CON_STATE_CONNECT_SERVER;
		}
		break;
	case CON_STATE_CONNECT_SERVER:
		func = con->plugins.con_connect_server;

		if (!func) { /* default implementation */
			con->state = CON_STATE_READ_HANDSHAKE;
		}

		break;

	case CON_STATE_SEND_HANDSHAKE:
		func = con->plugins.con_send_handshake;

		if (!func) { /* default implementation */
			con->state = CON_STATE_READ_AUTH;
		}

		break;
	case CON_STATE_READ_HANDSHAKE:
		func = con->plugins.con_read_handshake;

		break;
	case CON_STATE_READ_AUTH:
		func = con->plugins.con_read_auth;

		break;
	case CON_STATE_SEND_AUTH:
		func = con->plugins.con_send_auth;

		if (!func) { /* default implementation */
			con->state = CON_STATE_READ_AUTH_RESULT;
		}
		break;
	case CON_STATE_READ_AUTH_RESULT:
		func = con->plugins.con_read_auth_result;
		break;
	case CON_STATE_SEND_AUTH_RESULT:
		func = con->plugins.con_send_auth_result;

		if (!func) { /* default implementation */
			con->state = CON_STATE_READ_QUERY;
		}
		break;
	case CON_STATE_READ_QUERY:
		func = con->plugins.con_read_query;
		break;
	case CON_STATE_READ_QUERY_RESULT:
		func = con->plugins.con_read_query_result;
		break;
	case CON_STATE_SEND_QUERY_RESULT:
		func = con->plugins.con_send_query_result;

		if (!func) { /* default implementation */
			con->state = CON_STATE_READ_QUERY;
		}
		break;
	default:
		g_error("%s.%d: unhandled state: %d", 
				__FILE__, __LINE__,
				state);
	}
	if (!func) return RET_SUCCESS;

	return (*func)(srv, con);
}

/**
 * handle the different states of the MySQL protocol
 */
void network_mysqld_con_handle(int event_fd, short events, void *user_data) {
	guint ostate;
	network_mysqld_con *con = user_data;
	network_mysqld *srv = con->srv;

	g_assert(srv);
	g_assert(con);

	if (events == EV_READ) {
		int b = -1;

		if (ioctl(event_fd, FIONREAD, &b)) {
			g_critical("ioctl(%d, FIONREAD, ...) failed: %s", event_fd, strerror(errno));

			con->state = CON_STATE_ERROR;
		} else if (b != 0) {
			if (con->client && event_fd == con->client->fd) {
				con->client->to_read = b;
			} else if (con->server && event_fd == con->server->fd) {
				con->server->to_read = b;
			} else {
				g_error("%s.%d: neither nor", __FILE__, __LINE__);
			}
		} else {
			con->state = CON_STATE_ERROR;
		}
	}

#define WAIT_FOR_EVENT(ev_struct, ev_type, timeout) \
	event_set(&(ev_struct->event), ev_struct->fd, ev_type, network_mysqld_con_handle, user_data); \
	event_base_set(srv->event_base, &(ev_struct->event));\
	event_add(&(ev_struct->event), timeout);

	do {
		ostate = con->state;
		switch (con->state) {
		case CON_STATE_ERROR:
			/* we can't go on, close the connection */
			plugin_call_cleanup(srv, con);
			network_mysqld_con_free(con);

			con = NULL;

			return;
		case CON_STATE_INIT:
			/* if we are a proxy ask the remote server for the hand-shake packet 
			 * if not, we generate one */

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				/**
				 * no luck, let's close the connection
				 */
				g_critical("%s.%d: plugin_call(CON_STATE_INIT) != RET_SUCCESS", __FILE__, __LINE__);

				con->state = CON_STATE_ERROR;
				
				break;
			}

			break;
		case CON_STATE_CONNECT_SERVER:
			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			case RET_ERROR_RETRY:
				/* hack to force a retry */
				ostate = CON_STATE_INIT;

				break;
			case RET_ERROR:
				g_error("%s.%d: plugin_call(CON_STATE_CONNECT_SERVER) returned an error", __FILE__, __LINE__);
				break;
			default:
				g_error("%s.%d: ...", __FILE__, __LINE__);
				break;
			}

			g_assert(con->server);

			break;
		case CON_STATE_READ_HANDSHAKE: {
			/**
			 * read auth data from the remote mysql-server 
			 */
			network_socket *recv_sock;

			recv_sock = con->server;
			g_assert(events == 0 || event_fd == recv_sock->fd);

			switch (network_mysqld_read(srv, recv_sock)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				/* call us again when you have a event */
				WAIT_FOR_EVENT(con->server, EV_READ, NULL);

				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: plugin_call(CON_STATE_CONNECT_SERVER) returned an error", __FILE__, __LINE__);
				break;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_error("%s.%d: ...", __FILE__, __LINE__);
				break;
			}
	
			break; }
		case CON_STATE_SEND_HANDSHAKE: 
			/* send the hand-shake to the client and wait for a response */
			
			switch (network_mysqld_write(srv, con->client)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->client, EV_WRITE, NULL);
				
				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: network_mysqld_write(CON_STATE_SEND_HANDSHAKE) returned an error", __FILE__, __LINE__);
				break;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_error("%s.%d: plugin_call(CON_STATE_SEND_HANDSHAKE) != RET_SUCCESS", __FILE__, __LINE__);
				break;
			}

			break;
		case CON_STATE_READ_AUTH: {
			/* read auth from client */
			network_socket *recv_sock;

			recv_sock = con->client;

			g_assert(events == 0 || event_fd == recv_sock->fd);

			switch (network_mysqld_read(srv, recv_sock)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->client, EV_READ, NULL);

				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: network_mysqld_read(CON_STATE_READ_AUTH) returned an error", __FILE__, __LINE__);
				return;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_error("%s.%d: plugin_call(CON_STATE_READ_AUTH) != RET_SUCCESS", __FILE__, __LINE__);
				break;
			}

			break; }
		case CON_STATE_SEND_AUTH:
			/* send the auth-response to the server */
			switch (network_mysqld_write(srv, con->server)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->server, EV_WRITE, NULL);

				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				/* might be a connection close, we should just close the connection and be happy */
				g_error("%s.%d: network_mysqld_write(CON_STATE_SEND_AUTH) returned an error", __FILE__, __LINE__);
				return;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_error("%s.%d: plugin_call(CON_STATE_SEND_AUTH) != RET_SUCCESS", __FILE__, __LINE__);
				break;
			}

			break;
		case CON_STATE_READ_AUTH_RESULT: {
			/* read the auth result from the server */
			network_socket *recv_sock;

			recv_sock = con->server;

			g_assert(events == 0 || event_fd == recv_sock->fd);

			switch (network_mysqld_read(srv, recv_sock)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->server, EV_READ, NULL);
				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: network_mysqld_read(CON_STATE_READ_AUTH_RESULT) returned an error", __FILE__, __LINE__);
				return;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_critical("%s.%d: plugin_call(CON_STATE_READ_AUTH_RESULT) != RET_SUCCESS", __FILE__, __LINE__);

				con->state = CON_STATE_ERROR;
				break;
			}

			break; }
		case CON_STATE_SEND_AUTH_RESULT: {
			/* send the hand-shake to the client and wait for a response */

			switch (network_mysqld_write(srv, con->client)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->client, EV_WRITE, NULL);
				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: network_mysqld_write(CON_STATE_SEND_AUTH_RESULT) returned an error", __FILE__, __LINE__);
				return;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_error("%s.%d: ...", __FILE__, __LINE__);
				break;
			}
				
			break; }
		case CON_STATE_READ_QUERY: {
			network_socket *recv_sock;

			recv_sock = con->client;

			g_assert(events == 0 || event_fd == recv_sock->fd);

			switch (network_mysqld_read(srv, recv_sock)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->client, EV_READ, NULL);
				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: network_mysqld_read(CON_STATE_READ_QUERY) returned an error", __FILE__, __LINE__);
				return;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_error("%s.%d: ...", __FILE__, __LINE__);
				break;
			}

			break; }
		case CON_STATE_SEND_QUERY: 
			/* send the query to the server */

			if (con->server->send_queue->offset == 0) {
				/* only parse the packets once */
				GString *s;
				GList *chunk;

				chunk = con->server->send_queue->chunks->head;
				s = chunk->data;

				/* only parse once and don't care about the blocking read */
				if (con->parse.command == COM_QUERY &&
				    con->parse.state.query == PARSE_COM_QUERY_LOAD_DATA) {
					/* is this a LOAD DATA INFILE ... extra round ? */
					/* this isn't a command packet, but a LOAD DATA INFILE data-packet */
					if (s->str[0] == 0 && s->str[1] == 0 && s->str[2] == 0) {
						con->parse.state.query = PARSE_COM_QUERY_LOAD_DATA_END_DATA;
					}
				} else if (con->is_overlong_packet) {
					/* the last packet was a over-long packet
					 * this is the same command, just more data */
	
					if (con->parse.len != PACKET_LEN_MAX) {
						con->is_overlong_packet = 0;
					}
	
				} else {
					con->parse.command = s->str[4];
	
					if (con->parse.len == PACKET_LEN_MAX) {
						con->is_overlong_packet = 1;
					}
		
					/* init the parser for the commands */
					switch (con->parse.command) {
					case COM_QUERY:
					case COM_STMT_EXECUTE:
						con->parse.state.query = PARSE_COM_QUERY_INIT;
						break;
					case COM_STMT_PREPARE:
						con->parse.state.prepare.first_packet = 1;
						break;
					default:
						break;
					}
				}
			}
	
			switch (network_mysqld_write_len(srv, con->server, 1)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->server, EV_WRITE, NULL);
				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: network_mysqld_write(CON_STATE_SEND_QUERY) returned an error", __FILE__, __LINE__);
				return;
			}

			if (con->is_overlong_packet) {
				con->state = CON_STATE_READ_QUERY;
				break;
			}

			/* some statements don't have a server response */
			switch (con->parse.command) {
			case COM_STMT_SEND_LONG_DATA: /* not acked */
			case COM_STMT_CLOSE:
				con->state = CON_STATE_READ_QUERY;
				break;
			case COM_QUERY:
				if (con->parse.state.query == PARSE_COM_QUERY_LOAD_DATA) {
					con->state = CON_STATE_READ_QUERY;
				} else {
					con->state = CON_STATE_READ_QUERY_RESULT;
				}
				break;
			default:
				con->state = CON_STATE_READ_QUERY_RESULT;
				break;
			}
				
			break; 
		case CON_STATE_READ_QUERY_RESULT: 
			do {
				network_socket *recv_sock;

				recv_sock = con->server;

				g_assert(events == 0 || event_fd == recv_sock->fd);

				switch (network_mysqld_read(srv, recv_sock)) {
				case RET_SUCCESS:
					break;
				case RET_WAIT_FOR_EVENT:
					WAIT_FOR_EVENT(con->server, EV_READ, NULL);
					return;
				case RET_ERROR_RETRY:
				case RET_ERROR:
					g_error("%s.%d: network_mysqld_read(CON_STATE_READ_QUERY_RESULT) returned an error", __FILE__, __LINE__);
					return;
				}

				switch (plugin_call(srv, con, con->state)) {
				case RET_SUCCESS:
					break;
				default:
					g_error("%s.%d: ...", __FILE__, __LINE__);
					break;
				}

			} while (con->state == CON_STATE_READ_QUERY_RESULT);
	
			break; 
		case CON_STATE_SEND_QUERY_RESULT:
			/**
			 * send the query result-set to the client */

			switch (network_mysqld_write(srv, con->client)) {
			case RET_SUCCESS:
				break;
			case RET_WAIT_FOR_EVENT:
				WAIT_FOR_EVENT(con->client, EV_WRITE, NULL);
				return;
			case RET_ERROR_RETRY:
			case RET_ERROR:
				g_error("%s.%d: network_mysqld_write(CON_STATE_SEND_QUERY_RESULT) returned an error", __FILE__, __LINE__);
				return;
			}

			switch (plugin_call(srv, con, con->state)) {
			case RET_SUCCESS:
				break;
			default:
				g_error("%s.%d: ...", __FILE__, __LINE__);
				break;
			}

			break;
		}

		event_fd = -1;
		events   = 0;
	} while (ostate != con->state);

	return;
}

/**
 * we will be called by the event handler 
 *
 *
 */
void network_mysqld_con_accept(int event_fd, short events, void *user_data) {
	network_mysqld_con *con = user_data;
	network_mysqld_con *client_con;
	socklen_t addr_len;
	struct sockaddr_in ipv4;
	int fd;


	g_assert(events == EV_READ);
	g_assert(con->server);

	addr_len = sizeof(struct sockaddr_in);

	if (-1 == (fd = accept(event_fd, (struct sockaddr *)&ipv4, &addr_len))) {
		return ;
	}

	fcntl(fd, F_SETFL, O_NONBLOCK | O_RDWR);

	/* looks like we open a client connection */
	client_con = network_mysqld_con_init(con->srv);
	client_con->client = network_socket_init();
	client_con->client->addr.addr.ipv4 = ipv4;
	client_con->client->addr.len = addr_len;
	client_con->client->fd   = fd;

	/* copy the config */
	client_con->config = con->config;
	client_con->config.network_type = con->config.network_type;

	switch (con->config.network_type) {
	case NETWORK_TYPE_SERVER:
		network_mysqld_server_connection_init(NULL, client_con);
		break;
	case NETWORK_TYPE_PROXY:
		network_mysqld_proxy_connection_init(NULL, client_con);
		break;
	default:
		g_error("%s.%d", __FILE__, __LINE__);
		break;
	}

	network_mysqld_con_handle(-1, 0, client_con);

	return;
}


void handle_timeout() {
	if (!agent_shutdown) return;

	/* we have to shutdown, disable all events to leave the dispatch */
}

void *network_mysqld_thread(void *_srv) {
	network_mysqld *srv = _srv;

	/* create the connection array */


	/* setup the different handlers */

	if (srv->config.admin.address) {
		network_mysqld_con *con = NULL;

		con = network_mysqld_con_init(srv);
		con->config = srv->config;
		con->config.network_type = NETWORK_TYPE_SERVER;
		
		con->server = network_socket_init();

		if (0 != network_mysqld_server_init(srv, con->server)) {
			g_critical("%s.%d: network_mysqld_server_init() failed", __FILE__, __LINE__);

			return NULL;
		}

		/* keep the listen socket alive */
		event_set(&(con->server->event), con->server->fd, EV_READ|EV_PERSIST, network_mysqld_con_accept, con);
		event_base_set(srv->event_base, &(con->server->event));
		event_add(&(con->server->event), NULL);
	}

	if (srv->config.proxy.address) {
		network_mysqld_con *con = NULL;

		con = network_mysqld_con_init(srv);
		con->config = srv->config;
		con->config.network_type = NETWORK_TYPE_PROXY;
		
		con->server = network_socket_init();

		if (0 != network_mysqld_proxy_init(srv, con->server)) {
			g_critical("%s.%d: network_mysqld_server_init() failed", __FILE__, __LINE__);
			return NULL;
		}
	
		/* keep the listen socket alive */
		event_set(&(con->server->event), con->server->fd, EV_READ|EV_PERSIST, network_mysqld_con_accept, con);
		event_base_set(srv->event_base, &(con->server->event));
		event_add(&(con->server->event), NULL);
	}

	while (!agent_shutdown) {
		struct timeval timeout;
		int r;

		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		g_assert(event_base_loopexit(srv->event_base, &timeout) == 0);

		r = event_base_dispatch(srv->event_base);

		if (r == -1) {
			if (errno == EINTR) continue;

			break;
		}
	}

	return NULL;
}

int network_mysqld_con_send_resultset(network_socket *con, GPtrArray *fields, GPtrArray *rows) {
	GString *s;
	gsize i, j;

	g_assert(fields->len > 0 && fields->len < 251);

	s = g_string_new(NULL);

	/* - len = 99
	 *  \1\0\0\1 
	 *    \1 - one field
	 *  \'\0\0\2 
	 *    \3def 
	 *    \0 
	 *    \0 
	 *    \0 
	 *    \21@@version_comment 
	 *    \0            - org-name
	 *    \f            - filler
	 *    \10\0         - charset
	 *    \34\0\0\0     - length
	 *    \375          - type 
	 *    \1\0          - flags
	 *    \37           - decimals
	 *    \0\0          - filler 
	 *  \5\0\0\3 
	 *    \376\0\0\2\0
	 *  \35\0\0\4
	 *    \34MySQL Community Server (GPL)
	 *  \5\0\0\5
	 *    \376\0\0\2\0
	 */

	g_string_append_c(s, fields->len); /* the field-count */
	network_queue_append(con->send_queue, s->str, s->len, con->packet_id++);

	for (i = 0; i < fields->len; i++) {
		MYSQL_FIELD *field = fields->pdata[i];
		
		g_string_truncate(s, 0);

		g_string_lenenc_append(s, field->catalog ? field->catalog : "def");        /* catalog */
		g_string_lenenc_append(s, field->db);          /* database */
		g_string_lenenc_append(s, field->table);       /* table */
		g_string_lenenc_append(s, field->org_table);   /* org_table */
		g_string_lenenc_append(s, field->name);        /* name */
		g_string_lenenc_append(s, field->org_name);    /* org_name */

		g_string_append_c(s, '\x0c');                  /* length of the following block, 12 byte */
		g_string_append_len(s, "\x08\x00", 2);         /* charset */
		g_string_append_c(s, (field->length >> 0) & 0xff); /* len */
		g_string_append_c(s, (field->length >> 8) & 0xff); /* len */
		g_string_append_c(s, (field->length >> 16) & 0xff); /* len */
		g_string_append_c(s, (field->length >> 24) & 0xff); /* len */
		g_string_append_c(s, field->type);             /* type */
		g_string_append_c(s, field->flags & 0xff);     /* flags */
		g_string_append_c(s, (field->flags >> 8) & 0xff); /* flags */
		g_string_append_c(s, 0);                       /* decimals */
		g_string_append_len(s, "\x00\x00", 2);         /* filler */
#if 0
		/* this is in the docs, but not on the network */
		g_string_lenenc_append(s, field->def);         /* default-value */
#endif
		network_queue_append(con->send_queue, s->str, s->len, con->packet_id++);
	}

	g_string_truncate(s, 0);
	
	/* EOF */	
	g_string_append_len(s, "\xfe", 1); /* EOF */
	g_string_append_len(s, "\x00\x00", 2); /* warning count */
	g_string_append_len(s, "\x02\x00", 2); /* flags */
	
	network_queue_append(con->send_queue, s->str, s->len, con->packet_id++);

	for (i = 0; i < rows->len; i++) {
		GPtrArray *row = rows->pdata[i];

		g_string_truncate(s, 0);

		for (j = 0; j < row->len; j++) {
			g_string_lenenc_append(s, row->pdata[j]);
		}
		network_queue_append(con->send_queue, s->str, s->len, con->packet_id++);
	}

	g_string_truncate(s, 0);

	/* EOF */	
	g_string_append_len(s, "\xfe", 1); /* EOF */
	g_string_append_len(s, "\x00\x00", 2); /* warning count */
	g_string_append_len(s, "\x02\x00", 2); /* flags */

	network_queue_append(con->send_queue, s->str, s->len, con->packet_id++);

	g_string_free(s, TRUE);

	return 0;
}

