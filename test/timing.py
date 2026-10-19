#!/usr/bin/env python3
"""Timing test: real-time play in a server game (milestone M5).

Usage: python3 test/timing.py PLAYGROUND_DIR

Server-mode game in debug mode (the first player is named "wizard"), two
players, with short settings: a 1.5 s action window, an 8 s danger lock
and a 3 s prompt timeout.  Every monster is made peaceful first, so that
nothing but the test puts a hero in danger.  Checks that:
  - when one player acts and the other doesn't, the turn passes once the
    window is over, and then the game waits for someone to act;
  - when both act within the window, that's one turn;
  - a player who hasn't typed for a few moves isn't waited for;
  - a prompt left unanswered is cancelled, and holds up the game until
    then;
  - a hero in danger (just hurt, here) is waited for, the others are told
    why, and can still use commands that take no time meanwhile; other
    keys wait for their next move;
  - once the danger lock runs out, the hero in danger gives up the move.
The turn counter on the status line (the 'time' option) shows whether
time has passed.  Needs pyte (pip install pyte), and the user running it
must be in WIZARDS in the playground's sysconf.
"""

import os
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server import (GAME_STARTED, OPTIONS, Client, fail,  # noqa: E402
                    pump_all)

WINDOW, DANGER, PROMPT = 1.5, 8.0, 3.0


def turn(c):
    """The turn counter on c's status line."""
    m = re.search(r"T:(\d+)", c.screen.display[23])
    if not m:
        fail("no turn counter on %s's status line" % c.name, c)
    return int(m.group(1))


