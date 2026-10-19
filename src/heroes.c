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

#include <time.h>
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

/* the hybrid time model's state; see hero_schedule() */
static struct {
    boolean init;
    boolean strict;     /* NETHACK_TURNS=strict */
    long window_ms;     /* NETHACK_ACTION_WINDOW */
    long danger_ms;     /* NETHACK_DANGER_LOCK */
    long prompt_ms;     /* NETHACK_PROMPT_TIMEOUT */
    boolean started;    /* hero_schedule() has run: prompts time out */
    boolean open;       /* the action window is open... */
    long opened;        /* ...since this time */
    long turn;          /* the turn the phases are for */
    boolean free_only;  /* running a command out of turn */
    struct hero *timed_out; /* didn't answer a prompt in time */
} tm;

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
       others just watch (with the hybrid time model, everyone does
       while the game waits for someone to act) */
    curses_mp_watching(input_hero ? (to != input_hero->screen)
                                  : tm.started);

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
 * Observer messages (doc/multiplayer-design.md, section 7.1): tell the
 * other heroes who can see the current hero what they just did, e.g.
 * "Alice hits the jackal." or "Alice zaps a wand."  'mon' is the monster
 * involved, if any; 'obj' the object (for OBS_READ and OBS_THROW).
 *
 * Whether another hero sees the actor is worked out from the actor's
 * side, since vision is only computed for the current hero: line of sight
 * is symmetric, so it's whether the actor could see the observer's spot,
 * with the actor's own spot lit or the observer adjacent.  The observer
 * also mustn't be blind, and must be able to see the actor if invisible.
 * Heroes sharing the actor's screen (hot-seat) already saw the actor's
 * own message, so they don't get one.
 */
void
observe(int what, struct monst *mon, struct obj *obj)
{
    struct hero *actor = cur_hero, *h;
    boolean seen[MAX_HEROES], lit, actor_invis;
    char objbuf[BUFSZ];
    int i, n = 0;

    if (hero_count() < 2 || u.uswallow || u.uburied)
        return;
    lit = (levl[u.ux][u.uy].lit != 0);
    actor_invis = (Invis != 0);
    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        seen[i] = h->active && h != actor && h->screen != actor->screen
                  && on_level(&h->you.uz, &u.uz)
                  && couldsee(h->you.ux, h->you.uy)
                  && (lit || distmin(u.ux, u.uy, h->you.ux, h->you.uy) <= 1);
        n += seen[i];
    }
    if (!n)
        return;
    objbuf[0] = '\0';
    if (obj && what == OBS_THROW)
        Strcpy(objbuf, an(xname(obj)));

    for (i = 0; i < MAX_HEROES; i++) {
        if (!seen[i])
            continue;
        switch_hero(&heroes[i]);
        if (!Blind && (!actor_invis || See_invisible)) {
            switch (what) {
            case OBS_HIT:
                pline("%s hits %s.", actor->name, mon_nam(mon));
                break;
            case OBS_MISS:
                pline("%s misses %s.", actor->name, mon_nam(mon));
                break;
            case OBS_KILL:
                pline("%s %s %s!", actor->name,
                      nonliving(mon->data) ? "destroys" : "kills",
                      mon_nam(mon));
                break;
            case OBS_ZAP:
                pline("%s zaps a wand.", actor->name);
                break;
            case OBS_QUAFF:
                pline("%s drinks a potion.", actor->name);
                break;
            case OBS_READ:
                pline("%s reads a %s.", actor->name,
                      (obj && obj->oclass == SPBOOK_CLASS) ? "spellbook"
                                                           : "scroll");
                break;
            case OBS_CAST:
                pline("%s casts a spell.", actor->name);
                break;
            case OBS_PRAY:
                pline("%s prays.", actor->name);
                break;
            case OBS_THROW:
                pline("%s throws %s.", actor->name, objbuf);
                break;
            }
        }
    }
    switch_hero(actor);
}

/*
 * Tell the party when one of them becomes badly hurt (below a third of
 * their hit points), once; they're watched again after healing past half.
 * Called after each action that took time.
 */
