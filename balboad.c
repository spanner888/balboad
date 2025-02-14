/*
 * balboad.c - helper daemon for Balboa FPGA acceleration
 * library to do useful computation on an FPGA.
 *
 * Copyright (c) 2014 Andrew Isaacson <adi@hexapodia.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * Provided AS IS with no warranty.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "../libbalboa/balboa-int.h" // for BALBOA_DEFAULT_PORT
#include "novena/novena-eim.h"

double rtc(void)
{
    struct timeval t;
    gettimeofday(&t, 0);
    return t.tv_sec + t.tv_usec/1e6;
}

void die(char *fmt, ...) __attribute__((noreturn));

void die(char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "[%.3f] ", rtc());
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

void verbose(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%.3f] ", rtc());
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

int o_daemonize = 1;
int o_verbose = 0;
int o_fake = 0;
const char *o_streamdir = "/usr/share/balboa";
//const char *o_spidev = "/dev/spidev.2";
const char *o_spidev = "/dev/spidev2.0";

char *read_line(FILE *f)
{
    char buf[1024], *p;
    int n;

    if (fgets(buf, sizeof(buf), f) == NULL)
        return 0;

    n = strlen(buf);

    if (n == 0) return 0;
    p = malloc(n + 1);
    if (!p) return 0;
    memcpy(p, buf, n+1);
    return p;
}

char *read_file(const char *fname, int *flen)
{
    struct stat st;
    char *p;
    int fd, n;

    fd = open(fname, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "%s: %s\n", fname, strerror(errno));
        return 0;
    }
    if (fstat(fd, &st) == -1) {
        fprintf(stderr, "fstat(%s): %s\n", fname, strerror(errno));
        return 0;
    }
    if (st.st_size > 1024 * 1024 * 512) {
        fprintf(stderr, "refusing to read %d MB %s\n",
                (int)(st.st_size / 1024 / 1024),
                fname);
        return 0;
    }
    p = malloc(st.st_size + 1);
    if (!p) return 0;
    n = read(fd, p, st.st_size);
    close(fd);
    p[n] = '\0';
    if (flen) *flen = n;
    return p;
}

void process_option(const char *opt, const char *arg)
{
    if (!arg || !*arg) arg = "yes";

    if (!strcmp(opt, "daemonize"))
        o_daemonize = !strcmp(arg, "yes");
    else if (!strcmp(opt, "streamdir"))
        o_streamdir = strdup(arg);
    else if (!strcmp(opt, "spidev"))
        o_spidev = strdup(arg);
    else
        die("Unknown option '%s' '%s'\n", opt, arg ?: "");
}

void process_opt_line(char *buf)
{
    char *opt, *val;
    int i;

    i = strspn(buf, " \t");
    opt = buf + i;
    i = strcspn(buf + i, " \t");
    buf[i] = '\0';
    val = buf + i;
    val = val + strspn(opt, " \t");

    process_option(opt, val);
}

void read_config(const char *configfile)
{
    char *buf;
    FILE *f = fopen(configfile, "r");

    if (!f) die("%s: %s\n", configfile, strerror(errno));

    while ((buf = read_line(f)) != 0) {
        if (buf[0] == '#') continue;
        process_opt_line(buf);
        free(buf);
    }
    fclose(f);
}

void daemonize(void)
{
    int r;

    r = fork();
    if (r == -1)
        die("fork: %s\n", strerror(errno));
    if (r != 0)
        exit(0);
    r = fork();
    if (r == -1)
        die("fork: %s\n", strerror(errno));
    if (r != 0)
        exit(0);
    /* XXX close tty fds and switch to syslog and so on */
}

char *o_sockpath = BALBOA_DEFAULT_PORT;

int setup_listen(void)
{
    int sock, ret;
    struct sockaddr_un sa;
    int socklen;

    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, o_sockpath);
    socklen = offsetof(struct sockaddr_un, sun_path) + strlen(o_sockpath) + 1;

    sock = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (sock == -1)
        die("socket(AF_LOCAL): %s\n", strerror(errno));
    unlink(o_sockpath);
    ret = bind(sock, (struct sockaddr *)&sa, socklen);
    if (ret == -1)
        die("bind(%s): %s\n", o_sockpath, strerror(errno));
    ret = listen(sock, 10);
    if (ret == -1)
        die("listen(%d): %s\n", sock, strerror(errno));
    return sock;
}

int get_new_client(int sock)
{
    int fd, n;
    char buf[1024];

    fd = accept(sock, 0, 0);
    if (fd == -1)
        die("accept: %s\n", strerror(errno));
    n = read(fd, buf, sizeof(buf));
    if (n == -1)
        die("read(%d): %s\n", fd, strerror(errno));
    buf[n] = '\0';
    if (strcmp(buf, "hi\n")) {
        fprintf(stderr, "bad hello from client: '%s'\n", buf);
        return -1;
    }
    n = write(fd, "ok\n", 3);
    if (n != 3)
        die("write(%d): %s\n", fd, strerror(errno));

    return fd;
}

