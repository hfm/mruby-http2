/*
** mrb_http2 - http2 class for mruby
**
** Copyright (c) mod_mruby developers 2012-
**
** Permission is hereby granted, free of charge, to any person obtaining
** a copy of this software and associated documentation files (the
** "Software"), to deal in the Software without restriction, including
** without limitation the rights to use, copy, modify, merge, publish,
** distribute, sublicense, and/or sell copies of the Software, and to
** permit persons to whom the Software is furnished to do so, subject to
** the following conditions:
**
** The above copyright notice and this permission notice shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
** EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
** MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
** IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
** CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
** SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**
** [ MIT license: http://www.opensource.org/licenses/mit-license.php ]
*/

#include "mrb_http2.h"

#include <event.h>
#include <event2/event.h>
#include <event2/bufferevent_ssl.h>
#include <event2/listener.h>

typedef struct {
  unsigned int debug;
  const char *key;
  const char *cert;
  const char *service;
  const char *document_root;
} mrb_http2_config_t;

typedef struct {
  const char *service;
  mrb_value args;
  mrb_http2_config_t *config;
  mrb_state *mrb;
} mrb_http2_server_t;

typedef struct {
  SSL_CTX *ssl_ctx;
  struct event_base *evbase;
  mrb_http2_server_t *server;
} app_context;

typedef struct http2_stream_data {
  struct http2_stream_data *prev, *next;
  char *request_path;
  int32_t stream_id;
  int fd;
} http2_stream_data;

typedef struct http2_session_data {
  http2_stream_data root;
  struct bufferevent *bev;
  app_context *app_ctx;
  nghttp2_session *session;
  char *client_addr;
  size_t handshake_leftlen;
} http2_session_data;

static void mrb_http2_server_free(mrb_state *mrb, void *p)
{
  mrb_http2_server_t *server = (mrb_http2_server_t *)p;
  mrb_free(mrb, server->config);
  mrb_free(mrb, server);
  TRACER;
}

static const struct mrb_data_type mrb_http2_server_type = {
  "mrb_http2_server_t", mrb_http2_server_free,
};

//
//
// HTTP2::Server class
//
//

static unsigned char next_proto_list[256];
static size_t next_proto_list_len;

static void add_stream(http2_session_data *session_data, 
    http2_stream_data *stream_data)
{
  stream_data->next = session_data->root.next;
  session_data->root.next = stream_data;
  stream_data->prev = &session_data->root;
  TRACER;
  if(stream_data->next) {
    stream_data->next->prev = stream_data;
  }
}

static void remove_stream(http2_session_data *session_data, 
    http2_stream_data *stream_data)
{
  stream_data->prev->next = stream_data->next;
  TRACER;
  if(stream_data->next) {
    stream_data->next->prev = stream_data->prev;
  }
}

static http2_stream_data* create_http2_stream_data(mrb_state *mrb,
    http2_session_data *session_data, int32_t stream_id)
{
  http2_stream_data *stream_data;

  TRACER;
  stream_data = (http2_stream_data *)mrb_malloc(mrb, sizeof(http2_stream_data));
  memset(stream_data, 0, sizeof(http2_stream_data));
  stream_data->stream_id = stream_id;
  stream_data->fd = -1;

  add_stream(session_data, stream_data);
  return stream_data;
}

static void delete_http2_stream_data(mrb_state *mrb, 
    http2_stream_data *stream_data)
{
  TRACER;
  if(stream_data->fd != -1) {
    close(stream_data->fd);
  }
  mrb_free(mrb, stream_data->request_path);
  mrb_free(mrb, stream_data);
}

static void delete_http2_session_data(http2_session_data *session_data)
{
  http2_stream_data *stream_data;
  SSL *ssl = bufferevent_openssl_get_ssl(session_data->bev);
  mrb_state *mrb = session_data->app_ctx->server->mrb;
  mrb_http2_config_t *config = session_data->app_ctx->server->config;

  TRACER;
  if (config->debug) {
    fprintf(stderr, "%s disconnected\n", session_data->client_addr);
  }
  if(ssl) {
    SSL_shutdown(ssl);
  }
  bufferevent_free(session_data->bev);
  nghttp2_session_del(session_data->session);
  for(stream_data = session_data->root.next; stream_data;) {
    http2_stream_data *next = stream_data->next;
    delete_http2_stream_data(mrb, stream_data);
    stream_data = next;
  }
  TRACER;
  mrb_free(mrb, session_data->client_addr);
  mrb_free(mrb, session_data);
}

