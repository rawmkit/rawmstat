#include "rawmstat-config.h"

#include <errno.h>
#include <libconfig.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef SYSCONFDIR
#define SYSCONFDIR "/etc"
#endif

static void *
xcalloc(size_t n, size_t size)
{
  void *p = calloc(n, size);

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

static void
replace_string(char **dst, const char *src)
{
  free(*dst);
  *dst = xstrdup(src);
}

static void
free_block(RawmstatBlock *block)
{
  size_t i;

  free(block->name);
  free(block->prefix);
  for (i = 0; i < block->argc; ++i)
    free(block->argv[i]);
  free(block->argv);
  memset(block, 0, sizeof(*block));
}

static void
free_blocks(RawmstatBlock *blocks, size_t count)
{
  size_t i;

  for (i = 0; i < count; ++i)
    free_block(&blocks[i]);
  free(blocks);
}

int
rawmstat_utf8_valid(const unsigned char *text, size_t length)
{
  size_t i = 0;

  while (i < length) {
    unsigned char c = text[i++];

    if (c <= 0x7f)
      continue;
    if (c >= 0xc2 && c <= 0xdf) {
      if (i >= length || text[i] < 0x80 || text[i] > 0xbf)
        return 0;
      ++i;
      continue;
    }
    if (c == 0xe0) {
      if (i + 1 >= length || text[i] < 0xa0 || text[i] > 0xbf ||
          text[i + 1] < 0x80 || text[i + 1] > 0xbf)
        return 0;
      i += 2;
      continue;
    }
    if ((c >= 0xe1 && c <= 0xec) || (c >= 0xee && c <= 0xef)) {
      if (i + 1 >= length || text[i] < 0x80 || text[i] > 0xbf ||
          text[i + 1] < 0x80 || text[i + 1] > 0xbf)
        return 0;
      i += 2;
      continue;
    }
    if (c == 0xed) {
      if (i + 1 >= length || text[i] < 0x80 || text[i] > 0x9f ||
          text[i + 1] < 0x80 || text[i + 1] > 0xbf)
        return 0;
      i += 2;
      continue;
    }
    if (c == 0xf0) {
      if (i + 2 >= length || text[i] < 0x90 || text[i] > 0xbf ||
          text[i + 1] < 0x80 || text[i + 1] > 0xbf ||
          text[i + 2] < 0x80 || text[i + 2] > 0xbf)
        return 0;
      i += 3;
      continue;
    }
    if (c >= 0xf1 && c <= 0xf3) {
      if (i + 2 >= length || text[i] < 0x80 || text[i] > 0xbf ||
          text[i + 1] < 0x80 || text[i + 1] > 0xbf ||
          text[i + 2] < 0x80 || text[i + 2] > 0xbf)
        return 0;
      i += 3;
      continue;
    }
    if (c == 0xf4) {
      if (i + 2 >= length || text[i] < 0x80 || text[i] > 0x8f ||
          text[i + 1] < 0x80 || text[i + 1] > 0xbf ||
          text[i + 2] < 0x80 || text[i + 2] > 0xbf)
        return 0;
      i += 3;
      continue;
    }
    return 0;
  }
  return 1;
}

int
rawmstat_status_text_valid(const unsigned char *text, size_t length)
{
  size_t i;

  if (!rawmstat_utf8_valid(text, length))
    return 0;
  for (i = 0; i < length; ++i)
    if (text[i] < 0x20 || text[i] == 0x7f)
      return 0;
  return 1;
}

static int
status_id_valid(const char *name)
{
  size_t i, length;

  if (!name)
    return 0;
  length = strlen(name);
  if (!length || length > RAWMSTAT_STATUS_ID_MAX)
    return 0;
  for (i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)name[i];

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
      continue;
    return 0;
  }
  return 1;
}

const char *
rawmstat_protocol_name(RawmstatProtocol protocol)
{
  return protocol == RAWMSTAT_PROTOCOL_RAWM_V2 ? "rawm-v2" : NULL;
}

static int
parse_protocol(const char *name, RawmstatProtocol *protocol)
{
  if (!strcmp(name, "rawm-v2")) {
    *protocol = RAWMSTAT_PROTOCOL_RAWM_V2;
    return 0;
  }
  return -1;
}

