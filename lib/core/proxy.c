/*
 * Copyright (c) 2014,2015 DeNA Co., Ltd., Kazuho Oku, Masahiro Nagano
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include "picohttpparser.h"
#include "h2o.h"
#include "h2o/http1.h"
#include "h2o/http1client.h"
#include "h2o/tunnel.h"

struct rp_generator_t {
    h2o_generator_t super;
    h2o_req_t *src_req;
    h2o_http1client_t *client;
    struct {
        h2o_iovec_t bufs
            [3]; /* first buf is the request line and host header, the second is the rest headers, the third is the POST content */
        int is_head;
    } up_req;
    h2o_buffer_t *last_content_before_send;
    h2o_doublebuffer_t sending;
    int is_websocket_handshake;
    int had_body_error; /* set if an error happened while fetching the body so that we can propagate the error */
    void (*await_send)(h2o_http1client_t *);
};

struct rp_ws_upgrade_info_t {
    h2o_context_t *ctx;
    h2o_timeout_t *timeout;
    h2o_socket_t *upstream_sock;
};

static h2o_http1client_ctx_t *get_client_ctx(h2o_req_t *req)
{
    h2o_req_overrides_t *overrides = req->overrides;
    if (overrides != NULL && overrides->client_ctx != NULL)
        return overrides->client_ctx;
    return &req->conn->ctx->proxy.client_ctx;
}

static h2o_iovec_t rewrite_location(h2o_mem_pool_t *pool, const char *location, size_t location_len, h2o_url_t *match,
                                    const h2o_url_scheme_t *req_scheme, h2o_iovec_t req_authority, h2o_iovec_t req_basepath)
{
    h2o_url_t loc_parsed;

    if (h2o_url_parse(location, location_len, &loc_parsed) != 0)
        goto NoRewrite;
    if (loc_parsed.scheme != &H2O_URL_SCHEME_HTTP)
        goto NoRewrite;
    if (!h2o_url_hosts_are_equal(&loc_parsed, match))
        goto NoRewrite;
    if (h2o_url_get_port(&loc_parsed) != h2o_url_get_port(match))
        goto NoRewrite;
    if (loc_parsed.path.len < match->path.len)
        goto NoRewrite;
    if (memcmp(loc_parsed.path.base, match->path.base, match->path.len) != 0)
        goto NoRewrite;

    return h2o_concat(pool, req_scheme->name, h2o_iovec_init(H2O_STRLIT("://")), req_authority, req_basepath,
                      h2o_iovec_init(loc_parsed.path.base + match->path.len, loc_parsed.path.len - match->path.len));

NoRewrite:
    return (h2o_iovec_t){NULL};
}

static h2o_iovec_t build_request_merge_headers(h2o_mem_pool_t *pool, h2o_iovec_t merged, h2o_iovec_t added, int seperator)
{
    if (added.len == 0)
        return merged;
    if (merged.len == 0)
        return added;

    size_t newlen = merged.len + 2 + added.len;
    char *buf = h2o_mem_alloc_pool(pool, newlen);
    memcpy(buf, merged.base, merged.len);
    buf[merged.len] = seperator;
    buf[merged.len + 1] = ' ';
    memcpy(buf + merged.len + 2, added.base, added.len);
    merged.base = buf;
    merged.len = newlen;
    return merged;
}

/*
 * A request without neither Content-Length or Transfer-Encoding header implies a zero-length request body (see 6th rule of RFC 7230
 * 3.3.3).
 * OTOH, section 3.3.3 states:
 *
 *   A user agent SHOULD send a Content-Length in a request message when
 *   no Transfer-Encoding is sent and the request method defines a meaning
 *   for an enclosed payload body.  For example, a Content-Length header
 *   field is normally sent in a POST request even when the value is 0
 *   (indicating an empty payload body).  A user agent SHOULD NOT send a
 *   Content-Length header field when the request message does not contain
 *   a payload body and the method semantics do not anticipate such a
 *   body.
 *
 * PUT and POST define a meaning for the payload body, let's emit a
 * Content-Length header if it doesn't exist already, since the server
 * might send a '411 Length Required' response.
 *
 * see also: ML thread starting at https://lists.w3.org/Archives/Public/ietf-http-wg/2016JulSep/0580.html
 */