def wait_turn(clients, c, want, timeout):
    """Wait until c's turn counter reaches want; returns how long it took,
    or None."""
    start = time.time()
    while time.time() - start < timeout:
        pump_all(clients, 0.1)
        if turn(c) >= want:
            return time.time() - start
    return None


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    playground = os.path.abspath(sys.argv[1])
    binary = os.path.join(playground, "nethack")
    nhconnect = os.path.join(playground, "nh-connect")
    tmp = tempfile.mkdtemp(prefix="nh-timing-")
    sock = os.path.join(tmp, "game.sock")
    ben_name = "Ben%d" % (os.getpid() % 10000)

    env = dict(os.environ, NETHACK_SERVER=sock, NETHACK_HEROES="2",
               NETHACK_ACTION_WINDOW=str(WINDOW),
               NETHACK_DANGER_LOCK=str(DANGER),
               NETHACK_PROMPT_TIMEOUT=str(PROMPT),
               # the first hero isn't fast, so each of their moves is a
               # move Ben gives up (the other players choose for
               # themselves)
               NETHACKOPTIONS=OPTIONS + ",time,role:wizard,race:human",
               HOME=tmp)
    server = subprocess.Popen([binary, "-D"], env=env,
                              stdin=subprocess.DEVNULL,
                              stdout=open(os.path.join(tmp, "log"), "w"),
                              stderr=subprocess.STDOUT)
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    else:
        fail("server didn't create its socket")

    wiz = Client(nhconnect, sock, "Wiz")
    wiz.pump(0.5)
    ben = Client(nhconnect, sock, ben_name)
    clients = [wiz, ben]
    try:
        if wiz.seen(r"Old game in progress", 3):  # left by a killed run
            wiz.send("y", 1.0)
        if wiz.seen(r"Shall I pick", 5):
            wiz.send("y")
        wiz.expect(r"Is this ok\?")
        wiz.send("y")
        # debug mode can print a message during level creation that
        # waits for the first player at --More--
        for _ in range(30):
            if ben.seen(r"Shall I pick", 0.5):
                break
            wiz.seen(r"(?!)", 0.3)  # dismisses any More>>
        ben.expect(r"Shall I pick")
        ben.send("y")
        ben.expect(r"Is this ok\?")
        ben.send("y")
        wiz.expect(r"Dlvl:1")
        for _ in range(6):
            if re.search(GAME_STARTED, wiz.text()):
                break
            wiz.send("\r", 0.5)
        for c in clients:
            if not c.seen(r"Real-time play", 10):
                fail("%s wasn't told about real-time play" % c.name, c)
        print("ok: both players are told the game is played in real time")

        wiz.act("#wizcalm\r", 1.0)
        wiz.expect_past_more(r"calmed down")
        pump_all(clients, 1.0)

        # 1. the wizard acts and Ben doesn't: the turn passes once the
        #    window is over, then the game waits
        t0 = turn(wiz)
        wiz.act("ms", 0.1)
        took = wait_turn(clients, wiz, t0 + 1, WINDOW + 4)
        if took is None:
            fail("the turn didn't pass without Ben", wiz)
        if took < WINDOW - 0.4:
            fail("the turn passed after %.1fs, before the window was over"
                 % took, wiz)
        pump_all(clients, WINDOW + 1.0)
        if turn(wiz) != t0 + 1:
            fail("time went on with nobody acting", wiz)
        print("ok: a player who doesn't act gives up their move after the"
              " window (%.1fs)" % took)

        # 2. both act within the window: one turn
        t0 = turn(wiz)
        wiz.act("ms", 0.1)
        ben.act("ms", 0.1)
        pump_all(clients, 2 * WINDOW + 1.5)
        if turn(wiz) != t0 + 1:
            fail("both searching took %d turns, not one"
                 % (turn(wiz) - t0), wiz)
        print("ok: both players acting in the window is a single turn")

        # 3. Ben hasn't typed for a few moves: he isn't waited for
        for _ in range(3):
            t0 = turn(wiz)
            wiz.act("ms", 0.1)
            if wait_turn(clients, wiz, t0 + 1, WINDOW + 4) is None:
                fail("the turn didn't pass", wiz)
        pump_all(clients, 0.5)
        t0 = turn(wiz)
        wiz.act("ms", 0.05)
        took = wait_turn(clients, wiz, t0 + 1, WINDOW + 4)
        if took is None or took > WINDOW - 0.5:
            fail("still waiting for an idle player (%s)"
                 % ("%.1fs" % took if took else "no turn"), wiz)
        print("ok: an idle player isn't waited for (%.1fs)" % took)
        ben.act("ms", 0.1)  # back again
        pump_all(clients, WINDOW + 1.5)

        # 4. an unanswered prompt is cancelled; until then the game waits
        t0 = turn(wiz)
        mark = wiz.mark()
        wiz.act("#pray\r", 0.3)
        wiz.expect(r"Are you sure you want to pray")
        prompted = time.time()
        ben.act("ms", 0.1)
        pump_all(clients, PROMPT - 1.5)
        if turn(ben) != t0:
            fail("time passed while the wizard had a prompt open", ben)
        if not wiz.seen_since(mark, r"You took too long to answer",
                              PROMPT + 3):
            fail("the prompt wasn't cancelled", wiz)
        waited = time.time() - prompted
        if wait_turn(clients, ben, t0 + 1, WINDOW + 4) is None:
            fail("Ben's search didn't happen after the prompt", ben)
        print("ok: an unanswered prompt is cancelled (after %.1fs) and"
              " holds up the game until then" % waited)
        pump_all(clients, WINDOW + 1.0)

        # 5. Ben is hurt, so he's in danger: the wizard's move doesn't
        #    end the turn, and the wizard can look at their inventory
        #    meanwhile
        ben.act("#wizsethp\r")
        ben.expect(r"Set hit points to what")
        ben.send("3\r", 0.6)
        ben.expect_past_more(r"Your hit points are now 3")
        pump_all(clients, 1.0)
        t0 = turn(wiz)
        wmark, bmark = wiz.mark(), ben.mark()
        wiz.act("ms", 0.1)
        if not wiz.seen_since(wmark, r"Waiting for %s \(in danger\)"
                              % ben_name, WINDOW + 3):
            fail("the wizard wasn't told who the game is waiting for", wiz)
        if not ben.seen_since(bmark, r"Your allies are waiting for you", 3):
            fail("Ben wasn't told the others are waiting", ben)
        if turn(wiz) != t0:
            fail("the turn passed without the hero in danger", wiz)
        print("ok: a hero in danger is waited for, and everyone is told")

        wiz.act("i", 1.0)
        if not wiz.seen(r"Weapons|Armor|Spellbooks|Rings|Potions", 3):
            fail("the wizard couldn't look at their inventory", wiz)
        wiz.send("\x1b", 0.5)
        wiz.act("ms", 0.5)  # not the wizard's move: waits for it
        if turn(wiz) != t0:
            fail("the turn passed without the hero in danger", wiz)
        print("ok: commands that take no time can be used while waiting")

        # 6. Ben moves; the wizard's search, kept for their next move,
        #    starts the next turn, in which Ben is still in danger (hurt
        #    last turn); he doesn't act, and gives up his move once the
        #    danger lock runs out
        ben.act("ms", 0.1)
        if wait_turn(clients, wiz, t0 + 1, 3) is None:
            fail("Ben's move didn't end the turn", wiz)
        took = wait_turn(clients, wiz, t0 + 2, DANGER + 4)
        if took is None:
            fail("the danger lock didn't run out", wiz)
        if took < DANGER - 1.5:
            fail("the danger lock ran out after only %.1fs" % took, wiz)
        print("ok: keys typed out of turn wait for the next move, and the"
              " danger lock runs out (%.1fs)" % took)
        pump_all(clients, 1.0)

        # end the game properly, so no lock files are left behind
        for quitter in (wiz, ben, wiz):
            quitter.act("#quit\r")
            if quitter.seen(r"Really quit", 3):
                break
            quitter.send("\x1b", 0.5)
        else:
            fail("neither player could quit", wiz)
        quitter.send("y")
        for _ in range(12):
            quitter.send("q", 0.2)
            quitter.send("\r", 0.2)
            if server.poll() is not None:
                break
        server.wait(timeout=10)
    finally:
        if server.poll() is None:
            server.kill()
        for c in clients:
            try:
                os.kill(c.pid, 9)
            except OSError:
                pass


if __name__ == "__main__":
    main()