int beginswith(const char *a, const char *b)
{
    int i;

    for (i=0; a[i] && b[i]; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

void handle_failure(int fd)
{
    int ret;
    
    ret = write(fd, "err\n", 4);
    if (ret != 4)
        die("write returned %d: %s\n", ret, strerror(errno));
}

void eim_enable(void)
{
    if (o_fake) {
        verbose("faking out eim_enable\n");
        return;
    }
    prep_eim();
}

void eim_disable(void)
{
    if (o_fake) {
        verbose("faking out eim_disable\n");
        return;
    }
    // XXX devmem twiddles to disable mapping
}

void fpga_enable(void)
{
    if (o_fake) {
        verbose("faking out fpga_enable\n");
        return;
    }
    // XXX gpio twiddles to assert FPGA reset
}

void fpga_disable(void)
{
    if (o_fake) {
        verbose("faking out fpga_disable\n");
        return;
    }
    // XXX gpio twiddles to deassert FPGA reset
}

void load_bitstream(const char *bits, int n)
{
    int fd;
    int i, ret;
    int blksz = 128;

    if (o_fake) {
        verbose("faking out writing bitstream to SPI\n");
        return;
    }
    fd = open(o_spidev, O_WRONLY);
    if (fd == -1)
        die("%s: %s\n", o_spidev, strerror(errno));
    for (i=0; i<n; i+=blksz) {
        int nwrite = i+blksz < n ? blksz : n-i;
        ret = write(fd, bits+i, nwrite);
        if (ret == -1)
            die("spidev write failed: %s\n", strerror(errno));
        if (ret < nwrite)
            die("short write to spidev (%d of %d)\n", ret, nwrite);
    }
    close(fd);
}

/*
 * Load a bitstream from filesystem, reset the FPGA, write bitstream
 * into FPGA, reenable FPGA access.
 *
 * Returns (in *win, *size) the physical address window to access the core.
 */
int load_core(const char *corename, unsigned long long *win,
        unsigned long long *size)
{
    char *bitstream;
    int bitstream_len;
    char fname[1024];

    snprintf(fname, sizeof fname, "%s/%s", o_streamdir, corename);

    bitstream = read_file(fname, &bitstream_len);
    if (!bitstream) return 0;

    eim_disable();
    fpga_disable();

    load_bitstream(bitstream, bitstream_len);
    free(bitstream);

    fpga_enable();
    eim_enable();

    *win = EIM_CS0_BASE;
    *size = EIM_CS0_SIZE;

    return 1;
}

int client_send(int client_fd, const char *fmt, ...)
{
    va_list ap;
    char buf[1024];
    int n, ret;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ret = write(client_fd, buf, n);
    if (ret < n) {
        handle_failure(client_fd);
        return 0;
    }
    return 1;
}

void handle_success(int client_fd)
{
    write(client_fd, "ok\n", 3);
}

void handle_core(int client_fd, const char *corename)
{
    char core[1024];
    int i, ret;
    unsigned long long window, size;
    char *devpath;

    if (o_fake) {
        devpath = "/dev/zero";
    } else {
        devpath = "/dev/mem";
    }

    strcpy(core, corename);
    i = strcspn(core, "/ \t\n\r");
    if (i > 0)
        core[i] = '\0';

    verbose("loading core '%s'\n", core);
    ret = load_core(core, &window, &size);
    if (ret == 0) {
        handle_failure(client_fd);
    } else {
        client_send(client_fd, "ok core %s mem %s 0x%llx size 0x%llx\n",
                core, devpath, window, size);
    }
}

int handle_event(int client_fd)
{
    char buf[1024];
    int n;

    n = read(client_fd, buf, sizeof buf);
    if (n == -1)
        die("read(%d): %s\n", client_fd, strerror(errno));
    buf[n] = '\0';
    if (n == 0)
        return -1;

    if (beginswith(buf, "core")) {
        handle_core(client_fd, buf + 5);
    } else {
        handle_failure(client_fd);
    }
    return 0;
}

void event_loop(int listensock)
{
    fd_set fds;
    int i, nfd, clients[100], nclient = 100, ci = 0;
    int did_close, ret;

    while (1) {
        FD_ZERO(&fds);
        FD_SET(listensock, &fds);
        nfd = listensock + 1;

        for (i=0; i<ci; i++) {
            FD_SET(clients[i], &fds);
            if (clients[i] >= nfd)
                nfd = clients[i] + 1;
        }
        ret = select(nfd, &fds, 0, 0, 0);
        if (ret < 0)
            die("select: %s\n", strerror(errno));
        if (FD_ISSET(listensock, &fds)) {
            int newfd = get_new_client(listensock);
            if (newfd == -1) continue;
            clients[ci++] = newfd;
            if (ci > nclient) {
                die("too many clients! %d > %d\n", ci, nclient);
            }
        }
        did_close = 0;
        for (i=0; i<ci; i++) {
            if (FD_ISSET(clients[i], &fds)) {
                ret = handle_event(clients[i]);
                if (ret == -1) {
                    // remove dead client
                    close(clients[i]);
                    clients[i] = -1;
                    did_close++;
                }
            }
        }
        if (did_close) {
            for (i=0; i<ci; i++) {
                if (clients[i] == -1) {
                    ci--;
                    clients[i] = clients[ci];
                }
            }
        }
    }
}

void usage(const char *cmd)
{
    die("Usage: %s -[div] [-s bitstreamdir] [-f configfile]\n", cmd);
}

int main(int argc, char **argv)
{
    int c, sock;
    char *configfile = "/etc/balboad.conf";

    while ((c = getopt(argc, argv, "df:is:v")) != EOF) {
        switch (c) {
        case 'd':
            o_daemonize = 0;
            break;
        case 'f':
            configfile = optarg;
            break;
        case 'i':
            o_fake = 1;
            break;
        case 's':
            o_streamdir = optarg;
            break;
        case 'v':
            o_verbose++;
            break;
        default:
            usage(argv[0]);
        }
    }

//    read_config(configfile);		//buggy - eg over writes default val with "yes"

    if (o_daemonize) daemonize();

    sock = setup_listen();
    event_loop(sock);
    return 0;
}
