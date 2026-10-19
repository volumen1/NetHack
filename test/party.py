#!/usr/bin/env python3
"""Party test: observer messages, #party and badly-hurt alerts (M4).

Usage: python3 test/party.py PLAYGROUND_DIR

Server-mode game in debug mode (the first player is named "wizard"), two
players on their own terminals, starting next to each other, taking turns
(NETHACK_TURNS=strict, so that what each player sees is predictable):
  - the first player prays; the other sees "wizard prays.";
  - the other player opens #party, which lists both heroes and takes no
    time;
  - that player sets their hit points to 1 (#wizsethp) and waits a turn;
    the first player is told they're badly hurt;
  - the first player creates a lichen and attacks it; the other sees it.
Needs pyte (pip install pyte), and the user running it must be in
WIZARDS in the playground's sysconf.
"""

import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server import (OPTIONS, YOUR_TURN, Client, fail, pass_turn,  # noqa: E402
                    pump_all)


DIRS = {(-1, 0): "h", (1, 0): "l", (0, -1): "k", (0, 1): "j",
        (-1, -1): "y", (1, -1): "u", (-1, 1): "b", (1, 1): "n"}


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    playground = os.path.abspath(sys.argv[1])
    binary = os.path.join(playground, "nethack")
    nhconnect = os.path.join(playground, "nh-connect")
    tmp = tempfile.mkdtemp(prefix="nh-party-")
    sock = os.path.join(tmp, "game.sock")
    ben_name = "Ben%d" % (os.getpid() % 10000)

    env = dict(os.environ, NETHACK_SERVER=sock, NETHACK_HEROES="2",
               NETHACK_TURNS="strict", NETHACKOPTIONS=OPTIONS, HOME=tmp)
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
        wiz.expect(r"Shall I pick")
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
        for _ in range(4):
            if "welcome to NetHack" in wiz.text():
                break
            wiz.send("\r", 0.5)
        wiz.expect(r"welcome to NetHack")

        # 1. the wizard prays where Ben can see
        mark = ben.mark()
        wiz.act("#pray\r")
        wiz.expect(r"Are you sure you want to pray")
        wiz.send("y", 1.0)
        pump_all(clients, 1.0)
        if not ben.seen_since(mark, r"wizard prays\."):
            fail("Ben didn't see the wizard pray", ben)
        print("ok: another player sees the hero pray")

        # wait for Ben's turn (prayer takes a few turns)
        for _ in range(10):
            pump_all(clients, 0.5)
            if ben.seen_since(mark, YOUR_TURN, 2):
                break
            wiz.act("\r")
        else:
            fail("never got to Ben's turn", ben)

        # 2. #party lists everyone, without using up Ben's turn
        ben.act("#party\r")
        ben.expect(r"Your party")
        text = ben.text()
        if ("* " + ben_name) not in text or "wizard" not in text:
            fail("#party doesn't list both heroes", ben)
        ben.send("\x1b", 0.5)
        pass_turn(ben, wiz, clients)  # still Ben's turn: the search happens
        print("ok: #party lists both heroes and takes no time")

        # 3. Ben drops to 1 HP; the wizard is told
        pass_turn(wiz, ben, clients)
        ben.act("#wizsethp\r")
        ben.expect(r"Set hit points to what")
        ben.send("1\r", 0.6)
        ben.expect_past_more(r"Your hit points are now 1")
        mark = wiz.mark()
        pass_turn(ben, wiz, clients)
        if not wiz.seen_since(mark, r"%s is badly hurt!" % ben_name):
            fail("the wizard wasn't told Ben is badly hurt", wiz)
        print("ok: the party is told when an ally is badly hurt")

        # 4. the wizard fights a lichen; Ben sees the blows
        attacked = False
        for _ in range(4):
            wiz.act("\x07")              # ^G, #wizgenesis
            wiz.seen(r"What monster to create", 3)
            wiz.send("lichen\r", 0.8)
            wiz.seen(r"(?!)", 0.5)
            cx, cy = wiz.screen.cursor.x, wiz.screen.cursor.y
            for (dx, dy), key in DIRS.items():
                x, y = cx + dx, cy + dy
                if 0 <= y < 24 and 0 <= x < 80 \
                        and wiz.screen.display[y][x] == "F":
                    mark = ben.mark()
                    wiz.act("F" + key)
                    attacked = True
                    break
            if attacked:
                break
            pass_turn(wiz, ben, clients)
            pass_turn(ben, wiz, clients)
        if not attacked:
            fail("couldn't find the lichen next to the wizard", wiz)
        pump_all(clients, 1.0)
        if not ben.seen_since(mark, r"wizard (hits|misses|kills) the lichen"):
            fail("Ben didn't see the wizard's attack", ben)
        print("ok: another player sees the hero's attack")

        # end the game properly, so no lock files are left behind; it's
        # usually Ben's turn after the wizard's attack, but a fast wizard
        # can get a second move, so whoever has the turn quits
        for quitter in (ben, wiz, ben):
            quitter.act("#quit\r")
            if quitter.seen(r"Really quit", 3):
                break
            quitter.send("\x1b", 0.5)
        else:
            fail("neither player could quit", ben)
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
