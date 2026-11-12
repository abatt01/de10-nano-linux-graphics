/*
 * Part 4: Animate eight linked rectangles using double-buffered frame presentation.
 * Build: make part4
 * Run with video.ko double_buffer=1.
 * The shared helper functions are included here so this file builds on its own.
 */
#define LAB_PART 4
/* Each part sets LAB_PART before this code. No direct /dev/mem access. */
#define _POSIX_C_SOURCE 200809L
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

#define MAX_OBJECTS 16
#define BOX_SIZE 5
/* x/y are the top-left corner of an inclusive BOX_SIZE square. dx/dy
 * are independent signs, so every object follows one of four diagonals. */
struct object { int x, y, dx, dy; unsigned int color; };
static struct object objects[MAX_OBJECTS];
static const unsigned int palette[] = {
    0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f, 0xffff, 0xfd20
};
/* Levels 1-3 slow motion with extra sleep. Level 4 moves one pixel per
 * refresh; higher levels increase distance rather than the display rate. */
static const int delays_ms[] = {100, 60, 30, 0, 0, 0, 0, 0};
static const int steps[] = {1, 1, 1, 1, 2, 3, 4, 6};

/* Open each time: compatible with lab drivers whose reads use per-open EOF. */
static unsigned int read_input(const char *path)
{
    char buf[32], *end;
    ssize_t n;
    unsigned long value;
    int fd = open(path, O_RDONLY);
    if (fd < 0) fatal(path);
    do { n = read(fd, buf, sizeof(buf)-1); } while (n < 0 && errno == EINTR);
    if (n <= 0) { close(fd); fprintf(stderr, "No input data from %s\n", path); exit(1); }
    close(fd);
    buf[n] = 0;
    errno = 0;
    value = strtoul(buf, &end, 16);
    if (end == buf || errno || value > 0xffffffffUL) {
        fprintf(stderr, "Invalid hex input from %s: %s\n", path, buf); exit(1);
    }
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') ++end;
    if (*end) { fprintf(stderr, "Unexpected input format in %s\n", path); exit(1); }
    return (unsigned int)value;
}
/* Monotonic time avoids wall-clock adjustments affecting button debounce. */
static uint64_t milliseconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) fatal("clock_gettime");
    return (uint64_t)t.tv_sec * 1000 + (unsigned long)t.tv_nsec / 1000000;
}
/* A key advances a bounded setting and reverses at either endpoint.
 * Speed and object count keep separate direction variables in main. */
