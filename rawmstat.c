/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#ifndef NO_X
# include <X11/Xlib.h>
#endif

/*********************************************************************
 * Macros.
 */

#if defined(__OpenBSD__) || defined(__DragonFly__)
# define SIGPLUS      SIGUSR1+1
# define SIGMINUS     SIGUSR1-1
#else
# define SIGPLUS      SIGRTMIN
# define SIGMINUS     SIGRTMIN
#endif

#define LENGTH(X)     (sizeof(X) / sizeof(X[0]))
#define MIN(a, b)     ((a < b) ? a : b)

/*********************************************************************
 * Enums & Typedefs.
 */

typedef struct {
	char* icon;
	char* command;
	unsigned int interval;
	unsigned int signal;
} Block;

/*********************************************************************
 * Function declarations.
 */

#ifndef __OpenBSD__
void dummysighandler(int signum);
#endif
void sighandler(int signum);
void getcmds(int time);
void getsigcmds(unsigned int signal);
void setupsignals();
void sighandler(int signum);
int getstatus(char *str, char *last);
void statusloop();
void termhandler();
void pstdout();
#ifndef NO_X
void setroot();
static void (*writestatus) () = setroot;
static int setupX();
static Display *dpy;
static int screen;
static Window root;
#else
static void (*writestatus) () = pstdout;
#endif

/*********************************************************************
 * Global variables.
 */

#include "config.h"

/* Allow the following macro be redefined from config.h */
#ifndef CMDLENGTH
# define CMDLENGTH     50
#endif
#ifndef STATUSLENGTH
# define STATUSLENGTH  (LENGTH(blocks) * CMDLENGTH + 1)
#endif

static char statusbar[LENGTH(blocks)][CMDLENGTH] = {0};
static char statusstr[2][STATUSLENGTH];
static int statusContinue = 1;

/*********************************************************************
 * Function implementations.
 */

/*
 * Opens process *cmd and stores output in *output.
 */
void
getcmd(const Block *block, char *output)
{
	/* make sure status is same until output is ready */
	char tempstatus[CMDLENGTH] = {0};
	strcpy(tempstatus, block->icon);
	FILE *cmdf = popen(block->command, "r");
	if (!cmdf)
		return;
	int i = strlen(block->icon);
	fgets(tempstatus+i, CMDLENGTH-i-delimLen, cmdf);
	i = strlen(tempstatus);

	/* if block and command output are both not empty */
	if (i != 0) {
		/* only chop off newline if one is present at the end */
		i = tempstatus[i-1] == '\n' ? i-1 : i;
		if (delim[0] != '\0')
			strncpy(tempstatus+i, delim, delimLen);
		else
			tempstatus[i++] = '\0';
	}
	strcpy(output, tempstatus);
	pclose(cmdf);
}

void
getcmds(int time)
{
	const Block* current;
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if ((current->interval != 0 &&
		     time % current->interval == 0) || time == -1)
			getcmd(current, statusbar[i]);
	}
}

void
getsigcmds(unsigned int signal)
{
	const Block *current;
	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		current = blocks + i;
		if (current->signal == signal)
			getcmd(current, statusbar[i]);
	}
}

void
setupsignals()
{
#if !defined(__OpenBSD__) && !defined(__DragonFly__)
	/* initialize all real time signals with dummy handler */
	for (int i = SIGRTMIN; i <= SIGRTMAX; i++)
		signal(i, dummysighandler);
#endif

	for (unsigned int i = 0; i < LENGTH(blocks); i++) {
		if (blocks[i].signal > 0)
			signal(SIGMINUS + blocks[i].signal, sighandler);
	}
}

int
getstatus(char *str, char *last)
{
	strcpy(last, str);
	str[0] = '\0';
	for (unsigned int i = 0; i < LENGTH(blocks); i++)
		strcat(str, statusbar[i]);
	str[strlen(str) - strlen(delim)] = '\0';
	return strcmp(str, last); /* 0 if they are the same */
}

#ifndef NO_X
void
setroot()
{
	/* only set root if text has changed */
	if (!getstatus(statusstr[0], statusstr[1]))
		return;

	XStoreName(dpy, root, statusstr[0]);
	XFlush(dpy);
}

int
setupX()
{
	if (!(dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "rawmstat: cannot open display\n");
		return 1;
	}
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	return 0;
}
#endif

void
pstdout()
{
	/* only write out if text has changed */
	if (!getstatus(statusstr[0], statusstr[1]))
		return;

	printf("%s\n", statusstr[0]);
	fflush(stdout);
}


void
statusloop()
{
	setupsignals();
	int i = 0;
	getcmds(-1);
	while (1) {
		getcmds(i++);
		writestatus();
		if (!statusContinue)
			break;
		sleep(1.0);
	}
}

#ifndef __OpenBSD__
/*
 * This signal handler should do nothing.
 */
void
dummysighandler(__attribute__((unused))int signum)
{
    return;
}
#endif

void
sighandler(int signum)
{
	getsigcmds(signum-SIGPLUS);
	writestatus();
}

void
termhandler()
{
	statusContinue = 0;
}

int
main(int argc, char** argv)
{
	for (int i = 0; i < argc; i++) {
		/* handle command line arguments */
		if (!strcmp("-d", argv[i]))
			strncpy(delim, argv[++i], delimLen);
		else if (!strcmp("-p", argv[i]))
			writestatus = pstdout;
	}
#ifndef NO_X
	if (setupX() > 0)
		return EXIT_FAILURE;
#endif
	delimLen = MIN(delimLen, strlen(delim));
	delim[delimLen++] = '\0';
	signal(SIGTERM, termhandler);
	signal(SIGINT, termhandler);
	statusloop();
#ifndef NO_X
	XCloseDisplay(dpy);
#endif
	return EXIT_SUCCESS;
}

/* vim: cc=72 tw=70
 * End of file. */
