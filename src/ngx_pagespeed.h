/*
 * Copyright 2012 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: jefftk@google.com (Jeff Kaufman)

#ifndef NGX_PAGESPEED_H_
#define NGX_PAGESPEED_H_

// We might be compiled with syslog.h, which #defines LOG_INFO and LOG_WARNING
// as ints.  But logging.h assumes they're usable as names, within their
// namespace, so we need to #undef them before including logging.h
#ifdef LOG_INFO
#undef LOG_INFO
#endif
#ifdef LOG_WARNING
#undef LOG_WARNING
#endif
#include "base/logging.h"

extern "C" {
  #include <ngx_core.h>
  #include <ngx_http.h>
}

#include "net/instaweb/http/public/response_headers.h"
#include "net/instaweb/util/public/string_util.h"

namespace net_instaweb {

class GzipInflater;
class NgxBaseFetch;
class ProxyFetch;
class RewriteDriver;

}  // namespace net_instaweb

namespace ngx_psol {

// Allocate chain links and buffers from the supplied pool, and copy over the
// data from the string piece.  If the string piece is empty, return
// NGX_DECLINED immediately unless send_last_buf.
ngx_int_t string_piece_to_buffer_chain(
    ngx_pool_t* pool, StringPiece sp,
    ngx_chain_t** link_ptr, bool send_last_buf);

StringPiece str_to_string_piece(ngx_str_t s);

// s1: ngx_str_t, s2: string literal
// true if they're equal, false otherwise
#define STR_EQ_LITERAL(s1, s2)          \
    ((s1).len == (sizeof(s2)-1) &&      \
     ngx_strncmp((s1).data, (s2), (sizeof(s2)-1)) == 0)

// s1: ngx_str_t, s2: string literal
// true if they're equal ignoring case, false otherwise
#define STR_CASE_EQ_LITERAL(s1, s2)     \
    ((s1).len == (sizeof(s2)-1) &&      \
     ngx_strncasecmp((s1).data, (       \
       reinterpret_cast<u_char*>(       \
         const_cast<char*>(s2))),       \
       (sizeof(s2)-1)) == 0)

// Allocate memory out of the pool for the string piece, and copy the contents
// over.  Returns NULL if we can't get memory.
char* string_piece_to_pool_string(ngx_pool_t* pool, StringPiece sp);
ngx_int_t copy_response_headers_to_ngx(
    ngx_http_request_t* r,
    const net_instaweb::ResponseHeaders& pagespeed_headers);

typedef struct {
  net_instaweb::ProxyFetch* proxy_fetch;
  net_instaweb::NgxBaseFetch* base_fetch;
  net_instaweb::RewriteDriver* driver;
  bool data_received;
  int pipe_fd;
  ngx_connection_t* pagespeed_connection;
  ngx_http_request_t* r;
  bool is_resource_fetch;
  bool sent_headers;
  bool write_pending;
  net_instaweb::GzipInflater* inflater_;
  ngx_buf_t *header_in;
} ps_request_ctx_t;

}  // namespace ngx_psol

#endif  // NGX_PAGESPEED_H_
