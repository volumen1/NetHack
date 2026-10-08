/* NetHack 5.0  heroes.c */
/* Copyright (c) 2026 the multiplayer fork contributors. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Multiple heroes in one game (see doc/multiplayer-design.md, sec. 3.1).
 *
 * The bulk of each hero's state lives in struct hero (include/decl.h)
 * and is reached through the cur_hero pointer.  The rest is still kept
 * in ordinary globals scattered through the instance_globals structs;
 * switch_hero() saves those for the outgoing hero and loads them for the
 * incoming one.  hero_globals[] below is the list of such globals.  As
 * the refactor proceeds, entries move from this list into struct hero.
 */

#include "hack.h"

#define HG(v) { (genericptr_t) &(v), sizeof (v) }

static const struct hero_global {
    genericptr_t addr;
    size_t len;
} hero_globals[] = {
    /* multi-turn actions and their aftermath */
    HG(ga.afternmv),
    HG(gm.multi),
    HG(gm.multi_reason),
    HG(gm.multireasonbuf),
    HG(gn.nomovemsg),
    HG(go.occupation),
    HG(go.occtime),
    HG(go.occtxt),
    HG(gs.stealoid),
    HG(gs.stealmid),
    HG(gm.m_shot),
    /* command input */
    HG(gc.cmd_key),
    HG(gc.command_count),
    HG(gc.command_queue),
    HG(gl.last_command_count),
    HG(gd.domove_attempting),
    HG(gd.domove_succeeded),
    HG(iflags.travelcc),
    /* identity */
    HG(gu.urole),
    HG(gu.urace),
    HG(svp.plname),
    HG(svp.pl_character),
    HG(flags.female),
    HG(flags.initrole),
    HG(flags.initrace),
    HG(flags.initgend),
    HG(flags.initalign),
    HG(flags.beginner),
    /* knowledge and progress */
    HG(svs.spl_book),
    HG(svq.quest_status),
    HG(svk.killer),
    HG(gl.lastinvnr),
    HG(go.oldcap),
    /* in-progress actions and per-hero timers kept in the context */
    HG(svc.context.run),
    HG(svc.context.startingpet_mid),
    HG(svc.context.startingpet_typ),
    HG(svc.context.next_attrib_check),
    HG(svc.context.seer_turn),
    HG(svc.context.snickersnee_turn),
    HG(svc.context.stethoscope_seq),
    HG(svc.context.travel),
    HG(svc.context.travel1),
    HG(svc.context.forcefight),
    HG(svc.context.nopick),
    HG(svc.context.mv),
    HG(svc.context.door_opened),
    HG(svc.context.digging),
    HG(svc.context.victual),
    HG(svc.context.engraving),
    HG(svc.context.tin),
    HG(svc.context.spbook),
    HG(svc.context.takeoff),
    HG(svc.context.warntype),
    HG(svc.context.polearm),
};

#undef HG

/*
 * Display state that belongs to a player's screen rather than to a hero.
 * In hot-seat play all heroes share screen 0; in a server game each hero
 * has its own (see doc/multiplayer-design.md, section 10).
 */
static char *screen_stash[MAX_HEROES];
static struct hero *input_hero = 0; /* whose turn it is */

staticfn void switch_screen(int, int);
staticfn size_t hero_globals_size(void);
staticfn void stash_globals(struct hero *);
staticfn void unstash_globals(struct hero *);
staticfn void place_new_hero(struct hero *);

staticfn size_t
hero_globals_size(void)
{
    size_t i, total = 0;

    for (i = 0; i < SIZE(hero_globals); i++)
        total += hero_globals[i].len;
    return total;
}

/* copy the scattered per-hero globals into h's stash */
staticfn void
stash_globals(struct hero *h)
{
    size_t i;
    char *p;

    if (!h->stash)
        h->stash = (genericptr_t) alloc((unsigned) hero_globals_size());
    p = (char *) h->stash;
    for (i = 0; i < SIZE(hero_globals); i++) {
        (void) memcpy((genericptr_t) p, hero_globals[i].addr,
                      hero_globals[i].len);
        p += hero_globals[i].len;
    }
}

/* copy h's stash back into the scattered per-hero globals */
staticfn void
unstash_globals(struct hero *h)
{
    size_t i;
    const char *p = (const char *) h->stash;

    if (!p)
        return;
    for (i = 0; i < SIZE(hero_globals); i++) {
        (void) memcpy(hero_globals[i].addr, (const genericptr_t) p,
                      hero_globals[i].len);
        p += hero_globals[i].len;
    }
}

