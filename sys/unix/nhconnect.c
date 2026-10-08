/* NetHack 5.0  nhconnect.c */
/* Copyright (c) 2026 the multiplayer fork contributors. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * nh-connect: join a multiplayer NetHack game running on this machine.
 *
 * Usage: nh-connect [-s SOCKET] [NAME]
 *
 * It opens the controlling terminal, connects to the game server's Unix
 * socket (default: $NETHACK_SERVER), and hands the terminal itself over
 * to the server along with the player's name and terminal type.  From
 * then on the server reads keys from and draws on the terminal directly;
 * this program just waits until the game ends (the server closes the
 * socket) and then puts the terminal back the way it found it.
 *
 * Protocol (version 1):
 *   "NHMP1\t<name>\t<TERM>\n" with the terminal's file descriptor
 *   attached as SCM_RIGHTS ancillary data.
 * The server answers "ok\n" or "error <text>\n".  When the game ends it
 * sends "bye <why>\n" and closes the connection.
 *
 * This is a stand-alone program; it doesn't use the rest of NetHack.
 */

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

static void usage(void);
static int send_terminal(int, int, const char *, const char *);

static void
usage(void)
{
    fprintf(stderr, "usage: nh-connect [-s SOCKET] [NAME]\n");
    exit(2);
}

/* send the hello line with the terminal's descriptor attached */
static int
send_terminal(int sock, int ttyfd, const char *name, const char *term)
{
    char line[256];
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr hdr;
        char buf[CMSG_SPACE(sizeof (int))];
    } ctrl;
    struct cmsghdr *cmsg;
    int len = snprintf(line, sizeof line, "NHMP1\t%s\t%s\n", name, term);

    if (len < 0 || len >= (int) sizeof line)
        return -1;
    memset(&msg, 0, sizeof msg);
    memset(&ctrl, 0, sizeof ctrl);
    iov.iov_base = line;
    iov.iov_len = (size_t) len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof ctrl.buf;
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof (int));
    memcpy(CMSG_DATA(cmsg), &ttyfd, sizeof (int));
    return sendmsg(sock, &msg, 0) == (ssize_t) len ? 0 : -1;
}

int
main(int argc, char *argv[])
{
    const char *path = getenv("NETHACK_SERVER");
    const char *term = getenv("TERM");
    const char *name = NULL;
    struct sockaddr_un addr;
    struct termios saved;
    struct passwd *pw;
    char reply[256];
    ssize_t n;
    size_t got = 0;
    int sock, ttyfd, opt, have_saved;

    while ((opt = getopt(argc, argv, "s:")) != -1) {
        if (opt == 's')
            path = optarg;
        else
            usage();
    }
    if (optind < argc)
        name = argv[optind++];
    if (optind < argc)
        usage();
    if (!path || !*path) {
        fprintf(stderr, "nh-connect: no server socket; use -s SOCKET"
                        " or set NETHACK_SERVER\n");
        return 2;
    }
    if (!name || !*name) {
        name = getenv("USER");
        if ((!name || !*name) && (pw = getpwuid(getuid())) != NULL)
            name = pw->pw_name;
        if (!name || !*name)
            name = "player";
    }
    if (strchr(name, '\t') || strchr(name, '\n')) {
        fprintf(stderr, "nh-connect: bad name\n");
        return 2;
    }
    if (!term || !*term)
        term = "xterm";

    if ((ttyfd = open("/dev/tty", O_RDWR | O_NOCTTY)) < 0) {
        perror("nh-connect: /dev/tty");
        return 1;
    }
    have_saved = (tcgetattr(ttyfd, &saved) == 0);

    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("nh-connect: socket");
        return 1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "nh-connect: socket path too long\n");
        return 2;
    }
    strcpy(addr.sun_path, path);
    if (connect(sock, (struct sockaddr *) &addr, sizeof addr) < 0) {
        fprintf(stderr, "nh-connect: can't reach the game at %s: %s\n",
                path, strerror(errno));
        return 1;
    }
    if (send_terminal(sock, ttyfd, name, term) < 0) {
        perror("nh-connect: sending terminal");
        return 1;
    }
    /* The server now owns the terminal; ignore keyboard signals so that
       ^C and the like reach the game instead of killing us. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    /* first line is the server's verdict; after that, wait for EOF */
    while (got < sizeof reply - 1
           && (n = read(sock, reply + got, 1)) == 1 && reply[got] != '\n')
        got++;
    reply[got] = '\0';
    if (strcmp(reply, "ok") != 0) {
        fprintf(stderr, "nh-connect: %s\n",
                got ? reply : "server closed the connection");
        return 1;
    }
    /* wait for the end of the game; the server may say why it ended
       ("bye <text>") just before closing the connection */
    got = 0;
    while ((n = read(sock, reply + got, sizeof reply - 1 - got)) > 0
           || (n < 0 && errno == EINTR)) {
        if (n > 0) {
            got += (size_t) n;
            if (got >= sizeof reply - 1)
                got = 0; /* overlong; keep only what follows */
        }
    }
    reply[got] = '\0';

    /* game over (or the server went away): restore the terminal */
    if (have_saved)
        (void) tcsetattr(ttyfd, TCSANOW, &saved);
    (void) write(ttyfd, "\r\n", 2);
    if (!strncmp(reply, "bye ", 4)) {
        char *nl = strchr(reply, '\n');

        if (nl)
            *nl = '\0';
        fprintf(stderr, "%s\n", reply + 4);
    } else {
        fprintf(stderr, "The game server went away.\n");
    }
    return 0;
}
