#include "ipcclient.h"

#include "ovnum.h"
#include "ovthreads.h"

#include "ovutil/win32.h"

#include "file.h"

struct ipcclient {
  HANDLE pipe;
  void *buffer;
  size_t buffer_size;
};

NODISCARD static error
connect(wchar_t const *const name, uint32_t const signature, uint32_t const protocol_version, HANDLE *const pipe) {
  if (!name || !signature || !pipe) {
    return errg(err_invalid_arugment);
  }
  error err = eok();
  HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  // handshake
  {
    uint32_t t[2] = {signature, protocol_version};
    err = write(h, t, sizeof(t));
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    err = read(h, t, sizeof(protocol_version));
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (t[0] != protocol_version) {
      err = errg(err_unexpected);
      goto cleanup;
    }
  }
  *pipe = h;
  h = INVALID_HANDLE_VALUE;
cleanup:
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
    h = INVALID_HANDLE_VALUE;
  }
  return err;
}

static uint64_t get_now_in_ms(void) {
  struct timespec ts = {0};
  if (timespec_get(&ts, TIME_UTC) == 0) {
    return 0;
  }
  return (uint64_t)(ts.tv_sec) * UINT64_C(1000) + (uint64_t)(ts.tv_nsec) / UINT64_C(1000000);
}

NODISCARD error ipcclient_create(struct ipcclient **const c, struct ipcclient_options *opt) {
  if (!c || *c || !opt || !opt->pipe_name || !opt->signature) {
    return errg(err_invalid_arugment);
  }
  struct ipcclient *cli = NULL;
  error err = mem(c, 1, sizeof(struct ipcclient));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  cli = *c;
  *cli = (struct ipcclient){
      .pipe = INVALID_HANDLE_VALUE,
  };
  // If the server accepts connections without interruption, the connection may fail.
  // A retry mechanism is needed to reduce the failure rate.
  uint64_t const deadline = get_now_in_ms() + (opt->connect_timeout_msec ? opt->connect_timeout_msec : 50);
  for (;;) {
    HANDLE pipe = NULL;
    err = connect(opt->pipe_name, opt->signature, opt->protocol_version, &pipe);
    if (efailed(err)) {
      if (get_now_in_ms() < deadline &&
          (eis_hr(err, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ||
           eis_hr(err, HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE))) &&
          (!opt->is_aborted || !opt->is_aborted(opt->userdata))) {
        efree(&err);
        Sleep(10);
        continue;
      }
      err = ethru(err);
      goto cleanup;
    }
    cli->pipe = pipe;
    break;
  }
cleanup:
  if (efailed(err)) {
    if (cli) {
      if (cli->pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(cli->pipe);
        cli->pipe = INVALID_HANDLE_VALUE;
      }
      ereport(mem_free(c));
    }
  }
  return err;
}

NODISCARD error ipcclient_destroy(struct ipcclient **const c) {
  if (!c || !*c) {
    return errg(err_invalid_arugment);
  }
  struct ipcclient *cli = *c;
  if (cli->pipe != INVALID_HANDLE_VALUE) {
    CloseHandle(cli->pipe);
    cli->pipe = INVALID_HANDLE_VALUE;
  }
  if (cli->buffer) {
    ereport(mem_free(&cli->buffer));
  }
  ereport(mem_free(c));
  return eok();
}

NODISCARD error ipcclient_grow_buffer(struct ipcclient *const c, size_t const new_buffer_size, void **pptr) {
  error err = eok();
  if (c->buffer_size < new_buffer_size) {
    err = mem(&c->buffer, new_buffer_size, 1);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    c->buffer_size = new_buffer_size;
  }
  if (pptr) {
    *pptr = c->buffer;
  }
cleanup:
  return err;
}

NODISCARD error ipcclient_call(struct ipcclient *const c,
                               struct ipcclient_request const *const req,
                               struct ipcclient_response *const resp) {
  if (!c || !req || !req->event_id || !req->ptr || !req->size || !resp) {
    return errg(err_invalid_arugment);
  }
  error err = write(c->pipe, (uint32_t[2]){req->event_id, req->size}, sizeof(uint32_t[2]));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = write(c->pipe, req->ptr, req->size);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  uint32_t size = 0;
  err = read(c->pipe, &size, sizeof(size));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  if (size == 0) {
    resp->size = 0;
    resp->ptr = NULL;
    goto cleanup;
  }
  err = ipcclient_grow_buffer(c, size, NULL);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = read(c->pipe, c->buffer, size);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  resp->size = size;
  resp->ptr = c->buffer;
cleanup:
  return err;
}