/* display.c's record of what is on screen, which is per screen */
staticfn void
switch_screen(int from, int to)
{
    size_t len = sizeof gg.gbuf + sizeof gg.gbuf_start + sizeof gg.gbuf_stop;
    char *p;

    if (!screen_stash[from])
        screen_stash[from] = (char *) alloc((unsigned) len);
    p = screen_stash[from];
    (void) memcpy(p, gg.gbuf, sizeof gg.gbuf), p += sizeof gg.gbuf;
    (void) memcpy(p, gg.gbuf_start, sizeof gg.gbuf_start),
        p += sizeof gg.gbuf_start;
    (void) memcpy(p, gg.gbuf_stop, sizeof gg.gbuf_stop);

    curses_switch_player(to);
    /* the player whose turn it is gets the usual More>> stops; the
       others just watch */
    curses_mp_watching(input_hero && to != input_hero->screen);

    if ((p = screen_stash[to]) != 0) {
        (void) memcpy(gg.gbuf, p, sizeof gg.gbuf), p += sizeof gg.gbuf;
        (void) memcpy(gg.gbuf_start, p, sizeof gg.gbuf_start),
            p += sizeof gg.gbuf_start;
        (void) memcpy(gg.gbuf_stop, p, sizeof gg.gbuf_stop);
    } else {
        clear_glyph_buffer(); /* a new screen shows nothing yet */
    }
}

/* number of heroes in the game */
int
hero_count(void)
{
    int i, n = 0;

    for (i = 0; i < MAX_HEROES; i++)
        if (heroes[i].active)
            n++;
    return n;
}

/* make h the current hero */
void
switch_hero(struct hero *h)
{
    if (!h || h == cur_hero)
        return;
    /* remember how the outgoing hero looks, for other heroes' maps */
    cur_hero->glyph = hero_glyph;
    stash_globals(cur_hero);
    if (h->screen != cur_hero->screen)
        switch_screen(cur_hero->screen, h->screen);
    cur_hero = h;
    unstash_globals(cur_hero);
    gv.vision_full_recalc = 1;
    disp.botlx = TRUE;
}

/* another hero (not the current one) standing at <x,y> on this level */
struct hero *
other_hero_at(coordxy x, coordxy y)
{
    int i;
    struct hero *h;

    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (h->active && h != cur_hero && h->you.ux == x && h->you.uy == y
            && on_level(&h->you.uz, &u.uz))
            return h;
    }
    return (struct hero *) 0;
}

/* the hero carrying obj (directly or inside a container), or Null */
struct hero *
obj_hero(struct obj *obj)
{
    struct obj *top = obj, *otmp;
    int i;

    while (top->where == OBJ_CONTAINED && top->ocontainer)
        top = top->ocontainer;
    if (top->where != OBJ_INVENT)
        return (struct hero *) 0;
    for (i = 0; i < MAX_HEROES; i++) {
        if (!heroes[i].active)
            continue;
        for (otmp = heroes[i].inv; otmp; otmp = otmp->nobj)
            if (otmp == top)
                return &heroes[i];
    }
    return (struct hero *) 0;
}

/* the hero closest to <x,y> on the current level; ties go to the
   current hero, then to the lowest-numbered one */
struct hero *
closest_hero(coordxy x, coordxy y)
{
    int i, d, best_d = dist2(x, y, u.ux, u.uy);
    struct hero *h, *best = cur_hero;

    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (!h->active || h == cur_hero || !on_level(&h->you.uz, &u.uz))
            continue;
        d = dist2(x, y, h->you.ux, h->you.uy);
        if (d < best_d)
            best = h, best_d = d;
    }
    return best;
}

/* the next hero after the current one, in table order, who still has
   movement left this turn; Null if nobody does */
struct hero *
next_ready_hero(void)
{
    int i, cur = (int) (cur_hero - heroes);
    struct hero *h;

    for (i = 1; i <= MAX_HEROES; i++) {
        h = &heroes[(cur + i) % MAX_HEROES];
        if (h->active && h->you.umovement >= NORMAL_SPEED)
            return h;
    }
    return (struct hero *) 0;
}

/* the first hero, in table order, who has movement left this turn */
struct hero *
first_ready_hero(void)
{
    int i;

    for (i = 0; i < MAX_HEROES; i++)
        if (heroes[i].active && heroes[i].you.umovement >= NORMAL_SPEED)
            return &heroes[i];
    return (struct hero *) 0;
}

/* put a freshly initialized extra hero next to the first hero */
staticfn void
place_new_hero(struct hero *h)
{
    coord cc;
    struct hero *first = &heroes[0];

    u.uz = first->you.uz;
    if (!enexto(&cc, first->you.ux, first->you.uy, youmonst.data))
        panic("place_new_hero: no room for hero %d", (int) (h - heroes));
    u_on_newpos(cc.x, cc.y);
}

/*
 * Create the extra heroes for a hot-seat game.  Called from newgame()
 * once the first hero is complete.  The number of heroes comes from the
 * NETHACK_HEROES environment variable (default 1); the lobby that will
 * replace this is milestone M2.  Each extra player picks a character
 * with the usual selection dialog.
 */
