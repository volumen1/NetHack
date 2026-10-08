# Multiplayer NetHack — Design

Status: draft. Decisions marked **Decided** come from design discussion;
items marked **Proposed** are defaults awaiting confirmation; **Open** items
still need an answer.

This is a hard fork of the NetHack 5.0 development tree. Staying mergeable
with upstream is an explicit non-goal.

---

## 1. Goals and ground rules

| Topic | Decision |
|---|---|
| Players | 5–6 per game, trusted friends on a private server. |
| Mode | Fully cooperative. No deliberate PvP. |
| Accidental PvP | Allowed when you can't tell an ally from a monster (blind, hallucinating, confused/stunned, Conflict, explosions, bouncing rays). |
| Scoring | Separate score and leaderboard entry per character. Entries from one game share a game ID. |
| Levels | The party starts together and is free to split across levels. |
| Time | One global clock, using the hybrid turn model (§4). |
| Monster targeting | Monsters attack the closest hero (§5). |
| Map | Map memory and allies' live monster sightings are shared by the whole party (§6). |
| Discoveries | Shared by the whole party. |
| Death | The player rejoins with a new character (§9). |
| Quest | One quest branch per role. Branches for all 13 roles are pre-created at game start (§8). |
| Ascension | One hero ascending ends the game for everyone (§9). |
| Experience | Per hero, to the killer only. Monster difficulty uses the highest XL on the level (§12). |
| Peacefuls | Angering one makes it hostile to the whole party (§5.2). |

Non-goals for v1: public servers, anti-cheat, spectators of games you're
not in, cross-game bones, non-Unix platforms, any window port other than
the one we choose (§10).

---

## 2. How the current engine works (why this is hard)

- **One hero.** `struct you u` (`include/you.h`, `include/decl.h:99`) is a
  single global with about 8,500 `u.` references in `src/`. The hero as a
  monster is `gy.youmonst`.
- **One live level.** The current level is `svl.level` (`dlevel_t`,
  `include/rm.h:473`) plus many separate per-level globals: traps
  (`ftrap`), regions, rooms, doors, stairs, light sources, level flags.
  When the hero leaves, `goto_level()` (`src/do.c:1486`) calls `savelev()`
  to write the level to disk and `getlev()` to load the next one. Only one
  level is ever in memory.
- **The main loop is built around one hero.** `moveloop_core()`
  (`src/allmain.c:173`) gives the hero movement points, runs `movemon()`,
  and calls `rhack()` for one keystroke stream. `svc.context.move` records
  whether the last command used game time.
- **Vision is for one hero.** `gv.viz_array` (`src/vision.c`) holds what
  the one hero can see. Remembered glyphs live in the map square itself
  (`levl[x][y].glyph`). That makes the shared map easy (§6).
- **Messages are second person.** `pline()`/`You()` (`src/pline.c`) go to
  the one message window. `pline_xy()`/`pline_mon()` already tag a message
  with a map location, which helps with routing (§7).
- **One display.** The window port (`include/winprocs.h`) is a single set of
  global function pointers with port-specific global state.
- **The quest is single-role.** `dat/dungeon.lua` defines one "The Quest"
  dungeon. Its `x-strt`/`x-loca`/`x-goal` levels are renamed using
  `gu.urole.filecode` (`src/dungeon.c:1151`). Progress is one global
  `svq.quest_status`. `MAXDUNGEON` is 16, with 9 used.

---

## 3. Engine refactor strategy

### 3.1 Current-hero indirection

Replace the `u` global with an array of heroes and a "current hero"
pointer:

```c
struct hero {
    struct you you;          /* former global u */
    struct monst youmonst;   /* former gy.youmonst */
    struct obj *invent;      /* former gi.invent */
    /* ...every other per-hero global (see §6 audit) */
    struct player *player;   /* connection, window state, message log */
};

extern struct hero *heroes[MAX_HEROES];
extern struct hero *cur_hero;
#define u        (cur_hero->you)
#define youmonst (cur_hero->youmonst)
```

