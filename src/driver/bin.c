#include "common.h"


#define BUF_MAX_ (1024*1024*8)  /* = 8 MiB */


typedef struct bin_t_  bin_t_;
typedef struct task_t_ task_t_;


struct bin_t_ {
  uv_file          fd;
  upd_file_watch_t watch;

  size_t bytes;

  task_t_* last_task;

  unsigned read  : 1;
  unsigned write : 1;
  unsigned open  : 1;
};

struct task_t_ {
  uv_fs_t fsreq;

  upd_file_t* file;
  upd_req_t*  req;
  task_t_*    next;

  uint8_t* buf;

  void
  (*exec)(
    task_t_* task);
};


static
bool
bin_init_(
  upd_file_t* f,
  bool        r,
  bool        w);

static
bool
bin_init_r_(
  upd_file_t* f);

static
bool
bin_init_w_(
  upd_file_t* f);

static
bool
bin_init_rw_(
  upd_file_t* f);

static
void
bin_deinit_(
  upd_file_t* f);

static
bool
bin_handle_(
  upd_req_t* req);

const upd_driver_t upd_driver_bin_r = {
  .name = (uint8_t*) "upd.bin.r",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_BIN,
    0,
  },
  .init   = bin_init_r_,
  .deinit = bin_deinit_,
  .handle = bin_handle_,
};

const upd_driver_t upd_driver_bin_rw = {
  .name = (uint8_t*) "upd.bin.rw",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_BIN,
    0,
  },
  .init   = bin_init_rw_,
  .deinit = bin_deinit_,
  .handle = bin_handle_,
};

const upd_driver_t upd_driver_bin_w = {
  .name = (uint8_t*) "upd.bin.w",
  .cats = (upd_req_cat_t[]) {
    UPD_REQ_BIN,
    0,
  },
  .init   = bin_init_w_,
  .deinit = bin_deinit_,
  .handle = bin_handle_,
};


static
bool
task_queue_with_dup_(
  const task_t_* task);

static
void
task_finalize_(
  task_t_* task);


static
void
bin_watch_cb_(
  upd_file_watch_t* watch);

static
void
bin_deinit_close_cb_(
  uv_fs_t* fsreq);


static
void
task_stat_exec_cb_(
  task_t_* task);

static
void
task_stat_cb_(
  uv_fs_t* fsreq);

static
void
task_open_exec_cb_(
  task_t_* task);

static
void
task_open_cb_(
  uv_fs_t* fsreq);

static
void
task_read_exec_cb_(
  task_t_* task);

static
void
task_read_cb_(
  uv_fs_t* fsreq);

static
void
task_write_exec_cb_(
  task_t_* task);

static
void
task_write_cb_(
  uv_fs_t* fsreq);

static
void
task_close_exec_cb_(
  task_t_* task);

static
void
task_close_cb_(
  uv_fs_t* fsreq);


static bool bin_init_(upd_file_t* f, bool r, bool w) {
  if (HEDLEY_UNLIKELY(!f->npath)) {
    return false;
  }

  bin_t_* ctx = NULL;
  if (HEDLEY_UNLIKELY(!upd_malloc(&ctx, sizeof(*ctx)))) {
    return false;
  }
  *ctx = (bin_t_) {
    .read  = r,
    .write = w,
  };
  f->ctx = ctx;

  ctx->watch = (upd_file_watch_t) {
    .file = f,
    .cb   = bin_watch_cb_,
  };
  if (HEDLEY_UNLIKELY(!upd_file_watch(&ctx->watch))) {
    upd_free(&ctx);
    return false;
  }

  const bool q = task_queue_with_dup_(&(task_t_) {
      .file = f,
      .exec = task_stat_exec_cb_,
    });
  if (HEDLEY_UNLIKELY(!q)) {
    upd_file_unwatch(&ctx->watch);
    upd_free(&ctx);
    return false;
  }
  return true;
}

static bool bin_init_r_(upd_file_t* f) {
  return bin_init_(f, true, false);
}

static bool bin_init_rw_(upd_file_t* f) {
  return bin_init_(f, true, true);
}

static bool bin_init_w_(upd_file_t* f) {
  return bin_init_(f, false, true);
}

