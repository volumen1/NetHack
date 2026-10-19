/* NetHack 5.0  cursplay.c */
/* Copyright (c) 2026 the multiplayer fork contributors. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Multiplayer support for the curses interface: one ncurses SCREEN per
 * player terminal (doc/multiplayer-design.md, section 10).
 *
 * ncurses can drive several terminals from one process; set_term()
 * picks which one the curses calls act on.  The curses port's own
 * file-level variables (window pointers, message history, status
 * fields, ...) describe one screen, so each player gets a private copy:
 * the tables curs_state_*[] list them, and curses_switch_player() saves
 * the outgoing player's copy and loads the incoming one's.
 */

#include <poll.h>
#include "curses.h"
#include "hack.h"
#include "wincurs.h"
#include "cursmesg.h"
#include "curswins.h"

struct curs_player {
    boolean used;
    SCREEN *scr;         /* this player's ncurses screen */
    FILE *in, *out;      /* its terminal, for newterm() */
    int fd;              /* where its input comes from */
    WINDOW *peek;        /* for looking at typed-ahead keys; see
                          * curses_mp_peek_key() */
    char *stash;         /* port state while another player is current */
};

static const struct curs_state *const curs_states[] = {
    curs_state_dial, curs_state_init, curs_state_main, curs_state_mesg,
    curs_state_misc, curs_state_stat, curs_state_wins,
};

static struct curs_player cplayers[MAX_HEROES];
static int ccur = 0;          /* index of the current player */
static char *pristine = 0;    /* port state before any screen existed */

staticfn size_t curs_state_size(void);
staticfn void curs_stash(char **);
staticfn void curs_unstash(const char *);

staticfn size_t
curs_state_size(void)
{
    size_t total = 0;
    int t;
    const struct curs_state *cs;

    for (t = 0; t < SIZE(curs_states); t++)
        for (cs = curs_states[t]; cs->addr; cs++)
            total += cs->len;
    return total;
}

/* copy the port's per-player variables into *bufp */
staticfn void
curs_stash(char **bufp)
{
    int t;
    const struct curs_state *cs;
    char *p;

    if (!*bufp)
        *bufp = (char *) alloc((unsigned) curs_state_size());
    p = *bufp;
    for (t = 0; t < SIZE(curs_states); t++)
        for (cs = curs_states[t]; cs->addr; cs++) {
            (void) memcpy((genericptr_t) p, cs->addr, cs->len);
            p += cs->len;
        }
}

/* copy buf back into the port's per-player variables */
staticfn void
curs_unstash(const char *buf)
{
    int t;
    const struct curs_state *cs;
    const char *p = buf;

    for (t = 0; t < SIZE(curs_states); t++)
        for (cs = curs_states[t]; cs->addr; cs++) {
            (void) memcpy(cs->addr, (const genericptr_t) p, cs->len);
            p += cs->len;
        }
}

/* called once with the first player's newly created screen, before it
   is set up: remember the port's starting state, which is what each
   additional player begins with */
void
curses_mp_init(SCREEN *first)
{
    if (!pristine)
        curs_stash(&pristine);
    cplayers[0].used = TRUE;
    cplayers[0].scr = first;
    cplayers[0].fd = fileno(stdin);
    ccur = 0;
}

/*
 * Open a screen on another player's terminal (descriptor 'fd', terminal
 * type 'term') and set it up for play.  The new player becomes the
 * current one.  Returns its index, or -1 on failure (the current player
 * is unchanged then).
 */
int
curses_add_player(int fd, const char *term)
{
    int i, fd2;
    struct curs_player *cp;
    SCREEN *scr;

    for (i = 1; i < MAX_HEROES; i++)
        if (!cplayers[i].used)
            break;
    if (i >= MAX_HEROES || fd < 0)
        return -1;
    cp = &cplayers[i];

    /* separate descriptors so the two FILEs can be closed independently */
    if ((fd2 = dup(fd)) < 0)
        return -1;
    cp->in = fdopen(fd, "r");
    cp->out = fdopen(fd2, "w");
    if (!cp->in || !cp->out)
        return -1;

    curs_stash(&cplayers[ccur].stash);
    curs_unstash(pristine);
    scr = newterm(term, cp->out, cp->in);
    if (!scr) {
        curs_unstash(cplayers[ccur].stash);
        (void) set_term(cplayers[ccur].scr);
        return -1;
    }
    cp->scr = scr;
    cp->used = TRUE;
    cp->fd = fileno(cp->in);
    ccur = i;
    curses_setup_terminal(FALSE);
    curses_status_init_player();
    curses_clear_nhwin(MAP_WIN); /* the map buffer starts out blank */
    return i;
}