The macro keeps the existing 8,500 references compiling. They read and
write whichever hero is current. To act as or toward a hero, code calls
`set_cur_hero(h)`. This is a context switch: the per-hero state lives in
`struct hero`, so switching moves no data.

The real work is finding code that must see **all** heroes, not just the
current one:

- **Whether a hero is at a square.** `u_at(x,y)` (`include/you.h:562`, only
  checks the current hero), `m_at()`-style lookups,
  and movement blocking need a hero-occupancy check (proposed: a per-level
  `heroes[COLNO][ROWNO]` grid, like `monsters[][]`).
- **Monster AI and attacks** (§5).
- **Area effects.** Explosions, ray/beam travel (`buzz()`, `bhit()`),
  scare effects, Conflict, aggravate, noise and wake-up must loop over
  heroes on the level.
- **Vision and display** (§6) and **messages** (§7).

Each hero's actions run with `cur_hero` set to that hero. Each monster's
actions run with `cur_hero` set to its target (§5). This keeps most
existing code paths correct without edits.

### 3.2 Multiple live levels

Collect all per-level globals into one struct and keep several in memory:

```c
struct live_level {
    d_level   uz;            /* which level this is */
    dlevel_t  level;         /* former svl.level */
    struct trap *ftrap;
    /* rooms, doors, stairs, regions, light sources, level-bound
       timers, lregions, level flags, engravings, ... */
    struct hero *heroes_here[MAX_HEROES];
};
extern struct live_level *cur_lev;   /* svl.level becomes cur_lev->level */
```

- A level is **live** while at least one hero is on it. When the last hero
  leaves, it's written to disk with the existing `savelev()`.
- When a hero arrives on a level that's already live, no loading happens:
  the hero is just added to `heroes_here`.
- Monsters following a hero between levels already use
  `gm.migrating_mons`. Arrival on a live level gets them placed right away.
- The main loop goes over live levels (§4.4). Code that currently assumes
  "the level" runs with `cur_lev` set to the level being processed.
- The global `svm.moves` and global timers (hero intrinsic timeouts and
  the like) stay global. Level-bound timers (corpse rot, egg hatch,
  burning objects) belong to their level and run when it's processed.

**Risk:** the per-level globals are scattered, and auditing them all is
the biggest single refactor task. Mitigation: milestone M6 (§11) does this
after single-level multiplayer works, so bugs from it are isolated.

---

## 4. Time model (hybrid)

### 4.1 Rules

- The global turn counter `svm.moves` advances once per **world turn**.
- Each hero gains movement points per world turn as today (speed and
  encumbrance unchanged). A hero with at least `NORMAL_SPEED` movement left
  is **ready**.