void
rawmstat_config_init(RawmstatConfig *cfg)
{
  static const char *const argv[] = { "date", "+%a %b %d %H:%M" };
  RawmstatBlock *block;
  size_t i;

  memset(cfg, 0, sizeof(*cfg));
  cfg->protocol = RAWMSTAT_PROTOCOL_RAWM_V2;
  cfg->blocks = xcalloc(1, sizeof(*cfg->blocks));
  cfg->block_count = 1;

  block = &cfg->blocks[0];
  block->name = xstrdup("clock");
  block->prefix = xstrdup("");
  block->argc = sizeof(argv) / sizeof(argv[0]);
  block->argv = xcalloc(block->argc + 1, sizeof(*block->argv));
  for (i = 0; i < block->argc; ++i)
    block->argv[i] = xstrdup(argv[i]);
  block->interval = 5;
  block->signal = 0;
  block->timeout_ms = RAWMSTAT_DEFAULT_TIMEOUT_MS;
}

void
rawmstat_config_destroy(RawmstatConfig *cfg)
{
  free_blocks(cfg->blocks, cfg->block_count);
  memset(cfg, 0, sizeof(*cfg));
}

static int
cfg_error(FILE *err, const config_setting_t *setting, const char *fmt,
          const char *detail)
{
  const char *file = setting ? config_setting_source_file(setting) : NULL;
  unsigned int line = setting ? config_setting_source_line(setting) : 0;

  if (file && line)
    fprintf(err, "rawmstat: %s:%u: ", file, line);
  else
    fputs("rawmstat: configuration error: ", err);
  fprintf(err, fmt, detail);
  fputc('\n', err);
  return -1;
}

static bool
name_allowed(const char *name, const char *const *allowed, size_t count)
{
  size_t i;

  for (i = 0; i < count; ++i)
    if (!strcmp(name, allowed[i]))
      return true;
  return false;
}

static int
validate_members(FILE *err, const config_setting_t *group,
                 const char *const *allowed, size_t count)
{
  int i, n;

  if (!group || config_setting_type(group) != CONFIG_TYPE_GROUP)
    return cfg_error(err, group, "expected a group%s", "");
  n = config_setting_length(group);
  for (i = 0; i < n; ++i) {
    config_setting_t *member = config_setting_get_elem(group, (unsigned int)i);
    const char *name = config_setting_name(member);

    if (!name_allowed(name, allowed, count))
      return cfg_error(err, member, "unknown setting '%s'", name);
  }
  return 0;
}

static int
parse_status(FILE *err, RawmstatConfig *cfg, const config_setting_t *group)
{
  static const char *const allowed[] = { "protocol" };
  config_setting_t *setting;
  const char *value;

  if (validate_members(err, group, allowed, 1) < 0)
    return -1;

  setting = config_setting_get_member(group, "protocol");
  if (setting) {
    if (config_setting_type(setting) != CONFIG_TYPE_STRING)
      return cfg_error(err, setting, "status.protocol must be a string%s", "");
    value = config_setting_get_string(setting);
    if (parse_protocol(value, &cfg->protocol) < 0)
      return cfg_error(err, setting, "unsupported status protocol '%s'", value);
  }

  return 0;
}