static void bin_deinit_(upd_file_t* f) {
  bin_t_*    ctx = f->ctx;
  upd_iso_t* iso = f->iso;

  upd_file_unwatch(&ctx->watch);

  if (HEDLEY_LIKELY(!ctx->open)) {
    goto EXIT;
  }
  uv_fs_t* fsreq = upd_iso_stack(iso, sizeof(*fsreq));
  if (HEDLEY_UNLIKELY(fsreq == NULL)) {
    goto EXIT;
  }
  *fsreq = (uv_fs_t) { .data = iso, };

  const int err = uv_fs_close(&iso->loop, fsreq, ctx->fd, bin_deinit_close_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto EXIT;
  }

EXIT:
  upd_free(&ctx);
}

static bool bin_handle_(upd_req_t* req) {
  upd_file_t* f   = req->file;
  bin_t_*     ctx = f->ctx;

  switch (req->type) {
  case UPD_REQ_BIN_ACCESS:
    req->bin.access = (upd_req_bin_access_t) {
      .read  = ctx->read,
      .write = ctx->write,
    };
    break;

  case UPD_REQ_BIN_READ:
    if (HEDLEY_UNLIKELY(!ctx->open)) {
      const bool ok = task_queue_with_dup_(&(task_t_) {
          .file = f,
          .exec = task_open_exec_cb_,
        });
      if (HEDLEY_UNLIKELY(!ok)) {
        return false;
      }
    }
    return task_queue_with_dup_(&(task_t_) {
        .file = f,
        .req  = req,
        .exec = task_read_exec_cb_,
      });

  case UPD_REQ_BIN_WRITE:
    if (HEDLEY_UNLIKELY(!ctx->open)) {
      const bool ok = task_queue_with_dup_(&(task_t_) {
          .file = f,
          .exec = task_open_exec_cb_,
        });
      if (HEDLEY_UNLIKELY(!ok)) {
        return false;
      }
    }
    return task_queue_with_dup_(&(task_t_) {
        .file = f,
        .req  = req,
        .exec = task_write_exec_cb_,
      });

  default:
    return false;
  }
  req->cb(req);
  return true;
}


static bool task_queue_with_dup_(const task_t_* src) {
  upd_file_t* f   = src->file;
  bin_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  task_t_* task = upd_iso_stack(iso, sizeof(*task));
  if (HEDLEY_UNLIKELY(task == NULL)) {
    return false;
  }
  *task = *src;
  if (HEDLEY_LIKELY(ctx->last_task)) {
    ctx->last_task->next = task;
    ctx->last_task       = task;
  } else {
    ctx->last_task = task;
    task->exec(task);
  }
  upd_file_ref(f);
  return task;
}

static void task_finalize_(task_t_* task) {
  upd_file_t* f   = task->file;
  bin_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  if (HEDLEY_UNLIKELY(ctx->last_task == task)) {
    ctx->last_task = NULL;
  } else if (HEDLEY_UNLIKELY(task->next)) {
    task->next->exec(task->next);
  }
  upd_iso_unstack(iso, task);
  upd_file_unref(f);
}


static void bin_watch_cb_(upd_file_watch_t* watch) {
  upd_file_t* f   = watch->file;
  bin_t_*     ctx = f->ctx;

  if (HEDLEY_UNLIKELY(watch->event == UPD_FILE_UPDATE_N)) {
    task_queue_with_dup_(&(task_t_) {
        .file = f,
        .exec = task_stat_exec_cb_,
      });
    if (HEDLEY_LIKELY(ctx->open)) {
      task_queue_with_dup_(&(task_t_) {
          .file = f,
          .exec = task_close_exec_cb_,
        });
      task_queue_with_dup_(&(task_t_) {
          .file = f,
          .exec = task_open_exec_cb_,
        });
    }
    upd_file_trigger(f, UPD_FILE_UPDATE);
  }
}

static void bin_deinit_close_cb_(uv_fs_t* fsreq) {
  upd_iso_t* iso = fsreq->data;
  upd_iso_unstack(iso, fsreq);
}


static void task_stat_exec_cb_(task_t_* task) {
  upd_file_t* f   = task->file;
  upd_iso_t*  iso = f->iso;

  const int err = uv_fs_stat(
    &iso->loop, &task->fsreq, (char*) f->npath, task_stat_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto ABORT;
  }
  return;

ABORT:
  task_finalize_(task);
}

static void task_stat_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;

  const ssize_t result = fsreq->result;
  const size_t  bytes  = fsreq->statbuf.st_size;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    goto EXIT;
  }

  ctx->bytes = bytes;

EXIT:
  task_finalize_(task);
}

