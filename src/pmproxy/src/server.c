/*
 * Copyright (c) 2018-2019 Red Hat.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */
#include "server.h"
#include "uv_callback.h"
#include <assert.h>

void
proxylog(pmLogLevel level, sds message, void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    const char		*state = proxy->slots ? "" : "- DISCONNECTED - ";
    int			priority;

    switch (level) {
    case PMLOG_TRACE:
    case PMLOG_DEBUG:
	priority = LOG_DEBUG;
	break;
    case PMLOG_INFO:
	priority = LOG_INFO;
	break;
    case PMLOG_WARNING:
	priority = LOG_WARNING;
	break;
    case PMLOG_CORRUPT:
	priority = LOG_CRIT;
	break;
    default:
	priority = LOG_ERR;
	break;
    }
    pmNotifyErr(priority, "%s%s", state, message);
}

static struct proxy *
server_init(int portcount, const char *localpath)
{
    struct server	*servers;
    struct proxy	*proxy;
    int			count;

    if ((proxy = calloc(1, sizeof(struct proxy))) == NULL) {
	fprintf(stderr, "%s: out-of-memory in proxy server setup\n",
			pmGetProgname());
	return NULL;
    }

    count = portcount + (*localpath ? 1 : 0);
    if (count) {
	/* allocate space for maximum listen port data structures */
	if ((servers = calloc(count, sizeof(struct server))) == NULL) {
	    fprintf(stderr, "%s: out-of-memory allocating for %d ports\n",
			    pmGetProgname(), count);
	    free(proxy);
	    return NULL;
	}
	proxy->servers = servers;
    } else {
	fprintf(stderr, "%s: no ports or local paths specified\n",
			pmGetProgname());
	free(proxy);
	return NULL;
    }

    proxy->config = config;
    proxy->events = uv_default_loop();
    uv_loop_init(proxy->events);
    return proxy;
}

static void
signal_handler(uv_signal_t *sighandle, int signum)
{
    uv_handle_t		*handle = (uv_handle_t *)sighandle;
    struct proxy	*proxy = (struct proxy *)handle->data;
    uv_loop_t		*loop = proxy->events;

    if (signum == SIGHUP)
	return;
    pmNotifyErr(LOG_INFO, "pmproxy caught %s\n",
		signum == SIGINT ? "SIGINT" : "SIGTERM");
    uv_signal_stop(sighandle);
    uv_stop(loop);
}

static void
signal_init(struct proxy *proxy)
{
    static uv_signal_t	sighup, sigint, sigterm;
    uv_loop_t		*loop = proxy->events;

    signal(SIGPIPE, SIG_IGN);

    uv_signal_init(loop, &sighup);
    uv_signal_init(loop, &sigint);
    uv_signal_init(loop, &sigterm);
    sighup.data = sigint.data = sigterm.data = (void *)proxy;
    uv_signal_start(&sighup, signal_handler, SIGHUP);
    uv_signal_start(&sigint, signal_handler, SIGINT);
    uv_signal_start(&sigterm, signal_handler, SIGTERM);
}

void
on_buffer_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    if (pmDebugOptions.desperate)
	fprintf(stderr, "%s: handle %p buffer allocation of %lld bytes\n",
			"on_buffer_alloc", handle, (long long)suggested_size);

    if ((buf->base = sdsnewlen(SDS_NOINIT, suggested_size)) != NULL)
	buf->len = suggested_size;
    else
	buf->len = 0;
}

static void
on_client_close(uv_handle_t *handle)
{
    struct client	*client = (struct client *)handle;

    if (pmDebugOptions.context | pmDebugOptions.desperate)
	fprintf(stderr, "%s: client %p connection closed\n",
			"on_client_close", client);

    client_put(client);
}

void
client_get(struct client *client)
{
    uv_mutex_lock(&client->mutex);
    assert(client->refcount);
    client->refcount++;
    uv_mutex_unlock(&client->mutex);
}

void
client_put(struct client *client)
{
    unsigned int	refcount;

    uv_mutex_lock(&client->mutex);
    assert(client->refcount);
    refcount = --client->refcount;
    uv_mutex_unlock(&client->mutex);

    if (refcount == 0) {
	/* remove client from the doubly-linked list */
	if (client->next != NULL)
	    client->next->prev = client->prev;
	*client->prev = client->next;

	if (client->protocol & STREAM_PCP)
	    on_pcp_client_close(client);
	if (client->protocol & STREAM_HTTP)
	    on_http_client_close(client);
	if (client->protocol & STREAM_REDIS)
	    on_redis_client_close(client);
	if (client->protocol & STREAM_SECURE)
	    on_secure_client_close(client);

	memset(client, 0, sizeof(*client));
	free(client);
    }
}