static int
parse_blocks(FILE *err, RawmstatConfig *cfg, const config_setting_t *list)
{
  static const char *const allowed[] = {
    "name", "prefix", "command", "interval", "signal", "timeout_ms"
  };
  RawmstatBlock *blocks;
  int i, j, n;

  if (config_setting_type(list) != CONFIG_TYPE_LIST &&
      config_setting_type(list) != CONFIG_TYPE_ARRAY)
    return cfg_error(err, list, "blocks must be a list%s", "");

  n = config_setting_length(list);
  blocks = xcalloc((size_t)n, sizeof(*blocks));
  for (i = 0; i < n; ++i) {
    config_setting_t *entry = config_setting_get_elem(list, (unsigned int)i);
    config_setting_t *setting;
    int argc;

    if (validate_members(err, entry, allowed, 6) < 0)
      goto fail;

    setting = config_setting_get_member(entry, "name");
    if (!setting || config_setting_type(setting) != CONFIG_TYPE_STRING) {
      cfg_error(err, setting ? setting : entry,
                "block requires string setting '%s'", "name");
      goto fail;
    }
    blocks[i].name = xstrdup(config_setting_get_string(setting));
    blocks[i].prefix = xstrdup("");

    setting = config_setting_get_member(entry, "prefix");
    if (setting) {
      if (config_setting_type(setting) != CONFIG_TYPE_STRING) {
        cfg_error(err, setting, "block prefix must be a string%s", "");
        goto fail;
      }
      replace_string(&blocks[i].prefix, config_setting_get_string(setting));
    }

    setting = config_setting_get_member(entry, "command");
    if (!setting || (config_setting_type(setting) != CONFIG_TYPE_LIST &&
                     config_setting_type(setting) != CONFIG_TYPE_ARRAY)) {
      cfg_error(err, setting ? setting : entry,
                "block requires argv list setting '%s'", "command");
      goto fail;
    }
    argc = config_setting_length(setting);
    if (argc < 1) {
      cfg_error(err, setting, "block command must not be empty%s", "");
      goto fail;
    }
    blocks[i].argv = xcalloc((size_t)argc + 1, sizeof(*blocks[i].argv));
    blocks[i].argc = (size_t)argc;
    for (j = 0; j < argc; ++j) {
      config_setting_t *arg = config_setting_get_elem(setting, (unsigned int)j);
      if (config_setting_type(arg) != CONFIG_TYPE_STRING) {
        cfg_error(err, arg, "block command arguments must be strings%s", "");
        goto fail;
      }
      blocks[i].argv[j] = xstrdup(config_setting_get_string(arg));
    }

    setting = config_setting_get_member(entry, "interval");
    if (setting) {
      int number;
      if (config_setting_type(setting) != CONFIG_TYPE_INT) {
        cfg_error(err, setting, "block interval must be an integer%s", "");
        goto fail;
      }
      number = config_setting_get_int(setting);
      if (number < 0) {
        cfg_error(err, setting, "block interval must be >= %s", "0");
        goto fail;
      }
      blocks[i].interval = (unsigned int)number;
    }

    setting = config_setting_get_member(entry, "signal");
    if (setting) {
      int number;
      if (config_setting_type(setting) != CONFIG_TYPE_INT) {
        cfg_error(err, setting, "block signal must be an integer%s", "");
        goto fail;
      }
      number = config_setting_get_int(setting);
      if (number < 0) {
        cfg_error(err, setting, "block signal must be >= %s", "0");
        goto fail;
      }
      blocks[i].signal = (unsigned int)number;
    }

    blocks[i].timeout_ms = RAWMSTAT_DEFAULT_TIMEOUT_MS;
    setting = config_setting_get_member(entry, "timeout_ms");
    if (setting) {
      int number;
      if (config_setting_type(setting) != CONFIG_TYPE_INT) {
        cfg_error(err, setting, "block timeout_ms must be an integer%s", "");
        goto fail;
      }
      number = config_setting_get_int(setting);
      if (number < 1 || number > 600000) {
        cfg_error(err, setting, "block timeout_ms must be in range %s",
                  "1..600000");
        goto fail;
      }
      blocks[i].timeout_ms = (unsigned int)number;
    }
  }

  free_blocks(cfg->blocks, cfg->block_count);
  cfg->blocks = blocks;
  cfg->block_count = (size_t)n;
  return 0;

fail:
  free_blocks(blocks, (size_t)n);
  return -1;
}

static int
parse_layer(FILE *err, RawmstatConfig *cfg, config_t *source)
{
  static const char *const allowed[] = { "status", "blocks" };
  config_setting_t *root = config_root_setting(source);
  config_setting_t *setting;

  if (validate_members(err, root, allowed, 2) < 0)
    return -1;
  setting = config_setting_get_member(root, "status");
  if (setting && parse_status(err, cfg, setting) < 0)
    return -1;
  setting = config_setting_get_member(root, "blocks");
  if (setting && parse_blocks(err, cfg, setting) < 0)
    return -1;
  return 0;
}

static char *
dirname_dup(const char *path)
{
  const char *slash = strrchr(path, '/');
  size_t len;
  char *dir;

  if (!slash)
    return xstrdup(".");
  if (slash == path)
    return xstrdup("/");
  len = (size_t)(slash - path);
  dir = xcalloc(len + 1, 1);
  memcpy(dir, path, len);
  return dir;
}

