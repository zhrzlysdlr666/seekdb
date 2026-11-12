#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int (*stat_fn)(const char *, struct stat *);

int stat(const char *path, struct stat *result)
{
  static stat_fn real_stat = NULL;
  static int triggered = 0;
  if (real_stat == NULL) {
    real_stat = (stat_fn)dlsym(RTLD_NEXT, "stat");
  }
  if (real_stat == NULL) {
    return -1;
  }

  const char *target = getenv("FILE_SQL_RACE_TARGET");
  if (!triggered && target != NULL && strcmp(path, target) == 0) {
    triggered = 1;
    const char *from = getenv("FILE_SQL_RACE_FROM");
    const char *to = getenv("FILE_SQL_RACE_TO");
    const char *symlink_to = getenv("FILE_SQL_RACE_SYMLINK_TO");
    const char *marker = getenv("FILE_SQL_RACE_MARKER");
    const int rename_result = from != NULL && to != NULL ? rename(from, to) : -1;
    const int symlink_result = from != NULL && symlink_to != NULL ? symlink(symlink_to, from) : -1;
    const int fd = marker == NULL ? -1 : open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
      const char *message = (rename_result == 0 && symlink_result == 0) ? "fired\n" : "failed\n";
      if (write(fd, message, strlen(message)) != (ssize_t)strlen(message)) {
        (void)unlink(marker);
      }
      close(fd);
    }
  }
  return real_stat(path, result);
}