/* Serialize the frame and send (or buffer) the data to
   bufferevent. */
static int session_send(http2_session_data *session_data)
{
  int rv;

  TRACER;
  rv = nghttp2_session_send(session_data->session);
  if(rv != 0) {
    fprintf(stderr, "Fatal error: %s", nghttp2_strerror(rv));
    return -1;
  }
  TRACER;
  return 0;
}

/* Read the data in the bufferevent and feed them into nghttp2 library
   function. Invocation of nghttp2_session_mem_recv() may make
   additional pending frames, so call session_send() at the end of the
   function. */
static int session_recv(http2_session_data *session_data)
{
  int rv;
  struct evbuffer *input = bufferevent_get_input(session_data->bev);
  size_t datalen = evbuffer_get_length(input);
  unsigned char *data = evbuffer_pullup(input, -1);

  TRACER;
  rv = nghttp2_session_mem_recv(session_data->session, data, datalen);
  if(rv < 0) {
    fprintf(stderr, "Fatal error: %s", nghttp2_strerror(rv));
    return -1;
  }
  evbuffer_drain(input, rv);
  TRACER;
  if(session_send(session_data) != 0) {
    return -1;
  }
  TRACER;
  return 0;
}

static ssize_t server_send_callback(nghttp2_session *session, 
    const uint8_t *data, size_t length, int flags, void *user_data)
{
  http2_session_data *session_data = (http2_session_data *)user_data;
  mrb_state *mrb = session_data->app_ctx->server->mrb;

  http2_stream_data *stream_data;

  struct bufferevent *bev = session_data->bev;
  TRACER;
  /* Avoid excessive buffering in server side. */
  if(evbuffer_get_length(bufferevent_get_output(session_data->bev)) >=
     OUTPUT_WOULDBLOCK_THRESHOLD) {
    return NGHTTP2_ERR_WOULDBLOCK;
  }
  bufferevent_write(bev, data, length);
  TRACER;
  return length;
}

/* Returns nonzero if the string |s| ends with the substring |sub| */
static int ends_with(const char *s, const char *sub)
{
  size_t slen = strlen(s);
  size_t sublen = strlen(sub);

  TRACER;
  if(slen < sublen) {
    return 0;
  }
  TRACER;
  return memcmp(s + slen - sublen, sub, sublen) == 0;
}

/* Returns int value of hex string character |c| */
static uint8_t hex_to_uint(uint8_t c)
{
  if('0' <= c && c <= '9') {
    return c - '0';
  }
  if('A' <= c && c <= 'F') {
    return c - 'A' + 10;
  }
  if('a' <= c && c <= 'f') {
    return c - 'a' + 10;
  }
  return 0;
}

/* Decodes percent-encoded byte string |value| with length |valuelen|
   and returns the decoded byte string in allocated buffer. The return
   value is NULL terminated. The caller must free the returned
   string. */
static char* percent_decode(mrb_state *mrb, const uint8_t *value, size_t valuelen)
{
  char *res;

  TRACER;
  res = (char *)mrb_malloc(mrb, valuelen + 1);
  if(valuelen > 3) {
    size_t i, j;
    for(i = 0, j = 0; i < valuelen - 2;) {
      if(value[i] != '%' ||
         !isxdigit(value[i + 1]) || !isxdigit(value[i + 2])) {
        res[j++] = value[i++];
        continue;
      }
      res[j++] = (hex_to_uint(value[i + 1]) << 4) + hex_to_uint(value[i + 2]);
      i += 3;
    }
    memcpy(&res[j], &value[i], 2);
    res[j + 2] = '\0';
  } else {
    memcpy(res, value, valuelen);
    res[valuelen] = '\0';
  }
  TRACER;
  return res;
}

