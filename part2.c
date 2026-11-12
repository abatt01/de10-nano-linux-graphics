/*
 * Part 2: Exercise Bresenham lines with several slopes and endpoint orders.
 * Build: make part2
 * Run with video.ko double_buffer=0.
 * The shared helper functions are included here so this file builds on its own.
 */
#ifndef VIDEO_USER_H
#define VIDEO_USER_H
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
/* A signal handler only sets this flag. Drawing and device cleanup stay in
 * normal program context rather than running inside the signal handler. */
static volatile sig_atomic_t stopped;
static void stop_animation(int sig) { (void)sig; stopped = 1; }
static void fatal(const char *what) { perror(what); exit(EXIT_FAILURE); }
/* Submit a bounded, complete command. The driver does not reassemble partial
 * records, so an unexpected short write is an error rather than a retry tail. */
static void command(int fd, const char *format, ...)
{
    char buf[256];
    int len;
    ssize_t n;
    va_list ap;
    va_start(ap, format);
    len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        fprintf(stderr, "Video command too long\n"); exit(EXIT_FAILURE);
    }
    do { n = write(fd, buf, (size_t)len); } while (n < 0 && errno == EINTR);
    if (n < 0) fatal("write /dev/video");
    if (n != len) { fprintf(stderr, "Short video command write\n"); exit(EXIT_FAILURE); }
}
/* Read dimensions once, then reuse the descriptor for writes. The driver's
 * write handler intentionally ignores the resolution read's file offset. */
static int open_video(int *w, int *h)
{
    char buf[32];
    ssize_t n;
    int fd = open("/dev/video", O_RDWR);
    if (fd < 0) fatal("open /dev/video");
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) fatal("read video resolution");
    buf[n] = 0;
    if (sscanf(buf, "%d %d", w, h) != 2 || *w < 8 || *h < 8) {
        fprintf(stderr, "Invalid video resolution: %s\n", buf); exit(EXIT_FAILURE);
    }
    printf("Screen: %d x %d\n", *w, *h);
    signal(SIGINT, stop_animation);
    signal(SIGTERM, stop_animation);
    return fd;
}
#endif

int main(void)
{
    int w, h, fd = open_video(&w, &h);
    command(fd, "clear");
    /* Fan lines, a diagonal, a vertical, a horizontal, and reversed endpoints
     * exercise the driver's general line algorithm. Later lines overwrite
     * earlier colors where they intersect. */
    command(fd, "line 0,%d %d,0 FFE0", h-1, w-1);
    command(fd, "line 0,%d %d,0 07FF", h-1, w/2-1);
    command(fd, "line 0,%d %d,0 07E0", h-1, w/4-1);
    command(fd, "line 0,0 %d,%d F800", w-1, h-1);
    command(fd, "line %d,0 %d,%d FFFF", w/2, w/2, h-1);
    command(fd, "line 0,%d %d,%d 001F", h/2, w-1, h/2);
    command(fd, "line %d,%d 0,0 F81F", w-1, h/4);
    close(fd);
    return 0;
}