static int req_requires_content_length(h2o_req_t *req)
{
    int is_put_or_post =
        (req->method.len >= 1 && req->method.base[0] == 'P' && (h2o_memis(req->method.base, req->method.len, H2O_STRLIT("POST")) ||
                                                                h2o_memis(req->method.base, req->method.len, H2O_STRLIT("PUT"))));

    return is_put_or_post && h2o_find_header(&req->res.headers, H2O_TOKEN_TRANSFER_ENCODING, -1) == -1;
}

static h2o_iovec_t build_request_line_host(h2o_req_t *req, int use_proxy_protocol)
{
    h2o_iovec_t buf;
    size_t offset = 0;

    buf.len = req->method.len + req->path.len + req->authority.len + sizeof("  HTTP/1.1\r\nhost: \r\n");
    if (use_proxy_protocol)
        buf.len += H2O_PROXY_HEADER_MAX_LENGTH;
    buf.base = h2o_mem_alloc_pool(&req->pool, buf.len);

#define APPEND(s, l)                                                                                                               \
    do {                                                                                                                           \
        memcpy(buf.base + offset, (s), (l));                                                                                       \
        offset += (l);                                                                                                             \
    } while (0)
#define APPEND_STRLIT(lit) APPEND((lit), sizeof(lit) - 1)

    if (use_proxy_protocol)
        offset += h2o_stringify_proxy_header(req->conn, buf.base + offset);

    APPEND(req->method.base, req->method.len);
    buf.base[offset++] = ' ';
    APPEND(req->path.base, req->path.len);
    APPEND_STRLIT(" HTTP/1.1\r\nhost: ");
    APPEND(req->authority.base, req->authority.len);
    buf.base[offset++] = '\r';
    buf.base[offset++] = '\n';
    buf.base[offset++] = '\0'; /* for debugging */

#undef APPEND
#undef APPEND_STRLIT

    /* set the length */
    assert(offset <= buf.len);
    buf.len = offset - 1;

    return buf;
}

