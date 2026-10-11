/**
 * [ps5 port] Standard functions the PS5's libc doesn't export, used by the
 * runtime (ffs: math_gcc.cpp; timegm: STFS timestamps) and SDL3 (getresuid /
 * getresgid: its GTK loader, never used on the PS5). isatty / mkstemp: the
 * PS5 only exports them from libScePosixForWebKit, which a game can't use
 * (first boot: spdlog's isatty call jumped to address 0).
 */
#if defined(__PROSPERO__)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// [ps5 port] Imports only the system's libkernel_sys exports, which a game
// can't load: each one was a jump to address 0 (libc++'s create_hard_link
// called link() when the game reopened its save: a crash that looped behind
// "NOW LOADING"). They fail cleanly instead, and every caller has a fallback
// (hard link -> a copy).
int link(const char* from, const char* to) {
  (void)from, (void)to;
  errno = ENOTSUP;
  return -1;
}
int symlink(const char* target, const char* path) {
  (void)target, (void)path;
  errno = ENOTSUP;
  return -1;
}
ssize_t readlink(const char* path, char* buf, size_t size) {
  (void)path, (void)buf, (void)size;
  errno = EINVAL;  // (not a symbolic link: there are none)
  return -1;
}
long pathconf(const char* path, int name) {
  (void)path;
  switch (name) {
    case _PC_NAME_MAX: return 255;
    case _PC_PATH_MAX: return 1024;
    case _PC_LINK_MAX: return 1;
    default: errno = EINVAL; return -1;
  }
}
pid_t fork(void) {
  errno = ENOSYS;
  return -1;
}
pid_t vfork(void) {
  errno = ENOSYS;
  return -1;
}
pid_t setsid(void) {
  errno = EPERM;
  return -1;
}

// No terminals on a PS5.
int isatty(int fd) {
  (void)fd;
  errno = ENOTTY;
  return 0;
}

// mkstemp: replace the trailing XXXXXX until a new file can be created.
int mkstemp(char* path) {
  static const char kChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  size_t length = path ? strlen(path) : 0;
  if (length < 6 || strcmp(path + length - 6, "XXXXXX") != 0) {
    errno = EINVAL;
    return -1;
  }
  static uint64_t counter = 0;
  for (int attempt = 0; attempt < 1000; ++attempt) {
    uint64_t v = (uint64_t)time(NULL) * 2654435761u + (++counter) * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 6; ++i) {
      path[length - 6 + i] = kChars[v % 36];
      v /= 36;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0 || errno != EEXIST) return fd;
  }
  errno = EEXIST;
  return -1;
}

int ffs(int value) { return __builtin_ffs(value); }

// UTC broken-down time to seconds since the epoch (days_from_civil, Howard
// Hinnant's public-domain algorithm); normalizes like mktime does.
time_t timegm(struct tm* tm) {
  int64_t year = (int64_t)tm->tm_year + 1900;
  int64_t month = tm->tm_mon;  // 0..11, may be out of range
  year += month / 12;
  month %= 12;
  if (month < 0) {
    month += 12;
    --year;
  }
  int64_t m = month + 1;  // 1..12
  int64_t y = year - (m <= 2);
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  return (time_t)(days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 +
                  tm->tm_sec);
}

int getresuid(uid_t* ruid, uid_t* euid, uid_t* suid) {
  uid_t u = getuid(), e = geteuid();
  if (ruid) *ruid = u;
  if (euid) *euid = e;
  if (suid) *suid = e;
  return 0;
}

int getresgid(gid_t* rgid, gid_t* egid, gid_t* sgid) {
  gid_t g = getgid(), e = getegid();
  if (rgid) *rgid = g;
  if (egid) *egid = e;
  if (sgid) *sgid = e;
  return 0;
}
#endif