- **Free actions** never advance anything and run immediately: `i`, `;`,
  `\`, `^X`, options, and anything that leaves `svc.context.move` false.
- **Action window:** the first ready hero to enter a time-using command
  opens a window of `W` seconds (proposed default 1.5 s). Every other ready
  hero who enters a command before the window closes acts this phase.
- **Auto-wait:** when the window closes, ready heroes who haven't acted
  give up this move. Auto-wait is a pure pass: it uses movement but
  doesn't search, doesn't break multi-turn actions, and doesn't count as
  resting for any purpose that `.` would.
- **Danger lock:** a hero is in danger if a hostile monster is visible to
  them, or they took damage in the previous world turn. The window doesn't
  close on a hero in danger. Instead the phase waits up to `T` seconds
  (proposed 45 s), then auto-waits them. Other clients show
  "waiting on Bob (in danger)".
- **Fast heroes:** a hero with movement left after their first action
  gets another phase within the same world turn, under the same window
  rules. Speed keeps its meaning.
- **Multi-turn actions** (counts like `n20s`, travel `_`, running,
  eating, digging and other occupations) submit their next step
  automatically. They're interrupted exactly as today (a monster comes
  into view, and so on), which also triggers the danger lock.
- After all phases of a world turn: monsters move (`movemon` on every live
  level), then the world-turn upkeep runs (regeneration, hunger, timers,
  random spawning).

### 4.2 Prompts in the middle of a command — the hard technical problem

NetHack asks for input *inside* command execution. For example, `z` runs
`dozap()`, which asks which wand and then which direction, deep inside the
call stack. A single-threaded server can't block there while 5 other
players wait.

**Proposed approach: one coroutine per hero, with a per-level lock.**

- Each hero's command runs on its own coroutine (`ucontext` or a thread
  that only runs while holding a global engine lock, so there are no data
  races). When the window port needs input for that hero, the coroutine
  yields, and the server serves other heroes and clients.
- While a hero is inside a time-using command and waiting at a prompt,
  they hold their level's lock. Time-using commands by other heroes **on
  the same level** queue behind it. Heroes on other levels don't wait.
- This rule exists because other heroes acting while a prompt is open
  could free or move objects or monsters the paused command holds
  pointers to. A per-level lock prevents those crashes without auditing
  every command.
- A prompt has a timeout (proposed 30 s; longer if danger-locked), after
  which it's answered with ESC. That bounds how long one player can block
  their level.
- Free actions don't take the lock. They only read state, and running them
  while another hero's prompt is open is accepted as safe.

**Decided:** start with window `W` = 1.5 s, danger lock `T` = 45 s, and a
prompt timeout of 30 s. Make all three server settings, and retune them
during M5 playtests.

### 4.3 AFK and disconnected players

A disconnected hero is treated as permanently auto-waiting and never
danger-locks. **Proposed:** a disconnected hero is also "out of phase":
monsters ignore them and they can't be hurt, until they reconnect or the
host kicks them. Since the server is for trusted friends, there's no abuse
protection.

### 4.4 Main loop sketch

```
for each world turn:
    give movement to every hero and monster
    while any hero is ready:
        run one input phase (window, danger lock, prompts)
        execute the submitted commands (per-level lock)
    for each live level L:
        cur_lev = L
        movemon()          /* each monster sets cur_hero to its target */
        level upkeep (level timers, spawning, regions)
    global upkeep (moves++, hero timeouts, hunger, regen per hero)
```

---

## 5. Monsters and multiple heroes

### 5.1 Target choice — Decided: closest hero

- Distance is NetHack's usual grid distance (diagonal steps count as 1).
- Closest means closest to where the monster *thinks* the hero is.
  Invisibility and displacement keep working, using one "where is my
  target" guess per monster (`mux`/`muy`, set by `set_apparxy()`,
  `src/monmove.c`).
- **Stickiness (proposed):** keep the current target unless it's out of
  sight, or another hero is at least 2 squares closer. This stops monsters
  switching targets every step.
- **Ties:** prefer the hero who attacked this monster most recently, then
  pick at random.
- New field: `mtmp->mtarget` (hero index). Before a monster acts,
  `set_cur_hero(target)`. Existing `mhitu.c`/`monmove.c` code that says
  "you" then means the target.

### 5.2 Things monsters do to one hero

- **Engulfing, holding, sticky attacks** (`u.ustuck`): per hero, already
  in `struct you`. A monster can engulf or hold only one hero.
- **Stealing, seduction, item-specific attacks:** these act on the target.
- **Peaceful/hostile status:** stays per monster, not per hero.
  **Decided:** angering a peaceful monster makes it hostile to the whole
  party.
- **Elbereth and scare effects:** checked against the square of the hero
  being attacked, as now.
- **Covetous monsters, the Wizard of Yendor's harassment:** target the
  hero carrying the item they want; otherwise the closest hero.

### 5.3 Pets

- Each pet belongs to the hero who tamed it (new owner field in `struct
  edog`, `include/mextra.h:172`). It follows its owner between levels,
  and its owner's whistle, leash and steed rules apply.
- Pets never attack heroes, unless under Conflict or the pet goes feral.
- A pet whose owner dies stays tame and becomes unowned. **Proposed:** an
  ally can take it over with `#chat` or a leash.