void
party_health_check(void)
{
    struct hero *me = cur_hero, *h, *o;
    boolean told[MAX_HEROES];
    int i, j, hp, hpmax;

    if (hero_count() < 2)
        return;
    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (!h->active)
            continue;
        if (h->you.umonnum != h->you.umonster)
            hp = h->you.mh, hpmax = h->you.mhmax; /* polymorphed */
        else
            hp = h->you.uhp, hpmax = h->you.uhpmax;
        if (h->hurt_warned) {
            if (hp * 2 >= hpmax)
                h->hurt_warned = FALSE;
            continue;
        }
        if (hp <= 0 || hp * 3 >= hpmax)
            continue;
        h->hurt_warned = TRUE;
        /* once per screen: hot-seat heroes all share one */
        for (j = 0; j < MAX_HEROES; j++)
            told[j] = FALSE;
        for (j = 0; j < MAX_HEROES; j++) {
            o = &heroes[j];
            if (!o->active || o == h || told[o->screen])
                continue;
            told[o->screen] = TRUE;
            if (o->screen != me->screen)
                switch_hero(o);
            else
                switch_hero(me);
            pline("%s is badly hurt!  (HP %d of %d)", h->name, hp, hpmax);
        }
        switch_hero(me);
    }
}

/*
 * #party: list the heroes in the game, with what each player would want
 * to know about their allies at a glance (doc/multiplayer-design.md,
 * section 7.2).  Takes no time.
 */
int
doparty(void)
{
    struct hero *me = cur_hero, *h;
    char lines[MAX_HEROES][BUFSZ], cond[BUFSZ];
    winid win;
    int i, n = 0;

    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (!h->active)
            continue;
        switch_hero(h); /* some details only make sense as that hero */
        cond[0] = '\0';
        if (gm.multi < 0)
            Strcat(cond, ", helpless");
        if (Blind)
            Strcat(cond, ", blind");
        if (Hallucination)
            Strcat(cond, ", hallucinating");
        if (Confusion)
            Strcat(cond, ", confused");
        if (Stunned)
            Strcat(cond, ", stunned");
        if (Sick)
            Strcat(cond, ", ill");
        if (Stoned)
            Strcat(cond, ", turning to stone");
        if (u.uhs >= WEAK)
            Strcat(cond, ", weak from hunger");
        else if (u.uhs == HUNGRY)
            Strcat(cond, ", hungry");
        Snprintf(lines[n], BUFSZ,
                 "%s%-12s %s %s, level %d, HP %d(%d), Dlvl %d%s%s%s",
                 (h == me) ? "* " : "  ", h->name, gu.urace.adj,
                 (flags.female && gu.urole.name.f) ? gu.urole.name.f
                                                   : gu.urole.name.m,
                 u.ulevel, Upolyd ? u.mh : u.uhp,
                 Upolyd ? u.mhmax : u.uhpmax, depth(&u.uz),
                 (h == input_hero && h != me) ? ", moving now" : "",
                 cond[0] ? " -- " : "", cond[0] ? cond + 2 : "");
        n++;
    }
    switch_hero(me);

    win = create_nhwindow(NHW_TEXT);
    putstr(win, ATR_BOLD, "Your party (* is you):");
    putstr(win, 0, "");
    for (i = 0; i < n; i++)
        putstr(win, 0, lines[i]);
    display_nhwindow(win, TRUE);
    destroy_nhwindow(win);
    return ECMD_OK;
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
        /* still watching while this is added, so it goes next to what
           arrived meanwhile ("Alice kills the jackal!  It is your turn.")
           or scrolls it away, rather than stopping for More>>; from here
           on the usual More>> stops apply */
        pline("It is your turn.");
        curses_mp_watching(FALSE);
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
    if (hybrid_turns())
        flush_screen(1); /* someone else may be next, so finish this one */
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
        if (h->waiting_for != was && !hybrid_turns()) {
            pline("It is %s's turn.", was->name);
            h->waiting_for = was;
        }
        bot();
        flush_screen(1);
    }
    switch_hero(was);
    was->waiting_for = (struct hero *) 0;
}