static ssize_t file_read_callback(nghttp2_session *session, 
    int32_t stream_id, uint8_t *buf, size_t length, int *eof, 
    nghttp2_data_source *source, void *user_data)
{
  int fd = source->fd;
  ssize_t r;

  while((r = read(fd, buf, length)) == -1 && errno == EINTR);
  TRACER;
  if(r == -1) {
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
  }
  if(r == 0) {
    *eof = 1;
  }
  TRACER;
  return r;
}

static int send_response(nghttp2_session *session, int32_t stream_id, 
    nghttp2_nv *nva, size_t nvlen, int fd)
{
  int rv;
  nghttp2_data_provider data_prd;
  data_prd.source.fd = fd;
  data_prd.read_callback = file_read_callback;

  TRACER;
  rv = nghttp2_submit_response(session, stream_id, nva, nvlen, &data_prd);
  if(rv != 0) {
    fprintf(stderr, "Fatal error: %s", nghttp2_strerror(rv));
    return -1;
  }
  TRACER;
  return 0;
}

const char ERROR_HTML[] = "<html><head><title>404</title></head>"
  "<body><h1>404 Not Found</h1></body></html>";

static int error_reply(nghttp2_session *session, http2_stream_data *stream_data)
{
  int rv;
  int pipefd[2];
  nghttp2_nv hdrs[] = {
    MAKE_NV(":status", "404")
  };

  TRACER;
  rv = pipe(pipefd);
  if(rv != 0) {
    rv = nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, 
        stream_data->stream_id, NGHTTP2_INTERNAL_ERROR);
    if(rv != 0) {
    fprintf(stderr, "Fatal error: %s", nghttp2_strerror(rv));
      return -1;
    }
    return 0;
  }
  write(pipefd[1], ERROR_HTML, sizeof(ERROR_HTML) - 1);
  close(pipefd[1]);
  stream_data->fd = pipefd[0];
  TRACER;
  if(send_response(session, stream_data->stream_id, hdrs, ARRLEN(hdrs), 
        pipefd[0]) != 0) {
    close(pipefd[0]);
    return -1;
  }
  TRACER;
  return 0;
}

static int server_on_header_callback(nghttp2_session *session, 
    const nghttp2_frame *frame, const uint8_t *name, size_t namelen, 
    const uint8_t *value, size_t valuelen, void *user_data)
{
  http2_session_data *session_data = (http2_session_data *)user_data;
  mrb_state *mrb = session_data->app_ctx->server->mrb;

  http2_stream_data *stream_data;

  const char PATH[] = ":path";
  TRACER;
  switch(frame->hd.type) {
  case NGHTTP2_HEADERS:
    if(frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
      break;
    }
    stream_data = nghttp2_session_get_stream_user_data(session, 
        frame->hd.stream_id);
    if(!stream_data || stream_data->request_path) {
      break;
    }
    if(namelen == sizeof(PATH) - 1 && memcmp(PATH, name, namelen) == 0) {
      size_t j;
      for(j = 0; j < valuelen && value[j] != '?'; ++j);
      stream_data->request_path = percent_decode(mrb, value, j);
    }
    break;
  }
  TRACER;
  return 0;
}

static int server_on_begin_headers_callback(nghttp2_session *session, 
    const nghttp2_frame *frame, void *user_data)
{
  http2_session_data *session_data = (http2_session_data *)user_data;
  mrb_state *mrb = session_data->app_ctx->server->mrb;

  http2_stream_data *stream_data;

  TRACER;
  if(frame->hd.type != NGHTTP2_HEADERS ||
     frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
    return 0;
  }
  stream_data = create_http2_stream_data(mrb, session_data, frame->hd.stream_id);
  nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, 
      stream_data);

  TRACER;
  return 0;
}

/* Minimum check for directory traversal. Returns nonzero if it is
   safe. */
static int check_path(const char *path)
{
  /* We don't like '\' in url. */
  TRACER;
  return path[0] && path[0] == '/' &&
    strchr(path, '\\') == NULL &&
    strstr(path, "/../") == NULL &&
    strstr(path, "/./") == NULL &&
    !ends_with(path, "/..") && !ends_with(path, "/.");
}

