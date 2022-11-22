#include "mapped.h"

#include "ovutil/win32.h"

struct mapped {
  HANDLE file;
  HANDLE map;
  void *ptr;
  size_t mapped_size;
  int64_t mapped_base;
  int64_t pos;
  int64_t total_size;
};

static size_t const mapping_size = 32 * 1024 * 1024;

static inline size_t szmin(size_t const a, size_t const b) { return a > b ? b : a; }

int mapped_read(struct mapped *const mp, void *const buf, int const buf_size) {
  if (!mp || !buf || !buf_size) {
    return -EINVAL;
  }
  int read_size = buf_size;
  if (mp->pos + (int64_t)read_size > mp->total_size) {
    read_size = (int)(mp->total_size - mp->pos);
  }
  if ((mp->pos < mp->mapped_base) || ((mp->pos + (int64_t)read_size) > (mp->mapped_base + (int64_t)mp->mapped_size))) {
    SYSTEM_INFO si = {0};
    GetSystemInfo(&si);
    int64_t const block_size = (int64_t)si.dwAllocationGranularity;
    LARGE_INTEGER base = {
        .QuadPart = (mp->pos / block_size) * block_size,
    };
    size_t const mapped_size = szmin((size_t)(mp->total_size - base.QuadPart), mapping_size);
    void *ptr = MapViewOfFile(mp->map, FILE_MAP_READ, (DWORD)base.HighPart, base.LowPart, mapped_size);
    if (!ptr) {
      ereport(errhr(HRESULT_FROM_WIN32(GetLastError())));
      return -EIO;
    }
    if (mp->ptr) {
      UnmapViewOfFile(mp->ptr);
    }
    mp->ptr = ptr;
    mp->mapped_base = base.QuadPart;
    mp->mapped_size = mapped_size;
  }
  char *ptr = mp->ptr;
  memcpy(buf, ptr + (size_t)(mp->pos - mp->mapped_base), (size_t)read_size);
  mp->pos += read_size;
  return read_size;
}

int64_t mapped_seek(struct mapped *const mp, int64_t const offset, int const whence) {
  if (!mp) {
    return -EINVAL;
  }
  int64_t pos = 0;
  switch (whence) {
  case FILE_BEGIN:
    pos = offset;
    break;
  case FILE_CURRENT:
    pos = mp->pos + offset;
    break;
  case FILE_END:
    pos = mp->total_size + offset;
    break;
  default:
    return -EINVAL;
  }
  if (pos < 0 || pos > mp->total_size) {
    return -EINVAL;
  }
  mp->pos = pos;
  return pos;
}

int64_t mapped_get_size(struct mapped *const mp) {
  if (!mp) {
    return -EINVAL;
  }
  return mp->total_size;
}

void mapped_destroy(struct mapped **const mpp) {
  if (!mpp || !*mpp) {
    return;
  }
  struct mapped *mp = *mpp;
  if (mp->ptr) {
    UnmapViewOfFile(mp->ptr);
    mp->ptr = NULL;
  }
  if (mp->map) {
    CloseHandle(mp->map);
    mp->map = NULL;
  }
  if (mp->file != INVALID_HANDLE_VALUE) {
    CloseHandle(mp->file);
    mp->file = INVALID_HANDLE_VALUE;
  }
  ereport(mem_free(mpp));
}

NODISCARD error mapped_create(struct mapped **const mpp, wchar_t const *const filepath) {
  if (!mpp || *mpp || !filepath) {
    return errg(err_invalid_arugment);
  }
  struct mapped *mp = NULL;
  error err = mem(mpp, 1, sizeof(struct mapped));
  if (efailed(err)) {
    err = ethru(err);
    goto cleanup;
  }
  mp = *mpp;
  *mp = (struct mapped){
      .file = INVALID_HANDLE_VALUE,
  };
  mp->file = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (mp->file == INVALID_HANDLE_VALUE) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(mp->file, &sz)) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
  mp->total_size = sz.QuadPart;
  mp->map = CreateFileMappingW(mp->file, NULL, PAGE_READONLY, 0, 0, NULL);
  if (!mp->map) {
    err = errhr(HRESULT_FROM_WIN32(GetLastError()));
    goto cleanup;
  }
cleanup:
  if (efailed(err)) {
    mapped_destroy(mpp);
  }
  return err;
}
