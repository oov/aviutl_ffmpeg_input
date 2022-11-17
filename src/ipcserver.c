#include "ipcserver.h"

#include "ovthreads.h"
#include "ovutil/win32.h"

#include <stdatomic.h>

#include "ipccommon.h"

struct client {
  struct ipcserver *serv;
  HANDLE named_pipe;
  thrd_t th;
  struct cndvar *cv;
};

struct client_map_item {
  struct client *key;
};

struct ipcserver {
  struct ipcserver_options opt;
  struct wstr pipe_name;

  HANDLE first_named_pipe;

  struct hmap clients;

  thrd_t thread;
  atomic_bool closing;
  mtx_t mtx;
};

struct internal_context {
  struct ipcserver_context public_ctx;
  atomic_int count;
  error err;
  struct cndvar cv;
  size_t real_buffer_size;
};

static void internal_context_add_ref(struct internal_context *ictx) {
  atomic_fetch_add_explicit(&ictx->count, 1, memory_order_relaxed);
}

static void internal_context_release(struct internal_context *ictx) {
  int const c = atomic_fetch_sub_explicit(&ictx->count, 1, memory_order_relaxed);
  if (c == 1) {
    cndvar_exit(&ictx->cv);
    if (efailed(ictx->err)) {
      efree(&ictx->err);
    }
    if (ictx->public_ctx.buffer) {
      ereport(mem_free(&ictx->public_ctx.buffer));
    }
    ereport(mem_free(&ictx));
  }
}

static bool ipcserver_context_is_waiting(struct ipcserver_context *const ctx) {
  struct internal_context *ictx = (void *)ctx;
  return atomic_load_explicit(&ictx->count, memory_order_relaxed) > 1;
}

static NODISCARD error ipcserver_context_grow_buffer(struct ipcserver_context *const ctx,
                                                     uint32_t const new_buffer_size) {
  struct internal_context *ictx = (void *)ctx;
  error err = eok();
  if (ictx->real_buffer_size < (size_t)new_buffer_size) {
    err = mem(&ctx->buffer, (size_t)new_buffer_size, 1);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    ictx->real_buffer_size = (size_t)new_buffer_size;
  }
  ctx->buffer_size = new_buffer_size;
cleanup:
  return err;
}

static void ipcserver_context_finish(struct ipcserver_context *const ctx, error err) {
  struct internal_context *ictx = (void *)ctx;
  cndvar_lock(&ictx->cv);
  ictx->err = err;
  cndvar_signal(&ictx->cv, efailed(err) ? 2 : 1);
  cndvar_unlock(&ictx->cv);
  internal_context_release(ictx);
}

static bool ipcserver_is_closing(struct ipcserver *const serv) {
  return atomic_load_explicit(&serv->closing, memory_order_relaxed) != 0;
}

NODISCARD static error create_error_reply(struct ipcserver_context *const ctx, error e) {
  error err = ipcserver_context_grow_buffer(ctx, (uint32_t)(4 + 4 + 8 + 8 + e->msg.len * sizeof(NATIVE_CHAR)));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  uint32_t *b32 = ctx->buffer;
  b32[0] = 0;
  b32[1] = (uint32_t)e->type;
  int64_t *b64 = ctx->buffer;
  b64[1] = (int64_t)e->code;
  b64[2] = (int64_t)e->msg.len;
  wchar_t *buf = ctx->buffer;
  buf += 12;
  memcpy(buf, e->msg.ptr, e->msg.len * sizeof(NATIVE_CHAR));
cleanup:
  efree(&e);
  return err;
}