static int server_on_request_recv(nghttp2_session *session, 
    http2_session_data *session_data, http2_stream_data *stream_data)
{
  int fd;
  mrb_state *mrb = session_data->app_ctx->server->mrb;
  mrb_http2_config_t *config = session_data->app_ctx->server->config;

  nghttp2_nv hdrs[] = {
    MAKE_NV(":status", "200")
  };
  char *rel_path;

  TRACER;
  if(!stream_data->request_path) {
    if(error_reply(session, stream_data) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
  }
  if (config->debug) {
    fprintf(stderr, "%s GET %s\n", session_data->client_addr, 
        stream_data->request_path);
  }
  TRACER;
  if(!check_path(stream_data->request_path)) {
    if (config->debug) {
      fprintf(stderr, "%s invalid request_path: %s\n", session_data->client_addr, 
          stream_data->request_path);
    }
    if(error_reply(session, stream_data) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
  }
  for(rel_path = stream_data->request_path; *rel_path == '/'; ++rel_path);
  if (config->document_root) {
    char *full_path = NULL;
    size_t path_len = sizeof(config->document_root) + 
      sizeof(stream_data->request_path);

    full_path = (char *)mrb_malloc(mrb, path_len);
    snprintf(full_path, path_len, "%s%s", config->document_root, 
        stream_data->request_path);
    if (config->debug) {
      fprintf(stderr, "%s %s is mapped to %s\n", session_data->client_addr, 
          stream_data->request_path, full_path);
    }
    fd = open(full_path, O_RDONLY);
  } else {
    fd = open(rel_path, O_RDONLY);
  }
  TRACER;
  if(fd == -1) {
    if(error_reply(session, stream_data) != 0) {
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
  }
  stream_data->fd = fd;

  if(send_response(session, stream_data->stream_id, hdrs, 
        ARRLEN(hdrs), fd) != 0) {
    close(fd);
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }
  TRACER;
  return 0;
}

static int server_on_frame_recv_callback(nghttp2_session *session, 
    const nghttp2_frame *frame, void *user_data)
{
  http2_session_data *session_data = (http2_session_data *)user_data;
  mrb_state *mrb = session_data->app_ctx->server->mrb;
  http2_stream_data *stream_data;

  TRACER;
  switch(frame->hd.type) {
  case NGHTTP2_DATA:
  case NGHTTP2_HEADERS:
    /* Check that the client request has finished */
    if(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
      stream_data = nghttp2_session_get_stream_user_data(session, 
          frame->hd.stream_id);
      /* For DATA and HEADERS frame, this callback may be called after
         on_stream_close_callback. Check that stream still alive. */
      if(!stream_data) {
        return 0;
      }
      return server_on_request_recv(session, session_data, stream_data);
    }
    break;
  default:
    break;
  }
  TRACER;
  return 0;
}

static int server_on_stream_close_callback(nghttp2_session *session, 
    int32_t stream_id, nghttp2_error_code error_code, void *user_data)
{
  http2_session_data *session_data = (http2_session_data *)user_data;
  mrb_state *mrb = session_data->app_ctx->server->mrb;
  http2_stream_data *stream_data;

  TRACER;
  stream_data = nghttp2_session_get_stream_user_data(session, stream_id);
  if(!stream_data) {
    return 0;
  }
  remove_stream(session_data, stream_data);
  delete_http2_stream_data(mrb, stream_data);
  TRACER;
  return 0;
}

static void mrb_http2_server_session_init(http2_session_data *session_data)
{
  nghttp2_session_callbacks callbacks = {0};

  TRACER;
  callbacks.send_callback = server_send_callback;
  callbacks.on_frame_recv_callback = server_on_frame_recv_callback;
  callbacks.on_stream_close_callback = server_on_stream_close_callback;
  callbacks.on_header_callback = server_on_header_callback;
  callbacks.on_begin_headers_callback = server_on_begin_headers_callback;
  nghttp2_session_server_new(&session_data->session, &callbacks, session_data);
}

/* Send HTTP/2.0 client connection header, which includes 24 bytes
   magic octets and SETTINGS frame */
static int send_server_connection_header(http2_session_data *session_data)
{
  nghttp2_settings_entry iv[1] = {
    { NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100 }
  };
  int rv;

  rv = nghttp2_submit_settings(session_data->session, NGHTTP2_FLAG_NONE,
                               iv, ARRLEN(iv));
  TRACER;
  if(rv != 0) {
    fprintf(stderr, "Fatal error: %s", nghttp2_strerror(rv));
    return -1;
  }
  TRACER;
  return 0;
}

/* readcb for bufferevent after client connection header was
   checked. */
static void mrb_http2_server_readcb(struct bufferevent *bev, void *ptr)
{
  http2_session_data *session_data = (http2_session_data *)ptr;

  TRACER;
  if(session_recv(session_data) != 0) {
    delete_http2_session_data(session_data);
    return;
  }
}


static void mrb_http2_server_writecb(struct bufferevent *bev, void *ptr)
{
  http2_session_data *session_data = (http2_session_data *)ptr;

  TRACER;
  if(evbuffer_get_length(bufferevent_get_output(bev)) > 0) {
    return;
  } 
  TRACER;
  if(nghttp2_session_want_read(session_data->session) == 0 &&
     nghttp2_session_want_write(session_data->session) == 0) {
    delete_http2_session_data(session_data);
    return;
  } 
  TRACER;
  if(session_send(session_data) != 0) {
    delete_http2_session_data(session_data);
    return;
  }
}

/* eventcb for bufferevent */
static void mrb_http2_server_eventcb(struct bufferevent *bev, short events, 
    void *ptr)
{
  http2_session_data *session_data = (http2_session_data *)ptr;
  mrb_state *mrb = session_data->app_ctx->server->mrb;
  mrb_http2_config_t *config = session_data->app_ctx->server->config;

  TRACER;
  if(events & BEV_EVENT_CONNECTED) {
    if (config->debug) { 
      fprintf(stderr, "%s connected\n", session_data->client_addr);
    }
    return;
  } 
  if (config->debug) { 
    if(events & BEV_EVENT_EOF) {
      fprintf(stderr, "%s EOF\n", session_data->client_addr);
    } else if(events & BEV_EVENT_ERROR) {
      fprintf(stderr, "%s network error\n", session_data->client_addr);
    } else if(events & BEV_EVENT_TIMEOUT) {
      fprintf(stderr, "%s timeout\n", session_data->client_addr);
    }
  }
  TRACER;
  delete_http2_session_data(session_data);
}

/* readcb for bufferevent to check first 24 bytes client connection
   header. */
static void mrb_http2_server_handshake_readcb(struct bufferevent *bev, void *ptr)
{
  http2_session_data *session_data = (http2_session_data *)ptr;
  mrb_state *mrb = session_data->app_ctx->server->mrb;

  uint8_t data[24];
  struct evbuffer *input = bufferevent_get_input(session_data->bev);
  int readlen = evbuffer_remove(input, data, session_data->handshake_leftlen);
  const char *conhead = NGHTTP2_CLIENT_CONNECTION_HEADER;

  TRACER;
  if(memcmp(conhead + NGHTTP2_CLIENT_CONNECTION_HEADER_LEN 
        - session_data->handshake_leftlen, data, readlen) != 0) {
    delete_http2_session_data(session_data);
    return;
  }
  session_data->handshake_leftlen -= readlen;
  TRACER;
  if(session_data->handshake_leftlen == 0) {
    bufferevent_setcb(session_data->bev, mrb_http2_server_readcb, 
        mrb_http2_server_writecb, mrb_http2_server_eventcb, session_data);
    /* Process pending data in buffer since they are not notified
       further */
    mrb_http2_server_session_init(session_data);
    if(send_server_connection_header(session_data) != 0) {
      delete_http2_session_data(session_data);
      return;
    }
    if(session_recv(session_data) != 0) {
      delete_http2_session_data(session_data);
      return;
    }
  }
}

static SSL* mrb_http2_create_ssl(mrb_state *mrb, SSL_CTX *ssl_ctx)
{
  SSL *ssl;
  ssl = SSL_new(ssl_ctx);

  TRACER;
  if(!ssl) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "Could not create SSL/TLS session object: %S", 
        mrb_str_new_cstr(mrb, ERR_error_string(ERR_get_error(), NULL)));
  }
  TRACER;
  return ssl;
}

static http2_session_data* create_http2_session_data(mrb_state *mrb, 
    app_context *app_ctx, int fd, struct sockaddr *addr, int addrlen)
{
  int rv;
  http2_session_data *session_data;
  SSL *ssl;
  char host[NI_MAXHOST];
  int val = 1;

  TRACER;
  ssl = mrb_http2_create_ssl(mrb, app_ctx->ssl_ctx);
  session_data = (http2_session_data *)mrb_malloc(mrb, sizeof(http2_session_data));
  memset(session_data, 0, sizeof(http2_session_data));
  session_data->app_ctx = app_ctx;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&val, sizeof(val));
  session_data->bev = bufferevent_openssl_socket_new (app_ctx->evbase, fd, ssl,
     BUFFEREVENT_SSL_ACCEPTING, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  session_data->handshake_leftlen = NGHTTP2_CLIENT_CONNECTION_HEADER_LEN;
  rv = getnameinfo(addr, addrlen, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
  if(rv != 0) {
    session_data->client_addr = strdup("(unknown)");
  } else {
    session_data->client_addr = strdup(host);
  }
  TRACER;

  return session_data;
}

static void mrb_http2_acceptcb(struct evconnlistener *listener, int fd, 
    struct sockaddr *addr, int addrlen, void *ptr)
{
  app_context *app_ctx = (app_context *)ptr;
  http2_session_data *session_data;
  mrb_state *mrb = app_ctx->server->mrb;

  TRACER;
  session_data = create_http2_session_data(mrb, app_ctx, 
      fd, addr, addrlen);
  bufferevent_setcb(session_data->bev, mrb_http2_server_handshake_readcb, 
      NULL, mrb_http2_server_eventcb, session_data);
  TRACER;
}

static int next_proto_cb(SSL *s, const unsigned char **data, 
    unsigned int *len, void *arg)
{
  *data = next_proto_list;
  *len = next_proto_list_len;
  TRACER;
  return SSL_TLSEXT_ERR_OK;
}

static SSL_CTX* mrb_http2_create_ssl_ctx(mrb_state *mrb, 
    const char *key_file, const char *cert_file)
{
  SSL_CTX *ssl_ctx;
  ssl_ctx = SSL_CTX_new(SSLv23_server_method());
  TRACER;
  if(!ssl_ctx) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "Could not create SSL/TLS context: %S", 
        mrb_str_new_cstr(mrb, ERR_error_string(ERR_get_error(), NULL)));
  }
  SSL_CTX_set_options(ssl_ctx, 
      SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_COMPRESSION |
      SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION);

  if(SSL_CTX_use_PrivateKey_file(ssl_ctx, key_file, 
        SSL_FILETYPE_PEM) != 1) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "Could not read private key file %S", 
        mrb_str_new_cstr(mrb, key_file));
  }
  if(SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file) != 1) {
    mrb_raisef(mrb, E_RUNTIME_ERROR, "Could not read certificate file %S", 
        mrb_str_new_cstr(mrb, cert_file));
  }

  next_proto_list[0] = NGHTTP2_PROTO_VERSION_ID_LEN;
  memcpy(&next_proto_list[1], NGHTTP2_PROTO_VERSION_ID, 
      NGHTTP2_PROTO_VERSION_ID_LEN);
  next_proto_list_len = 1 + NGHTTP2_PROTO_VERSION_ID_LEN;

  SSL_CTX_set_next_protos_advertised_cb(ssl_ctx, next_proto_cb, NULL);
  TRACER;
  return ssl_ctx;
}