static h2o_iovec_t build_request_rest_headers(h2o_req_t *req, int keepalive, int is_websocket_handshake, int *te_chunked)
{
    h2o_iovec_t buf;
    size_t offset = 0, remote_addr_len = SIZE_MAX;
    char remote_addr[NI_MAXHOST];
    struct sockaddr_storage ss;
    socklen_t sslen;
    h2o_iovec_t cookie_buf = {NULL}, xff_buf = {NULL}, via_buf = {NULL};
    int preserve_x_forwarded_proto = req->conn->ctx->globalconf->proxy.preserve_x_forwarded_proto;
    int emit_x_forwarded_headers = req->conn->ctx->globalconf->proxy.emit_x_forwarded_headers;
    int emit_via_header = req->conn->ctx->globalconf->proxy.emit_via_header;

    /* for x-f-f */
    if ((sslen = req->conn->callbacks->get_peername(req->conn, (void *)&ss)) != 0)
        remote_addr_len = h2o_socket_getnumerichost((void *)&ss, sslen, remote_addr);

    /* build response */
    buf.len = 512;
    buf.base = h2o_mem_alloc_pool(&req->pool, buf.len);

#define RESERVE(sz)                                                                                                                \
    do {                                                                                                                           \
        size_t required = offset + sz + 4 /* for "\r\n\r\n" */;                                                                    \
        if (required > buf.len) {                                                                                                  \
            do {                                                                                                                   \
                buf.len *= 2;                                                                                                      \
            } while (required > buf.len);                                                                                          \
            char *newp = h2o_mem_alloc_pool(&req->pool, buf.len);                                                                  \
            memcpy(newp, buf.base, offset);                                                                                        \
            buf.base = newp;                                                                                                       \
        }                                                                                                                          \
    } while (0)
#define APPEND(s, l)                                                                                                               \
    do {                                                                                                                           \
        memcpy(buf.base + offset, (s), (l));                                                                                       \
        offset += (l);                                                                                                             \
    } while (0)
#define APPEND_STRLIT(lit) APPEND((lit), sizeof(lit) - 1)
#define FLATTEN_PREFIXED_VALUE(prefix, value, add_size)                                                                            \
    do {                                                                                                                           \
        RESERVE(sizeof(prefix) - 1 + value.len + 2 + add_size);                                                                    \
        APPEND_STRLIT(prefix);                                                                                                     \
        if (value.len != 0) {                                                                                                      \
            APPEND(value.base, value.len);                                                                                         \
            if (add_size != 0) {                                                                                                   \
                buf.base[offset++] = ',';                                                                                          \
                buf.base[offset++] = ' ';                                                                                          \
            }                                                                                                                      \
        }                                                                                                                          \
    } while (0)

    APPEND_STRLIT("connection: ");
    if (is_websocket_handshake) {
        APPEND_STRLIT("upgrade\r\nupgrade: websocket\r\n");
    } else if (keepalive) {
        APPEND_STRLIT("keep-alive\r\n");
    } else {
        APPEND_STRLIT("close\r\n");
    }
    assert(offset <= buf.len);

    /* CL or TE? Depends on whether we're streaming the request body or
       not, and if CL was advertised in the original request */
    if (req->proceed_req == NULL) {
        if (req->entity.base != NULL || req_requires_content_length(req)) {
            RESERVE(sizeof("content-length: " H2O_UINT64_LONGEST_STR) - 1);
            offset += sprintf(buf.base + offset, "content-length: %zu\r\n", req->entity.len);
        }
    } else {
        if (req->content_length != SIZE_MAX) {
            RESERVE(sizeof("content-length: " H2O_UINT64_LONGEST_STR) - 1);
            offset += sprintf(buf.base + offset, "content-length: %zu\r\n", req->content_length);
        } else {
            *te_chunked = 1;
            APPEND_STRLIT("transfer-encoding: chunked\r\n");
        }
    }

    /* rewrite headers if necessary */
    h2o_headers_t req_headers = req->headers;
    if (req->overrides != NULL && req->overrides->headers_cmds != NULL) {
        req_headers.entries = NULL;
        req_headers.size = 0;
        req_headers.capacity = 0;
        h2o_headers_command_t *cmd;
        h2o_vector_reserve(&req->pool, &req_headers, req->headers.capacity);
        memcpy(req_headers.entries, req->headers.entries, sizeof(req->headers.entries[0]) * req->headers.size);
        req_headers.size = req->headers.size;
        for (cmd = req->overrides->headers_cmds; cmd->cmd != H2O_HEADERS_CMD_NULL; ++cmd)
            h2o_rewrite_headers(&req->pool, &req_headers, cmd);
    }

    {
        const h2o_header_t *h, *h_end;
        for (h = req_headers.entries, h_end = h + req_headers.size; h != h_end; ++h) {
            if (h2o_iovec_is_token(h->name)) {
                const h2o_token_t *token = (void *)h->name;
                if (token->proxy_should_drop_for_req) {
                    continue;
                } else if (token == H2O_TOKEN_COOKIE) {
                    /* merge the cookie headers; see HTTP/2 8.1.2.5 and HTTP/1 (RFC6265 5.4) */
                    /* FIXME current algorithm is O(n^2) against the number of cookie headers */
                    cookie_buf = build_request_merge_headers(&req->pool, cookie_buf, h->value, ';');
                    continue;
                } else if (token == H2O_TOKEN_VIA) {
                    if (!emit_via_header) {
                        goto AddHeader;
                    }
                    via_buf = build_request_merge_headers(&req->pool, via_buf, h->value, ',');
                    continue;
                } else if (token == H2O_TOKEN_X_FORWARDED_FOR) {
                    if (!emit_x_forwarded_headers) {
                        goto AddHeader;
                    }
                    xff_buf = build_request_merge_headers(&req->pool, xff_buf, h->value, ',');
                    continue;
                }
            }
            if (!preserve_x_forwarded_proto && h2o_lcstris(h->name->base, h->name->len, H2O_STRLIT("x-forwarded-proto")))
                continue;
        AddHeader:
            RESERVE(h->name->len + h->value.len + 2);
            APPEND(h->orig_name ? h->orig_name : h->name->base, h->name->len);
            buf.base[offset++] = ':';
            buf.base[offset++] = ' ';
            APPEND(h->value.base, h->value.len);
            buf.base[offset++] = '\r';
            buf.base[offset++] = '\n';
        }
    }

    if (cookie_buf.len != 0) {
        FLATTEN_PREFIXED_VALUE("cookie: ", cookie_buf, 0);
        buf.base[offset++] = '\r';
        buf.base[offset++] = '\n';
    }
    if (emit_x_forwarded_headers) {
        if (!preserve_x_forwarded_proto) {
            FLATTEN_PREFIXED_VALUE("x-forwarded-proto: ", req->input.scheme->name, 0);
            buf.base[offset++] = '\r';
            buf.base[offset++] = '\n';
        }
        if (remote_addr_len != SIZE_MAX) {
            FLATTEN_PREFIXED_VALUE("x-forwarded-for: ", xff_buf, remote_addr_len);
            APPEND(remote_addr, remote_addr_len);
        } else {
            FLATTEN_PREFIXED_VALUE("x-forwarded-for: ", xff_buf, 0);
        }
        buf.base[offset++] = '\r';
        buf.base[offset++] = '\n';
    }
    if (emit_via_header) {
        FLATTEN_PREFIXED_VALUE("via: ", via_buf, sizeof("1.1 ") - 1 + req->input.authority.len);
        if (req->version < 0x200) {
            buf.base[offset++] = '1';
            buf.base[offset++] = '.';
            buf.base[offset++] = '0' + (0x100 <= req->version && req->version <= 0x109 ? req->version - 0x100 : 0);
        } else {
            buf.base[offset++] = '2';
        }
        buf.base[offset++] = ' ';
        APPEND(req->input.authority.base, req->input.authority.len);
        buf.base[offset++] = '\r';
        buf.base[offset++] = '\n';
    }
    APPEND_STRLIT("\r\n");

#undef RESERVE
#undef APPEND
#undef APPEND_STRLIT
#undef FLATTEN_PREFIXED_VALUE

    /* set the length */
    assert(offset <= buf.len);
    buf.len = offset;

    return buf;
}

