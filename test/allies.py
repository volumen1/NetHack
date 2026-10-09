#!/usr/bin/env python3
"""Ally test: heroes swap places, and attacking an ally asks first
(milestone M3).

Usage: python3 test/allies.py PLAYGROUND_DIR

Hot-seat game with two heroes standing next to each other:
  - the first hero moves into the second and they swap places;
  - the second uses F toward the first, declines "Really attack?" (no
    time passes), then accepts and attacks.
Needs the 'pyte' terminal emulator (pip install pyte).
"""

import os
import pty
import re
import select
import sys
import tempfile
import time

try:
    import pyte
except ImportError:
    print("SKIP: this test needs pyte (pip install pyte)")
    sys.exit(0)

OPTIONS = ("windowtype:curses,name:Ally%d,role:valkyrie,race:human,"
           "gender:female,align:lawful,!autopickup,!tutorial,"
           "suppress_alert:5.0,number_pad:0") % (os.getpid() % 10000)
NAME = "Ally%d" % (os.getpid() % 10000)

# direction keys for (dx, dy)
KEYS = {(-1, 0): "h", (1, 0): "l", (0, -1): "k", (0, 1): "j",
        (-1, -1): "y", (1, -1): "u", (-1, 1): "b", (1, 1): "n"}


class Game:
    def __init__(self, binary, home, args=()):
        self.screen = pyte.Screen(80, 24)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = bytearray()  # everything received, for one-off messages
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ.update(TERM="xterm", HOME=home, LINES="24",
                              COLUMNS="80", NETHACKOPTIONS=OPTIONS,
                              NETHACK_HEROES="2")
            os.execv(binary, [binary] + list(args))

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if ready:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                self.stream.feed(data)
                self.raw.extend(data)

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def top(self):
        return self.screen.display[0]

    def seen(self, pattern, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.text()):
                return True
            if ">>" in self.top():
                self.send("\r", 0.3)
            self.pump(0.1)
        return False

    def expect(self, pattern, timeout=10):
        if not self.seen(pattern, timeout):
            fail("timed out waiting for %r" % pattern, self)

    def send(self, keys, settle=0.4):
        os.write(self.fd, keys.encode("latin-1"))
        self.pump(settle)

    def settle(self):
        """Dismiss any pending More>> so the map and cursor are current."""
        for _ in range(10):
            self.pump(0.3)
            if ">>" not in self.top():
                return
            self.send("\r", 0.3)

    def heroes(self):
        """(current hero's position, the other hero's position)."""
        self.settle()
        cur = (self.screen.cursor.x, self.screen.cursor.y)
        ats = [(x, y) for y in range(1, 22)
               for x, ch in enumerate(self.screen.display[y]) if ch == "@"]
        if cur not in ats or len(ats) != 2:
            fail("can't find both heroes (cursor %r, @ at %r)" % (cur, ats),
                 self)
        other = [p for p in ats if p != cur][0]
        return cur, other

    def whose_turn(self):
        status = self.screen.display[22]
        return "Bob" if status.startswith("Bob ") else NAME

    def start(self):
        """Get through character creation for both heroes."""
        if self.seen(r"Old game in progress", 3):  # a killed earlier run
            self.send("y", 1.0)
        self.expect(r"Player 2, what is your name\?")
        self.send("Bob\r")
        self.send(" ")
        self.expect(r"Shall I pick")
        self.send("y")
        self.expect(r"Is this ok\?")
        self.send("y")
        self.expect(r"Dlvl:1")
        for _ in range(4):
            if "welcome to NetHack" in self.text():
                break
            self.send("\r", 0.5)
        self.expect(r"welcome to NetHack")

    def quit(self):
        """Quit properly, so no lock files are left behind."""
        for _ in range(10):  # a hero may be asleep for a few turns
            self.settle()
            self.send("\x1b", 0.2)
            self.send("#quit\r", 0.6)
            if self.seen(r"Really quit", 3):
                break
        else:
            fail("couldn't quit", self)
        self.send("y", 0.6)
        for _ in range(12):
            self.send("q", 0.2)
            self.send("\r", 0.2)
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return


def fail(msg, game=None):
    print("FAIL:", msg)
    if game is not None:
        print("--- screen ---")
        print(game.text())
    sys.exit(1)