void
add_extra_heroes(void)
{
    const char *env = nh_getenv("NETHACK_HEROES");
    int i, want = env ? atoi(env) : 1;
    struct hero *first = &heroes[0];
    char qbuf[QBUFSZ], namebuf[BUFSZ]; /* getlin() fills up to BUFSZ */

    first->active = TRUE;
    Strcpy(first->name, svp.plname);
    if (want > MAX_HEROES)
        want = MAX_HEROES;

    for (i = 1; i < want; i++) {
        struct hero *h = &heroes[i];

        h->active = TRUE;
        h->screen = first->screen;
        if (i < mp_player_count()) {
            /* server game: this player has their own terminal */
            int scr = curses_add_player(mp_player_ttyfd(i),
                                        mp_player_term(i));

            if (scr < 0)
                panic("can't open a screen for player %d (%s)", i + 1,
                      mp_player_name(i));
            /* curses_add_player() made the new screen current; go back
               until switch_hero() moves the whole display over */
            curses_switch_player(first->screen);
            h->screen = scr;
        }
        /* the new hero starts from a copy of the first hero's scattered
           globals; reset everything that must not be shared */
        stash_globals(h);
        /* the new hero has no position yet; keep the map from being drawn
           until it's standing somewhere (u_init_misc() zeroes it again) */
        gi.in_mklev = TRUE;
        switch_hero(h);
        gm.multi = 0;
        gm.multi_reason = (const char *) 0;
        ga.afternmv = (int (*)(void)) 0;
        gn.nomovemsg = (const char *) 0;
        go.occupation = (int (*)(void)) 0;
        (void) memset((genericptr_t) gc.command_queue, 0,
                      sizeof gc.command_queue);
        (void) memset((genericptr_t) svs.spl_book, 0, sizeof svs.spl_book);
        (void) memset((genericptr_t) &svk.killer, 0, sizeof svk.killer);
        (void) memset((genericptr_t) &svc.context.digging, 0,
                      sizeof svc.context.digging);
        svc.context.run = 0;
        svc.context.travel = svc.context.travel1 = svc.context.mv = FALSE;
        svc.context.startingpet_mid = 0;
        svc.context.next_attrib_check = 600L;
        gl.lastinvnr = 51;

        if (i < mp_player_count()) {
            Strcpy(namebuf, mp_player_name(i)); /* from nh-connect */
        } else {
            Sprintf(qbuf, "Player %d, what is your name?", i + 1);
            getlin(qbuf, namebuf);
            (void) mungspaces(namebuf);
        }
        if (!*namebuf || *namebuf == '\033')
            Sprintf(namebuf, "Hero%d", i + 1);
        (void) strncpy(svp.plname, namebuf, PL_NSIZ - 1);
        svp.plname[PL_NSIZ - 1] = '\0';
        Strcpy(h->name, svp.plname);

        /* let this player choose a character */
        flags.initrole = flags.initrace = ROLE_NONE;
        flags.initgend = flags.initalign = ROLE_NONE;
        svp.pl_character[0] = '\0';
        player_selection();
        role_init();
        u_init_misc();
        place_new_hero(h);
        gi.in_mklev = FALSE;
        (void) makedog();
        u_init_inventory_attrs();
        u_init_skills_discoveries();
        h->glyph = hero_glyph;
    }
    switch_hero(first);
    if (want > 1) {
        docrt();
        bot();
        refresh_other_screens();
    }
}

/* tell the players whose turn it is */
void
announce_hero_turn(void)
{
    if (hero_count() < 2)
        return;
    if (mp_player_count() > 1) {
        /* everyone has a screen; the others get told by
           refresh_other_screens() */
        input_hero = cur_hero;
        curses_mp_watching(FALSE);
        /* what arrived while watching has been on screen; start the turn
           on a fresh line, as before any command */
        clear_nhwindow(WIN_MESSAGE);
        pline("It is your turn.");
        return;
    }
    docrt();
    bot();
    pline("%s, it is your turn.", cur_hero->name);
}

/*
 * Server game: bring every waiting player's screen up to date from their
 * own hero's point of view, and tell them whose turn it is.  Called after
 * each action that took time.
 */
void
refresh_other_screens(void)
{
    struct hero *was = cur_hero, *h;
    int i, j;

    input_hero = was;
    if (mp_player_count() < 2)
        return;
    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (!h->active || h->screen == was->screen)
            continue;
        switch_hero(h);
        vision_recalc(0);
        docrt_flags(docrtNocls); /* redraw from map memory and vision */
        for (j = 0; j < MAX_HEROES; j++) /* allies, even out of sight */
            if (heroes[j].active && &heroes[j] != h)
                newsym(heroes[j].you.ux, heroes[j].you.uy);
        if (h->waiting_for != was) {
            pline("It is %s's turn.", was->name);
            h->waiting_for = was;
        }
        bot();
        flush_screen(1);
    }
    switch_hero(was);
    was->waiting_for = (struct hero *) 0;
}

/*heroes.c*/