static void do_close(h2o_generator_t *generator, h2o_req_t *req)
{
    struct rp_generator_t *self = (void *)generator;

    if (self->client != NULL) {
        h2o_http1client_cancel(self->client);
        self->client = NULL;
    }
}

static void do_send(struct rp_generator_t *self)
{
    h2o_iovec_t vecs[1];
    size_t veccnt;
    h2o_send_state_t ststate;

    assert(self->sending.bytes_inflight == 0);

    vecs[0] = h2o_doublebuffer_prepare(&self->sending,
                                       self->client != NULL ? &self->client->sock->input : &self->last_content_before_send,
                                       self->src_req->preferred_chunk_size);

    if (self->client == NULL && vecs[0].len == self->sending.buf->size && self->last_content_before_send->size == 0) {
        veccnt = vecs[0].len != 0 ? 1 : 0;
        ststate = H2O_SEND_STATE_FINAL;
    } else {
        if (vecs[0].len == 0)
            return;
        veccnt = 1;
        ststate = H2O_SEND_STATE_IN_PROGRESS;
    }

    if (self->had_body_error)
        ststate = H2O_SEND_STATE_ERROR;

    h2o_send(self->src_req, vecs, veccnt, ststate);
}

static void do_proceed(h2o_generator_t *generator, h2o_req_t *req)
{
    struct rp_generator_t *self = (void *)generator;

    h2o_doublebuffer_consume(&self->sending);
    do_send(self);
    if (self->await_send) {
        self->await_send(self->client);
        self->await_send = NULL;
    }
}

static void on_websocket_upgrade_complete(void *_info, h2o_socket_t *sock, size_t reqsize)
{
    struct rp_ws_upgrade_info_t *info = _info;

    if (sock != NULL) {
        h2o_buffer_consume(&sock->input, reqsize); // It is detached from conn. Let's trash unused data.
        h2o_tunnel_establish(info->ctx, sock, info->upstream_sock, info->timeout);
    } else {
        h2o_socket_close(info->upstream_sock);
    }
    free(info);
}