/*
 * The hybrid time model for server games (doc/multiplayer-design.md,
 * section 4).  Instead of taking turns in a fixed order, every hero who
 * has a move left can act whenever their player types a command:
 *
 *  - A *phase* is one move for each hero who has one left.  The first
 *    action that takes time opens the action window; the other heroes
 *    then have NETHACK_ACTION_WINDOW seconds (default 1.5) to act too,
 *    after which those who haven't simply give up that move.  A fast
 *    hero's extra move in a turn is a phase of its own, whose window
 *    opens as soon as it starts.
 *  - A hero in danger (a hostile monster in view, or hurt this turn or
 *    last) is waited for longer: NETHACK_DANGER_LOCK seconds (45).
 *  - Multi-turn actions (running, travel, counts, occupations) carry on
 *    by themselves, as does a helpless hero's turn.
 *  - A hero who has given up IDLE_AFTER moves in a row is taken to be
 *    away, and isn't waited for at all until their player types again
 *    (unless they're in danger).
 *  - Commands that take no time can be used out of turn; other keys
 *    typed out of turn wait until that hero's move comes up.
 *  - A prompt left unanswered for NETHACK_PROMPT_TIMEOUT seconds (30;
 *    the danger lock if longer, for a hero in danger) is cancelled.
 *
 * The game is single-threaded: a command runs to the end, prompts and
 * all, before anyone else's.  With every hero on one level that is the
 * per-level lock of section 4.2; running other players' free actions
 * during a prompt needs the coroutines planned for M6.
 *
 * NETHACK_TURNS=strict keeps the old way, where heroes take turns in
 * order and each player waits for theirs.
 */
#define IDLE_AFTER 3 /* moves given up in a row before a hero is away */
#define WAIT_NOTICE_TURNS 20 /* how often to say who's being waited for */


staticfn long now_ms(void);
staticfn long env_ms(const char *, long);
staticfn void timing_init(void);
staticfn genericptr_t hero_global(struct hero *, genericptr_t);
staticfn boolean can_move_now(struct hero *);
staticfn boolean hero_busy(struct hero *);
staticfn boolean hostile_in_view(void);
staticfn void check_dangers(void);
staticfn void tell_hero(struct hero *, const char *);
staticfn void take_move(struct hero *);
staticfn void out_of_turn(struct hero *);
staticfn void waiting_notices(void);

staticfn long
now_ms(void)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000L + (long) (ts.tv_nsec / 1000000L);
}

/* a time setting, in seconds (fractions allowed), as milliseconds */
staticfn long
env_ms(const char *name, long dflt)
{
    const char *val = nh_getenv(name);
    double secs;

    if (!val || !*val)
        return dflt;
    secs = atof(val);
    return (secs < 0.0) ? dflt : (long) (secs * 1000.0 + 0.5);
}

staticfn void
timing_init(void)
{
    const char *turns;

    if (tm.init)
        return;
    tm.init = TRUE;
    tm.window_ms = env_ms("NETHACK_ACTION_WINDOW", 1500L);
    tm.danger_ms = env_ms("NETHACK_DANGER_LOCK", 45000L);
    tm.prompt_ms = env_ms("NETHACK_PROMPT_TIMEOUT", 30000L);
    turns = nh_getenv("NETHACK_TURNS");
    tm.strict = (turns && !strcmpi(turns, "strict"));
}

/* whether this game uses the hybrid time model: a server game where
   every hero has their own player */
boolean
hybrid_turns(void)
{
    timing_init();
    return (!tm.strict && hero_count() > 1
            && mp_player_count() >= hero_count());
}

/* where hero h's copy of one of the hero_globals[] is */
staticfn genericptr_t
hero_global(struct hero *h, genericptr_t addr)
{
    size_t i;
    char *p = (char *) h->stash;

    if (h == cur_hero || !p)
        return addr;
    for (i = 0; i < SIZE(hero_globals); i++) {
        if (hero_globals[i].addr == addr)
            return (genericptr_t) p;
        p += hero_globals[i].len;
    }
    impossible("hero_global: not a per-hero global");
    return addr;
}

/* h has a move left in this phase */
staticfn boolean
can_move_now(struct hero *h)
{
    return (h->active && !h->acted && h->you.umovement >= NORMAL_SPEED);
}

/* h's next move doesn't need their player: a multi-turn action is under
   way, or they're helpless */
staticfn boolean
hero_busy(struct hero *h)
{
    return (*(int *) hero_global(h, (genericptr_t) &gm.multi) != 0
            || *(int (**)(void)) hero_global(h,
                                       (genericptr_t) &go.occupation) != 0);
}