int
client_is_closed(struct client *client)
{
    return !client->opened;
}

void
client_close(struct client *client)
{
    if (client->opened == 1) {
	client->opened = 0;
	uv_close((uv_handle_t *)client, on_client_close);
    }
}

void
on_client_write(uv_write_t *writer, int status)
{
    struct client	*client = (struct client *)writer->handle;
    stream_write_baton	*request = (stream_write_baton *)writer;

    if (pmDebugOptions.af)
	fprintf(stderr, "%s: completed write [sts=%d] to client %p\n",
			"on_client_write", status, client);

    sdsfree(request->buffer[0].base);
    request->buffer[0].base = NULL;
    if (request->buffer[1].base) {	/* optional second buffer */
	sdsfree(request->buffer[1].base);
	request->buffer[1].base = NULL;
    }
    free(request);

    if (status == 0)
	return;

    if (pmDebugOptions.af)
	fprintf(stderr, "%s: %s\n", "on_client_write", uv_strerror(status));
    client_close(client);
}

void *
on_write_callback(uv_callback_t *handle, void *data)
{
    stream_write_baton	*request = (stream_write_baton *)data;

    uv_write(&request->writer, request->stream,
		&request->buffer[0], request->nbuffers, request->callback);
    (void)handle;
    return 0;
}

void
client_write(struct client *client, sds buffer, sds suffix)
{
    stream_write_baton	*request;
    struct proxy	*proxy = client->proxy;
    unsigned int	nbuffers = 0;

    if (client_is_closed(client))
	return;

    if ((request = calloc(1, sizeof(stream_write_baton))) != NULL) {
	if (pmDebugOptions.af)
	    fprintf(stderr, "%s: sending %ld bytes [0] to client %p\n",
			"client_write", (long)sdslen(buffer), client);
	request->buffer[nbuffers++] = uv_buf_init(buffer, sdslen(buffer));
	if (suffix != NULL) {
	    if (pmDebugOptions.af)
		fprintf(stderr, "%s: sending %ld bytes [1] to client %p\n",
			"client_write", (long)sdslen(suffix), client);
	    request->buffer[nbuffers++] = uv_buf_init(suffix, sdslen(suffix));
	}
	request->nbuffers = nbuffers;
	request->callback = on_client_write;
	request->stream = (uv_stream_t *)&client->stream;

	if (client->stream.secure) {
	    secure_client_write(client, request);
	} else {
	    uv_callback_fire(&proxy->write_callbacks, request, NULL);
	}
    } else {
	client_close(client);
    }
}

static stream_protocol
client_protocol(int key)
{
    switch (key) {
    case 'p':	/* PCP pmproxy */
	return STREAM_PCP;
    case 'G':	/* HTTP GET */
    case 'H':	/* HTTP HEAD */
    case 'P':	/* HTTP POST, PUT, PATCH */
    case 'D':	/* HTTP DELETE */
    case 'T':	/* HTTP TRACE */
    case 'O':	/* HTTP OPTIONS */
    case 'C':	/* HTTP CONNECT */
	return STREAM_HTTP;
    case '-':	/* RESP error */
    case '+':	/* RESP status */
    case ':':	/* RESP integer */
    case '$':	/* RESP string */
    case '*':	/* RESP array */
	return STREAM_REDIS;
    case 0x14:	/* TLS ChangeCipherSpec */
    case 0x15:	/* TLS Alert */
    case 0x16:	/* TLS Handshake */
    case 0x17:	/* TLS Application */
    case 0x18:	/* TLS Heartbeat */
	return STREAM_SECURE;
    default:
	break;
    }
    return STREAM_UNKNOWN;
}

