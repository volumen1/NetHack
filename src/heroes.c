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
staticfn void swap_with_hero(struct hero *, coordxy, coordxy);
staticfn void attack_hero(struct hero *, boolean);

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

/*
 * The hero monster 'mtmp' is after (doc/multiplayer-design.md, sec. 5.1):
 * the closest hero on the level, by king-move distance, except that a
 * monster sticks with the hero it was already after unless that hero has
 * left, the monster has lost sight of them, or another hero is at least
 * 2 squares closer.  Without the stickiness monsters would flip between
 * heroes every step as the party moves around them.
 */
struct hero *
monster_target(struct monst *mtmp)
{
    int i, d, best_d = 0;
    struct hero *h, *best = 0, *cur = 0;

    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (!h->active || !on_level(&h->you.uz, &u.uz))
            continue;
        d = distmin(mtmp->mx, mtmp->my, h->you.ux, h->you.uy);
        if (!best || d < best_d)
            best = h, best_d = d;
        if (mtmp->mtarget == i + 1)
            cur = h;
    }
    if (cur && cur != best
        && m_cansee(mtmp, cur->you.ux, cur->you.uy)
        && distmin(mtmp->mx, mtmp->my, cur->you.ux, cur->you.uy)
               < best_d + 2)
        best = cur; /* keep after the same hero */
    if (best)
        mtmp->mtarget = (long) (best - heroes) + 1;
    return best ? best : cur_hero;
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

/*
 * The current hero tries to move onto <x,y>, where hero 'oh' stands
 * (doc/multiplayer-design.md, section 6.3).  A hero who can tell who is
 * there swaps places with them, or with F asks before attacking.  A hero
 * who can't (blind or hallucinating) treats them like any monster in the
 * way: a blind hero first bumps into "something" and attacks if they
 * try that spot again; a hallucinating one just attacks.  Sets
 * svc.context.move to say whether this took time.
 */
void
move_into_hero(struct hero *oh, coordxy x, coordxy y)
{
    boolean recognized = !Blind && !Hallucination;

    if (svc.context.run && recognized) {
        nomul(0); /* don't run into allies */
        svc.context.move = 0;
        return;
    }
    if (!recognized) {
        /* The 'I' marker can't be relied on to remember the bump: map
           memory is shared, and the ally's own view of their square
           replaces it.  So each hero remembers where they bumped. */
        if (Blind && (cur_hero->bumped.x != x || cur_hero->bumped.y != y)) {
            pline("Wait!  There's %s there you can't see!", something);
            map_invisible(x, y);
            cur_hero->bumped.x = x, cur_hero->bumped.y = y;
            nomul(0);
            return; /* uses the move, as for an unseen monster */
        }
        attack_hero(oh, FALSE);
        return;
    }
    if (svc.context.forcefight) {
        char qbuf[QBUFSZ];

        Snprintf(qbuf, sizeof qbuf, "Really attack %s?", oh->name);
        if (y_n(qbuf) != 'y') {
            nomul(0);
            svc.context.move = 0;
            return;
        }
        attack_hero(oh, TRUE);
        return;
    }
    swap_with_hero(oh, x, y);
}

/* swap places with hero 'oh', who is at <x,y> next to the current hero */
staticfn void
swap_with_hero(struct hero *oh, coordxy x, coordxy y)
{
    coordxy ox = u.ux, oy = u.uy;
    struct hero *me = cur_hero;
    boolean ok;

    ok = !u.utrap && !oh->you.utrap         /* neither is stuck */
         && !u.ustuck && !oh->you.ustuck    /* or held */
         && !Punished && !oh->ball          /* or chained */
         && !u.usteed && !oh->you.usteed    /* or riding */
         /* where they'd end up must be safe for them, and we must be
            able to make the move ourselves */
         && !is_pool_or_lava(ox, oy) && !t_at(ox, oy)
         && test_move(ox, oy, x - ox, y - oy, TEST_MOVE);
    if (!ok) {
        You("stop.  %s is in your way.", oh->name);
        nomul(0);
        svc.context.move = 0;
        return;
    }

    switch_hero(oh);
    u_on_newpos(ox, oy);
    stop_occupation();
    pline("%s swaps places with you.", me->name);
    switch_hero(me);

    u_on_newpos(x, y);
    u.umoved = TRUE;
    You("swap places with %s.", oh->name);
    newsym(ox, oy);
    newsym(x, y);
    gv.vision_full_recalc = 1;
    spoteffects(TRUE);
}

