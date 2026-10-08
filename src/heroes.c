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

        Sprintf(qbuf, "Player %d, what is your name?", i + 1);
        getlin(qbuf, namebuf);
        (void) mungspaces(namebuf);
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
    }
}

/* tell the players whose turn it is (hot-seat play) */
void
announce_hero_turn(void)
{
    if (hero_count() < 2)
        return;
    docrt();
    bot();
    pline("%s, it is your turn.", cur_hero->name);
}

/*heroes.c*/