static inline void on_websocket_upgrade(struct rp_generator_t *self, h2o_timeout_t *timeout, int rlen)
{
    h2o_req_t *req = self->src_req;
    h2o_socket_t *sock = h2o_http1client_steal_socket(self->client);
    h2o_buffer_consume(&sock->input, rlen); // trash data after stealing sock.
    struct rp_ws_upgrade_info_t *info = h2o_mem_alloc(sizeof(*info));
    info->upstream_sock = sock;
    info->timeout = timeout;
    info->ctx = req->conn->ctx;
    h2o_http1_upgrade(req, NULL, 0, on_websocket_upgrade_complete, info);
}

static void await_send(h2o_http1client_t *client)
{
    if (client)
        h2o_http1client_body_read_resume(client);
}

static int on_body(h2o_http1client_t *client, const char *errstr)
{
    struct rp_generator_t *self = client->data;
    h2o_req_overrides_t *overrides = self->src_req->overrides;

    if (errstr != NULL) {
        /* detach the content */
        self->last_content_before_send = self->client->sock->input;
        h2o_buffer_init(&self->client->sock->input, &h2o_socket_buffer_prototype);
        self->client = NULL;
        if (errstr != h2o_http1client_error_is_eos) {
            h2o_req_log_error(self->src_req, "lib/core/proxy.c", "%s", errstr);
            self->had_body_error = 1;
        }
    }
    if (self->sending.bytes_inflight == 0)
        do_send(self);

    if (self->client && self->client->sock && overrides && self->client->sock->input->size > overrides->max_buffer_size) {
        self->await_send = await_send;
        h2o_http1client_body_read_stop(self->client);
    }

    return 0;
}

static char compress_hint_to_enum(const char *val, size_t len)
{
    if (h2o_lcstris(val, len, H2O_STRLIT("on"))) {
        return H2O_COMPRESS_HINT_ENABLE;
    }
    if (h2o_lcstris(val, len, H2O_STRLIT("off"))) {
        return H2O_COMPRESS_HINT_DISABLE;
    }
    return H2O_COMPRESS_HINT_AUTO;
}

static h2o_http1client_body_cb on_head(h2o_http1client_t *client, const char *errstr, int minor_version, int status,
                                       h2o_iovec_t msg, h2o_header_t *headers, size_t num_headers, int rlen)
{
    struct rp_generator_t *self = client->data;
    h2o_req_t *req = self->src_req;
    size_t i;

    if (errstr != NULL && errstr != h2o_http1client_error_is_eos) {
        self->client = NULL;
        h2o_req_log_error(req, "lib/core/proxy.c", "%s", errstr);
        h2o_send_error_502(req, "Gateway Error", errstr, 0);
        return NULL;
    }

    /* copy the response (note: all the headers must be copied; http1client discards the input once we return from this callback) */
    req->res.status = status;
    req->res.reason = h2o_strdup(&req->pool, msg.base, msg.len).base;
    for (i = 0; i != num_headers; ++i) {
        if (h2o_iovec_is_token(headers[i].name)) {
            const h2o_token_t *token = H2O_STRUCT_FROM_MEMBER(h2o_token_t, buf, headers[i].name);
            h2o_iovec_t value;
            if (token->proxy_should_drop_for_res) {
                goto Skip;
            }
            if (token == H2O_TOKEN_CONTENT_LENGTH) {
                if (req->res.content_length != SIZE_MAX ||
                    (req->res.content_length = h2o_strtosize(headers[i].value.base, headers[i].value.len)) == SIZE_MAX) {
                    self->client = NULL;
                    h2o_req_log_error(req, "lib/core/proxy.c", "%s", "invalid response from upstream (malformed content-length)");
                    h2o_send_error_502(req, "Gateway Error", "invalid response from upstream", 0);
                    return NULL;
                }
                goto Skip;
            } else if (token == H2O_TOKEN_LOCATION) {
                if (req->res_is_delegated && (300 <= status && status <= 399) && status != 304) {
                    self->client = NULL;
                    h2o_iovec_t method = h2o_get_redirect_method(req->method, status);
                    h2o_send_redirect_internal(req, method, headers[i].value.base, headers[i].value.len, 1);
                    return NULL;
                }
                if (req->overrides != NULL && req->overrides->location_rewrite.match != NULL) {
                    value = rewrite_location(&req->pool, headers[i].value.base, headers[i].value.len,
                                             req->overrides->location_rewrite.match, req->input.scheme, req->input.authority,
                                             req->overrides->location_rewrite.path_prefix);
                    if (value.base != NULL)
                        goto AddHeader;
                }
                goto AddHeaderDuped;
            } else if (token == H2O_TOKEN_LINK) {
                h2o_iovec_t new_value;
                new_value = h2o_push_path_in_link_header(req, headers[i].value.base, headers[i].value.len);
                if (!new_value.len)
                    goto Skip;
                headers[i].value.base = new_value.base;
                headers[i].value.len = new_value.len;
            } else if (token == H2O_TOKEN_SERVER) {
                if (!req->conn->ctx->globalconf->proxy.preserve_server_header)
                    goto Skip;
            } else if (token == H2O_TOKEN_X_COMPRESS_HINT) {
                req->compress_hint = compress_hint_to_enum(headers[i].value.base, headers[i].value.len);
                goto Skip;
            }
        /* default behaviour, transfer the header downstream */
        AddHeaderDuped:
            value = h2o_strdup(&req->pool, headers[i].value.base, headers[i].value.len);
        AddHeader:
            h2o_add_header(&req->pool, &req->res.headers, token, headers[i].orig_name, value.base, value.len);
        Skip:;
        } else {
            h2o_iovec_t name = h2o_strdup(&req->pool, headers[i].name->base, headers[i].name->len);
            h2o_iovec_t value = h2o_strdup(&req->pool, headers[i].value.base, headers[i].value.len);
            h2o_add_header_by_str(&req->pool, &req->res.headers, name.base, name.len, 0, headers[i].orig_name, value.base,
                                  value.len);
        }
    }

    if (self->is_websocket_handshake && req->res.status == 101) {
        h2o_http1client_ctx_t *client_ctx = get_client_ctx(req);
        assert(client_ctx->websocket_timeout != NULL);
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_UPGRADE, NULL, H2O_STRLIT("websocket"));
        on_websocket_upgrade(self, client_ctx->websocket_timeout, rlen);
        self->client = NULL;
        return NULL;
    }
    /* declare the start of the response */
    h2o_start_response(req, &self->super);

    if (errstr == h2o_http1client_error_is_eos) {
        self->client = NULL;
        h2o_send(req, NULL, 0, H2O_SEND_STATE_FINAL);
        return NULL;
    }

    return on_body;
}

