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

#include "curses.h"
#include "hack.h"
#include "wincurs.h"
#include "cursmesg.h"
#include "curswins.h"

struct curs_player {
    boolean used;
    SCREEN *scr;         /* this player's ncurses screen */
    FILE *in, *out;      /* its terminal, for newterm() */
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
