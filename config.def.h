/* rawmstat customization.
 * See LICENSE file for copyright and license details. */

/*
 * CMDLENGTH is a maximum possible length of output from block,
 * expressed in number of characters.  Declare it if need to increase
 * the default length.
 */
/*#define CMDLENGTH 50*/

/*
 * Modify this file to change what commands output to your statusbar,
 * and recompile using the make command.
 */
static const Block blocks[] = {
/*
 * Icon  Command                      Update Interval  Update Signal
 */
  {"",   "date '+%a %b %d %H:%M'",    5,               0},
};

/*
 * Sets delimiter between status commands.
 * NULL character ('\0') means no delimiter.
 */
static char delim[] = " | ";
static unsigned int delimLen = 5;

/* vim: cc=72 tw=70
 * End of file. */