static int
load_file(FILE *err, RawmstatConfig *cfg, const char *path, bool required)
{
  config_t source;
  struct stat st;
  char *dir;
  int rc;

  if (stat(path, &st) < 0) {
    if (!required && errno == ENOENT)
      return 0;
    fprintf(err, "rawmstat: cannot read configuration %s: %s\n", path,
            strerror(errno));
    return -1;
  }
  if (!S_ISREG(st.st_mode)) {
    fprintf(err, "rawmstat: configuration %s is not a regular file\n", path);
    return -1;
  }

  config_init(&source);
  dir = dirname_dup(path);
  config_set_include_dir(&source, dir);
  if (!config_read_file(&source, path)) {
    fprintf(err, "rawmstat: %s:%d: %s\n",
            config_error_file(&source) ? config_error_file(&source) : path,
            config_error_line(&source),
            config_error_text(&source) ? config_error_text(&source)
                                       : "configuration error");
    free(dir);
    config_destroy(&source);
    return -1;
  }
  rc = parse_layer(err, cfg, &source);
  free(dir);
  config_destroy(&source);
  return rc;
}

static char *
user_config_path(void)
{
  const char *xdg = getenv("XDG_CONFIG_HOME");
  const char *home;
  size_t len;
  char *path;

  if (xdg && *xdg && xdg[0] == '/') {
    len = strlen(xdg) + strlen("/rawm/rawmstat.conf") + 1;
    path = xcalloc(len, 1);
    snprintf(path, len, "%s/rawm/rawmstat.conf", xdg);
    return path;
  }
  home = getenv("HOME");
  if (!home || !*home)
    return NULL;
  len = strlen(home) + strlen("/.config/rawm/rawmstat.conf") + 1;
  path = xcalloc(len, 1);
  snprintf(path, len, "%s/.config/rawm/rawmstat.conf", home);
  return path;
}

int
rawmstat_block_signal_number(const RawmstatBlock *block)
{
  if (!block || block->signal == 0)
    return 0;
#ifdef SIGRTMIN
  if (block->signal > (unsigned int)(SIGRTMAX - SIGRTMIN))
    return -1;
  return SIGRTMIN + (int)block->signal;
#else
  return -1;
#endif
}

int
rawmstat_config_validate(const RawmstatConfig *cfg, FILE *err)
{
  size_t i, j;

  if (!rawmstat_protocol_name(cfg->protocol)) {
    fputs("rawmstat: configuration error: invalid status protocol\n", err);
    return -1;
  }
  for (i = 0; i < cfg->block_count; ++i) {
    const RawmstatBlock *block = &cfg->blocks[i];

    if (!status_id_valid(block->name)) {
      fputs("rawmstat: configuration error: block name must be 1..64 characters from [A-Za-z0-9._-]\n", err);
      return -1;
    }
    if (!block->prefix || strlen(block->prefix) > RAWMSTAT_RAWM_V2_MAX ||
        !rawmstat_status_text_valid((const unsigned char *)block->prefix,
                                    strlen(block->prefix))) {
      fprintf(err, "rawmstat: configuration error: block '%s' prefix must be printable UTF-8 within the rawm-v2 limit\n",
              block->name);
      return -1;
    }
    if (!block->argv || block->argc == 0 || !block->argv[0] || !*block->argv[0]) {
      fprintf(err, "rawmstat: configuration error: block '%s' has no executable\n",
              block->name);
      return -1;
    }
    if (block->timeout_ms < 1 || block->timeout_ms > 600000) {
      fprintf(err,
              "rawmstat: configuration error: block '%s' timeout_ms must be in range 1..600000\n",
              block->name);
      return -1;
    }
    {
      int signum = rawmstat_block_signal_number(block);
      if (signum < 0 || signum > UCHAR_MAX) {
        fprintf(err,
                "rawmstat: configuration error: block '%s' signal offset %u is unavailable\n",
                block->name, block->signal);
        return -1;
      }
    }
    for (j = 0; j < i; ++j) {
      if (!strcmp(cfg->blocks[j].name, block->name)) {
        fprintf(err, "rawmstat: configuration error: duplicate block name '%s'\n",
                block->name);
        return -1;
      }
    }
  }
  return 0;
}

int
rawmstat_config_load(RawmstatConfig *cfg, const char *explicit_path, FILE *err)
{
  char system_path[PATH_MAX];
  char *user_path;

  if (explicit_path) {
    if (load_file(err, cfg, explicit_path, true) < 0)
      return -1;
    return rawmstat_config_validate(cfg, err);
  }

  snprintf(system_path, sizeof(system_path), "%s/rawm/rawmstat.conf",
           SYSCONFDIR);
  if (load_file(err, cfg, system_path, false) < 0)
    return -1;
  user_path = user_config_path();
  if (user_path) {
    int rc = load_file(err, cfg, user_path, false);
    free(user_path);
    if (rc < 0)
      return -1;
  }
  return rawmstat_config_validate(cfg, err);
}
