#include "common.h"


typedef enum http_state_t_ {
  REQUEST_,
  RESPONSE_,
  WSOCK_,
  END_,
} http_state_t_;


typedef struct http_t_ http_t_;
typedef struct req_t_  req_t_;

struct req_t_ {
  http_t_*   ctx;
  upd_req_t* req;

  uint8_t* method;
  size_t   method_len;

  uint8_t* path;
  size_t   path_len;

  int minor_version;

  struct phr_header headers[64];
  size_t headers_cnt;

  upd_file_t* file;
};

struct http_t_ {
  upd_file_t*   file;
  http_state_t_ state;
  upd_buf_t     in;
  upd_buf_t     out;
};


static
bool
prog_init_(
  upd_file_t* f);

static
void
prog_deinit_(
  upd_file_t* f);

static
bool
prog_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_program_http = {
  .name = (uint8_t*) "upd.program.http",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_PROGRAM,
    0,
  },
  .init   = prog_init_,
  .deinit = prog_deinit_,
  .handle = prog_handle_,
};


static
bool
stream_init_(
  upd_file_t* f);

static
void
stream_deinit_(
  upd_file_t* f);

static
bool
stream_handle_(
  upd_req_t* req);

static const upd_driver_t stream_driver_ = {
  .name = (uint8_t*) "upd.program.http.stream",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_STREAM,
    0,
  },
  .init   = stream_init_,
  .deinit = stream_deinit_,
  .handle = stream_handle_,
};


static
bool
stream_parse_req_(
  http_t_*   ctx,
  upd_req_t* req);

static
bool
stream_pipe_wsock_input_(
  http_t_*   ctx,
  upd_req_t* req);

static
bool
stream_output_http_error_(
  http_t_*    ctx,
  uint16_t    code,
  const char* msg);


static
void
req_pathfind_cb_(
  upd_req_pathfind_t* pf);

static
void
req_bin_access_cb_(
  upd_req_t* req);

static
void
req_lock_for_read_cb_(
  upd_file_lock_t* lock);

static
void
req_read_cb_(
  upd_req_t* req);


static bool prog_init_(upd_file_t* f) {
  (void) f;
  return true;
}

static void prog_deinit_(upd_file_t* f) {
  (void) f;
}

static bool prog_handle_(upd_req_t* req) {
  upd_iso_t* iso = req->file->iso;

  switch (req->type) {
  case UPD_REQ_PROGRAM_ACCESS:
    req->program.access = (upd_req_program_access_t) {
      .exec = true,
    };
    break;
  case UPD_REQ_PROGRAM_EXEC: {
    upd_file_t* f = upd_file_new(iso, &stream_driver_);
    if (HEDLEY_UNLIKELY(f == NULL)) {
      return false;
    }
    req->program.exec = f;
  } break;
  default:
    return false;
  }
  req->cb(req);
  return true;
}


static bool stream_init_(upd_file_t* f) {
  http_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (http_t_) {
    .file = f,
  };
  f->ctx = ctx;
  return true;
}

static void stream_deinit_(upd_file_t* f) {
  http_t_* ctx = f->ctx;
  upd_free(&ctx);
}

static bool stream_handle_(upd_req_t* req) {
  http_t_* ctx = req->file->ctx;

  switch (req->type) {
  case UPD_REQ_STREAM_ACCESS:
    req->stream.access = (upd_req_stream_access_t) {
      .input  = true,
      .output = true,
    };
    break;

  case UPD_REQ_STREAM_INPUT:
    switch (ctx->state) {
    case REQUEST_:
      return stream_parse_req_(ctx, req);
    case RESPONSE_:
      return upd_buf_append(&ctx->in, req->stream.io.buf, req->stream.io.size);
    case WSOCK_:
      return stream_pipe_wsock_input_(ctx, req);
    default:
      return false;
    }

  case UPD_REQ_STREAM_OUTPUT: {
    if (HEDLEY_UNLIKELY(ctx->state == END_ && !ctx->out.size)) {
      return false;
    }
    req->stream.io = (upd_req_stream_io_t) {
      .buf  = ctx->out.ptr,
      .size = ctx->out.size,
    };
    upd_buf_t oldbuf = ctx->out;
    ctx->out = (upd_buf_t) {0};

    req->cb(req);
    upd_buf_clear(&oldbuf);
  } return true;

  default:
    return false;
  }
  req->cb(req);
  return true;
}


