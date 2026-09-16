#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void
pause_for_wm(void)
{
  struct timespec ts = { 0, 20000000L };
  nanosleep(&ts, NULL);
}

static Window
find_bar(Display *dpy, Window root)
{
  Window root_return, parent_return, *children = NULL;
  unsigned int count = 0, i;
  int screen_width = DisplayWidth(dpy, DefaultScreen(dpy));
  Window bar = None;

  if (!XQueryTree(dpy, root, &root_return, &parent_return, &children, &count))
    return None;
  for (i = 0; i < count; ++i) {
    XWindowAttributes attr;

    if (!XGetWindowAttributes(dpy, children[i], &attr))
      continue;
    if (attr.map_state == IsViewable && attr.width == screen_width &&
        attr.height > 0 && attr.height < 128) {
      bar = children[i];
      break;
    }
  }
  if (children)
    XFree(children);
  return bar;
}

static uint64_t
checksum_window(Display *dpy, Window window)
{
  XWindowAttributes attr;
  XImage *image;
  uint64_t hash = UINT64_C(1469598103934665603);
  int x, y;

  if (!XGetWindowAttributes(dpy, window, &attr))
    return 0;
  image = XGetImage(dpy, window, 0, 0, (unsigned int)attr.width,
                    (unsigned int)attr.height, AllPlanes, ZPixmap);
  if (!image)
    return 0;
  for (y = 0; y < attr.height; ++y) {
    for (x = 0; x < attr.width; ++x) {
      hash ^= (uint64_t)XGetPixel(image, x, y);
      hash *= UINT64_C(1099511628211);
    }
  }
  XDestroyImage(image);
  return hash;
}

int
main(void)
{
  Display *dpy;
  Window root, bar = None;
  int tries;

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fputs("bar-checksum: cannot open display\n", stderr);
    return EXIT_FAILURE;
  }
  root = DefaultRootWindow(dpy);
  for (tries = 0; tries < 100 && bar == None; ++tries) {
    bar = find_bar(dpy, root);
    if (bar == None)
      pause_for_wm();
  }
  if (bar == None) {
    fputs("bar-checksum: cannot find rawm bar window\n", stderr);
    XCloseDisplay(dpy);
    return EXIT_FAILURE;
  }
  printf("%llu\n", (unsigned long long)checksum_window(dpy, bar));
  XCloseDisplay(dpy);
  return EXIT_SUCCESS;
}