/* make player 'idx' the one curses output and input go to */
void
curses_switch_player(int idx)
{
    if (idx == ccur || idx < 0 || idx >= MAX_HEROES || !cplayers[idx].used
        || !cplayers[idx].scr)
        return;
    curs_stash(&cplayers[ccur].stash);
    curs_unstash(cplayers[idx].stash);
    (void) set_term(cplayers[idx].scr);
    ccur = idx;
}

/* tell the current screen whether its player is only watching (someone
   else is moving): if so its messages scroll by rather than stop for
   More>>, since that player isn't the one being asked for input */
void
curses_mp_watching(boolean watching)
{
    curses_set_watching(watching);
}

/*
 * The next key player 'idx' has typed but that hasn't been read yet,
 * without taking it: -1 if there's none, -2 if their terminal has
 * something that isn't a key yet (or has hung up; the normal input code
 * deals with that).  Keys can be waiting in two places: the terminal,
 * and ncurses' own queue (it reads ahead while matching escape
 * sequences, and ungetch() puts keys back there).  The only way to see
 * the queue is to take a key and put it back, which is done on a 1x1
 * window that is never drawn in, so the wgetch() doesn't refresh
 * anything.
 */
int
curses_mp_peek_key(int idx)
{
    struct curs_player *cp;
    struct pollfd pfd;
    int was = ccur, ch;

    if (idx < 0 || idx >= MAX_HEROES || !cplayers[idx].used
        || !cplayers[idx].scr)
        return -1;
    cp = &cplayers[idx];
    curses_switch_player(idx);
    if (!cp->peek) {
        cp->peek = newwin(1, 1, LINES - 1, COLS - 1);
        if (cp->peek) {
            keypad(cp->peek, TRUE);
            nodelay(cp->peek, TRUE);
            untouchwin(cp->peek);
        }
    }
    ch = cp->peek ? wgetch(cp->peek) : ERR;
    if (ch != ERR) {
        (void) ungetch(ch);
    } else {
        pfd.fd = cp->fd, pfd.events = POLLIN, pfd.revents = 0;
        ch = (poll(&pfd, 1, 0) > 0 && pfd.revents != 0) ? -2 : -1;
    }
    curses_switch_player(was);
    return ch;
}

/*
 * Wait until one of the n players in idx[] may have typed something, or
 * 'ms' milliseconds have passed (no limit if negative).  Callers check
 * curses_mp_peek_key() first, since keys in ncurses' queue don't wake
 * this up.
 */
void
curses_mp_wait_input(const int *idx, int n, long ms)
{
    struct pollfd pfd[MAX_HEROES];
    int i, k = 0;

    for (i = 0; i < n && k < MAX_HEROES; i++) {
        if (idx[i] < 0 || idx[i] >= MAX_HEROES || !cplayers[idx[i]].used)
            continue;
        pfd[k].fd = cplayers[idx[i]].fd;
        pfd[k].events = POLLIN;
        pfd[k].revents = 0;
        k++;
    }
    if (ms > 24L * 60 * 60 * 1000)
        ms = 24L * 60 * 60 * 1000;
    (void) poll(pfd, (nfds_t) k, (int) ms);
}

/* wait up to 'ms' milliseconds for the current player to type
   something; FALSE if they didn't */
boolean
curses_mp_key_wait(long ms)
{
    int me = ccur;

    if (curses_mp_peek_key(me) != -1)
        return TRUE;
    curses_mp_wait_input(&me, 1, ms);
    return curses_mp_peek_key(me) != -1;
}

/* end of game: put the other players' terminals back in normal mode;
   the current player's is handled by the usual exit code */
void
curses_exit_other_players(void)
{
    int i, was = ccur;

    for (i = 0; i < MAX_HEROES; i++) {
        if (i == was || !cplayers[i].used || !cplayers[i].scr)
            continue;
        curses_switch_player(i);
        curs_set(1);
        endwin();
    }
    curses_switch_player(was);
}

/*cursplay.c*/