---

## 6. Per-hero and shared state

### 6.1 Vision and map memory — Decided: shared map

- **Per hero:** current vision (`viz_array`, lit squares, what's in line
  of sight), telepathy, warning, monster detection that lasts while
  wearing an item, and "remembered, unseen monster" markers (`I`).
- **Shared:** remembered map squares and objects (`levl[x][y].glyph`,
  which is already one copy per level). Whatever any hero sees updates the
  memory the whole party sees.
- **Hallucination:** a hallucinating hero's view doesn't write to shared
  memory. Otherwise the whole party would see random monsters and objects.
- **One-time detection** (scroll, crystal ball, spell, potion):
  results go into shared memory, so the whole party benefits.
- **Allies are always shown** with their own glyph and color, even out of
  line of sight, as long as they're on the same level.
- **Decided: allies' monster sightings are shared live.** A hero sees
  every monster that any ally on the same level currently perceives, in
  addition to their own. Ongoing telepathy and warning stay personal: a
  telepath shares only what they're currently sensing, as part of their
  live sightings.
- **Exception for accidental PvP (Decided):** while a hero is blind or
  hallucinating, they lose the ally overlay and shared sightings and
  see only what they perceive themselves. Otherwise they could never mistake
  an ally for a monster (§6.3). A hallucinating hero's sightings aren't
  shared with the party either.

### 6.2 Audit: which state belongs to whom

| State | Owner | Notes |
|---|---|---|
| Inventory, attributes, HP/Pw, XP, intrinsics, conducts, achievements | Hero | Already in or next to `struct you`. |
| Luck, prayer timeout, god anger, alignment record | Hero | |
| Discoveries (identified object types, user-given names) | **Shared** | Decided. |
| Map memory | **Shared** | §6.1. |
| Shop bill and debt (`struct eshk`) | Hero | **Decided:** a bill per hero. The shopkeeper blocks the door only for a hero who owes money; other heroes come and go freely. Only the guilty hero is chased by an angry shopkeeper. |
| Temple priest, donations | Hero | |
| Vault guard | Hero | The guard handles the hero inside the vault. |
| Oracle consultations | Hero | |
| Quest status (`svq.quest_status`) | Hero | §8. |
| Sokoban: puzzle state, luck penalty, prize | Shared level state. The luck penalty goes to the hero who broke the rule. The prize can be taken once. |
| One-time things: throne and fountain wishes, Medusa, Vlad, Rodney, the Riders | Shared world | They exist once, and any hero can deal with them. |
| Amulet of Yendor, invocation items | Shared world | One Amulet. Any hero can carry it. |
| Score, death record | Hero | Each character gets its own leaderboard entry. |

The audit of every other global (`ga.`..`gz.`, `sva.`..`svz.`) gets done
in milestone M7, one file at a time.

### 6.3 Ally interactions

- **Moving into an ally** swaps places, as with pets. If they can't
  swap (e.g. Sokoban rules, a trapped ally), you just don't move.
- **Attacking a visible, recognized ally:** confirm "Really attack Bob?",
  in the same place the peaceful-monster check happens
  (`flags.confirm`, `src/uhitm.c:308`). Fighting with `F` toward an ally
  still asks.
- **Accidental PvP:**
  - When blind or hallucinating, an ally you can't identify appears as
    `I` or a random monster, so attacking that square hits them with no
    confirmation.
  - Explosions, rays, area spells and Conflict hit heroes like any other
    target.
  - Death from this counts as "killed by Bob's wand of fire" on the
    leaderboard.
- **Giving items:** dropping and picking up works with no new code.
  **Proposed:** add a `#give` command (adjacent ally, one item, no time
  for the receiver) for convenience.
- **Healing and other beneficial effects** (spells, wands, potions):
  target an ally via the normal direction prompts. They already work on
  monsters, so it's mostly about making heroes valid targets.

---

## 7. Messages and UI

### 7.1 Message routing