NODISCARD static int client_worker(void *userdata) {
  struct client *const cli = userdata;
  HANDLE named_pipe = cli->named_pipe;
  struct ipcserver *const serv = cli->serv;

  struct internal_context *ictx = NULL;
  error err = mem(&ictx, 1, sizeof(struct internal_context));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *ictx = (struct internal_context){
      .err = eok(),
  };
  ictx->public_ctx = (struct ipcserver_context){
      .s = serv,
      .userdata = serv->opt.userdata,
      .is_waiting = ipcserver_context_is_waiting,
      .grow_buffer = ipcserver_context_grow_buffer,
      .finish = ipcserver_context_finish,
  };
  cndvar_init(&ictx->cv);
  internal_context_add_ref(ictx);
  cli->cv = &ictx->cv;

  // handshake
  {
    uint32_t t[2] = {0};
    err = ipccommon_read(named_pipe, &t, sizeof(t));
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (t[0] != serv->opt.signature) {
      err = emsg(err_type_generic, err_unexpected, &native_unmanaged_const(NSTR("signature mismatch")));
      goto cleanup;
    }
    if (t[1] != serv->opt.protocol_version) {
      err = emsg(err_type_generic, err_unexpected, &native_unmanaged_const(NSTR("protocol version mismatch")));
      goto cleanup;
    }
    err = ipccommon_write(named_pipe, &serv->opt.protocol_version, sizeof(serv->opt.protocol_version));
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
  }

  for (;;) {
    if (ipcserver_is_closing(serv)) {
      break;
    }

    // receive event id
    {
      uint32_t t = 0;
      err = ipccommon_read(named_pipe, &t, sizeof(t));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      ictx->public_ctx.event_id = t;
    }

    // receive request
    {
      uint32_t t = 0;
      err = ipccommon_read(named_pipe, &t, sizeof(t));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      err = ipcserver_context_grow_buffer(&ictx->public_ctx, t);
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      err = ipccommon_read(named_pipe, ictx->public_ctx.buffer, (size_t)t);
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
    }

    // call handler
    {
      cndvar_lock(&ictx->cv);
      ictx->cv.var = 0;
      internal_context_add_ref(ictx);
      cndvar_unlock(&ictx->cv);
      serv->opt.handler(&ictx->public_ctx);
      cndvar_lock(&ictx->cv);
      cndvar_wait_while(&ictx->cv, 0);
      bool const aborted = ictx->cv.var == 3;
      ictx->cv.var = 0;
      cndvar_unlock(&ictx->cv);
      if (aborted) {
        break;
      }
    }

    // send response
    {
      if (efailed(ictx->err)) {
        err = create_error_reply(&ictx->public_ctx, ictx->err);
        if (efailed(err)) {
          err = ethru(err);
          goto cleanup;
        }
        err = ipccommon_write(named_pipe, ictx->public_ctx.buffer, ictx->public_ctx.buffer_size);
        if (efailed(err)) {
          err = ethru(err);
          goto cleanup;
        }
        ictx->err = eok();
        continue;
      }
      err = ipccommon_write(named_pipe, &ictx->public_ctx.buffer_size, sizeof(ictx->public_ctx.buffer_size));
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
      err = ipccommon_write(named_pipe, ictx->public_ctx.buffer, ictx->public_ctx.buffer_size);
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
    }
  }

cleanup:
  if (esucceeded(err)) {
    ereport(ipccommon_flush(named_pipe));
  }
  DisconnectNamedPipe(named_pipe);
  CloseHandle(named_pipe);
  if (eis_hr(err, HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) || eis_hr(err, HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE))) {
    efree(&err);
  }
  ereport(err);
  mtx_lock(&serv->mtx);
  ereport(hmdelete(&serv->clients,
                   &((struct client_map_item){
                       .key = userdata,
                   }),
                   NULL));
  mtx_unlock(&serv->mtx);
  internal_context_release(ictx);
  ereport(mem_free(&userdata));
  return 0;
}

NODISCARD static error create_named_pipe(wchar_t const *const name, HANDLE *r) {
  HANDLE h = CreateNamedPipeW(name,
                              PIPE_ACCESS_DUPLEX,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                              PIPE_UNLIMITED_INSTANCES,
                              0,
                              0,
                              60,
                              NULL);
  if (h == INVALID_HANDLE_VALUE) {
    return errhr(HRESULT_FROM_WIN32(GetLastError()));
  }
  *r = h;
  return eok();
}

