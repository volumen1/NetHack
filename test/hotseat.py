#!/usr/bin/env python3
"""Hot-seat test: two heroes take turns on one terminal (milestone M1).

Usage: python3 test/hotseat.py PLAYGROUND_DIR

Starts a two-hero game, checks that the heroes alternate, that saving is
refused while there's more than one hero, and quits.  Exits 0 on success
and 1 on failure.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke import Game, fail  # noqa: E402

import tempfile  # noqa: E402


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    playground = os.path.abspath(sys.argv[1])
    binary = os.path.join(playground, "nethack")
    if not os.access(binary, os.X_OK):
        fail("no nethack binary in %s" % playground)
    home = tempfile.mkdtemp(prefix="nh-hotseat-")

    os.environ["NETHACK_HEROES"] = "2"
    game = Game(binary, home)
    game.expect(r"Player 2, what is your name\?")
    game.send("Bob\r")
    game.send(" ")   # dismiss the --More-- after the name
    game.expect(r"Shall I pick")
    game.send("y")
    game.expect(r"Is this ok\?")
    game.send("y")
    game.expect(r"Dlvl:1")
    for _ in range(4):  # the intro text and any --More-- before the welcome
        if "welcome to NetHack" in game.text():
            break
        game.send("\r", 0.5)
    game.expect(r"welcome to NetHack")
    print("ok: second hero created")

    # Each search passes the turn to the other hero.
    for turn in range(3):
        game.out = b""
        game.send("s", 0.5)
        game.expect(r"Bob, it is your turn")
        game.out = b""
        game.send("s", 0.5)
        game.expect(r"smoke\d+, it is your turn")
    print("ok: heroes alternate")

    game.out = b""
    game.send("S")
    game.expect(r"can't be saved yet")
    print("ok: saving refused with two heroes")

    game.send("#quit\r")
    game.expect(r"Really quit")
    game.send("y")
    for _ in range(10):  # step through the end-of-game prompts
        game.send("q", 0.2)
        game.send("\r", 0.2)
    game.wait_exit()
    print("ok: game quit")


if __name__ == "__main__":
    main()
