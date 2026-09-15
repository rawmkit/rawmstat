/* See LICENSE file for copyright and license details. */

#include "rawmstat-config.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef NO_X
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

#ifndef VERSION
#define VERSION "unknown"
#endif

typedef struct {
  char *text;
  uint64_t next_update;
} BlockState;

static int signal_pipe[2] = { -1, -1 };

#ifndef NO_X
static Display *dpy;
static Window root;
static Atom status_atom;
static Atom utf8_atom;
#endif

static void
usage(FILE *stream)
{
  fprintf(stream,
          "usage: rawmstat [-p] [-c path]\n"
          "       rawmstat -C [-c path]\n"
          "       rawmstat -v\n");
}

static void *
xmalloc(size_t size)
{
  void *p = malloc(size ? size : 1);

  if (!p) {
    fputs("rawmstat: out of memory\n", stderr);
    exit(EXIT_FAILURE);
  }
  return p;
}

static char *
xstrdup(const char *s)
{
  char *p = strdup(s ? s : "");

  if (!p) {
    fputs("rawmstat: out of memory\n", stderr);
    exit(EXIT_FAILURE);
  }
  return p;
}

static uint64_t
monotonic_msec(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
    perror("rawmstat: clock_gettime");
    exit(EXIT_FAILURE);
  }
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int
set_nonblock_cloexec(int fd)
{
  int flags;

  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return -1;
  flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
    return -1;
  return 0;
}

static void
signal_handler(int signum)
{
  unsigned char byte = (unsigned char)signum;
  int saved_errno = errno;

  if (signal_pipe[1] >= 0)
    (void)write(signal_pipe[1], &byte, 1);
  errno = saved_errno;
}

static int
install_signal_handler(int signum)
{
  struct sigaction action;

  memset(&action, 0, sizeof(action));
  action.sa_handler = signal_handler;
  sigemptyset(&action.sa_mask);
  return sigaction(signum, &action, NULL);
}

static int
setup_signals(const RawmstatConfig *cfg)
{
  size_t i, j;

  if (pipe(signal_pipe) < 0 ||
      set_nonblock_cloexec(signal_pipe[0]) < 0 ||
      set_nonblock_cloexec(signal_pipe[1]) < 0) {
    perror("rawmstat: signal pipe");
    return -1;
  }
  if (install_signal_handler(SIGINT) < 0 ||
      install_signal_handler(SIGTERM) < 0) {
    perror("rawmstat: sigaction");
    return -1;
  }

  for (i = 0; i < cfg->block_count; ++i) {
    int signum = rawmstat_block_signal_number(&cfg->blocks[i]);

    if (signum <= 0)
      continue;
    for (j = 0; j < i; ++j)
      if (rawmstat_block_signal_number(&cfg->blocks[j]) == signum)
        break;
    if (j != i)
      continue;
    if (install_signal_handler(signum) < 0) {
      perror("rawmstat: sigaction");
      return -1;
    }
  }
  return 0;
}

static void
close_signals(void)
{
  if (signal_pipe[0] >= 0)
    close(signal_pipe[0]);
  if (signal_pipe[1] >= 0)
    close(signal_pipe[1]);
  signal_pipe[0] = signal_pipe[1] = -1;
}