static int on_1xx(h2o_http1client_t *client, int minor_version, int status, h2o_iovec_t msg, h2o_header_t *headers,
                  size_t num_headers)
{
    struct rp_generator_t *self = client->data;
    size_t i;

    for (i = 0; i != num_headers; ++i) {
        if (headers[i].name == &H2O_TOKEN_LINK->buf)
            h2o_push_path_in_link_header(self->src_req, headers[i].value.base, headers[i].value.len);
    }

    return 0;
}

static void proceed_request(h2o_http1client_t *client, size_t written, int is_end_stream)
{
    struct rp_generator_t *self = client->data;
    if (self->src_req->proceed_req != NULL)
        self->src_req->proceed_req(self->src_req, written, is_end_stream);
}

static int write_req(void *ctx, h2o_iovec_t chunk, int is_end_stream)
{
    struct rp_generator_t *self = ctx;

    if (is_end_stream) {
        self->src_req->write_req.cb = NULL;
    }
    return h2o_http1client_write_req(self->client->sock, chunk, is_end_stream);
}

static h2o_http1client_head_cb on_connect(h2o_http1client_t *client, const char *errstr, h2o_iovec_t **reqbufs, size_t *reqbufcnt,
                                          int *method_is_head, h2o_http1client_proceed_req_cb *proceed_req_cb,
                                          h2o_iovec_t *cur_body, h2o_url_t *origin)
{
    struct rp_generator_t *self = client->data;

    h2o_req_t *req = self->src_req;

    if (errstr == NULL) {
        assert(origin != NULL);
        int use_proxy_protocol = 0;
        if (req->overrides != NULL) {
            use_proxy_protocol = req->overrides->use_proxy_protocol;
            req->overrides->location_rewrite.match = origin;

            if (!req->overrides->proxy_preserve_host) {
                req->scheme = origin->scheme;
                req->authority = origin->authority;
            }
        }
        self->up_req.bufs[0] = build_request_line_host(req, use_proxy_protocol);
    }

    if (errstr != NULL) {
        self->client = NULL;
        h2o_req_log_error(self->src_req, "lib/core/proxy.c", "%s", errstr);
        h2o_send_error_502(self->src_req, "Gateway Error", errstr, 0);
        return NULL;
    }

    *reqbufs = self->up_req.bufs;
    *reqbufcnt = 2;
    *method_is_head = self->up_req.is_head;

    if (self->src_req->entity.base != NULL) {
        if (self->src_req->proceed_req != NULL) {
            *cur_body = self->src_req->entity;
            *proceed_req_cb = proceed_request;
            self->src_req->write_req.cb = write_req;
            self->src_req->write_req.ctx = self;
        } else {
            self->up_req.bufs[2] = self->src_req->entity;
            *reqbufcnt = 3;
        }
    }
    self->client->informational_cb = on_1xx;
    return on_head;
}

