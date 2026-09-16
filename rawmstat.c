/* See LICENSE file for copyright and license details. */

#include "rawmstat-config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

#define RAWMSTAT_SHUTDOWN_GRACE_MS 500u

typedef enum {
  RAWMSTAT_STATE_NORMAL,
  RAWMSTAT_STATE_WARNING,
  RAWMSTAT_STATE_CRITICAL
} RawmstatState;

typedef struct {
  char *text;
  RawmstatState state;
  uint64_t next_update;
  bool active;
  bool pending;
  bool timed_out;
  pid_t pid;
  pid_t pgid;
  int fd;
  int wait_status;
  uint64_t deadline;
  unsigned char *output;
  size_t output_len;
  bool line_done;
  bool too_long;
  bool embedded_nul;
  bool read_error;
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

static void *
xcalloc(size_t n, size_t size)
{
  void *p = calloc(n ? n : 1, size ? size : 1);

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

static int
set_cloexec(int fd)
{
  int flags = fcntl(fd, F_GETFD, 0);

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
      install_signal_handler(SIGTERM) < 0 ||
      install_signal_handler(SIGHUP) < 0 ||
      install_signal_handler(SIGCHLD) < 0) {
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

static bool
block_running(const BlockState *state)
{
  return state->active;
}

static void
kill_block_group(const BlockState *state, int signum)
{
  if (state->pgid <= 0)
    return;
  if (kill(-state->pgid, signum) < 0 && errno != ESRCH &&
      state->pid > 0)
    (void)kill(state->pid, signum);
}

static int
start_block(const RawmstatBlock *block, BlockState *state, uint64_t now)
{
  int fds[2];
  pid_t pid;

  if (block_running(state)) {
    state->pending = true;
    return 0;
  }
  if (pipe(fds) < 0) {
    perror("rawmstat: pipe");
    return -1;
  }
  if (set_nonblock_cloexec(fds[0]) < 0 || set_cloexec(fds[1]) < 0) {
    perror("rawmstat: block pipe");
    close(fds[0]);
    close(fds[1]);
    return -1;
  }

  pid = fork();
  if (pid < 0) {
    perror("rawmstat: fork");
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    (void)setpgid(0, 0);
    if (dup2(fds[1], STDOUT_FILENO) < 0)
      _exit(127);
    close(fds[0]);
    close(fds[1]);
    execvp(block->argv[0], block->argv);
    perror(block->argv[0]);
    _exit(127);
  }

  close(fds[1]);
  if (setpgid(pid, pid) < 0 && errno != EACCES && errno != ESRCH)
    perror("rawmstat: setpgid");

  state->active = true;
  state->pending = false;
  state->timed_out = false;
  state->pid = pid;
  state->pgid = pid;
  state->fd = fds[0];
  state->wait_status = 0;
  state->deadline = now + block->timeout_ms;
  state->output = xmalloc((size_t)RAWMSTAT_RAWM_V2_MAX + 1);
  state->output_len = 0;
  state->line_done = false;
  state->too_long = false;
  state->embedded_nul = false;
  state->read_error = false;
  return 0;
}

static void
read_block_output(BlockState *state)
{
  unsigned char buffer[512];
  ssize_t n;

  if (state->fd < 0)
    return;
  for (;;) {
    n = read(state->fd, buffer, sizeof(buffer));
    if (n > 0) {
      ssize_t i;

      for (i = 0; i < n; ++i) {
        unsigned char ch = buffer[i];

        if (state->line_done)
          continue;
        if (ch == '\n') {
          state->line_done = true;
          continue;
        }
        if (ch == '\0') {
          state->embedded_nul = true;
          continue;
        }
        if (state->output_len < RAWMSTAT_RAWM_V2_MAX)
          state->output[state->output_len++] = ch;
        else
          state->too_long = true;
      }
      continue;
    }
    if (n == 0) {
      close(state->fd);
      state->fd = -1;
      return;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    perror("rawmstat: read block output");
    close(state->fd);
    state->fd = -1;
    state->read_error = true;
    return;
  }
}

static void
reap_children(BlockState *states, size_t count)
{
  int status;
  pid_t pid;
  size_t i;

  for (;;) {
    pid = waitpid(-1, &status, WNOHANG);
    if (pid <= 0)
      break;
    for (i = 0; i < count; ++i) {
      if (states[i].active && states[i].pid == pid) {
        states[i].pid = 0;
        states[i].wait_status = status;
        break;
      }
    }
  }
}

static const char *
state_name(RawmstatState state)
{
  switch (state) {
  case RAWMSTAT_STATE_WARNING:
    return "warning";
  case RAWMSTAT_STATE_CRITICAL:
    return "critical";
  case RAWMSTAT_STATE_NORMAL:
  default:
    return "normal";
  }
}

static int
parse_state(const unsigned char *text, size_t length, RawmstatState *state)
{
  if (length == 6 && !memcmp(text, "normal", 6))
    *state = RAWMSTAT_STATE_NORMAL;
  else if (length == 7 && !memcmp(text, "warning", 7))
    *state = RAWMSTAT_STATE_WARNING;
  else if (length == 8 && !memcmp(text, "critical", 8))
    *state = RAWMSTAT_STATE_CRITICAL;
  else
    return 0;
  return 1;
}

static int
set_block_sample(BlockState *state, RawmstatState sample_state,
                 const char *text)
{
  char *copy = xstrdup(text);

  if (state->text && state->state == sample_state && !strcmp(state->text, copy)) {
    free(copy);
    return 0;
  }
  free(state->text);
  state->text = copy;
  state->state = sample_state;
  return 1;
}

static int
finish_block(const RawmstatBlock *block, BlockState *state)
{
  bool success;
  int changed = 0;
  size_t prefix_len, total, sample_len;
  const unsigned char *sample;
  const unsigned char *tab;
  RawmstatState sample_state = RAWMSTAT_STATE_NORMAL;
  char *text;
  const char *sample_error = NULL;

  if (!state->active || state->pid > 0 || state->fd >= 0)
    return 0;

  /* The command owns its process group for the duration of one sample. */
  kill_block_group(state, SIGKILL);

  success = !state->timed_out && !state->too_long && !state->embedded_nul &&
            !state->read_error &&
            WIFEXITED(state->wait_status) &&
            WEXITSTATUS(state->wait_status) == 0;

  while (state->output_len && state->output[state->output_len - 1] == '\r')
    --state->output_len;

  sample = state->output;
  sample_len = state->output_len;
  if (success && sample_len) {
    tab = memchr(sample, '\t', sample_len);
    if (tab) {
      size_t state_len = (size_t)(tab - sample);
      RawmstatState parsed;

      if (parse_state(sample, state_len, &parsed)) {
        sample_state = parsed;
        sample_len -= state_len + 1;
        sample = tab + 1;
      }
    }
  }

  if (success && sample_len &&
      !rawmstat_status_text_valid(sample, sample_len)) {
    sample_error = "sample text must be printable UTF-8";
    success = false;
  }

  if (success && sample_len == 0) {
    changed = set_block_sample(state, RAWMSTAT_STATE_NORMAL, "");
  } else if (success) {
    prefix_len = strlen(block->prefix);
    total = prefix_len + sample_len;
    if (total > RAWMSTAT_RAWM_V2_MAX) {
      sample_error = "sample text exceeds rawm-v2 limit";
      success = false;
    } else {
      text = xmalloc(total + 1);
      memcpy(text, block->prefix, prefix_len);
      memcpy(text + prefix_len, sample, sample_len);
      text[total] = '\0';
      changed = set_block_sample(state, sample_state, text);
      free(text);
    }
  }

  if (!success && !state->timed_out) {
    if (state->read_error) {
      fprintf(stderr,
              "rawmstat: block '%s' output could not be read; keeping previous value\n",
              block->name);
    } else if (state->too_long || state->embedded_nul) {
      fprintf(stderr,
              "rawmstat: block '%s' produced invalid rawm-v2 text%s\n",
              block->name, state->too_long ? " (too long)" : "");
    } else if (sample_error) {
      fprintf(stderr,
              "rawmstat: block '%s' produced invalid rawm-v2 sample: %s; keeping previous value\n",
              block->name, sample_error);
    } else if (WIFEXITED(state->wait_status)) {
      fprintf(stderr,
              "rawmstat: block '%s' exited with status %d; keeping previous value\n",
              block->name, WEXITSTATUS(state->wait_status));
    } else if (WIFSIGNALED(state->wait_status)) {
      fprintf(stderr,
              "rawmstat: block '%s' was terminated by signal %d; keeping previous value\n",
              block->name, WTERMSIG(state->wait_status));
    }
  }

  free(state->output);
  state->output = NULL;
  state->output_len = 0;
  state->active = false;
  state->timed_out = false;
  state->pid = 0;
  state->pgid = 0;
  state->fd = -1;
  state->deadline = 0;
  state->line_done = false;
  state->too_long = false;
  state->embedded_nul = false;
  state->read_error = false;
  return changed;
}

static int
finish_ready_blocks(const RawmstatConfig *cfg, BlockState *states, uint64_t now)
{
  size_t i;
  int changed = 0;

  for (i = 0; i < cfg->block_count; ++i) {
    if (!states[i].active || states[i].pid > 0 || states[i].fd >= 0)
      continue;
    if (finish_block(&cfg->blocks[i], &states[i]) > 0)
      changed = 1;
    if (states[i].pending)
      (void)start_block(&cfg->blocks[i], &states[i], now);
  }
  return changed;
}

static char *
build_status(const RawmstatConfig *cfg, const BlockState *states)
{
  size_t i, len = 0;
  char *status;

  for (i = 0; i < cfg->block_count; ++i) {
    const char *text = states[i].text ? states[i].text : "";
    const char *state = state_name(states[i].state);
    size_t id_len, state_len, text_len, extra;

    if (!text[0])
      continue;
    id_len = strlen(cfg->blocks[i].name);
    state_len = strlen(state);
    text_len = strlen(text);
    extra = id_len + 1 + state_len + 1 + text_len + 1;
    if (extra > RAWMSTAT_RAWM_V2_MAX - len) {
      fprintf(stderr,
              "rawmstat: composed status exceeds rawm-v2 %u-byte limit\n",
              RAWMSTAT_RAWM_V2_MAX);
      return NULL;
    }
    len += extra;
  }

  status = xmalloc(len + 1);
  len = 0;
  for (i = 0; i < cfg->block_count; ++i) {
    const char *text = states[i].text ? states[i].text : "";
    const char *state = state_name(states[i].state);
    size_t id_len, state_len, text_len;

    if (!text[0])
      continue;
    id_len = strlen(cfg->blocks[i].name);
    state_len = strlen(state);
    text_len = strlen(text);
    memcpy(status + len, cfg->blocks[i].name, id_len);
    len += id_len;
    status[len++] = '\t';
    memcpy(status + len, state, state_len);
    len += state_len;
    status[len++] = '\t';
    memcpy(status + len, text, text_len);
    len += text_len;
    status[len++] = '\n';
  }
  status[len] = '\0';
  return status;
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
  status_atom = XInternAtom(dpy, "_RAWM_STATUS_V2", False);
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
    fputs(status, stdout);
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
poll_timeout(const RawmstatConfig *cfg, const BlockState *states, uint64_t now)
{
  size_t i;
  uint64_t earliest = UINT64_MAX;

  for (i = 0; i < cfg->block_count; ++i) {
    if (cfg->blocks[i].interval && states[i].next_update < earliest)
      earliest = states[i].next_update;
    if (states[i].active && !states[i].timed_out &&
        states[i].deadline < earliest)
      earliest = states[i].deadline;
  }
  if (earliest == UINT64_MAX)
    return -1;
  if (earliest <= now)
    return 0;
  if (earliest - now > (uint64_t)INT_MAX)
    return INT_MAX;
  return (int)(earliest - now);
}

static size_t
build_pollfds(const RawmstatConfig *cfg, const BlockState *states,
              struct pollfd *pfds, size_t *map)
{
  size_t i, count = 1;

  pfds[0].fd = signal_pipe[0];
  pfds[0].events = POLLIN;
  pfds[0].revents = 0;
  map[0] = SIZE_MAX;

  for (i = 0; i < cfg->block_count; ++i) {
    if (!states[i].active || states[i].fd < 0)
      continue;
    pfds[count].fd = states[i].fd;
    pfds[count].events = POLLIN | POLLHUP | POLLERR;
    pfds[count].revents = 0;
    map[count] = i;
    ++count;
  }
  return count;
}

static void
process_timeouts(const RawmstatConfig *cfg, BlockState *states, uint64_t now)
{
  size_t i;

  for (i = 0; i < cfg->block_count; ++i) {
    if (!states[i].active || states[i].timed_out ||
        states[i].deadline > now)
      continue;
    fprintf(stderr, "rawmstat: block '%s' timed out after %u ms\n",
            cfg->blocks[i].name, cfg->blocks[i].timeout_ms);
    states[i].timed_out = true;
    kill_block_group(&states[i], SIGKILL);
  }
}

static void
request_due_blocks(const RawmstatConfig *cfg, BlockState *states, uint64_t now)
{
  size_t i;

  for (i = 0; i < cfg->block_count; ++i) {
    if (!cfg->blocks[i].interval || states[i].next_update > now)
      continue;
    if (states[i].active)
      states[i].pending = true;
    else
      (void)start_block(&cfg->blocks[i], &states[i], now);
    advance_timer(&cfg->blocks[i], &states[i], now);
  }
}

static void
request_signaled_blocks(const RawmstatConfig *cfg, BlockState *states,
                        const unsigned char signals[256], uint64_t now)
{
  size_t i;

  for (i = 0; i < cfg->block_count; ++i) {
    int signum = rawmstat_block_signal_number(&cfg->blocks[i]);

    if (signum <= 0 || !signals[(unsigned char)signum])
      continue;
    if (states[i].active)
      states[i].pending = true;
    else
      (void)start_block(&cfg->blocks[i], &states[i], now);
  }
}

static void
terminate_children(BlockState *states, size_t count)
{
  uint64_t deadline;
  size_t i;
  struct timespec pause = { 0, 20000000L };
  bool any;

  for (i = 0; i < count; ++i)
    if (states[i].active)
      kill_block_group(&states[i], SIGTERM);

  deadline = monotonic_msec() + RAWMSTAT_SHUTDOWN_GRACE_MS;
  do {
    reap_children(states, count);
    any = false;
    for (i = 0; i < count; ++i)
      if (states[i].active && states[i].pid > 0)
        any = true;
    if (!any || monotonic_msec() >= deadline)
      break;
    nanosleep(&pause, NULL);
  } while (true);

  for (i = 0; i < count; ++i)
    if (states[i].active)
      kill_block_group(&states[i], SIGKILL);

  for (i = 0; i < count; ++i) {
    int status;

    if (states[i].pid > 0) {
      while (waitpid(states[i].pid, &status, 0) < 0 && errno == EINTR)
        ;
      states[i].pid = 0;
    }
    if (states[i].fd >= 0) {
      close(states[i].fd);
      states[i].fd = -1;
    }
    free(states[i].output);
    states[i].output = NULL;
    states[i].active = false;
    states[i].pgid = 0;
  }
}

static int
status_loop(const RawmstatConfig *cfg, bool stdout_mode, bool *restart)
{
  BlockState *states;
  struct pollfd *pfds;
  size_t *map;
  char *last = NULL;
  size_t i;
  bool running = true;
  bool dirty = false;
  bool failed = false;
  uint64_t now;

  states = xcalloc(cfg->block_count, sizeof(*states));
  pfds = xcalloc(cfg->block_count + 1, sizeof(*pfds));
  map = xcalloc(cfg->block_count + 1, sizeof(*map));
  for (i = 0; i < cfg->block_count; ++i)
    states[i].fd = -1;

#ifndef NO_X
  if (!stdout_mode && publish_status("", false, &last) < 0) {
    failed = true;
    goto out;
  }
#endif

  now = monotonic_msec();
  for (i = 0; i < cfg->block_count; ++i) {
    (void)start_block(&cfg->blocks[i], &states[i], now);
    advance_timer(&cfg->blocks[i], &states[i], now);
  }

  while (running) {
    size_t poll_count;
    int timeout, rc;

    if (dirty) {
      char *status = build_status(cfg, states);

      if (status) {
        if (publish_status(status, stdout_mode, &last) < 0) {
          free(status);
          failed = true;
          break;
        }
        free(status);
      }
      dirty = false;
    }

    now = monotonic_msec();
    poll_count = build_pollfds(cfg, states, pfds, map);
    timeout = poll_timeout(cfg, states, now);
    rc = poll(pfds, (nfds_t)poll_count, timeout);
    if (rc < 0 && errno != EINTR) {
      perror("rawmstat: poll");
      failed = true;
      break;
    }

    if (rc > 0 && (pfds[0].revents & POLLIN)) {
      unsigned char signals[256] = {0};
      unsigned char bytes[64];
      ssize_t n;

      while ((n = read(signal_pipe[0], bytes, sizeof(bytes))) > 0) {
        ssize_t j;
        for (j = 0; j < n; ++j)
          signals[bytes[j]] = 1;
      }
      if (signals[(unsigned char)SIGINT] ||
          signals[(unsigned char)SIGTERM]) {
        running = false;
      } else if (signals[(unsigned char)SIGHUP]) {
        *restart = true;
        running = false;
      } else {
        now = monotonic_msec();
        request_signaled_blocks(cfg, states, signals, now);
      }
    }

    if (rc > 0) {
      size_t p;

      for (p = 1; p < poll_count; ++p) {
        if (pfds[p].revents & (POLLIN | POLLHUP | POLLERR))
          read_block_output(&states[map[p]]);
      }
    }

    reap_children(states, cfg->block_count);
    now = monotonic_msec();
    process_timeouts(cfg, states, now);
    request_due_blocks(cfg, states, now);
    reap_children(states, cfg->block_count);
    if (finish_ready_blocks(cfg, states, now) > 0)
      dirty = true;
  }

out:
  terminate_children(states, cfg->block_count);
  for (i = 0; i < cfg->block_count; ++i) {
    free(states[i].text);
    free(states[i].output);
  }
  free(states);
  free(pfds);
  free(map);
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
  bool restart = false;
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
  if (status_loop(&config, stdout_mode, &restart) < 0)
    goto out_signals;
  rc = EXIT_SUCCESS;

out_signals:
  close_signals();
out_x:
#ifndef NO_X
  if (dpy) {
    XCloseDisplay(dpy);
    dpy = NULL;
  }
#endif
out:
  rawmstat_config_destroy(&config);
  if (restart && rc == EXIT_SUCCESS) {
    execvp(argv[0], argv);
    perror("rawmstat: execvp");
    return EXIT_FAILURE;
  }
  return rc;
}