static int
run_block(const RawmstatBlock *block, BlockState *state)
{
  unsigned char line[RAWMSTAT_STATUS_MAX + 1];
  unsigned char buffer[512];
  size_t line_len = 0;
  bool line_done = false;
  bool too_long = false;
  bool embedded_nul = false;
  int fds[2];
  pid_t pid;
  int status;
  ssize_t n;
  char *text;
  size_t prefix_len, total;

  if (pipe(fds) < 0) {
    perror("rawmstat: pipe");
    return -1;
  }
  (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);

  pid = fork();
  if (pid < 0) {
    perror("rawmstat: fork");
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    if (dup2(fds[1], STDOUT_FILENO) < 0)
      _exit(127);
    close(fds[0]);
    close(fds[1]);
    execvp(block->argv[0], block->argv);
    perror(block->argv[0]);
    _exit(127);
  }

  close(fds[1]);
  while ((n = read(fds[0], buffer, sizeof(buffer))) != 0) {
    ssize_t i;

    if (n < 0) {
      if (errno == EINTR)
        continue;
      perror("rawmstat: read block output");
      close(fds[0]);
      (void)waitpid(pid, NULL, 0);
      return -1;
    }
    for (i = 0; i < n; ++i) {
      unsigned char ch = buffer[i];

      if (line_done)
        continue;
      if (ch == '\n') {
        line_done = true;
        continue;
      }
      if (ch == '\0') {
        embedded_nul = true;
        continue;
      }
      if (line_len < RAWMSTAT_STATUS_MAX)
        line[line_len++] = ch;
      else
        too_long = true;
    }
  }
  close(fds[0]);

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      perror("rawmstat: waitpid");
      return -1;
    }
  }

  if (too_long || embedded_nul) {
    fprintf(stderr,
            "rawmstat: block '%s' produced invalid rawm-v1 text%s\n",
            block->name, too_long ? " (too long)" : "");
    return -1;
  }
  while (line_len && line[line_len - 1] == '\r')
    --line_len;
  if (!rawmstat_utf8_valid(line, line_len)) {
    fprintf(stderr, "rawmstat: block '%s' produced invalid UTF-8\n",
            block->name);
    return -1;
  }
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && line_len == 0)
    return -1;

  prefix_len = strlen(block->prefix);
  total = prefix_len + line_len;
  if (total > RAWMSTAT_STATUS_MAX) {
    fprintf(stderr, "rawmstat: block '%s' text exceeds rawm-v1 limit\n",
            block->name);
    return -1;
  }
  text = xmalloc(total + 1);
  memcpy(text, block->prefix, prefix_len);
  memcpy(text + prefix_len, line, line_len);
  text[total] = '\0';

  if (state->text && !strcmp(state->text, text)) {
    free(text);
    return 0;
  }
  free(state->text);
  state->text = text;
  return 1;
}

static int
build_status(const RawmstatConfig *cfg, const BlockState *states, char *status,
             size_t size)
{
  size_t i, len = 0;
  bool first = true;
  size_t delimiter_len = strlen(cfg->delimiter);

  if (!rawmstat_utf8_valid((const unsigned char *)cfg->delimiter,
                           delimiter_len)) {
    fputs("rawmstat: status delimiter is not valid UTF-8\n", stderr);
    return -1;
  }

  for (i = 0; i < cfg->block_count; ++i) {
    const char *text = states[i].text ? states[i].text : "";
    size_t text_len = strlen(text);
    size_t extra;

    if (!text_len)
      continue;
    extra = text_len + (first ? 0 : delimiter_len);
    if (extra > RAWMSTAT_STATUS_MAX - len || len + extra + 1 > size) {
      fputs("rawmstat: composed status exceeds rawm-v1 255-byte limit\n",
            stderr);
      return -1;
    }
    if (!first) {
      memcpy(status + len, cfg->delimiter, delimiter_len);
      len += delimiter_len;
    }
    memcpy(status + len, text, text_len);
    len += text_len;
    first = false;
  }
  status[len] = '\0';
  return 0;
}

#ifndef NO_X
static int
setup_x(void)
{
  int screen;

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fputs("rawmstat: cannot open display\n", stderr);
    return -1;
  }
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);
  status_atom = XInternAtom(dpy, "_RAWM_STATUS_V1", False);
  utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);
  return 0;
}

static void
publish_x(const char *status)
{
  XChangeProperty(dpy, root, status_atom, utf8_atom, 8, PropModeReplace,
                  (const unsigned char *)status, (int)strlen(status));
  XFlush(dpy);
}
#endif

static int
publish_status(const char *status, bool stdout_mode, char **last)
{
  if (*last && !strcmp(*last, status))
    return 0;
  free(*last);
  *last = xstrdup(status);

  if (stdout_mode) {
    puts(status);
    fflush(stdout);
    return 0;
  }
#ifndef NO_X
  publish_x(status);
  return 0;
#else
  (void)status;
  fputs("rawmstat: built without X11 support; use -p\n", stderr);
  return -1;
#endif
}

static int
poll_timeout(const RawmstatConfig *cfg, const BlockState *states, uint64_t now)
{
  size_t i;
  uint64_t earliest = UINT64_MAX;

  for (i = 0; i < cfg->block_count; ++i) {
    if (!cfg->blocks[i].interval)
      continue;
    if (states[i].next_update <= now)
      return 0;
    if (states[i].next_update < earliest)
      earliest = states[i].next_update;
  }
  if (earliest == UINT64_MAX)
    return -1;
  if (earliest - now > (uint64_t)INT32_MAX)
    return INT32_MAX;
  return (int)(earliest - now);
}

