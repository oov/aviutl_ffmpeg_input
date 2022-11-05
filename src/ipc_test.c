#include "ovtest.h"

#include "ipcclient.h"
#include "ipcserver.h"
#include "ovthreads.h"

#define PIPE_NAME L"\\\\.\\pipe\\ipctest"
#define IPC_SIGNATURE 0x1234abcd
#define IPC_VERSION 1

static int client(void *userdata) {
  (void)userdata;
  struct ipcclient *c = NULL;
  error err = ipcclient_create(&c,
                               &(struct ipcclient_options){
                                   .pipe_name = PIPE_NAME,
                                   .signature = IPC_SIGNATURE,
                                   .protocol_version = IPC_VERSION,
                               });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  for (int i = 0; i < 1000; ++i) {
    uint32_t v = (uint32_t)(get_global_hint()) / 2;
    struct ipcclient_response resp = {0};
    err = ipcclient_call(c,
                         &(struct ipcclient_request){
                             .event_id = 1,
                             .ptr = &v,
                             .size = sizeof(v),
                         },
                         &resp);
    if (efailed(err)) {
      err = ethru(err);
      goto cleanup;
    }
    if (resp.size != sizeof(uint32_t)) {
      err = errg(err_unexpected);
      goto cleanup;
    }
    if (*(uint32_t *)resp.ptr != v * 2) {
      err = errg(err_fail);
      goto cleanup;
    }
  }
cleanup:
  if (c) {
    ereport(ipcclient_destroy(&c));
  }
  ereport(err);
  return 0;
}

static void handler_2x(struct ipcserver_context *const ctx) {
  uint32_t v = *(uint32_t *)ctx->buffer;
  error err = ctx->grow_buffer(ctx, sizeof(uint32_t));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  *(uint32_t *)ctx->buffer = v * 2;
cleanup:
  ctx->finish(ctx, err);
}

static void test_basic(void) {
  struct ipcserver *serv = NULL;
  error err = ipcserver_create(&serv,
                               &(struct ipcserver_options){
                                   .pipe_name = PIPE_NAME,
                                   .signature = IPC_SIGNATURE,
                                   .protocol_version = IPC_VERSION,

                                   .handler = handler_2x,
                               });
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  enum {
    num_threads = 3,
  };
  thrd_t th[num_threads];
  for (int i = 0; i < num_threads; ++i) {
    thrd_create(th + i, client, NULL);
  }
  for (int i = 0; i < num_threads; ++i) {
    thrd_join(th[i], NULL);
  }
cleanup:
  if (serv) {
    ereport(ipcserver_destroy(&serv));
  }
  ereport(err);
}

TEST_LIST = {
    {"test_basic", test_basic},
    {NULL, NULL},
};