def direction(frm, to):
    d = (to[0] - frm[0], to[1] - frm[1])
    if d not in KEYS:
        return None
    return KEYS[d]


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    playground = os.path.abspath(sys.argv[1])
    binary = os.path.join(playground, "nethack")
    game = Game(binary, tempfile.mkdtemp(prefix="nh-allies-"))
    try:
        game.start()

        # 1. the first hero moves into the second: they swap places
        if game.whose_turn() != NAME:
            fail("expected the first hero to move first", game)
        me, ally = game.heroes()
        key = direction(me, ally)
        if not key:
            fail("heroes don't start next to each other", game)
        game.send(key, 0.8)
        if not game.seen(r"You swap places with Bob"):
            if game.seen(r"Bob is in your way", 1):
                print("SKIP: the swap was blocked by the terrain here")
                return
            fail("no swap message", game)
        game.settle()
        if game.whose_turn() == NAME:  # a fast hero may move twice
            game.send("ms", 0.8)
            game.settle()
        now_me, now_ally = game.heroes()   # now it's Bob's turn
        if game.whose_turn() != "Bob":
            fail("expected Bob's turn after the swap", game)
        if (now_me, now_ally) != (me, ally):
            fail("positions didn't swap: before %r/%r, after Bob at %r"
                 % (me, ally, now_me), game)
        print("ok: moving into an ally swaps places")

        # 2. Bob uses F toward the first hero and declines
        key = direction(now_me, now_ally)
        game.send("F" + key, 0.6)
        game.expect(r"Really attack %s\?" % NAME)
        game.send("n", 0.6)
        game.settle()
        if game.whose_turn() != "Bob":
            fail("declining the attack shouldn't use up the turn", game)
        print("ok: F toward an ally asks first; declining takes no time")

        # 3. ... and then attacks
        game.send("F" + key, 0.6)
        game.expect(r"Really attack %s\?" % NAME)
        game.send("y", 0.8)
        game.expect(r"You (hit|miss) %s" % NAME)
        print("ok: confirmed attack on an ally happens")

        game.quit()
    finally:
        try:
            os.kill(game.pid, 9)
        except OSError:
            pass

    blind_test(binary)


def blind_test(binary):
    """A blindfolded hero can't tell their ally from a monster: walking
    into them first bumps into 'something', then attacks it."""
    global NAME
    game = Game(binary, tempfile.mkdtemp(prefix="nh-allies-"), ["-D"])
    NAME = "wizard"  # debug mode renames the first hero
    try:
        game.start()
        me, ally = game.heroes()
        key = direction(me, ally)
        if not key:
            fail("heroes don't start next to each other", game)

        # wish for a blindfold and put it on (takes the first hero's turn)
        game.send("\x17", 0.5)
        game.send("blindfold\r", 0.8)
        game.seen(r"\b[a-zA-Z] - a blindfold")
        m = re.search(r"\b([a-zA-Z]) - a blindfold", game.text())
        if not m:
            fail("no blindfold from the wish", game)
        game.settle()
        game.send("P" + m.group(1), 0.8)
        game.settle()
        while game.whose_turn() != "Bob":   # in case the hero is fast
            game.send("ms", 0.8)
            game.settle()
        bob, first = game.heroes()
        if first != me:
            fail("the first hero moved", game)
        game.send("ms", 0.8)    # Bob waits
        game.settle()
        if game.whose_turn() != NAME:
            game.send("ms", 0.8)
            game.settle()

        # blind: the first move into the ally finds "something"
        game.send(key, 0.8)
        game.expect(r"There's something there you can't see")
        print("ok: a blind hero bumps into an ally they can't see")
        while game.whose_turn() != NAME:
            game.send("ms", 0.8)
            game.settle()
        game.send(key, 0.8)
        game.expect(r"You (hit|miss) it")
        if "Really attack" in game.text():
            fail("a blind hero shouldn't be asked before hitting", game)
        print("ok: ... and then hits it without being asked (accidental)")
        game.quit()
    finally:
        try:
            os.kill(game.pid, 9)
        except OSError:
            pass


if __name__ == "__main__":
    main()
