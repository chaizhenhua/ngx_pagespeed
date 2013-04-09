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

#include "ngx_base_fetch.h"

#include "ngx_pagespeed.h"

#include "net/instaweb/util/public/google_message_handler.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/http/public/response_headers.h"

namespace net_instaweb {

NgxBaseFetch::NgxBaseFetch(ngx_http_request_t* r, int pipe_fd,
                           const RequestContextPtr& request_ctx)
    : AsyncFetch(request_ctx), request_(r), done_called_(false),
      last_buf_sent_(false), pipe_fd_(pipe_fd), references_(2) {
  if (pthread_mutex_init(&mutex_, NULL)) CHECK(0);
  PopulateRequestHeaders();
}

NgxBaseFetch::~NgxBaseFetch() {
  pthread_mutex_destroy(&mutex_);
}

void NgxBaseFetch::Lock() {
  pthread_mutex_lock(&mutex_);
}

void NgxBaseFetch::Unlock() {
  pthread_mutex_unlock(&mutex_);
}

void NgxBaseFetch::PopulateRequestHeaders() {
  CopyHeadersFromTable<RequestHeaders>(&request_->headers_in.headers,
                                       request_headers());
}

void NgxBaseFetch::PopulateResponseHeaders() {
  CopyHeadersFromTable<ResponseHeaders>(&request_->headers_out.headers,
                                        response_headers());

  response_headers()->set_status_code(request_->headers_out.status);

  // Manually copy over the content type because it's not included in
  // request_->headers_out.headers.
  response_headers()->Add(
      HttpAttributes::kContentType,
      ngx_psol::str_to_string_piece(request_->headers_out.content_type));

  // TODO(oschaaf): ComputeCaching should be called in setupforhtml()?
  response_headers()->ComputeCaching();
}

template<class HeadersT>
void NgxBaseFetch::CopyHeadersFromTable(ngx_list_t* headers_from,
                                        HeadersT* headers_to) {
  // http_version is the version number of protocol; 1.1 = 1001. See
  // NGX_HTTP_VERSION_* in ngx_http_request.h
  headers_to->set_major_version(request_->http_version / 1000);
  headers_to->set_minor_version(request_->http_version % 1000);

  // Standard nginx idiom for iterating over a list.  See ngx_list.h
  ngx_uint_t i;
  ngx_list_part_t* part = &headers_from->part;
  ngx_table_elt_t* header = static_cast<ngx_table_elt_t*>(part->elts);

  for (i = 0 ; /* void */; i++) {
    if (i >= part->nelts) {
      if (part->next == NULL) {
        break;
      }

      part = part->next;
      header = static_cast<ngx_table_elt_t*>(part->elts);
      i = 0;
    }

    StringPiece key = ngx_psol::str_to_string_piece(header[i].key);
    StringPiece value = ngx_psol::str_to_string_piece(header[i].value);

    headers_to->Add(key, value);
  }
}

bool NgxBaseFetch::HandleWrite(const StringPiece& sp,
                               MessageHandler* handler) {
  Lock();
  buffer_.append(sp.data(), sp.size());
  Unlock();
  return true;
}

ngx_int_t NgxBaseFetch::CopyBufferToNginx(ngx_chain_t** link_ptr) {
  if (done_called_ && last_buf_sent_) {
    return NGX_DECLINED;
  }

  int rc = ngx_psol::string_piece_to_buffer_chain(
      request_->pool, buffer_, link_ptr, done_called_ /* send_last_buf */);
  if (rc != NGX_OK) {
    return rc;
  }

  // Done with buffer contents now.
  buffer_.clear();

  if (done_called_) {
    last_buf_sent_ = true;
  }

  return NGX_OK;
}

// There may also be a race condition if this is called between the last Write()
// and Done() such that we're sending an empty buffer with last_buf set, which I
// think nginx will reject.
ngx_int_t NgxBaseFetch::CollectAccumulatedWrites(ngx_chain_t** link_ptr) {
  Lock();
  pending_signals_ = 0;

  ngx_int_t rc = CopyBufferToNginx(link_ptr);
  Unlock();

  if (rc == NGX_DECLINED) {
    *link_ptr = NULL;
    return NGX_OK;
  }
  return rc;
}

ngx_int_t NgxBaseFetch::CollectHeaders(ngx_http_headers_out_t* headers_out) {
  Lock();
  const ResponseHeaders* pagespeed_headers = response_headers();
  pending_signals_ --;
  Unlock();
  return ngx_psol::copy_response_headers_to_ngx(request_, *pagespeed_headers);
}

void NgxBaseFetch::RequestCollection() {
  if (pending_signals_) {
    return;
  }
  signaler_->Signal(request_);
}

void NgxBaseFetch::HandleHeadersComplete() {
  RequestCollection();  // Headers available.
}

bool NgxBaseFetch::HandleFlush(MessageHandler* handler) {
  RequestCollection();  // A new part of the response body is available.
  return true;
}

void NgxBaseFetch::Release() {
  DecrefAndDeleteIfUnreferenced();
}

void NgxBaseFetch::DecrefAndDeleteIfUnreferenced() {
  // Creates a full memory barrier.
  if (__sync_add_and_fetch(&references_, -1) == 0) {
    delete this;
  }
}

void NgxBaseFetch::HandleDone(bool success) {
  // TODO(jefftk): it's possible that instead of locking here we can just modify
  // CopyBufferToNginx to only read done_called_ once.
  Lock();
  done_called_ = true;
  Unlock();

  DecrefAndDeleteIfUnreferenced();
}

NgxBaseFetchEvent::NgxBaseFetchEvent(ngx_log_t *cycle) : log_(cycle->log) {
    int fds[2];
    if (::pipe(fds) != 0) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_errno, "pipe() failed");
        fds[0] = fds[1] = -1;
        /* fatal */
        exit(2);
    }
    readfd_ = fds[1];

    if (ngx_nonblocking(readfd_) == -1) {
        ngx_log_error(NGX_LOG_EMERG, cycle->log, ngx_socket_errno,
                      ngx_nonblocking_n " pipe[0] failed");
        /* fatal */
        exit(2);
    }

    if (ngx_add_channel_event(cycle, readfd_, NGX_READ_EVENT, EventHandler) == NGX_ERROR ) {
        /* fatal */
        exit(2);
    }
}