static void init_app_context(app_context *actx, SSL_CTX *ssl_ctx, 
    struct event_base *evbase)
{
  memset(actx, 0, sizeof(app_context));
  actx->ssl_ctx = ssl_ctx;
  actx->evbase = evbase;
}

static void mrb_start_listen(struct event_base *evbase, const char *service, app_context *app_ctx)
{
  int rv;
  struct addrinfo hints;
  struct addrinfo *res, *rp;
  mrb_state *mrb = app_ctx->server->mrb;

  TRACER;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
#ifdef AI_ADDRCONFIG
  hints.ai_flags |= AI_ADDRCONFIG;
#endif

  rv = getaddrinfo(NULL, service, &hints, &res);
  if(rv != 0) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "getaddrinfo failed");
  }
  TRACER;
  for(rp = res; rp; rp = rp->ai_next) {
    struct evconnlistener *listener;
    listener = evconnlistener_new_bind(evbase, mrb_http2_acceptcb, app_ctx, 
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, 16, rp->ai_addr, 
        rp->ai_addrlen);

    if(listener) {
      return;
    }
  }
  mrb_raise(mrb, E_RUNTIME_ERROR, "Could not start listener");
}

static mrb_value mrb_http2_server_run(mrb_state *mrb, mrb_value self)
{
  mrb_http2_server_t *server = DATA_PTR(self);  
  SSL_CTX *ssl_ctx;
  struct event_base *evbase;
  app_context app_ctx;

  ssl_ctx = mrb_http2_create_ssl_ctx(mrb, server->config->key, server->config->cert);
  evbase = event_base_new();

  init_app_context(&app_ctx, ssl_ctx, evbase);
  app_ctx.server = server;

  TRACER;
  mrb_start_listen(evbase, server->config->service, &app_ctx);
  event_base_loop(app_ctx.evbase, 0);
  event_base_free(app_ctx.evbase);
  SSL_CTX_free(app_ctx.ssl_ctx);
  TRACER;

  return self;
}