static void task_open_exec_cb_(task_t_* task) {
  upd_file_t* f   = task->file;
  bin_t_*     ctx = f->ctx;
  upd_iso_t*  iso = f->iso;

  const int mode =
    ctx->read && ctx->write? O_RDWR:
    ctx->read?               O_RDONLY:
    ctx->write?              O_WRONLY: 0;

  const int open = uv_fs_open(
    &iso->loop, &task->fsreq, (char*) f->npath, 0, mode, task_open_cb_);
  if (HEDLEY_UNLIKELY(0 > open)) {
    goto ABORT;
  }
  return;

ABORT:
  task_finalize_(task);
}

static void task_open_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    goto EXIT;
  }

  ctx->fd   = result;
  ctx->open = true;

EXIT:
  task_finalize_(task);
}

static void task_read_exec_cb_(task_t_* task) {
  upd_file_t* f    = task->file;
  upd_req_t*  req  = task->req;
  bin_t_*     ctx  = f->ctx;
  upd_iso_t*  iso  = f->iso;

  if (HEDLEY_UNLIKELY(!ctx->open)) {
    goto ABORT;
  }

  size_t sz = req->bin.rw.size;
  if (HEDLEY_LIKELY(sz > ctx->bytes)) {
    sz = ctx->bytes;
  }
  if (HEDLEY_LIKELY(sz > BUF_MAX_)) {
    sz = BUF_MAX_;
  }
  if (HEDLEY_UNLIKELY(sz == 0)) {
    goto ABORT;
  }

  task->buf = upd_iso_stack(iso, sz);
  if (HEDLEY_UNLIKELY(task->buf == NULL)) {
    goto ABORT;
  }

  const uv_buf_t buf = uv_buf_init((char*) task->buf, sz);

  const size_t off = req->bin.rw.offset;
  const int    err = uv_fs_read(
    &iso->loop, &task->fsreq, ctx->fd, &buf, 1, off, task_read_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    upd_iso_unstack(iso, task->buf);
    goto ABORT;
  }
  return;

ABORT:
  req->bin.rw.size = 0;
  req->cb(req);
  task_finalize_(task);
}

static void task_read_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  upd_req_t*  req  = task->req;
  upd_iso_t*  iso  = f->iso;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    req->bin.rw.size = 0;
    goto EXIT;
  }

  const size_t off = req->bin.rw.offset;
  req->bin.rw = (upd_req_bin_rw_t) {
    .offset = off,
    .size   = result,
    .buf    = task->buf,
  };

EXIT:
  req->cb(req);
  upd_iso_unstack(iso, task->buf);
  task_finalize_(task);
}

static void task_write_exec_cb_(task_t_* task) {
  upd_file_t* f    = task->file;
  upd_req_t*  req  = task->req;
  bin_t_*     ctx  = f->ctx;
  upd_iso_t*  iso  = f->iso;

  if (HEDLEY_UNLIKELY(!ctx->open)) {
    goto ABORT;
  }

  const size_t sz  = req->bin.rw.size;
  const size_t off = req->bin.rw.offset;

  const uv_buf_t buf = uv_buf_init((char*) req->bin.rw.buf, sz);

  const int err = uv_fs_write(
    &iso->loop, &task->fsreq, ctx->fd, &buf, 1, off, task_write_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto ABORT;
  }
  return;

ABORT:
  req->bin.rw.size = 0;
  req->cb(req);
  task_finalize_(task);
}

static void task_write_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_req_t*  req  = task->req;

  const ssize_t result = fsreq->result;
  uv_fs_req_cleanup(fsreq);

  if (HEDLEY_UNLIKELY(result < 0)) {
    req->bin.rw.size = 0;
    goto EXIT;
  }
  req->bin.rw.size = result;

EXIT:
  req->cb(req);
  task_finalize_(task);
}

static void task_close_exec_cb_(task_t_* task) {
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;
  upd_iso_t*  iso  = f->iso;

  if (HEDLEY_UNLIKELY(!ctx->open)) {
    goto ABORT;
  }

  const int err = uv_fs_close(
    &iso->loop, &task->fsreq, ctx->fd, task_close_cb_);
  if (HEDLEY_UNLIKELY(0 > err)) {
    goto ABORT;
  }
  return;

ABORT:
  task_finalize_(task);
}

static void task_close_cb_(uv_fs_t* fsreq) {
  task_t_*    task = (void*) fsreq;
  upd_file_t* f    = task->file;
  bin_t_*     ctx  = f->ctx;

  ctx->open = false;
  task_finalize_(task);
}