void
on_protocol_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct proxy	*proxy = (struct proxy *)stream->data;
    struct client	*client = (struct client *)stream;

    if (nread < 0)
	return;

    if ((client->protocol & (STREAM_PCP|STREAM_HTTP|STREAM_REDIS)) == 0)
	client->protocol |= client_protocol(*buf->base);

    if (client->protocol & STREAM_PCP)
	on_pcp_client_read(proxy, client, nread, buf);
    else if (client->protocol & STREAM_HTTP)
	on_http_client_read(proxy, client, nread, buf);
    else if (client->protocol & STREAM_REDIS)
	on_redis_client_read(proxy, client, nread, buf);
    else {
	if (pmDebugOptions.af)
	    fprintf(stderr, "%s: unknown protocol key '%c' (0x%x)"
			    " - disconnecting client %p\n", "on_protocol_read",
		    *buf->base, (unsigned int)*buf->base, proxy);
	client_close(client);
    }
}

static void
on_client_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    struct proxy	*proxy = (struct proxy *)stream->data;
    struct client	*client = (struct client *)stream;

    if (nread > 0) {
	if (client->protocol == STREAM_UNKNOWN)
	    client->protocol |= client_protocol(*buf->base);
	if (client->protocol & STREAM_SECURE)
	    on_secure_client_read(proxy, client, nread, buf);
	else
	    on_protocol_read(stream, nread, buf);
    } else if (nread < 0) {
	if (pmDebugOptions.af)
	    fprintf(stderr, "%s: read error %ld "
		    "- disconnecting client %p\n", "on_client_read",
		    (long)nread, client);
	client_close(client);
    }
    sdsfree(buf->base);
}