/*
 * The current hero attacks hero 'oh' in melee.  'knows' is whether the
 * attacker can tell who it is.  This is deliberately simpler than
 * attacking a monster: to-hit and damage use the usual weapon, skill,
 * strength and luck bonuses, but weapon specials (artifacts, poison,
 * silver and so on) aren't applied yet.
 */
staticfn void
attack_hero(struct hero *oh, boolean knows)
{
    struct hero *me = cur_hero;
    struct monst *victim = &oh->mon;
    char kbuf[BUFSZ], vbuf[BUFSZ], nbuf[BUFSZ];
    int tohit, dmg = 0;
    boolean hit;

    tohit = 1 + Luck + abon() + oh->you.uac + u.uhitinc + u.ulevel;
    if (uwep && (uwep->oclass == WEAPON_CLASS || is_weptool(uwep)))
        tohit += hitval(uwep, victim) + weapon_hit_bonus(uwep);
    else if (!uwep)
        tohit += weapon_hit_bonus((struct obj *) 0);
    hit = (tohit > rnd(20));
    if (hit) {
        if (uwep && (uwep->oclass == WEAPON_CLASS || is_weptool(uwep)))
            dmg = dmgval(uwep, victim) + weapon_dam_bonus(uwep);
        else if (uwep)
            dmg = rnd(2); /* bashing with something that isn't a weapon */
        else
            dmg = rnd(martial_bonus() ? 4 : 2)
                  + weapon_dam_bonus((struct obj *) 0);
        dmg += dbon();
        if (dmg < 1)
            dmg = 1;
    }

    /* the attacker's side */
    if (knows)
        Strcpy(vbuf, oh->name);
    else if (Hallucination)
        Strcpy(vbuf, an(rndmonnam(nbuf)));
    else
        Strcpy(vbuf, "it");
    if (hit)
        You("hit %s%s", vbuf, knows ? "!" : ".");
    else
        You("miss %s.", vbuf);
    wake_nearby(FALSE);

    /* the victim's side */
    Snprintf(kbuf, sizeof kbuf, "%s, a fellow adventurer", me->name);
    switch_hero(oh);
    if (Blind)
        Strcpy(vbuf, "It");
    else if (Hallucination)
        Strcpy(vbuf, upstart(an(rndmonnam(nbuf))));
    else
        Strcpy(vbuf, me->name);
    stop_occupation();
    if (hit) {
        pline("%s hits you!", vbuf);
        losehp(Maybe_Half_Phys(dmg), kbuf, KILLED_BY);
    } else {
        pline("%s misses you.", vbuf);
    }
    switch_hero(me);
}

/*
 * Hot-seat play: heroes share one screen, so a message about a hero other
 * than the one taking their turn (e.g. a monster attacking them, or a
 * hero swapping places with them) is labelled with that hero's name.
 * Returns the name to use, or Null for no label.
 */
const char *
hero_message_owner(void)
{
    if (hero_count() < 2 || mp_player_count() > 1 || !input_hero
        || cur_hero == input_hero)
        return (const char *) 0;
    return cur_hero->name;
}

/* tell the players whose turn it is */
void
announce_hero_turn(void)
{
    if (hero_count() < 2)
        return;
    input_hero = cur_hero;
    if (gm.multi < 0)
        return; /* helpless (asleep, paralyzed...); the turn just passes */
    if (mp_player_count() > 1) {
        /* everyone has a screen; the others get told by
           refresh_other_screens() */
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
