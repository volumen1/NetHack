#!/usr/bin/env python3
"""Ray and explosion test: they hit the other hero too (milestone M3).

Usage: python3 test/blasts.py PLAYGROUND_DIR

Hot-seat debug-mode game with two heroes next to each other.  The first
hero zaps a sleep ray at the second, then breaks a wand of cold next to
them.  In hot-seat play, messages about the hero whose turn it isn't are
labelled with their name ("Bob: ..."), which is how the test tells that
the second hero was the one affected.  Needs pyte (pip install pyte).
"""

import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import allies  # noqa: E402
from allies import Game, direction, fail  # noqa: E402


def wish(game, item):
    """Wish for item; return its inventory letter."""
    game.settle()
    game.send("\x17", 0.5)
    game.send(item + "\r", 0.8)
    game.seen(r"\b[a-zA-Z] - ")
    m = re.search(r"\b([a-zA-Z]) - [^.]*\bwand\b", game.text())
    if not m:
        fail("wish for %s didn't work" % item, game)
    game.settle()
    return m.group(1)


def first_heros_turn(game):
    for _ in range(10):
        game.settle()
        if game.whose_turn() == allies.NAME:
            return
        game.send("ms", 0.8)
    fail("never got back to the first hero", game)


def output(game):
    return bytes(game.raw).decode("latin-1")


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    binary = os.path.join(os.path.abspath(sys.argv[1]), "nethack")
    game = Game(binary, tempfile.mkdtemp(prefix="nh-blasts-"), ["-D"])
    allies.NAME = "wizard"  # debug mode renames the first hero
    try:
        game.start()
        me, bob = game.heroes()
        key = direction(me, bob)
        if not key:
            fail("heroes don't start next to each other", game)

        # 1. a sleep ray toward Bob affects Bob
        first_heros_turn(game)
        wand = wish(game, "wand of sleep")
        first_heros_turn(game)
        me, bob = game.heroes()
        key = direction(me, bob)
        start = len(game.raw)
        game.send("z" + wand, 0.6)
        game.send(key, 1.5)
        game.settle()
        text = output(game)[start:]
        if not re.search(r"Bob: (The sleep ray (hits|whizzes by) you"
                         r"|But it reflects)", text):
            fail("the sleep ray didn't reach Bob", game)
        print("ok: a ray reaches the other hero")

        # 2. breaking a wand of cold next to Bob catches Bob in the blast
        first_heros_turn(game)
        wand = wish(game, "wand of cold (0:2)")
        first_heros_turn(game)
        me, bob = game.heroes()
        if not direction(me, bob):
            fail("Bob isn't next to the first hero any more", game)
        start = len(game.raw)
        game.send("a" + wand, 0.6)
        game.seen(r"really sure you want to break")
        game.send("y", 1.5)
        game.settle()
        text = output(game)[start:]
        if not re.search(r"Bob: You are caught in", text):
            fail("Bob wasn't caught in the explosion", game)
        print("ok: an explosion catches the other hero")
        game.quit()
    finally:
        try:
            os.kill(game.pid, 9)
        except OSError:
            pass


if __name__ == "__main__":
    main()
