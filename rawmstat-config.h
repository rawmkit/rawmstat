#ifndef RAWMSTAT_CONFIG_H
#define RAWMSTAT_CONFIG_H

#include <stddef.h>
#include <stdio.h>

#define RAWMSTAT_STATUS_MAX 255

typedef enum {
  RAWMSTAT_PROTOCOL_RAWM_V1
} RawmstatProtocol;

typedef struct {
  char *name;
  char *prefix;
  char **argv;
  size_t argc;
  unsigned int interval;
  unsigned int signal;
} RawmstatBlock;

typedef struct {
  RawmstatProtocol protocol;
  char *delimiter;
  RawmstatBlock *blocks;
  size_t block_count;
} RawmstatConfig;

void rawmstat_config_init(RawmstatConfig *cfg);
void rawmstat_config_destroy(RawmstatConfig *cfg);
int rawmstat_config_load(RawmstatConfig *cfg, const char *explicit_path,
                         FILE *err);
int rawmstat_config_validate(const RawmstatConfig *cfg, FILE *err);
const char *rawmstat_protocol_name(RawmstatProtocol protocol);
int rawmstat_block_signal_number(const RawmstatBlock *block);
int rawmstat_utf8_valid(const unsigned char *text, size_t length);

#endif