static int worker(void *const userdata) {
  error err = eok();
  struct ipcserver *const serv = userdata;
  HANDLE named_pipe = serv->first_named_pipe;
  serv->first_named_pipe = INVALID_HANDLE_VALUE;
  struct client *cli = NULL;
  bool locked = false;
  for (;;) {
    mtx_lock(&serv->mtx);
    locked = true;
    if (ipcserver_is_closing(serv)) {
      break;
    }

    if (named_pipe == INVALID_HANDLE_VALUE) {
      err = create_named_pipe(serv->pipe_name.ptr, &named_pipe);
      if (efailed(err)) {
        err = ethru(err);
        goto cleanup;
      }
    }
    mtx_unlock(&serv->mtx);
    locked = false;
    if (!ConnectNamedPipe(named_pipe, NULL)) {
      HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
      if (hr != HRESULT_FROM_WIN32(ERROR_PIPE_CONNECTED)) {
        err = errhr(hr);
        if (eis_hr(err, HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED)) ||
            eis_hr(err, HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE))) {
          efree(&err);
        }
        CloseHandle(named_pipe);
        named_pipe = INVALID_HANDLE_VALUE;
        ereport(err);
        err = NULL;
        continue;
      }
    }
    mtx_lock(&serv->mtx);
    locked = true;
    if (ipcserver_is_closing(serv)) {
      break;
    }
    err = mem(&cli, 1, sizeof(struct client));
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    *cli = (struct client){
        .serv = serv,
        .named_pipe = named_pipe,
    };
    if (thrd_create(&cli->th, client_worker, cli) != thrd_success) {
      err = errg(err_unexpected);
      goto cleanup;
    }
    err = hmset(&serv->clients,
                (&(struct client_map_item){
                    .key = cli,
                }),
                NULL);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    cli = NULL;
    named_pipe = INVALID_HANDLE_VALUE;
    mtx_unlock(&serv->mtx);
    locked = false;
  }
cleanup:
  if (locked) {
    mtx_unlock(&serv->mtx);
    locked = false;
  }
  if (cli) {
    ereport(mem_free(&cli));
  }
  if (named_pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(named_pipe);
    named_pipe = INVALID_HANDLE_VALUE;
  }
  if (efailed(err)) {
    if (serv->opt.error_handler) {
      serv->opt.error_handler(serv, serv->opt.userdata, err);
    } else {
      ereport(err);
    }
  }
  return 0;
}

NODISCARD error ipcserver_create(struct ipcserver **const s, struct ipcserver_options const *const opt) {
  if (!s || *s || !opt || !opt->pipe_name || !opt->signature || !opt->handler) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  struct ipcserver *serv = NULL;
  err = mem(s, 1, sizeof(struct ipcserver));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  serv = *s;
  *serv = (struct ipcserver){
      .opt = *opt,
      .first_named_pipe = INVALID_HANDLE_VALUE,
  };
  mtx_init(&serv->mtx, mtx_plain | mtx_recursive);
  atomic_store_explicit(&serv->closing, false, memory_order_relaxed);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = hmnews(&serv->clients, sizeof(struct client_map_item), 4, sizeof(struct client *));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = scpy(&serv->pipe_name, serv->opt.pipe_name);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = create_named_pipe(serv->pipe_name.ptr, &serv->first_named_pipe);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (thrd_create(&serv->thread, worker, serv) != thrd_success) {
    err = errg(err_fail);
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    if (serv) {
      if (serv->first_named_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(serv->first_named_pipe);
        serv->first_named_pipe = INVALID_HANDLE_VALUE;
      }
      mtx_destroy(&serv->mtx);
      ereport(hmfree(&serv->clients));
      ereport(sfree(&serv->pipe_name));
      ereport(mem_free(s));
    }
  }
  return err;
}

static bool cancel_client(void const *const item, void *const udata) {
  (void)udata;
  struct client_map_item const *const it = item;
  struct client *const cli = it->key;
  cndvar_lock(cli->cv);
  cndvar_signal(cli->cv, 3);
  CancelSynchronousIo(*(HANDLE *)&cli->th);
  cndvar_unlock(cli->cv);
  thrd_join(cli->th, NULL);
  return true;
}

NODISCARD error ipcserver_destroy(struct ipcserver **const s) {
  if (!s || !*s) {
    return errg(err_invalid_arugment);
  }
  struct ipcserver *serv = *s;
  atomic_store_explicit(&serv->closing, true, memory_order_relaxed);
  Sleep(1);
  CancelSynchronousIo(*(HANDLE *)&serv->thread);
  thrd_join(serv->thread, NULL);
  ereport(hmscan(&serv->clients, cancel_client, NULL));

  mtx_destroy(&serv->mtx);
  ereport(hmfree(&serv->clients));
  ereport(sfree(&serv->pipe_name));
  ereport(mem_free(s));
  return eok();
}
