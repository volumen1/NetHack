/* NetHack 5.0  mpserver.c */
/* Copyright (c) 2026 the multiplayer fork contributors. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Multiplayer server mode (doc/multiplayer-design.md, section 10).
 *
 * When NETHACK_SERVER names a socket path, the game doesn't use the
 * terminal it was started from.  Instead it listens on that Unix socket
 * and waits for NETHACK_HEROES players (default 2) to join with
 * nh-connect, which hands over the player's terminal.  The first
 * player's terminal becomes the game's standard input and output, so
 * normal startup runs on it; the others get their own curses screens
 * when their heroes are created (see add_extra_heroes()).
 */

#include "hack.h"

#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#define MP_HELLO "NHMP1"

struct mp_player {
    int ttyfd;                  /* the player's terminal */
    int ctlfd;                  /* socket to their nh-connect */
    char name[PL_NSIZ];
    char term[64];
};

static struct mp_player mp_players[MAX_HEROES];
static int mp_nplayers = 0;
static char mp_sockpath[sizeof ((struct sockaddr_un *) 0)->sun_path];
static char mp_goodbye[BUFSZ]; /* why the game ended, for nh-connect */

staticfn void mp_cleanup(void);
staticfn int mp_listen(const char *);
staticfn boolean mp_accept_one(int, struct mp_player *);
staticfn void mp_reply(int, const char *);

/* when the game ends: tell every nh-connect why, so it can say so once
   it has given the terminal back, and remove the socket file */
staticfn void
mp_cleanup(void)
{
    char line[BUFSZ + 10];
    int i;

    Snprintf(line, sizeof line, "bye %s\n",
             *mp_goodbye ? mp_goodbye : "The game is over.");
    for (i = 0; i < mp_nplayers; i++)
        if (mp_players[i].ctlfd >= 0)
            mp_reply(mp_players[i].ctlfd, line);
    if (*mp_sockpath)
        (void) unlink(mp_sockpath);
}

/* record why the game is ending, for the players' nh-connect */
void
mp_set_goodbye(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    (void) vsnprintf(mp_goodbye, sizeof mp_goodbye, fmt, args);
    va_end(args);
}

staticfn int
mp_listen(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (strlen(path) >= sizeof addr.sun_path) {
        fprintf(stderr, "NETHACK_SERVER path is too long: %s\n", path);
        return -1;
    }
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "server socket: %s\n", strerror(errno));
        return -1;
    }
    (void) memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    Strcpy(addr.sun_path, path);
    (void) unlink(path); /* a stale socket from an earlier game */
    if (bind(fd, (struct sockaddr *) &addr, sizeof addr) < 0
        || listen(fd, MAX_HEROES) < 0) {
        fprintf(stderr, "can't listen on %s: %s\n", path, strerror(errno));
        (void) close(fd);
        return -1;
    }
    Strcpy(mp_sockpath, path);
    (void) atexit(mp_cleanup);
    /* players are trusted friends with their own accounts on this host;
       access is controlled by the permissions of the socket's directory */
    (void) chmod(path, 0666);
    return fd;
}

staticfn void
mp_reply(int fd, const char *text)
{
    /* the player may already be gone; that mustn't kill the game */
    (void) send(fd, text, strlen(text), MSG_NOSIGNAL);
}

