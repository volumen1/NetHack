#!/usr/bin/env python3
"""Smoke test: start nethack in a pseudo-terminal, move, save, restore, quit.

Usage: python3 test/smoke.py PLAYGROUND_DIR

PLAYGROUND_DIR is the installed game directory (the one holding the
'nethack' binary, e.g. ./playground after 'make install').  Exits 0 on
success and 1 on failure, printing the tail of the screen output.
"""

import os
import pty
import re
import select
import sys
import tempfile
import time

OPTIONS = ("windowtype:curses,name:smoke%d,role:valkyrie,race:human,"
           "gender:female,align:lawful,!autopickup,!tutorial,"
           "suppress_alert:5.0") % os.getpid()


class Game:
    def __init__(self, binary, home):
        self.out = b""
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ.update(TERM="xterm", HOME=home, NETHACKOPTIONS=OPTIONS,
                              LINES="24", COLUMNS="80")
            os.execv(binary, [binary])

    def text(self):
        raw = re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Z0-9]|\x1b[=>]",
                     b" ", self.out)
        return re.sub(r"\s+", " ", raw.decode("latin-1"))

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
                self.out += data

    def expect(self, pattern, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.text()):
                return True
            self.pump(0.1)
        fail("timed out waiting for %r" % pattern, self)

    def send(self, keys, settle=0.3):
        os.write(self.fd, keys.encode("latin-1"))
        self.pump(settle)

    def wait_exit(self, timeout=10):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.1)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                return status
        fail("game did not exit", self)


def fail(msg, game=None):
    print("FAIL:", msg)
    if game is not None:
        print("--- screen tail ---")
        print(game.text()[-2000:])
    sys.exit(1)


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    playground = os.path.abspath(sys.argv[1])
    binary = os.path.join(playground, "nethack")
    if not os.access(binary, os.X_OK):
        fail("no nethack binary in %s" % playground)
    home = tempfile.mkdtemp(prefix="nh-smoke-")

    # 1. New game: reach the map, take a step, save.
    game = Game(binary, home)
    game.expect(r"Dlvl:1")
    game.send("\r")  # dismiss the intro text
    game.expect(r"welcome to NetHack")
    game.send("s")   # search one turn
    game.send("S")
    game.expect(r"Really save\?")
    game.send("y")
    game.expect(r"Saving")
    for _ in range(3):  # dismiss the --More-- after saving
        game.send("\r")
    game.wait_exit()
    print("ok: new game started and saved")

    # 2. Restore the save, then quit for good.
    game = Game(binary, home)
    game.expect(r"Restoring save file")
    game.expect(r"welcome back")
    game.send("\r")
    game.send("#quit\r")
    game.expect(r"Really quit")
    game.send("y")
    for _ in range(10):  # step through the end-of-game prompts
        game.send("q", 0.2)
        game.send("\r", 0.2)
    game.wait_exit()
    print("ok: save restored and game quit")


if __name__ == "__main__":
    main()
