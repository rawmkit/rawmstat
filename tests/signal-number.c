#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
#ifdef SIGRTMIN
  char *end;
  long offset;
  int signum;

  if (argc != 2)
    return 2;
  offset = strtol(argv[1], &end, 10);
  if (!*argv[1] || *end || offset < 0)
    return 2;
  signum = SIGRTMIN + (int)offset;
  if (signum > SIGRTMAX)
    return 77;
  printf("%d\n", signum);
  return 0;
#else
  (void)argc;
  (void)argv;
  return 77;
#endif
}
