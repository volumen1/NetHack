#!/usr/bin/env python3
"""Server test: two players join with nh-connect, each on their own
terminal (milestone M2).

Usage: python3 test/server.py PLAYGROUND_DIR

Starts the game in server mode, connects two nh-connect clients on
separate pseudo-terminals, has both pick characters, checks that each
player sees their own screen, that turns alternate, and that the waiting
player is told whose turn it is.  Exits 0 on success and 1 on failure.
Needs the 'pyte' terminal emulator (pip install pyte).
"""

import os
import pty
import re
import select
import subprocess
import sys
import tempfile
import time

try:
    import pyte
except ImportError:
    print("SKIP: this test needs pyte (pip install pyte)")
    sys.exit(0)

OPTIONS = "windowtype:curses,!autopickup,!tutorial,suppress_alert:5.0"


class Client:
    """One player's terminal running nh-connect."""

    def __init__(self, nhconnect, sock, name):
        self.name = name
        ALL_CLIENTS.append(self)
        self.screen = pyte.Screen(80, 24)
        self.stream = pyte.ByteStream(self.screen)
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ.update(TERM="xterm", LINES="24", COLUMNS="80")
            os.execv(nhconnect, [nhconnect, "-s", sock, name])

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

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def expect(self, pattern, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.text()):
                return
            self.pump(0.1)
        fail("%s: timed out waiting for %r" % (self.name, pattern), self)

    def send(self, keys, settle=0.4):
        os.write(self.fd, keys.encode("latin-1"))
        self.pump(settle)

    def seen(self, pattern, timeout=15):
        """Wait for pattern, dismissing More>> prompts along the way.
        Returns whether it showed up."""
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.text()):
                return True
            if ">>" in self.screen.display[0]:
                self.send("\r", 0.3)
            self.pump(0.1)
        return False

    def expect_past_more(self, pattern, timeout=15):
        """Like expect(), but dismiss More>> prompts along the way."""
        if not self.seen(pattern, timeout):
            fail("%s: timed out waiting for %r" % (self.name, pattern), self)

    def act(self, keys, settle=0.6):
        """Send a command, first dismissing any pending More>>."""
        for _ in range(10):
            self.pump(0.2)
            if ">>" not in self.screen.display[0]:
                break
            self.send("\r", 0.3)
        self.send(keys, settle)


ALL_CLIENTS = []


def fail(msg, client=None):
    print("FAIL:", msg)
    for c in ALL_CLIENTS or ([client] if client else []):
        print("--- %s's screen ---" % c.name)
        print(c.text())
    sys.exit(1)


def pass_turn(actor, other, clients, tries=5):
    """actor searches until it is other's turn."""
    for _ in range(tries):
        actor.act("ms")
        pump_all(clients, 0.5)
        if other.seen(r"It is your turn", 4):
            return
    fail("the turn never passed from %s to %s" % (actor.name, other.name))


def pump_all(clients, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for c in clients:
            c.pump(0.05)


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    playground = os.path.abspath(sys.argv[1])
    binary = os.path.join(playground, "nethack")
    nhconnect = os.path.join(playground, "nh-connect")
    for prog in (binary, nhconnect):
        if not os.access(prog, os.X_OK):
            fail("missing %s" % prog)
    tmp = tempfile.mkdtemp(prefix="nh-server-")
    sock = os.path.join(tmp, "game.sock")
    tag = os.getpid() % 10000

    env = dict(os.environ, NETHACK_SERVER=sock, NETHACK_HEROES="2",
               NETHACKOPTIONS=OPTIONS, HOME=tmp)
    log = open(os.path.join(tmp, "server.log"), "w")
    server = subprocess.Popen([binary], env=env, stdin=subprocess.DEVNULL,
                              stdout=log, stderr=subprocess.STDOUT)
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    else:
        fail("server didn't create its socket")

    alice = Client(nhconnect, sock, "Ann%d" % tag)
    alice.pump(0.5)
    bob = Client(nhconnect, sock, "Ben%d" % tag)
    clients = [alice, bob]

    try:
        # each player picks a random character on their own terminal
        alice.expect(r"Shall I pick")
        alice.send("y")
        alice.expect(r"Is this ok\?")
        alice.send("y")
        bob.expect(r"Shall I pick")
        bob.send("y")
        bob.expect(r"Is this ok\?")
        bob.send("y")
        alice.expect(r"Dlvl:1")
        for _ in range(4):  # intro text and --More-- prompts
            if "welcome to NetHack" in alice.text():
                break
            alice.send("\r", 0.5)
        alice.expect(r"welcome to NetHack")
        print("ok: both players created characters on their own terminals")

        alice.expect(r"Ann%d the" % tag)
        bob.expect(r"Ben%d the" % tag)
        print("ok: each status line shows that player's own hero")

        # 'ms' (forced search) always takes a turn; plain 's' doesn't when
        # a monster is already known to be adjacent
        # (a fast hero can get two moves in a turn, so act until it passes)
        for _ in range(3):
            pass_turn(alice, bob, clients)
            alice.expect(r"It is Ben%d's turn" % tag)
            pass_turn(bob, alice, clients)
            bob.expect(r"It is Ann%d's turn" % tag)
        print("ok: turns alternate and the waiting player is told whose")

        # each map shows both heroes
        for c in clients:
            body = "\n".join(c.screen.display[1:22])
            if body.count("@") < 2:
                fail("%s's map doesn't show both heroes" % c.name, c)
        print("ok: both maps show both heroes")

        alice.act("#quit\r")
        alice.expect(r"Really quit")
        alice.send("y")
        for _ in range(12):
            alice.send("q", 0.2)
            alice.send("\r", 0.2)
            if server.poll() is not None:
                break
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            fail("server didn't exit after quitting", alice)
        pump_all(clients, 1.0)
        print("ok: game ended and the server exited")

        # nh-connect notices the end of the game and gives the terminal back
        for c in clients:
            for _ in range(50):
                pid, _ = os.waitpid(c.pid, os.WNOHANG)
                if pid:
                    break
                c.pump(0.1)
            else:
                fail("%s's nh-connect didn't exit" % c.name, c)
        print("ok: both nh-connect clients exited")
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
