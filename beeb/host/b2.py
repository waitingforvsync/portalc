"""b2.py - drive b2 through its HTTP API, on 127.0.0.1:48075: peek, reset
into a config, boot a disc.

A b2 that's already running is used as it is. With none, private() starts
one with no window, at full speed and in a config folder of its own (so
your b2 settings are never touched), for the caller to stop().
"""

import os
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = 'http://127.0.0.1:48075'
WIN = 'b2'
MASTER = 'Master 128 (MOS 3.20)'
B2 = '/snap/bin/b2'
CONFIG_FOLDER = os.path.expanduser('~/snap/b2/common/portalc-b2')


class B2Error(Exception):
    pass


def request(path, body=None, ctype=None, timeout=30):
    req = urllib.request.Request(BASE + path, data=body, method='POST' if body is not None else 'GET')
    if ctype:
        req.add_header('Content-Type', ctype)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        raise B2Error('%s: HTTP %d %s' % (path, e.code, e.read().decode(errors='replace').strip()))


def alive():
    try:
        request('/peek/%s/0/+1' % WIN, timeout=2)
        return True
    except (OSError, B2Error):
        return False


def peek(addr, n):
    data = request('/peek/%s/%x/+%d' % (WIN, addr, n))
    if len(data) != n:
        raise B2Error('peek &%X+%d gave %d bytes' % (addr, n, len(data)))
    return data


def reset(config=MASTER):
    request('/reset/%s?config=%s' % (WIN, urllib.parse.quote(config, safe='')))


def boot(disc):
    """Mount the disc in drive 0 and boot it, SHIFT held."""
    with open(disc, 'rb') as f:
        image = f.read()
    request('/run/%s?name=%s' % (WIN, urllib.parse.quote(os.path.basename(disc))),
            body=image, ctype='application/x.acorn.disc-image.ssd')


def private(config=MASTER, timeout=60):
    """Start a b2 of our own; it answers before this returns."""
    env = dict(os.environ, SDL_VIDEODRIVER='offscreen', SDL_AUDIODRIVER='dummy')
    proc = subprocess.Popen([B2, '--config-folder=' + CONFIG_FOLDER, '--config=' + config,
                             '--no-limit-speed'],
                            env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + timeout
    while not alive():
        if proc.poll() is not None or time.time() > end:
            proc.kill()
            raise B2Error('b2 did not start')
        time.sleep(0.25)
    return proc


def stop(proc):
    if proc is not None:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