static void on_generator_dispose(void *_self)
{
    struct rp_generator_t *self = _self;

    if (self->client != NULL) {
        h2o_http1client_cancel(self->client);
        self->client = NULL;
    }
    h2o_buffer_dispose(&self->last_content_before_send);
    h2o_doublebuffer_dispose(&self->sending);
}

static struct rp_generator_t *proxy_send_prepare(h2o_req_t *req, int keepalive, int *te_chunked)
{
    struct rp_generator_t *self = h2o_mem_alloc_shared(&req->pool, sizeof(*self), on_generator_dispose);
    h2o_http1client_ctx_t *client_ctx = get_client_ctx(req);

    self->super.proceed = do_proceed;
    self->super.stop = do_close;
    self->src_req = req;
    if (client_ctx->websocket_timeout != NULL && h2o_lcstris(req->upgrade.base, req->upgrade.len, H2O_STRLIT("websocket"))) {
        self->is_websocket_handshake = 1;
    } else {
        self->is_websocket_handshake = 0;
    }
    self->had_body_error = 0;
    self->await_send = NULL;
    self->up_req.bufs[1] = build_request_rest_headers(req, keepalive, self->is_websocket_handshake, te_chunked);
    self->up_req.is_head = h2o_memis(req->method.base, req->method.len, H2O_STRLIT("HEAD"));
    h2o_buffer_init(&self->last_content_before_send, &h2o_socket_buffer_prototype);
    h2o_doublebuffer_init(&self->sending, &h2o_socket_buffer_prototype);

    return self;
}

void h2o__proxy_process_request(h2o_req_t *req)
{
    h2o_req_overrides_t *overrides = req->overrides;
    h2o_http1client_ctx_t *client_ctx = get_client_ctx(req);
    int te_chunked = 0;

    h2o_socketpool_t *sockpool = overrides != NULL ? overrides->socketpool : NULL;
    if (sockpool == NULL) {
        sockpool = req->conn->ctx->proxy.global_socketpool;
    }
    int keepalive = h2o_socketpool_can_keepalive(sockpool);
    if (overrides != NULL && overrides->use_proxy_protocol) {
        keepalive = 0;
    }

    struct rp_generator_t *self = proxy_send_prepare(req, keepalive, &te_chunked);

    h2o_url_t upstream;
    if (overrides != NULL && overrides->upstream != NULL) {
        upstream = *overrides->upstream;
    } else {
        h2o_url_init(&upstream, req->scheme, req->authority, h2o_iovec_init(H2O_STRLIT("/")));
    }

    /*
      When the PROXY protocol is being used (i.e. when overrides->use_proxy_protocol is set), the client needs to establish a new
     connection even when there is a pooled connection to the peer, since the header (as defined in
     https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt) needs to be sent at the beginning of the connection.

     However, currently h2o_http1client_connect doesn't provide an interface to enforce estabilishing a new connection. In other
     words, there is a chance that we would use a pool connection here.

     OTOH, the probability of seeing such issue is rare; it would only happen if the same destination identified by its host:port is
     accessed in both ways (i.e. in one path with use_proxy_protocol set and in the other path without).

     So I leave this as it is for the time being.
     */
    h2o_http1client_connect(&self->client, self, client_ctx, sockpool, &upstream, on_connect, te_chunked);
}