/* the current hero is in danger: a hostile monster in view, or they got
   hurt this turn or last */
staticfn boolean
hostile_in_view(void)
{
    struct monst *mtmp;

    if (cur_hero->hurt_turn && svm.moves - cur_hero->hurt_turn <= 1L)
        return TRUE;
    if (gv.vision_full_recalc)
        vision_recalc(0);
    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp) || mtmp->mpeaceful || mtmp->mtame)
            continue;
        if (canspotmon(mtmp))
            return TRUE;
    }
    return FALSE;
}

/* note who got hurt, and who is in danger */
staticfn void
check_dangers(void)
{
    struct hero *was = cur_hero, *h;
    int i, hp;

    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (!h->active)
            continue;
        hp = (h->you.umonnum != h->you.umonster) ? h->you.mh : h->you.uhp;
        if (hp < h->last_hp)
            h->hurt_turn = svm.moves;
        h->last_hp = hp;
        if (!can_move_now(h))
            continue;
        switch_hero(h);
        h->in_danger = hostile_in_view();
    }
    switch_hero(was);
}

/* show a message on hero o's screen */
staticfn void
tell_hero(struct hero *o, const char *msg)
{
    struct hero *was = cur_hero;

    switch_hero(o);
    if (o != input_hero)
        curses_mp_watching(TRUE); /* not being asked anything */
    pline("%s", msg);
    flush_screen(1);
    switch_hero(was);
}

/* h makes their move now */
staticfn void
take_move(struct hero *h)
{
    input_hero = h; /* before switching, so h's screen stops watching */
    if (h != cur_hero) {
        flush_screen(1);
        switch_hero(h);
    }
    curses_mp_watching(FALSE);
}

/* h typed a command that takes no time while it isn't their move */
staticfn void
out_of_turn(struct hero *h)
{
    struct hero *was = cur_hero, *was_input = input_hero;
    boolean was_move = svc.context.move;

    take_move(h);
    tm.free_only = TRUE;
    rhack(0);
    tm.free_only = FALSE;
    flush_screen(1);
    input_hero = was_input;
    switch_hero(was);
    svc.context.move = was_move;
}

/* the phase is waiting for heroes in danger: say so, now and then */
staticfn void
waiting_notices(void)
{
    struct hero *h, *o;
    char buf[BUFSZ];
    int i, j;

    for (i = 0; i < MAX_HEROES; i++) {
        h = &heroes[i];
        if (!can_move_now(h)
            || (h->told_waiting
                && svm.moves < h->told_waiting + WAIT_NOTICE_TURNS))
            continue;
        h->told_waiting = svm.moves;
        tell_hero(h, "Your allies are waiting for you.");
        Snprintf(buf, sizeof buf, "Waiting for %s (in danger).", h->name);
        for (j = 0; j < MAX_HEROES; j++) {
            o = &heroes[j];
            if (o->active && o != h)
                tell_hero(o, buf);
        }
    }
}

/* the time model's part of moveloop_core(): the current hero just used
   up a move (or gave it up) */
void
hero_took_time(void)
{
    if (!hybrid_turns())
        return;
    cur_hero->acted = TRUE;
    if (!tm.open) {
        tm.open = TRUE;
        tm.opened = now_ms();
    }
}

/*
 * Server game: choose the hero who moves next, waiting for their player
 * to type if need be (see the comment above), and make them the current
 * hero.  Called from moveloop_core() before each command.  Returns TRUE
 * if that hero gives up their move instead of doing something.
 */