NgxBaseFetchEvent::~NgxBaseFetchEvent() {
    ngx_close_channel(&writefd_, log_);
}

void NgxBaseFetchEvent::Signal(ngx_http_request_t *r) {
    ngx_int_t rc = 0;
    while (rc != sizeof(ngx_http_request_t *)) {
        rc = write(writefd_, static_cast<void *>(&r), sizeof(ngx_http_request_t *));
    }
}

void NgxBaseFetchEvent::EventHandler(ngx_event_t *ev) {
    ngx_int_t		   n;
    ngx_channel_t	   ch;
    ngx_connection_t  *c;
    ngx_int_t          rc;
    ngx_uint_t         i;

    if (ev->timedout) {
        ev->timedout = 0;
        return;
    }

    c = ev->data;

    ngx_log_debug0(NGX_LOG_DEBUG_CORE, ev->log, 0, "channel handler");

    for ( ;; ) {
        // TODO: PIPE_BUF_SIZE
        ngx_http_request_t **requests[512];
        rc = read(c->fd, static_cast<void *>(requests), sizeof(requests));

        if (rc == -1) {
            if (ngx_errno == EINTR) {
                continue;
            } else if (ngx_errno == EAGAIN) {
                break;
            }

            // NGX_ERROR
            if (ngx_event_flags & NGX_USE_EPOLL_EVENT) {
                ngx_del_conn(c, 0);
            }

            ngx_close_connection(c);
            break;
        }

        for (i = 0; i < rc / sizeof(ngx_http_request_t *); i++) {
            ngx_psol::process_base_fetch_output(requests[i]);
        }
    }
}

}  // namespace net_instaweb