static void stream_end_(http_t_* ctx) {
  ctx->state = END_;
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);
}

static bool stream_parse_req_(http_t_* ctx, upd_req_t* req) {
  upd_req_stream_io_t* io = &req->stream.io;

  req_t_* hreq = upd_iso_stack(ctx->file->iso, sizeof(*hreq));
  if (HEDLEY_UNLIKELY(hreq == NULL)) {
    return false;
  }
  *hreq = (req_t_) {
    .ctx         = ctx,
    .req         = req,
    .headers_cnt = sizeof(hreq->headers)/sizeof(hreq->headers[0]),
  };

  const int result = phr_parse_request(
    (char*) io->buf, io->size,
    (const char**) &hreq->method, &hreq->method_len,
    (const char**) &hreq->path, &hreq->path_len,
    &hreq->minor_version,
    hreq->headers, &hreq->headers_cnt, 0);

  if (HEDLEY_UNLIKELY(result == -1)) {
    upd_iso_unstack(ctx->file->iso, hreq);
    req->cb(req);
    return stream_output_http_error_(ctx, 400, "invalid request");
  }

  if (HEDLEY_LIKELY(result == -2)) {
    upd_iso_unstack(ctx->file->iso, hreq);
    io->size = 0;
    req->cb(req);
    return true;
  }

  io->size   = result;
  ctx->state = RESPONSE_;

  const bool is_get =
    hreq->method_len == 3 &&
    utf8ncasecmp(hreq->method, "GET", 3) == 0;
  if (HEDLEY_UNLIKELY(!is_get)) {
    upd_iso_unstack(ctx->file->iso, hreq);
    req->cb(req);
    return stream_output_http_error_(ctx, 405, "unknown method");
  }

  upd_file_ref(ctx->file);
  const bool pathfind = upd_req_pathfind_with_dup(&(upd_req_pathfind_t) {
      .iso   = ctx->file->iso,
      .path  = hreq->path,
      .len   = hreq->path_len,
      .udata = hreq,
      .cb    = req_pathfind_cb_,
    });
  if (HEDLEY_UNLIKELY(!pathfind)) {
    upd_file_unref(ctx->file);
    upd_iso_unstack(ctx->file->iso, hreq);
    req->cb(req);
    return stream_output_http_error_(ctx, 500, "pathfind failure");
  }
  return true;
}

static bool stream_pipe_wsock_input_(http_t_* ctx, upd_req_t* req) {
  (void) ctx;
  (void) req;
  /* TODO */
  return false;
}

static bool stream_output_http_error_(
    http_t_* ctx, uint16_t code, const char* msg) {
  uint8_t temp[1024] = {0};
  const size_t len = snprintf((char*) temp, sizeof(temp),
    "HTTP/1.1 %"PRIu16" %s\r\n"
    "Content-Type: text/plain\r\n"
    "\r\n"
    "UNPARANOID HTTP stream error: %s (%"PRIu16")\r\n",
    code, msg, msg, code);

  const bool ret = upd_buf_append(&ctx->out, temp, len);
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);

  stream_end_(ctx);
  return ret;
}


static void req_pathfind_cb_(upd_req_pathfind_t* pf) {
  req_t_*  req = pf->udata;
  http_t_* ctx = req->ctx;

  req->file = pf->len? NULL: pf->base;
  upd_iso_unstack(ctx->file->iso, pf);

  if (HEDLEY_UNLIKELY(!req->file)) {
    stream_output_http_error_(ctx, 404, "not found");
    goto ABORT;
  }

  /* TODO: check upgrade header */

  const bool access = upd_req_with_dup(&(upd_req_t) {
      .file  = req->file,
      .type  = UPD_REQ_BIN_ACCESS,
      .udata = req,
      .cb    = req_bin_access_cb_,
    });
  if (HEDLEY_UNLIKELY(!access)) {
    stream_output_http_error_(ctx, 403, "refused access request");
    goto ABORT;
  }
  return;

ABORT:
  req->req->cb(req->req);
  upd_iso_unstack(ctx->file->iso, req);
  upd_file_unref(ctx->file);
}

