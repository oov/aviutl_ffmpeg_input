#include "process.h"

#include "ovnum.h"
#include "ovthreads.h"

#include "ovutil/str.h"
#include "ovutil/win32.h"

struct process {
  struct process_options opt;
  wchar_t unique_id[16];
  HANDLE process;
  HANDLE event;
  thrd_t thread;
};

static NODISCARD error create_event(HANDLE *const event, wchar_t *const name16) {
  if (!event || !name16) {
    return errg(err_invalid_arugment);
  }
  wchar_t name[16];
  HANDLE h = NULL;
  size_t retry = 0;
  while (h == NULL) {
    wsprintfW(name, L"ipc_%08x", get_global_hint() & 0xffffffff);
    h = CreateEventW(NULL, FALSE, FALSE, name);
    HRESULT const hr = HRESULT_FROM_WIN32(GetLastError());
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
      CloseHandle(h);
      h = NULL;
      if (++retry < 5) {
        continue;
      }
    }
    if (FAILED(hr)) {
      return errhr(hr);
    }
  }
  *event = h;
  wcscpy(name16, name);
  return eok();
}

static int worker(void *userdata) {
  struct process *p = userdata;
  WaitForSingleObject(p->process, INFINITE);
  if (p->opt.on_terminate) {
    p->opt.on_terminate(p->opt.userdata);
  }
  return 0;
}

NODISCARD error process_create(struct process **const pp, struct process_options const *const opt) {
  if (!pp || *pp || !opt || !opt->module_path) {
    return errg(err_unexpected);
  }
  struct process *p = NULL;
  struct wstr module = {0};
  struct wstr dir = {0};
  struct wstr param = {0};
  PROCESS_INFORMATION pi = {
      .hProcess = INVALID_HANDLE_VALUE,
      .hThread = INVALID_HANDLE_VALUE,
  };
  error err = mem(pp, 1, sizeof(struct process));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  p = *pp;
  *p = (struct process){
      .opt = *opt,
      .process = INVALID_HANDLE_VALUE,
  };
  err = create_event(&p->event, p->unique_id);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = scpy(&module, opt->module_path);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  size_t fnpos = 0;
  err = extract_file_name(&module, &fnpos);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  err = sncpy(&dir, module.ptr, fnpos);
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  wchar_t prbuf[32];
  err = scatm(&param,
              L"rundll32 \"",
              module.ptr,
              L"\",BridgeMain ",
              p->unique_id,
              L" ",
              ov_utoa((uint64_t)GetCurrentProcessId(), prbuf));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  STARTUPINFOW si = {
      .cb = sizeof(STARTUPINFOW),
      .dwFlags = STARTF_USESHOWWINDOW,
      .wShowWindow = SW_SHOWDEFAULT,
  };
  if (!CreateProcessW(
          NULL, param.ptr, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, NULL, dir.ptr, &si, &pi)) {
    err = emsg(err_type_hresult,
               HRESULT_FROM_WIN32(GetLastError()),
               &native_unmanaged_const(NSTR("プロセスの起動に失敗しました。")));
    goto cleanup;
  }
  if (thrd_create(&p->thread, worker, p) != thrd_success) {
    err = errg(err_unexpected);
    goto cleanup;
  }
  p->process = pi.hProcess;
  pi.hProcess = INVALID_HANDLE_VALUE;
cleanup:
  if (pi.hProcess != INVALID_HANDLE_VALUE) {
    CloseHandle(pi.hProcess);
    pi.hProcess = INVALID_HANDLE_VALUE;
  }
  if (pi.hThread != INVALID_HANDLE_VALUE) {
    CloseHandle(pi.hThread);
    pi.hThread = INVALID_HANDLE_VALUE;
  }
  ereport(sfree(&param));
  ereport(sfree(&dir));
  ereport(sfree(&module));
  if (efailed(err)) {
    if (p && p->event) {
      CloseHandle(p->event);
      p->event = NULL;
    }
    ereport(mem_free(pp));
  }
  return err;
}

NODISCARD error process_destroy(struct process **const pp) {
  if (!pp || !*pp) {
    return errg(err_unexpected);
  }
  struct process *const p = *pp;
  SetEvent(p->event);
  // If it does not work well, it may cause freezes, so consider forced termination.
  if (WaitForSingleObject(p->process, 5000) == WAIT_TIMEOUT) {
    TerminateProcess(p->process, 1);
  }
  thrd_join(p->thread, NULL);
  CloseHandle(p->process);
  CloseHandle(p->event);
  ereport(mem_free(pp));
  return eok();
}

wchar_t const *process_get_unique_id(struct process *const p) {
  if (!p) {
    return L"ipc_xxxxxxxx";
  }
  return p->unique_id;
}