/* accept one nh-connect and collect its hello and terminal */
staticfn boolean
mp_accept_one(int lfd, struct mp_player *p)
{
    char buf[256], *field[3], *cp;
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr hdr;
        char buf[CMSG_SPACE(sizeof (int))];
    } ctrl;
    struct cmsghdr *cmsg;
    ssize_t n;
    int cfd, i;

    if ((cfd = accept(lfd, (struct sockaddr *) 0, (socklen_t *) 0)) < 0)
        return FALSE;
    (void) memset(&msg, 0, sizeof msg);
    iov.iov_base = buf;
    iov.iov_len = sizeof buf - 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.buf;
    msg.msg_controllen = sizeof ctrl.buf;
    n = recvmsg(cfd, &msg, 0);
    cmsg = CMSG_FIRSTHDR(&msg);
    if (n <= 0 || !cmsg || cmsg->cmsg_level != SOL_SOCKET
        || cmsg->cmsg_type != SCM_RIGHTS) {
        mp_reply(cfd, "error no terminal received\n");
        (void) close(cfd);
        return FALSE;
    }
    (void) memcpy(&p->ttyfd, CMSG_DATA(cmsg), sizeof (int));
    buf[n] = '\0';
    if ((cp = strchr(buf, '\n')) != 0)
        *cp = '\0';
    /* "NHMP1\t<name>\t<TERM>" */
    field[0] = buf;
    for (i = 1; i < 3; i++) {
        if (!(cp = strchr(field[i - 1], '\t')))
            break;
        *cp = '\0';
        field[i] = cp + 1;
    }
    if (i < 3 || strcmp(field[0], MP_HELLO) || !*field[1] || !*field[2]) {
        mp_reply(cfd, "error bad hello; is nh-connect out of date?\n");
        (void) close(p->ttyfd);
        (void) close(cfd);
        return FALSE;
    }
    for (cp = field[1]; *cp; cp++) /* keep names printable */
        if (!(*cp > ' ' && *cp < 0x7f))
            *cp = '_';
    (void) strncpy(p->name, field[1], sizeof p->name - 1);
    p->name[sizeof p->name - 1] = '\0';
    (void) strncpy(p->term, field[2], sizeof p->term - 1);
    p->term[sizeof p->term - 1] = '\0';
    p->ctlfd = cfd;
    mp_reply(cfd, "ok\n");
    return TRUE;
}

/*
 * Server mode startup, called before the window system is initialized.
 * Returns TRUE if this game is a multiplayer server.
 */
boolean
mp_server_start(void)
{
    const char *path = nh_getenv("NETHACK_SERVER"),
               *count = nh_getenv("NETHACK_HEROES");
    int lfd, want = count ? atoi(count) : 2;
    char nbuf[20];

    if (!path || !*path)
        return FALSE;
    if (want < 1)
        want = 1;
    if (want > MAX_HEROES)
        want = MAX_HEROES;
    if ((lfd = mp_listen(path)) < 0)
        nh_terminate(EXIT_FAILURE);
    /* a player dropping off mustn't kill the game with SIGPIPE */
    (void) signal(SIGPIPE, SIG_IGN);

    /* progress goes to the server's own log, not to any player */
    fprintf(stderr, "Waiting for %d player%s on %s ...\n", want, plur(want),
            path);
    while (mp_nplayers < want) {
        struct mp_player *p = &mp_players[mp_nplayers];

        if (!mp_accept_one(lfd, p))
            continue;
        mp_nplayers++;
        fprintf(stderr, "  %s joined (%d of %d).\n", p->name, mp_nplayers,
                want);
    }
    (void) close(lfd);  /* the party is complete */
    Sprintf(nbuf, "%d", want);
    (void) setenv("NETHACK_HEROES", nbuf, 1);

    /* the first player's terminal becomes the game's own terminal */
    if (dup2(mp_players[0].ttyfd, 0) < 0
        || dup2(mp_players[0].ttyfd, 1) < 0) {
        fprintf(stderr, "can't attach the first player's terminal: %s\n",
                strerror(errno));
        nh_terminate(EXIT_FAILURE);
    }
    (void) setenv("TERM", mp_players[0].term, 1);
    if (!*svp.plname) {
        (void) strncpy(svp.plname, mp_players[0].name, PL_NSIZ - 1);
        svp.plname[PL_NSIZ - 1] = '\0';
    }
    return TRUE;
}

/* number of players who joined, or 0 when not a server */
int
mp_player_count(void)
{
    return mp_nplayers;
}

/* name, terminal type and terminal descriptor of player 'i' */
const char *
mp_player_name(int i)
{
    return (i >= 0 && i < mp_nplayers) ? mp_players[i].name : "";
}

const char *
mp_player_term(int i)
{
    return (i >= 0 && i < mp_nplayers) ? mp_players[i].term : "";
}

int
mp_player_ttyfd(int i)
{
    return (i == 0) ? 0 : (i > 0 && i < mp_nplayers) ? mp_players[i].ttyfd
                                                     : -1;
}

/*mpserver.c*/