- **v1 (minimum):** `pline()` and friends go only to `cur_hero`'s player.
  The player who acts reads their messages as today.
- **Location-based messages:** `pline_xy()` and `pline_mon()` already know
  where something happened. Send these to every hero who can see that
  square, using the observer form if available.
- **Observer messages:** add an API for one message with two forms:
  `pline_obs(x, y, "You zap a wand.", "%s zaps a wand.")`. The actor gets
  form 1; heroes who see it get form 2 with the actor's name.
  Convert high-value messages first: combat, zapping, reading, deaths,
  level changes. Unconverted messages fall back to the v1 behaviour.
- **Sounds:** `You_hear()` messages go to every hero within hearing
  range on that level.
- **Party messages:** level changes, deaths, rejoin, and quest events
  ("Alice has entered the Quest") go to everyone.

### 7.2 UI additions

- **Party panel:** for each ally — name, role, HP/max, XL, depth, status
  (in danger, AFK, in prompt).
- **Turn indicator:** "waiting on Bob (in danger)" during a danger lock.
- **Chat:** `#chat` stays for monsters. **Proposed:** a new `#say`
  extended command (plus a free key binding) that sends free text to every
  connected player. It's a free action.

---

## 8. Quest — Decided: one branch per role, all pre-created

- At game start, create a quest dungeon for **all 13 roles**, regardless
  of who's in the party. Levels are generated only when first visited, so
  unused branches cost only dungeon-table entries. Raise `MAXDUNGEON` to
  fit (9 existing − 1 generic quest + 13 role quests = 21; proposed new
  limit 24).
- Each role's quest dungeon uses that role's level files directly (no
  more `x-` renaming with `gu.urole.filecode`).
- **Portals:** a portal for a role is placed only if that role is, or has
  been, in the party. If a role joins after its portal level was already
  generated (a rejoining character), the portal is added the next time
  that level is loaded. **Proposed:** all portals are on the same level,
  so the party finds them together.
