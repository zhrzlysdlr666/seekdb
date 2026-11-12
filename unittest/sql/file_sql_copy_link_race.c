/* Test-only LD_PRELOAD hook: replace the staging entry immediately before
 * publication, then restore it before cleanup. Applies only to race.csv. */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int linkat(int olddir, const char *oldname, int newdir, const char *newname, int flags)
{
  typedef int (*link_fn)(int, const char *, int, const char *, int);
  link_fn real_link = (link_fn)dlsym(RTLD_NEXT, "linkat");
  if (strcmp(newname, "race.csv") != 0 || getenv("FILE_SQL_COPY_RACE_LOG") == NULL) {
    return real_link(olddir, oldname, newdir, newname, flags);
  }
  DIR *directory = fdopendir(dup(newdir));
  char stage[256] = {0};
  struct dirent *entry;
  while (directory != NULL && (entry = readdir(directory)) != NULL) {
    if (strncmp(entry->d_name, ".seekdb-copy-", 13) == 0) {
      snprintf(stage, sizeof(stage), "%s", entry->d_name);
      break;
    }
  }
  if (directory != NULL) { closedir(directory); }
  const char *held = ".file-sql-race-held";
  if (stage[0] == '\0' || renameat(newdir, stage, newdir, held) != 0) {
    errno = EIO;
    return -1;
  }
  int replacement = openat(newdir, stage, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (replacement < 0) {
    renameat(newdir, held, newdir, stage);
    errno = EIO;
    return -1;
  }
  static const char injected[] = "injected-wrong-inode\n";
  const ssize_t written = write(replacement, injected, sizeof(injected) - 1);
  close(replacement);
  if (written != sizeof(injected) - 1) {
    unlinkat(newdir, stage, 0);
    renameat(newdir, held, newdir, stage);
    errno = EIO;
    return -1;
  }
  int result = real_link(olddir, oldname, newdir, newname, flags);
  int saved_errno = errno;
  unlinkat(newdir, stage, 0);
  renameat(newdir, held, newdir, stage);
  int logfd = open(getenv("FILE_SQL_COPY_RACE_LOG"), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (logfd >= 0) {
    static const char message[] = "staging entry replaced during linkat\n";
    write(logfd, message, sizeof(message) - 1);
    close(logfd);
  }
  errno = saved_errno;
  return result;
}