static void
advance_timer(const RawmstatBlock *block, BlockState *state, uint64_t now)
{
  uint64_t step;

  if (!block->interval)
    return;
  step = (uint64_t)block->interval * 1000u;
  if (!state->next_update)
    state->next_update = now + step;
  else {
    do {
      state->next_update += step;
    } while (state->next_update <= now);
  }
}

static int
status_loop(const RawmstatConfig *cfg, bool stdout_mode)
{
  BlockState *states;
  struct pollfd pfd;
  char status[RAWMSTAT_STATUS_MAX + 1];
  char *last = NULL;
  size_t i;
  bool running = true;
  bool dirty = true;
  bool failed = false;
  uint64_t now;

  states = calloc(cfg->block_count ? cfg->block_count : 1, sizeof(*states));
  if (!states) {
    fputs("rawmstat: out of memory\n", stderr);
    return -1;
  }

  now = monotonic_msec();
  for (i = 0; i < cfg->block_count; ++i) {
    (void)run_block(&cfg->blocks[i], &states[i]);
    advance_timer(&cfg->blocks[i], &states[i], now);
  }

  pfd.fd = signal_pipe[0];
  pfd.events = POLLIN;

  while (running) {
    int timeout;
    int rc;

    if (dirty) {
      if (build_status(cfg, states, status, sizeof(status)) == 0 &&
          publish_status(status, stdout_mode, &last) < 0) {
        failed = true;
        running = false;
        break;
      }
      dirty = false;
    }

    now = monotonic_msec();
    timeout = poll_timeout(cfg, states, now);
    rc = poll(&pfd, 1, timeout);
    if (rc < 0 && errno != EINTR) {
      perror("rawmstat: poll");
      failed = true;
      running = false;
      break;
    }

    if (rc > 0 && (pfd.revents & POLLIN)) {
      unsigned char signals[256] = {0};
      unsigned char bytes[64];
      ssize_t n;

      while ((n = read(signal_pipe[0], bytes, sizeof(bytes))) > 0) {
        ssize_t j;
        for (j = 0; j < n; ++j)
          signals[bytes[j]] = 1;
      }
      if (signals[(unsigned char)SIGINT] || signals[(unsigned char)SIGTERM])
        running = false;
      for (i = 0; running && i < cfg->block_count; ++i) {
        int signum = rawmstat_block_signal_number(&cfg->blocks[i]);
        if (signum > 0 && signals[(unsigned char)signum]) {
          if (run_block(&cfg->blocks[i], &states[i]) > 0)
            dirty = true;
        }
      }
    }

    now = monotonic_msec();
    for (i = 0; running && i < cfg->block_count; ++i) {
      if (cfg->blocks[i].interval && states[i].next_update <= now) {
        if (run_block(&cfg->blocks[i], &states[i]) > 0)
          dirty = true;
        advance_timer(&cfg->blocks[i], &states[i], now);
      }
    }
  }

  for (i = 0; i < cfg->block_count; ++i)
    free(states[i].text);
  free(states);
  free(last);
  return failed ? -1 : 0;
}

int
main(int argc, char **argv)
{
  RawmstatConfig config;
  const char *config_path = NULL;
  bool check_config = false;
  bool stdout_mode = false;
  int i;
  int rc = EXIT_FAILURE;

#ifdef NO_X
  stdout_mode = true;
#endif

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
      if (++i >= argc) {
        usage(stderr);
        return EXIT_FAILURE;
      }
      config_path = argv[i];
    } else if (!strcmp(argv[i], "-C") || !strcmp(argv[i], "--check-config")) {
      check_config = true;
    } else if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--stdout")) {
      stdout_mode = true;
    } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
      printf("rawmstat %s\n", VERSION);
      return EXIT_SUCCESS;
    } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      usage(stdout);
      return EXIT_SUCCESS;
    } else {
      usage(stderr);
      return EXIT_FAILURE;
    }
  }

  rawmstat_config_init(&config);
  if (rawmstat_config_load(&config, config_path, stderr) < 0)
    goto out;
  if (check_config) {
    rc = EXIT_SUCCESS;
    goto out;
  }

#ifndef NO_X
  if (!stdout_mode && setup_x() < 0)
    goto out;
#endif
  if (setup_signals(&config) < 0)
    goto out_x;
  if (status_loop(&config, stdout_mode) < 0)
    goto out_signals;
  rc = EXIT_SUCCESS;

out_signals:
  close_signals();
out_x:
#ifndef NO_X
  if (dpy)
    XCloseDisplay(dpy);
#endif
out:
  rawmstat_config_destroy(&config);
  return rc;
}
