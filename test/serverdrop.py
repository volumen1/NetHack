#!/usr/bin/env python3
"""Server test: a player's connection drops mid-game (milestone M2).

Usage: python3 test/serverdrop.py PLAYGROUND_DIR

Two players join; the second one's terminal hangs up.  When the game
next needs that player, it must end cleanly: the server exits normally,
no partial save is written, and the remaining player's nh-connect says
why the game ended.  Needs the 'pyte' module (pip install pyte).
"""

import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from server import Client, OPTIONS, fail, pump_all  # noqa: E402


def main():
    if len(sys.argv) != 2:
        fail(__doc__)
    playground = os.path.abspath(sys.argv[1])
    binary = os.path.join(playground, "nethack")
    nhconnect = os.path.join(playground, "nh-connect")
    savedir = os.path.join(playground, "save")
    tmp = tempfile.mkdtemp(prefix="nh-drop-")
    sock = os.path.join(tmp, "game.sock")
    tag = os.getpid() % 10000
    saves_before = set(os.listdir(savedir))

    env = dict(os.environ, NETHACK_SERVER=sock, NETHACK_HEROES="2",
               NETHACKOPTIONS=OPTIONS, HOME=tmp)
    server = subprocess.Popen([binary], env=env, stdin=subprocess.DEVNULL,
                              stdout=open(os.path.join(tmp, "log"), "w"),
                              stderr=subprocess.STDOUT)
    for _ in range(50):
        if os.path.exists(sock):
            break
        time.sleep(0.1)
    else:
        fail("server didn't create its socket")

    ann = Client(nhconnect, sock, "Ann%d" % tag)
    ann.pump(0.5)
    ben = Client(nhconnect, sock, "Ben%d" % tag)
    try:
        ann.expect(r"Shall I pick")
        ann.send("y")
        ann.expect(r"Is this ok\?")
        ann.send("y")
        ben.expect(r"Shall I pick")
        ben.send("y")
        ben.expect(r"Is this ok\?")
        ben.send("y")
        ann.expect(r"Dlvl:1")
        for _ in range(4):
            if "welcome to NetHack" in ann.text():
                break
            ann.send("\r", 0.5)
        ann.expect(r"welcome to NetHack")

        # Ben's session dies; then Ann moves, so the game needs Ben
        os.kill(ben.pid, 9)
        os.close(ben.fd)
        ann.act("ms")
        try:
            code = server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            fail("server didn't end the game after Ben dropped", ann)
        if code != 0:
            fail("server exited with status %d" % code, ann)
        print("ok: the game ended cleanly when a player dropped")

        ann.expect(r"Ben%d lost their connection" % tag)
        print("ok: the remaining player was told why")

        new_saves = set(os.listdir(savedir)) - saves_before
        if new_saves:
            fail("a partial save was written: %s" % ", ".join(new_saves))
        print("ok: no partial save")
    finally:
        if server.poll() is None:
            server.kill()
        for c in (ann, ben):
            try:
                os.kill(c.pid, 9)
            except OSError:
                pass


if __name__ == "__main__":
    main()