static void
on_client_connection(uv_stream_t *stream, int status)
{
    struct proxy	*proxy = (struct proxy *)stream->data;
    struct client	*client;
    uv_handle_t		*handle;

    if (status != 0) {
	fprintf(stderr, "%s: client connection failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	return;
    }

    if ((client = calloc(1, sizeof(*client))) == NULL) {
	fprintf(stderr, "%s: out-of-memory for new client\n",
			pmGetProgname());
	return;
    }
    if (pmDebugOptions.context | pmDebugOptions.af)
	fprintf(stderr, "%s: accept new client %p\n",
			"on_client_connection", client);

    /* prepare per-client lock for reference counting */
    uv_mutex_init(&client->mutex);
    client->refcount = 1;
    client->opened = 1;

    status = uv_tcp_init(proxy->events, &client->stream.u.tcp);
    if (status != 0) {
	fprintf(stderr, "%s: client tcp init failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	client_put(client);
	return;
    }

    status = uv_accept(stream, (uv_stream_t *)&client->stream.u.tcp);
    if (status != 0) {
	fprintf(stderr, "%s: client tcp init failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	client_put(client);
	return;
    }
    handle = (uv_handle_t *)&client->stream.u.tcp;
    handle->data = (void *)proxy;
    client->proxy = proxy;

    /* insert client into doubly-linked list at the head */
    if ((client->next = proxy->first) != NULL)
	proxy->first->prev = &client->next;
    proxy->first = client;
    client->prev = &proxy->first;

    status = uv_read_start((uv_stream_t *)&client->stream.u.tcp,
			    on_buffer_alloc, on_client_read);
    if (status != 0) {
	fprintf(stderr, "%s: client read start failed: %s\n",
			pmGetProgname(), uv_strerror(status));
	client_close(client);
    }
}

static int
open_request_port(struct proxy *proxy, struct server *server, stream_family family,
		const struct sockaddr *addr, int port, int maxpending)
{
    struct stream	*stream = &server->stream;
    uv_handle_t		*handle;
    sds			option;
    int			sts, flags = 0, keepalive = 45;

    if ((option = pmIniFileLookup(proxy->config, "pmproxy", "keepalive")))
	keepalive = atoi(option);

    stream->family = family;
    if (family == STREAM_TCP6)
	flags = UV_TCP_IPV6ONLY;
    stream->port = port;

    uv_tcp_init(proxy->events, &stream->u.tcp);
    handle = (uv_handle_t *)&stream->u.tcp;
    handle->data = (void *)proxy;

    uv_tcp_bind(&stream->u.tcp, addr, flags);
    uv_tcp_nodelay(&stream->u.tcp, 1);
    uv_tcp_keepalive(&stream->u.tcp, keepalive > 0, keepalive);

    sts = uv_listen((uv_stream_t *)&stream->u.tcp, maxpending, on_client_connection);
    if (sts != 0) {
	fprintf(stderr, "%s: socket listen error %s\n",
			pmGetProgname(), uv_strerror(sts));
	return -ENOTCONN;
    }
    stream->active = 1;
    if (__pmServerHasFeature(PM_SERVER_FEATURE_DISCOVERY))
	server->presence = __pmServerAdvertisePresence(PM_SERVER_PROXY_SPEC, port);
    return 0;
}

static int
open_request_local(struct proxy *proxy, struct server *server,
		const char *name, int maxpending)
{
    uv_handle_t		*handle;
    struct stream	*stream = &server->stream;
    int			sts;

    stream->family = STREAM_LOCAL;

    uv_pipe_init(proxy->events, &stream->u.local, 0);
    handle = (uv_handle_t *)&stream->u.local;
    handle->data = (void *)proxy;

    uv_pipe_bind(&stream->u.local, name);
#ifdef HAVE_UV_PIPE_CHMOD
    uv_pipe_chmod(&stream->u.local, UV_READABLE);
#endif

    sts = uv_listen((uv_stream_t *)&stream->u.local, maxpending, on_client_connection);
    if (sts != 0) {
	fprintf(stderr, "%s: local listen error %s\n",
			pmGetProgname(), uv_strerror(sts));
        return -ENOTCONN;
    }
    stream->active = 1;
    __pmServerSetFeature(PM_SERVER_FEATURE_UNIX_DOMAIN);
    return 0;
}

typedef struct proxyaddr {
    __pmSockAddr	*addr;
    const char		*address;
    int			port;
} proxyaddr;

static void *
open_request_ports(const char *localpath, int maxpending)
{
    int			inaddr, total, count, port, sts, i, n;
    int			with_ipv6 = strcmp(pmGetAPIConfig("ipv6"), "true") == 0;
    const char		*address;
    __pmSockAddr	*addr;
    struct proxyaddr	*addrlist;
    const struct sockaddr *sockaddr;
    stream_family	family;
    struct server	*server;
    struct proxy	*proxy;

    if ((sts = total = __pmServerSetupRequestPorts()) < 0)
	return NULL;

    /* allow for both IPv6 and IPv4 addresses for each port */
    if ((addrlist = calloc(total * 2, sizeof(proxyaddr))) == NULL)
	return NULL;

    /* fill in sockaddr structs for subsequent listen calls */
    for (i = n = 0; i < total; i++) {
	__pmServerGetRequestPort(i, &address, &port);
	addrlist[n].address = address;
	addrlist[n].port = port;

	if (address != NULL &&
	    strcmp(address, "INADDR_ANY") != 0 &&
	    strcmp(address, "INADDR_LOOPBACK") != 0) {
	    addr = __pmStringToSockAddr(address);
	    if (__pmSockAddrGetFamily(addr) == AF_UNSPEC)
		__pmSockAddrFree(addr);
	    else {
		__pmSockAddrSetPort(addr, port);
		addrlist[n++].addr = addr;
		continue;
	    }
	}

	/* address unspecified - create both ipv4 and ipv6 entries */
	if (address == NULL || strcmp(address, "INADDR_ANY") == 0)
	    inaddr = INADDR_ANY;
	else if (strcmp(address, "INADDR_LOOPBACK") == 0)
	    inaddr = INADDR_LOOPBACK;
	else
	    continue;

	addrlist[n].addr = __pmSockAddrAlloc();
	__pmSockAddrInit(addrlist[n].addr, AF_INET, inaddr, port);
	n++;

	if (!with_ipv6)
	     continue;

	addrlist[n].addr = __pmSockAddrAlloc();
	__pmSockAddrInit(addrlist[n].addr, AF_INET6, inaddr, port);
	n++;
    }
    total = n;

    if ((proxy = server_init(total, localpath)) == NULL)
	goto fail;

    signal_init(proxy);

    count = n = 0;
    if (*localpath) {
	server = &proxy->servers[n++];
	server->stream.address = localpath;
	if (open_request_local(proxy, server, localpath, maxpending) == 0)
	    count++;
    }

    for (i = 0; i < total; i++) {
	sockaddr = (const struct sockaddr *)addrlist[i].addr;
	family = __pmSockAddrGetFamily(addrlist[i].addr) == AF_INET ?
					STREAM_TCP4 : STREAM_TCP6;
	port = __pmSockAddrGetPort(addrlist[i].addr);
	server = &proxy->servers[n++];
	server->stream.address = addrlist[i].address;
	if (open_request_port(proxy, server, family, sockaddr, port, maxpending) == 0)
	    count++;
	__pmSockAddrFree(addrlist[i].addr);
    }
    free(addrlist);

    if (count == 0) {
	pmNotifyErr(LOG_ERR, "%s: can't open any request ports, exiting\n",
		pmGetProgname());
	free(proxy);
	return NULL;
    }
    proxy->nservers = count;
    return proxy;

fail:
    for (i = 0; i < n; i++)
	__pmSockAddrFree(addrlist[i].addr);
    free(addrlist);
    return NULL;
}

static void
close_proxy(struct proxy *proxy)
{
    close_pcp_module(proxy);
    close_http_module(proxy);
    close_redis_module(proxy);
    close_secure_module(proxy);
}

static void
shutdown_ports(void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    struct server	*server;
    struct stream	*stream;
    int			i;

    for (i = 0; i < proxy->nservers; i++) {
	server = &proxy->servers[i++];
	stream = &server->stream;
	if (stream->active == 0)
	    continue;
	if (server->presence)
	    __pmServerUnadvertisePresence(server->presence);
    }
    proxy->nservers = 0;
    free(proxy->servers);
    proxy->servers = NULL;

    close_proxy(proxy);

    if (proxy->config) {
	pmIniFileFree(proxy->config);
	proxy->config = NULL;
    }
}

static void
dump_request_ports(FILE *output, void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    struct stream	*stream;
    uv_os_fd_t		uv_fd;
    int			i, fd;

    fprintf(output, "%s request port(s):\n"
		"  sts fd   port  family address\n"
		"  === ==== ===== ====== =======\n", pmGetProgname());

    for (i = 0; i < proxy->nservers; i++) {
	stream = &proxy->servers[i].stream;
	fd = (uv_fileno((uv_handle_t *)stream, &uv_fd) < 0) ? -1 : (int)uv_fd;
	if (stream->family == STREAM_LOCAL)
	    fprintf(output, "  %-3s %4d %5s %-6s %s\n",
		    stream->active ? "ok" : "err", fd, "",
		    "unix", stream->address);
	else
	    fprintf(output, "  %-3s %4d %5d %-6s %s\n",
		    stream->active ? "ok" : "err", fd, stream->port,
		    stream->family == STREAM_TCP4 ? "inet" : "ipv6",
		    stream->address ? stream->address : "INADDR_ANY");
    }
}

/*
 * Initial setup for each of the major sub-systems modules,
 * which is achieved via a timer that expires immediately.
 * Once any connections are established (async) modules are
 * again informed via their individual setup routines.
 */
static void
setup_proxy(uv_timer_t *arg)
{
    uv_handle_t		*handle = (uv_handle_t *)arg;
    struct proxy	*proxy = (struct proxy *)handle->data;

    setup_secure_module(proxy);
    setup_redis_module(proxy);
    setup_http_module(proxy);
    setup_pcp_module(proxy);
}

static void
prepare_proxy(uv_prepare_t *arg)
{
    uv_handle_t		*handle = (uv_handle_t *)arg;
    struct proxy	*proxy = (struct proxy *)handle->data;

    flush_secure_module(proxy);
}

static void
check_proxy(uv_check_t *arg)
{
    uv_handle_t		*handle = (uv_handle_t *)arg;
    struct proxy	*proxy = (struct proxy *)handle->data;

    flush_secure_module(proxy);
}

static void
main_loop(void *arg)
{
    struct proxy	*proxy = (struct proxy *)arg;
    uv_timer_t		initial_io;
    uv_prepare_t	before_io;
    uv_check_t		after_io;
    uv_handle_t		*handle;

    uv_timer_init(proxy->events, &initial_io);
    handle = (uv_handle_t *)&initial_io;
    handle->data = (void *)proxy;
    uv_timer_start(&initial_io, setup_proxy, 0, 0);

    uv_prepare_init(proxy->events, &before_io);
    handle = (uv_handle_t *)&before_io;
    handle->data = (void *)proxy;
    uv_prepare_start(&before_io, prepare_proxy);

    uv_check_init(proxy->events, &after_io);
    handle = (uv_handle_t *)&after_io;
    handle->data = (void *)proxy;
    uv_check_start(&after_io, check_proxy);

    uv_callback_init(proxy->events, &proxy->write_callbacks,
		    on_write_callback, UV_DEFAULT);

    uv_run(proxy->events, UV_RUN_DEFAULT);
    uv_loop_close(proxy->events);
}

struct pmproxy libuv_pmproxy = {
    .openports	= open_request_ports,
    .dumpports	= dump_request_ports,
    .shutdown	= shutdown_ports,
    .loop 	= main_loop,
};