- **Telepathic summons** ("You receive a faint telepathic message from
  …") go only to heroes of that role.
- **Quest leader:** talks only to heroes of their role. XL14 check,
  alignment check, and expulsion are per hero. Other heroes can enter any
  quest branch and help.
- **Nemesis and Bell:** anyone can kill the nemesis. Each nemesis drops a
  Bell of Opening, so several Bells can exist. Only one is needed for the
  invocation.
- **Quest artifact:** the artifact exists once per branch. The "gift"
  handling (leader's reaction when you return with it) applies to heroes
  of that role.
- **Duplicate roles:** two heroes of the same role share that role's
  branch. The first to take the artifact owns it. Both can get the
  leader's permission.

---

## 9. Death, rejoin, game end

### 9.1 Decided

When a hero dies, their player rejoins with a new character in the same
game.

### 9.2 Details

- **Death record:** the dead character's score goes on the leaderboard as
  a normal entry, with the game ID.
- **Corpse and gear:** no bones file inside the same game. The dead
  character's inventory drops on the death square, so allies (or the
  player's new character) can recover it. **Decided:** no ghost in v1.
- **New character:** the player picks role, race, gender and alignment
  as at game start. **Decided:** they start at XL1 with normal starting
  gear, at the normal starting position (the up stairs of DL1). If the role is new to the party, its quest portal is
  added (§8).
- **Death penalties for the party:** **Decided:** none, other than losing
  the character.
- **Pets** of the dead hero become unowned (§5.3).
- **Ascension:** **Decided:** when one hero ascends with the Amulet, the
  game ends for everyone. The ascending hero gets the ascension score.
- **Game end:** the game also ends when the host ends it, or (proposed) when
  no player has been connected for a set time. Every character still
  alive gets a leaderboard entry marked "alive at game end".

### 9.3 Leaderboard

Extend the record file entries with a game ID and party list. Each
character counts separately. A viewer can group entries by game.

---

## 10. Server and networking

- **One server process** holds the whole game. The engine runs on one
  thread, with one coroutine per hero (§4.2).
- **Window port:** use the curses port. ncurses supports several terminals
  in one process (`newterm()` and `set_term()`), so each player gets their
  own `SCREEN`. Switching `cur_hero` also calls `set_term()` and swaps the
  port's global state (the same indirection pattern as §3.1).
- **Connecting:** players `ssh` to the host. A small launcher authenticates
  them and passes their terminal to the server through a Unix socket.
  There's no custom client in v1.
- **Rendering:** each player's screen is drawn from their own vision plus
  shared memory (§6.1). Clients only redraw squares that changed, which
  curses already handles.
- **Saving:** save the whole game: every hero, every live level (written
  through `savelev()`), the party file, and the turn state. Restore only
  with the same game ID. Players reconnect into their heroes.
- **Joining:** v1 creates the party at game start (everyone in the lobby
  makes a character). Joining a running game uses the rejoin path (§9).
- Remove all other window ports and non-Unix platform code from the
  fork, to reduce what the refactors must keep compiling.

---

## 11. Milestones

Each milestone should end in something that runs.

| # | Milestone | Proves |
|---|---|---|
| M0 | Fork cleanup: remove other ports/platforms, build only curses on Linux. **Done** (§11.1). | Clean base. |
| M1 | Hero indirection (§3.1). Two heroes on one level, **hot-seat** on one terminal, alternating turns. | The `cur_hero` approach works; most code runs unchanged. |
| M2 | Network: server process, ssh launcher, one curses `SCREEN` per player. Turns still alternate. | Several players, several screens. |
| M3 | Closest-hero targeting (§5.1), ally display, swap places, attack confirmation, accidental PvP. | Multiple heroes in the same fight. |
| M4 | Message routing v1 + location-based messages (§7.1), party panel. | Each player understands what's going on. |
| M5 | Hybrid time model with coroutines and per-level lock (§4). | The game feels right. **Highest gameplay risk; playtest heavily.** |
| M6 | Multiple live levels (§3.2). | The party can split. **Highest engine risk.** |
| M7 | Per-hero state audit (§6.2): shops, priests, pets, quest status, etc. | Correct rules everywhere. |
| M8 | Per-role quest branches (§8). | Everyone gets their quest. |
| M9 | Death/rejoin, leaderboard, whole-game save/restore (§9, §10). | A full game can be played start to finish. |
| M10 | Observer messages (`pline_obs`), party chat, balance tuning (§12). | Polish. |


### 11.1 M0 notes

- Removed: `sys/{amiga,atari,mac68k,msdos,vms,windows,libnh}`, the tty, X11,
  Qt, GEM, macOS, Win32, chain and shim window ports, all sound libraries,
  the PDCurses submodules, `outdated/`, cross-compiling support, and the
  Xcode, BSD and macOS build files.
- The build has one configuration: `sys/unix/hints/linux.501`, curses only
  (`multiw-1.501` and `multiw-2.501` no longer offer a choice).
- `test/smoke.py` drives the installed game in a pseudo-terminal: new game,
  save, restore, quit. CI (`.github/workflows/build.yml`) builds and runs it
  on every push.
- **Not done yet:** platform `#ifdef` blocks inside `src/`, `include/` and
  `sys/share/` (MSDOS, VMS, AMIGA, WIN32, TTY_GRAPHICS, and so on) and their
  headers are still there. They're inactive in our build, and get removed
  when the refactors touch those files.

---

## 12. Balance (to tune after M9 playtests)

- **Monster difficulty:** today it depends on hero XL and depth.
  **Decided:** use the highest XL among heroes on the level.
- **Spawn rate:** **Proposed:** scale by the number of heroes on the
  level, with diminishing returns (for example `sqrt(n)`).
- **Experience:** **Decided:** XP is per hero and goes to the killer
  only. Nothing is shared.
- **Loot and wishes:** leave as in solo play. Scarcity encourages sharing.

---

## 13. Open questions

None at the moment. Items still marked **Proposed** are defaults we'll keep
unless playtesting says otherwise.