boolean
hero_schedule(void)
{
    struct hero *h, *pick;
    boolean any, held[MAX_HEROES];
    int i, n, key, scr[MAX_HEROES];
    long now, due, wait;
    char buf[BUFSZ];

    if (!hybrid_turns())
        return FALSE;
    input_hero = 0; /* nobody's being asked anything yet */
    curses_mp_watching(TRUE);
    if (!tm.started) {
        tm.started = TRUE;
        Snprintf(buf, sizeof buf,
                 "Real-time play: once someone acts, the rest have %ld.%lds.",
                 tm.window_ms / 1000L, (tm.window_ms % 1000L) / 100L);
        for (i = 0; i < MAX_HEROES; i++)
            if (heroes[i].active)
                tell_hero(&heroes[i], buf);
    }
    if (tm.timed_out) {
        tell_hero(tm.timed_out, "You took too long to answer.");
        tm.timed_out = 0;
    }
    check_dangers();

    for (;;) {
        /* a new turn: everyone gets a move, and the window waits for
           someone to act */
        if (svm.moves != tm.turn) {
            for (i = 0; i < MAX_HEROES; i++)
                heroes[i].acted = FALSE;
            tm.open = FALSE;
            tm.turn = svm.moves;
        }
        /* everyone with a move left has had one, but a fast hero has
           another: a new phase; the turn is already under way, so its
           window opens at once (otherwise the others, with no move left,
           would wait for the fast hero for as long as their player
           took) */
        for (any = FALSE, i = 0; i < MAX_HEROES; i++)
            if (can_move_now(&heroes[i]))
                any = TRUE;
        if (!any) {
            for (i = 0; i < MAX_HEROES; i++)
                heroes[i].acted = FALSE;
            tm.open = TRUE;
            tm.opened = now_ms();
            check_dangers();
        }

        /* multi-turn actions carry on by themselves */
        pick = (can_move_now(cur_hero) && hero_busy(cur_hero)) ? cur_hero
                                                                : 0;
        for (i = 0; !pick && i < MAX_HEROES; i++)
            if (can_move_now(&heroes[i]) && hero_busy(&heroes[i]))
                pick = &heroes[i];
        if (pick) {
            take_move(pick);
            return FALSE;
        }

        /* typed commands, and commands queued by an earlier one */
        for (i = 0; i < MAX_HEROES; i++) {
            h = &heroes[i];
            held[i] = FALSE;
            if (!h->active)
                continue;
            key = curses_mp_peek_key(h->screen);
            if (key == -1
                && !((struct _cmd_queue **) hero_global(
                         h, (genericptr_t) gc.command_queue))[CQ_CANNED])
                continue;
            if (can_move_now(h)) {
                h->autowaits = 0;
                take_move(h);
                return FALSE;
            }
            if (key == -2 || key_runs_free_cmd(key)) {
                out_of_turn(h);
                i = -1; /* things may have changed; look again */
                continue;
            }
            held[i] = TRUE; /* kept for this hero's next move */
        }

        /* heroes who haven't acted in time give up their move */
        now = now_ms();
        wait = -1L;
        if (tm.open) {
            for (i = 0; i < MAX_HEROES; i++) {
                h = &heroes[i];
                if (!can_move_now(h))
                    continue;
                if (h->in_danger && tm.danger_ms > 0L)
                    due = tm.danger_ms;
                else if (h->autowaits >= IDLE_AFTER)
                    due = 0L;
                else
                    due = tm.window_ms;
                due += tm.opened;
                if (now >= due) {
                    h->autowaits++;
                    take_move(h);
                    return TRUE;
                }
                if (wait < 0L || due - now < wait)
                    wait = due - now;
            }
            if (now >= tm.opened + tm.window_ms)
                waiting_notices();
            else if (wait > tm.opened + tm.window_ms - now)
                wait = tm.opened + tm.window_ms - now; /* to tell them */
        }

        /* wait for someone to type; a hero whose keys are being kept
           for later would wake this up straight away */
        for (n = 0, i = 0; i < MAX_HEROES; i++)
            if (heroes[i].active && !held[i])
                scr[n++] = heroes[i].screen;
        curses_mp_wait_input(scr, n, wait);
    }
}

/* how long the current player may take to answer a prompt, in
   milliseconds; negative for no limit */
long
hero_prompt_timeout(void)
{
    if (!tm.started || tm.prompt_ms <= 0L || program_state.gameover
        || !hybrid_turns())
        return -1L;
    if (cur_hero->in_danger && tm.danger_ms > tm.prompt_ms)
        return tm.danger_ms;
    return tm.prompt_ms;
}

/* the current player didn't answer a prompt in time */
void
hero_prompt_timed_out(void)
{
    tm.timed_out = cur_hero;
}

/* a command is being run out of turn: only those that take no time */
boolean
hero_free_only(void)
{
    return tm.free_only;
}

/*heroes.c*/