static mrb_value mrb_http2_server_init(mrb_state *mrb, mrb_value self)
{
  mrb_http2_server_t *server;
  struct sigaction act;
  mrb_value args, port, debug, docroot;
  char *service, *key_file, *cert_file;

  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &act, NULL);

  SSL_load_error_strings();
  SSL_library_init();

  mrb_get_args(mrb, "H", &args);
  port = mrb_hash_get(mrb, args, mrb_symbol_value(mrb_intern_lit(mrb, "port")));
  service = mrb_str_to_cstr(mrb, mrb_fixnum_to_str(mrb, port, 10));
  key_file = mrb_str_to_cstr(mrb, mrb_hash_get(mrb, args, 
        mrb_symbol_value(mrb_intern_lit(mrb, "key"))));
  cert_file = mrb_str_to_cstr(mrb, mrb_hash_get(mrb, args, 
        mrb_symbol_value(mrb_intern_lit(mrb, "crt"))));
  debug = mrb_hash_get(mrb, args, mrb_symbol_value(mrb_intern_lit(mrb, "debug")));

  // server context
  server = (mrb_http2_server_t *)mrb_malloc(mrb, sizeof(mrb_http2_server_t));
  memset(server, 0, sizeof(mrb_http2_server_t));
  server->args = args;
  server->mrb = mrb;

  // server config create
  server->config = (mrb_http2_config_t *)mrb_malloc(mrb, 
      sizeof(mrb_http2_config_t));
  memset(server->config, 0, sizeof(mrb_http2_config_t));

  server->config->service = service;
  server->config->key = key_file;
  server->config->cert = cert_file;

  if (mrb_nil_p(docroot = mrb_hash_get(mrb, args,
                  mrb_symbol_value(mrb_intern_lit(mrb, "document_root"))))) {
    mrb_raise(mrb, E_ARGUMENT_ERROR, "document_root not found");
  } else {
    server->config->document_root = mrb_str_to_cstr(mrb, docroot);
  }

  if (!mrb_nil_p(debug)) {
    server->config->debug = MRB_HTTP2_CONFIG_ENABLED;
  } else {
    server->config->debug = MRB_HTTP2_CONFIG_DISABLED;
  }

  DATA_TYPE(self) = &mrb_http2_server_type;
  DATA_PTR(self) = server;
  TRACER;

  return self;
}

void mrb_http2_server_class_init(mrb_state *mrb, struct RClass *http2)
{
  struct RClass *server;

  server = mrb_define_class_under(mrb, http2, "Server", mrb->object_class);
  MRB_SET_INSTANCE_TT(server, MRB_TT_DATA);

  mrb_define_method(mrb, server, "initialize", mrb_http2_server_init, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, server, "run", mrb_http2_server_run, MRB_ARGS_NONE());
  DONE;
}