static void req_bin_access_cb_(upd_req_t* req) {
  req_t_*  hreq = req->udata;
  http_t_* ctx  = hreq->ctx;

  const bool readable = req->bin.access.read;
  upd_iso_unstack(ctx->file->iso, req);

  if (HEDLEY_UNLIKELY(!readable)) {
    stream_output_http_error_(ctx, 403, "file is not readable");
    goto ABORT;
  }

  const bool lock = upd_file_lock_with_dup(&(upd_file_lock_t) {
      .file  = req->file,
      .udata = req,
      .cb    = req_lock_for_read_cb_,
    });
  if (HEDLEY_UNLIKELY(!lock)) {
    stream_output_http_error_(ctx, 500, "lock context allocation failure");
    goto ABORT;
  }
  return;

ABORT:
  hreq->req->cb(hreq->req);
  upd_iso_unstack(ctx->file->iso, hreq);
  upd_file_unref(ctx->file);
}

static void req_lock_for_read_cb_(upd_file_lock_t* lock) {
  req_t_*  req = lock->udata;
  http_t_* ctx = req->ctx;

  if (HEDLEY_UNLIKELY(!lock->ok)) {
    stream_output_http_error_(ctx, 409, "lock failure");
    goto ABORT;
  }

  uint8_t temp[1024];
  const size_t len = snprintf((char*) temp, sizeof(temp),
    "HTTP/1.1 200 OK\r\n"
    "Content-type: text/plain\r\n"
    "\r\n");

  const bool header =
    upd_buf_append(&ctx->out, temp, len);
  if (HEDLEY_UNLIKELY(!header)) {
    stream_output_http_error_(ctx, 500, "buffer allocation failure");
    goto ABORT;
  }
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);

  const bool read = upd_req_with_dup(&(upd_req_t) {
      .file  = req->file,
      .type  = UPD_REQ_BIN_READ,
      .bin   = { .rw = {
        .size = UINT64_MAX,
      }, },
      .udata = lock,
      .cb    = req_read_cb_,
    });
  if (HEDLEY_UNLIKELY(!read)) {
    stream_end_(ctx);
    goto ABORT;
  }
  return;

ABORT:
  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  req->req->cb(req->req);
  upd_iso_unstack(ctx->file->iso, req);

  upd_file_unref(ctx->file);
}

static void req_read_cb_(upd_req_t* req) {
  upd_file_lock_t* lock = req->udata;
  req_t_*          hreq = lock->udata;
  http_t_*         ctx  = hreq->ctx;

  const upd_req_bin_rw_t rw = req->bin.rw;
  if (!rw.size) {
    goto FINALIZE;
  }

  if (HEDLEY_UNLIKELY(!upd_buf_append(&ctx->out, rw.buf, rw.size))) {
    upd_iso_msgf(ctx->file->iso,
      "HTTP response was too long, so we trimmed it! X(\n");
    goto FINALIZE;
  }
  upd_file_trigger(ctx->file, UPD_FILE_UPDATE);

  *req = (upd_req_t) {
      .file  = req->file,
      .type  = UPD_REQ_BIN_READ,
      .bin   = { .rw = {
        .offset = rw.offset + rw.size,
        .size   = UINT64_MAX,
      }, },
      .udata = lock,
      .cb    = req_read_cb_,
    };
  if (HEDLEY_UNLIKELY(!upd_req(req))) {
    goto FINALIZE;
  }
  return;

FINALIZE:
  upd_iso_unstack(ctx->file->iso, req);

  upd_file_unlock(lock);
  upd_iso_unstack(ctx->file->iso, lock);

  hreq->req->cb(hreq->req);
  upd_iso_unstack(ctx->file->iso, hreq);

  stream_end_(ctx);
  upd_file_unref(ctx->file);
}