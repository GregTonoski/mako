/*!
 * sys.c - system functions for mako
 * Copyright (c) 2020, Christopher Jeffrey (MIT License).
 * https://github.com/chjj/mako
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <io/core.h>

/*
 * Compat
 */

#undef HAVE_SYSCTL

#if defined(__APPLE__)     \
 || defined(__FreeBSD__)   \
 || defined(__OpenBSD__)   \
 || defined(__NetBSD__)    \
 || defined(__DragonFly__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  if defined(CTL_HW) && (defined(HW_AVAILCPU) || defined(HW_NCPU))
#    define HAVE_SYSCTL
#  endif
#elif defined(__hpux)
#  include <sys/mpctl.h>
#endif

/*
 * System
 */

#ifdef HAVE_SYSCTL
static int
try_sysctl(int name) {
  int ret = -1;
  size_t len;
  int mib[4];

  len = sizeof(ret);

  mib[0] = CTL_HW;
  mib[1] = name;

  if (sysctl(mib, 2, &ret, &len, NULL, 0) != 0)
    return -1;

  return ret;
}
#endif

int
btc_sys_numcpu(void) {
  /* https://stackoverflow.com/questions/150355 */
#if defined(__linux__) || defined(__sun) || defined(_AIX)
  /* Linux, Solaris, AIX */
# if defined(_SC_NPROCESSORS_ONLN)
  return (int)sysconf(_SC_NPROCESSORS_ONLN);
# else
  return -1;
# endif
#elif defined(HAVE_SYSCTL)
  /* Apple, FreeBSD, OpenBSD, NetBSD, DragonFly BSD */
  int ret = -1;
# if defined(HW_AVAILCPU)
  ret = try_sysctl(HW_AVAILCPU);
# endif
# if defined(HW_NCPU)
  if (ret < 1)
    ret = try_sysctl(HW_NCPU);
# endif
  return ret;
#elif defined(__hpux)
  /* HP-UX */
  return mpctl(MPC_GETNUMSPUS, NULL, NULL);
#elif defined(__sgi)
  /* IRIX */
# if defined(_SC_NPROC_ONLN)
  return (int)sysconf(_SC_NPROC_ONLN);
# else
  return -1;
# endif
#else
  return -1;
#endif
}

int
btc_sys_homedir(char *buf, size_t size) {
  char *home = getenv("HOME");
  struct passwd *pwd;
  size_t len;

  if (home == NULL) {
    uid_t uid = geteuid();

    do {
      pwd = getpwuid(uid);
    } while (pwd == NULL && errno == EINTR);

    if (pwd == NULL)
      return 0;

    home = pwd->pw_dir;
  }

  len = strlen(home);

  if (len + 1 > size)
    return 0;

  memcpy(buf, home, len + 1);

  return 1;
}

int
btc_sys_datadir(char *buf, size_t size, const char *name) {
  char home[BTC_PATH_MAX];

  if (!btc_sys_homedir(home, sizeof(home)))
    return 0;

#if defined(__APPLE__)
  if (strlen(home) + strlen(name) + 30 > size)
    return 0;

  sprintf(buf, "%s/Library/Application Support/%c%s",
               home, name[0] & ~32, name + 1);
#else
  if (strlen(home) + strlen(name) + 3 > size)
    return 0;

  sprintf(buf, "%s/.%s", home, name);
#endif

  return 1;
}