static void sweep(int *value, int *direction, int low, int high)
{
    if (*value >= high) *direction = -1;
    if (*value <= low) *direction = 1;
    *value += *direction;
}
static void move_axis(int *position, int *direction, int limit, int distance)
{
    /* Unit substeps preserve the remainder when reflecting at an edge. */
    int s;
    for (s = 0; s < distance; ++s) {
        if (*position == 0) *direction = 1;
        if (*position == limit) *direction = -1;
        *position += *direction;
    }
}
int main(int argc, char **argv)
{
    int w, h, i, fd, count = 8, count_dir = 1, level = 3, speed_dir = 1;
    int lines = 1, single = 0, key_level_mode = 0;
    unsigned int previous_keys = 0, keys, events, sw;
    uint64_t frames = 0, now, last_event[2] = {0, 0};
    struct timespec delay;
    const char *key_path = getenv("KEY_DEVICE");
    const char *sw_path = getenv("SW_DEVICE");
    if (!key_path) key_path = "/dev/KEY";
    if (!sw_path) sw_path = "/dev/SW";
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--single")) single = 1;
        else if (!strcmp(argv[i], "--key-level")) key_level_mode = 1;
        else { fprintf(stderr, "Usage: %s [--single] [--key-level]\n", argv[0]); return 1; }
    }
    /* Allocate all possible object states up front. Changing the visible
     * count later requires no allocation and preserves hidden object states. */
    fd = open_video(&w, &h);
    srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
    for (i = 0; i < MAX_OBJECTS; ++i) {
        objects[i].x = rand() % (w - BOX_SIZE + 1);
        objects[i].y = rand() % (h - BOX_SIZE + 1);
        objects[i].dx = (rand() & 1) ? 1 : -1;
        objects[i].dy = (rand() & 1) ? 1 : -1;
        objects[i].color = palette[i % 8];
    }
    if (LAB_PART >= 5) {
        (void)read_input(sw_path);
        /* Consume stale edge events; level mode masks an already held key. */
        previous_keys = read_input(key_path) & 3;
        printf("KEY0: speed sweep; KEY1: object count sweep; SW=0: lines on\n");
        printf("Input format: hex %s; default speed level 4/8\n",
               key_level_mode ? "active-high pressed-key states" : "press edge events");
    }
    command(fd, "clear");
    if (LAB_PART >= 6) { command(fd, "erase"); command(fd, "text 0,0 Frames: 0"); }
    while (!stopped) {
        if (LAB_PART >= 5) {
            /* Default KEY reads are captured press events. Existing drivers
             * reporting pressed levels use --key-level to detect new presses. */
            keys = read_input(key_path) & 3;
            events = key_level_mode ? keys & ~previous_keys : keys;
            previous_keys = keys;
            now = milliseconds();
            /* Debounce each key independently; simultaneous events can update
             * both settings. Faster repeated taps inside 150 ms are ignored. */
            for (i = 0; i < 2; ++i) {
                if ((events & (1u << i)) && now - last_event[i] >= 150) {
                    last_event[i] = now;
                    if (i == 0) sweep(&level, &speed_dir, 0, 7);
                    else sweep(&count, &count_dir, 1, MAX_OBJECTS);
                    printf("speed=%d/8 step=%d delay=%dms objects=%d\n",
                           level+1, steps[level], delays_ms[level], count);
                    fflush(stdout);
                }
            }
            sw = read_input(sw_path);
            lines = (sw == 0);
        }
        /* Construct the entire next image in the current drawing buffer.
         * Clearing each frame also removes links when SW hides the chain. */
        command(fd, "clear");
        /* Connect consecutive rectangles: N objects form N-1 chain links. */
        if (lines)
            for (i = 0; i < count-1; ++i)
                command(fd, "line %d,%d %d,%d FFFF",
                        objects[i].x+BOX_SIZE/2, objects[i].y+BOX_SIZE/2,
                        objects[i+1].x+BOX_SIZE/2, objects[i+1].y+BOX_SIZE/2);
        for (i = 0; i < count; ++i)
            command(fd, "box %d,%d %d,%d %04X", objects[i].x, objects[i].y,
                    objects[i].x+BOX_SIZE-1, objects[i].y+BOX_SIZE-1,
                    objects[i].color);
        /* Count a frame only after its presentation request succeeds. Pixel
         * swaps do not buffer the independent character-memory overlay. */
        command(fd, "sync");
        ++frames;
        if (LAB_PART >= 6)
            command(fd, "text 0,0 Frames: %llu", (unsigned long long)frames);
        for (i = 0; i < count; ++i) {
            move_axis(&objects[i].x, &objects[i].dx, w-BOX_SIZE, steps[level]);
            move_axis(&objects[i].y, &objects[i].dy, h-BOX_SIZE, steps[level]);
        }
        /* Extra sleep intentionally reduces frame rate at slow settings.
         * nanosleep returns the remainder after a signal interruption. */
        if (LAB_PART >= 5 && delays_ms[level]) {
            delay.tv_sec = 0; delay.tv_nsec = delays_ms[level] * 1000000L;
            while (nanosleep(&delay, &delay) && errno == EINTR && !stopped) {}
        }
    }
    /* With two buffers, clear/present both before exiting. */
    command(fd, "clear"); command(fd, "sync");
    command(fd, "clear");
    if (!single) command(fd, "sync");
    if (LAB_PART >= 6) command(fd, "erase");
    printf("Presented %llu frames\n", (unsigned long long)frames);
    close(fd);
    return 0;
}
