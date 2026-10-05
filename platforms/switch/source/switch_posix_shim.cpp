// aleks2: the POSIX that Horizon does not have.
//
// Newlib on this console is a C library without an operating system underneath
// it in the places that matter here: no users, no processes, no file
// descriptors for directories, no signals worth the name. The libraries the
// game pulls in were written for a system that has all of that, and they call
// into it on paths they never actually take on a console -- a fallback for a
// permissions error, a lookup of the current user's home directory.
//
// So each of these answers the way the real function answers when it cannot do
// its job, and that is a deliberate choice over returning success: a caller that
// checks gets a plain failure it knows how to handle, and one that does not gets
// a sensible value rather than a wrong one. The exceptions are the handful that
// really do have an answer on Horizon -- there is a page size, and there are
// four cores -- and those give it.
//
// Nothing here is for the game itself. If the game ever needs one of these, it
// wants the libnx call, not this.
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

extern "C" {

// ---------------------------------------------------------------------------
// Users and permissions. A Switch application is alone and owns everything it
// can see, so the honest answer to every one of these is root.

uid_t getuid() { return 0; }
uid_t geteuid() { return 0; }
gid_t getgid() { return 0; }
gid_t getegid() { return 0; }

int fchown(int, uid_t, gid_t) {
  // Nothing to change: the filesystem has no ownership to begin with.
  return 0;
}

int getpwuid_r(uid_t, struct passwd*, char*, size_t, struct passwd** result) {
  // "No such user", which is what a caller looking for a home directory
  // handles; it falls back to its own default, and on Switch that is what the
  // paths in switch_fs.cpp already give it.
  if (result != nullptr) {
    *result = nullptr;
  }
  return ENOSYS;
}

// ---------------------------------------------------------------------------
// Locking and directory descriptors.

int flock(int, int) {
  // Success, and this one is the exception to the rule above. There is exactly
  // one process, so a lock always would be granted; failing here would make
  // SQLite and the config writer take their contention paths for no reason.
  return 0;
}

int dirfd(DIR*) {
  // Horizon's directory handles are not file descriptors and cannot be made
  // into one, so anything built on *at() has to take its non-fd path.
  return -1;
}

int fstatat(int, const char*, struct stat*, int) {
  errno = ENOSYS;
  return -1;
}

// ---------------------------------------------------------------------------
// Processes and signals. There is no fork on this console and no second
// process to send anything to.

int pthread_sigmask(int, const sigset_t*, sigset_t*) {
  // Success with nothing done: threads here have no signal mask to change, and
  // a caller blocking signals around a critical section is protecting itself
  // from something that cannot arrive.
  return 0;
}

int execv(const char*, char* const[]) {
  errno = ENOSYS;
  return -1;
}

int execvp(const char*, char* const[]) {
  errno = ENOSYS;
  return -1;
}

pid_t waitpid(pid_t, int*, int) {
  // ECHILD rather than ENOSYS: there are no children, which is a state waitpid
  // has a word for.
  errno = ECHILD;
  return -1;
}

// ---------------------------------------------------------------------------
// System limits. These are the ones with real answers.

long sysconf(int name) {
  switch (name) {
  case _SC_PAGESIZE:
    return 4096;
  case _SC_NPROCESSORS_CONF:
  case _SC_NPROCESSORS_ONLN:
    // The Tegra X1's four A57s. Three of them are ours -- the system keeps the
    // fourth -- but a library asking this wants to know how wide the machine
    // is, and thread::hardware_concurrency reads better at four.
    return 4;
  case _SC_PHYS_PAGES:
  case _SC_AVPHYS_PAGES:
    return (4L * 1024 * 1024 * 1024) / 4096;
  case _SC_OPEN_MAX:
    return 128;
  case _SC_CLK_TCK:
    return 100;
  default:
    errno = EINVAL;
    return -1;
  }
}

// ---------------------------------------------------------------------------
// POSIX regular expressions, which newlib does not ship.
//
// These do not pretend: compiling always succeeds so that a caller does not
// treat its own pattern as malformed, and matching always says "no match", so
// a feature built on regex quietly finds nothing rather than misbehaving. The
// game does not use regex; this is for a library's error-message formatting.

int regcomp(regex_t*, const char*, int) { return 0; }

int regexec(const regex_t*, const char*, size_t, regmatch_t*, int) { return REG_NOMATCH; }

void regfree(regex_t*) {}

size_t regerror(int, const regex_t*, char* buffer, size_t capacity) {
  static const char message[] = "regular expressions are not available on Switch";
  if (buffer != nullptr && capacity > 0) {
    size_t copied = sizeof(message) < capacity ? sizeof(message) : capacity;
    for (size_t i = 0; i < copied; i++) {
      buffer[i] = message[i];
    }
    buffer[capacity - 1] = '\0';
  }
  return sizeof(message);
}

}  // extern "C"
