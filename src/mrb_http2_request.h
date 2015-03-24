/*
// mrb_http2_request.h - to provide http2 methods
//
// See Copyright Notice in mrb_http2.c
*/

#ifndef MRB_HTTP2_REQUEST_H
#define MRB_HTTP2_REQUEST_H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mrb_http2_upstream.h"
#include "mruby.h"

typedef enum mrb_http2_response_type {
  MRB_HTTP2_RESPONSE_STATIC,
  MRB_HTTP2_RESPONSE_TYPE_NONE
} mrb_http2_response_type;

typedef enum mrb_http2_server_phase {
  MRB_HTTP2_SERVER_INIT_REQUEST,
  MRB_HTTP2_SERVER_READ_REQUEST,
  MRB_HTTP2_SERVER_MAP_TO_STORAGE,
  MRB_HTTP2_SERVER_ACCESS_CHECKER,
  MRB_HTTP2_SERVER_FIXUPS,
  MRB_HTTP2_SERVER_CONTENT,
  MRB_HTTP2_SERVER_LOGGING
} mrb_http2_server_phase;

#define HTTP_CONTINUE                        100
#define HTTP_SWITCHING_PROTOCOLS             101
#define HTTP_PROCESSING                      102
#define HTTP_OK                              200
#define HTTP_CREATED                         201
#define HTTP_ACCEPTED                        202
#define HTTP_NON_AUTHORITATIVE               203
#define HTTP_NO_CONTENT                      204
#define HTTP_RESET_CONTENT                   205
#define HTTP_PARTIAL_CONTENT                 206
#define HTTP_MULTI_STATUS                    207
#define HTTP_ALREADY_REPORTED                208
#define HTTP_IM_USED                         226
#define HTTP_MULTIPLE_CHOICES                300
#define HTTP_MOVED_PERMANENTLY               301
#define HTTP_MOVED_TEMPORARILY               302
#define HTTP_SEE_OTHER                       303
#define HTTP_NOT_MODIFIED                    304
#define HTTP_USE_PROXY                       305
#define HTTP_TEMPORARY_REDIRECT              307
#define HTTP_PERMANENT_REDIRECT              308
#define HTTP_BAD_REQUEST                     400
#define HTTP_UNAUTHORIZED                    401
#define HTTP_PAYMENT_REQUIRED                402
#define HTTP_FORBIDDEN                       403
#define HTTP_NOT_FOUND                       404
#define HTTP_METHOD_NOT_ALLOWED              405
#define HTTP_NOT_ACCEPTABLE                  406
#define HTTP_PROXY_AUTHENTICATION_REQUIRED   407
#define HTTP_REQUEST_TIME_OUT                408
#define HTTP_CONFLICT                        409
#define HTTP_GONE                            410
#define HTTP_LENGTH_REQUIRED                 411
#define HTTP_PRECONDITION_FAILED             412
#define HTTP_REQUEST_ENTITY_TOO_LARGE        413
#define HTTP_REQUEST_URI_TOO_LARGE           414
#define HTTP_UNSUPPORTED_MEDIA_TYPE          415
#define HTTP_RANGE_NOT_SATISFIABLE           416
#define HTTP_EXPECTATION_FAILED              417
#define HTTP_UNPROCESSABLE_ENTITY            422
#define HTTP_LOCKED                          423
#define HTTP_FAILED_DEPENDENCY               424
#define HTTP_UPGRADE_REQUIRED                426
#define HTTP_PRECONDITION_REQUIRED           428
#define HTTP_TOO_MANY_REQUESTS               429
#define HTTP_REQUEST_HEADER_FIELDS_TOO_LARGE 431
#define HTTP_INTERNAL_SERVER_ERROR           500
#define HTTP_NOT_IMPLEMENTED                 501
#define HTTP_BAD_GATEWAY                     502
#define HTTP_SERVICE_UNAVAILABLE             503
#define HTTP_GATEWAY_TIME_OUT                504
#define HTTP_VERSION_NOT_SUPPORTED           505
#define HTTP_VARIANT_ALSO_VARIES             506
#define HTTP_INSUFFICIENT_STORAGE            507
#define HTTP_LOOP_DETECTED                   508
#define HTTP_NOT_EXTENDED                    510
#define HTTP_NETWORK_AUTHENTICATION_REQUIRED 511

typedef struct {
  // client ipaddress
  char *client_ip;

} mrb_http2_conn_rec;

typedef struct {
  // http status code
  unsigned int status;

  // http status code line
  char status_line[4];

  // request uri
  char *uri;

  // request unparsed uri was encoded
  char *percent_encode_uri;

  // request unparsed uri was decoded
  char *unparsed_uri;

  // request args
  char *args;

  // request method
  char *method;

  // request scheme
  char *scheme;

  // request authority(hostname and port)
  char *authority;

  // request body
  char *request_body;

  // filename is mapped from uri
  char *filename;

  // file stat infomation from fstat
  struct stat *finfo;

  // previous request time for strftime cache per sec
  time_t prev_req_time;
  time_t prev_last_modified;

  // date header
  char date[64];

  // last_modified header
  char last_modified[64];

  // content_length header
  char content_length[64];

  // connection record
  mrb_http2_conn_rec *conn;

  // request header table
  nghttp2_nv *reqhdr;

  // the number of request header
  size_t reqhdrlen;

  // response header table
  nghttp2_nv reshdrs[MRB_HTTP2_HEADER_MAX];

  // the number of response header
  size_t reshdrslen;

  // upstream information when using proxy
  mrb_http2_upstream *upstream;

  // write fd when using pipe
  int write_fd;

  // write size when sing pipe from mruby
  int64_t write_size;

  // enable mruby script using new mrb_state each request
  unsigned int mruby;

  // enable mruby script using shared mrb_state
  unsigned int shared_mruby;

  // current request phase
  mrb_http2_server_phase phase;

  // response type
  mrb_http2_response_type response_type;

} mrb_http2_request_rec;

mrb_http2_request_rec *mrb_http2_request_rec_init(mrb_state *mrb);
void mrb_http2_request_rec_free(mrb_state *mrb, mrb_http2_request_rec *r);

#endif
