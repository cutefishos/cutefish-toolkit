#!/usr/bin/python3
# SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
# SPDX-License-Identifier: BSD-3-Clause

# Runs a test where it cannot reach the desktop session it runs beside.
#
#   isolate.py [--keep PATH]... [--watch PATH]... [--record FILE] -- COMMAND...
#
# The command runs in user, mount and PID namespaces of its own: /run/user and
# /tmp are empty (no session bus, Wayland display, X server, Mach broker,
# keyring or sound server), HOME, CFFIXED_USER_HOME and XDG_RUNTIME_DIR are
# fresh, and the account's home is an empty tmpfs that holds only the
# directories of the kept paths, so what a program writes into the account's
# home goes nowhere. Every process the command starts dies with it, also when
# the launcher itself is killed. A write into the account's home under a
# watched path (relative to the home) fails the run and is appended to the
# record file.
#
#   isolate.py --start DIRECTORY
#
# Empties the directory the record and the --since file below live in, and
# makes the --since file.
#
#   isolate.py --check FILE --since FILE [--ctest CTEST] ROOT
#
# Fails when the record file names a write, when a process started after
# the --since file was made outlived its test (an orphan whose command line,
# executable or working directory is under ROOT, or an orphaned private
# session bus and what it started), or, given --ctest, when a test of the
# build in ROOT does not run through this launcher.

import ctypes
import json
import os
import pwd
import select
import shutil
import signal
import subprocess
import sys
import time

CLONE_NEWNS = 0x00020000
CLONE_NEWUSER = 0x10000000
CLONE_NEWPID = 0x20000000
MS_NOSUID = 0x2
MS_NODEV = 0x4
MS_NOEXEC = 0x8
MS_REC = 0x4000
MS_BIND = 0x1000
MS_PRIVATE = 1 << 18
MNT_DETACH = 2
PR_SET_PDEATHSIG = 1

libc = ctypes.CDLL(None, use_errno=True)

# Names that would lead a program back to the session or the account's own
# directories.
DROPPED = ('DBUS_SESSION_BUS_ADDRESS', 'DBUS_STARTER_ADDRESS', 'DBUS_STARTER_BUS_TYPE', 'WAYLAND_DISPLAY',
           'WAYLAND_SOCKET', 'DISPLAY', 'XAUTHORITY', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME', 'XDG_DATA_HOME',
           'XDG_STATE_HOME', 'PIPEWIRE_RUNTIME_DIR', 'PIPEWIRE_REMOTE', 'SSH_AUTH_SOCK',
           'GNOME_KEYRING_CONTROL')


def fail(message):
    print('isolate: ' + message, file=sys.stderr, flush=True)
    sys.exit(125)


def mount(source, target, kind, flags, data=None):
    if libc.mount(source and source.encode(), target.encode(), kind and kind.encode(), flags,
                  data and data.encode()) != 0:
        error = ctypes.get_errno()
        fail('cannot mount %s on %s: %s' % (kind or source, target, os.strerror(error)))


def write(path, text):
    with open(path, 'w') as file:
        file.write(text)


def hide_home(home, kept):
    """Puts an empty tmpfs over the home with the kept paths' top directories
    bound back into it, and returns their names."""
    names = sorted({path[len(home) + 1:].split('/')[0] for path in kept if path.startswith(home + '/')})
    names = [name for name in names if os.path.isdir(os.path.join(home, name))]
    staging = '/run/user/.kept'
    os.mkdir(staging, 0o700)
    for name in names:
        os.mkdir(os.path.join(staging, name))
        mount(os.path.join(home, name), os.path.join(staging, name), None, MS_BIND | MS_REC)
    mount('none', home, 'tmpfs', MS_NOSUID | MS_NODEV, 'mode=%o' % (os.stat(home).st_mode & 0o7777))
    for name in names:
        os.mkdir(os.path.join(home, name))
        mount(os.path.join(staging, name), os.path.join(home, name), None, MS_BIND | MS_REC)
        libc.umount2(os.path.join(staging, name).encode(), MNT_DETACH)
        os.rmdir(os.path.join(staging, name))
    os.rmdir(staging)
    return names


def written(home, kept, watched):
    found = []
    for root, folders, files in os.walk(home):
        if root == home:
            folders[:] = [name for name in folders if name not in kept]
        for name in folders + files:
            path = os.path.join(root, name)
            relative = path[len(home) + 1:]
            if any(relative == watch or relative.startswith(watch + '/') for watch in watched):
                found.append(path)
    return found


def run(argv):
    kept = []
    watched = []
    record = None
    while argv and argv[0] != '--':
        if argv[0] == '--keep' and len(argv) > 1:
            kept.append(os.path.realpath(argv[1]))
            argv = argv[2:]
        elif argv[0] == '--watch' and len(argv) > 1:
            watched.append(argv[1].strip('/'))
            argv = argv[2:]
        elif argv[0] == '--record' and len(argv) > 1:
            record = os.path.realpath(argv[1])
            kept.append(os.path.dirname(record))
            argv = argv[2:]
        else:
            fail('unknown argument %s' % argv[0])
    command = argv[1:]
    if not command:
        fail('no command')
    uid = os.getuid()
    gid = os.getgid()
    home = os.path.realpath(pwd.getpwuid(uid).pw_dir)
    here = os.path.realpath(os.getcwd())
    if here != home:
        kept.append(here)
    if home in kept or home == '/':
        fail('the account home %s cannot be kept whole' % home)

    try:
        os.unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID)
    except OSError as error:
        fail('cannot make namespaces for the test (%s); unprivileged user namespaces are needed' % error)
    write('/proc/self/setgroups', 'deny')
    write('/proc/self/uid_map', '%d %d 1' % (uid, uid))
    write('/proc/self/gid_map', '%d %d 1' % (gid, gid))
    mount(None, '/', None, MS_REC | MS_PRIVATE)
    mount('none', '/run/user', 'tmpfs', MS_NOSUID | MS_NODEV, 'mode=755')
    runtime = '/run/user/%d' % uid
    os.mkdir(runtime, 0o700)
    scratch = runtime + '/home'
    os.mkdir(scratch, 0o700)
    mount('none', '/tmp', 'tmpfs', MS_NOSUID | MS_NODEV, 'mode=1777')
    kept = hide_home(home, kept)

    environment = {name: value for name, value in os.environ.items() if name not in DROPPED}
    environment.update(HOME=scratch, CFFIXED_USER_HOME=scratch, XDG_RUNTIME_DIR=runtime)

    # The launcher holds the pipe's write end, so its end shows as end of file;
    # the parent's PID is not visible from inside the new namespace.
    alive, holder = os.pipe()
    init = os.fork()
    if init == 0:
        os.close(holder)
        libc.prctl(PR_SET_PDEATHSIG, signal.SIGKILL)
        if select.select([alive], [], [], 0)[0]:
            os._exit(125)
        os.close(alive)
        mount('proc', '/proc', 'proc', MS_NOSUID | MS_NODEV | MS_NOEXEC)
        os._exit(supervise(command, environment, (home, kept, watched), record))
    os.close(alive)

    def forward(number, frame):
        try:
            os.kill(init, number)
        except ProcessLookupError:
            pass
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGQUIT):
        signal.signal(number, forward)
    while True:
        try:
            _, status = os.waitpid(init, 0)
            break
        except ChildProcessError:
            return 125
    return os.waitstatus_to_exitcode(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status)


def supervise(command, environment, view, record):
    """Runs as the namespace's first process: its exit ends every other one."""
    main = os.fork()
    if main == 0:
        for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGQUIT):
            signal.signal(number, signal.SIG_DFL)
        try:
            os.execvpe(command[0], command, environment)
        except OSError as error:
            print('isolate: cannot run %s: %s' % (command[0], error), file=sys.stderr, flush=True)
            os._exit(127)

    def forward(number, frame):
        try:
            os.kill(main, number)
        except ProcessLookupError:
            pass
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGQUIT):
        signal.signal(number, forward)

    code = 125
    while True:
        try:
            pid, status = os.waitpid(-1, 0)
        except ChildProcessError:
            break
        if pid == main:
            code = os.waitstatus_to_exitcode(status)
            if code < 0:
                code = 128 - code
            break

    # Helpers the test left (an activated service, a forked child) end with
    # it: asked first, then the namespace's end kills what is left.
    try:
        os.kill(-1, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            if os.waitpid(-1, os.WNOHANG) == (0, 0):
                time.sleep(0.02)
        except ChildProcessError:
            break

    found = written(*view)
    if found:
        lines = ['wrote the account home: %s (%s)' % (path, ' '.join(command)) for path in found]
        for line in lines:
            print('isolate: ' + line, file=sys.stderr, flush=True)
        if record:
            with open(record, 'a') as file:
                file.write(''.join(line + '\n' for line in lines))
        if code == 0:
            code = 1
    return code


def processes():
    for name in os.listdir('/proc'):
        if name.isdigit():
            yield int(name)


def read(path):
    try:
        with open(path, 'rb') as file:
            return file.read()
    except OSError:
        return b''


def status_of(pid):
    """Parent and start time (clock ticks after boot) of a process."""
    text = read('/proc/%d/stat' % pid).decode(errors='replace')
    fields = text[text.rfind(')') + 2:].split()
    if len(fields) < 20:
        return None
    return int(fields[1]), int(fields[19])


def outside(ctest, root):
    """The tests of the build in root whose command is not this launcher's."""
    listing = subprocess.run([ctest, '--show-only=json-v1'], cwd=root, capture_output=True, text=True)
    if listing.returncode != 0:
        return ['cannot list the tests: ' + listing.stderr.strip()]
    launcher = os.path.realpath(__file__)
    problems = []
    for test in json.loads(listing.stdout).get('tests', []):
        command = test.get('command')
        if not command:
            continue
        if len(command) < 2 or os.path.realpath(command[1]) != launcher:
            problems.append('runs outside the launcher: %s' % test.get('name'))
    return problems


def check(argv):
    ctest = None
    if len(argv) == 7 and argv[4] == '--ctest':
        ctest = argv[5]
        del argv[4:6]
    if len(argv) != 5 or argv[0] != '--check' or argv[2] != '--since':
        fail('usage: isolate.py --check FILE --since FILE [--ctest CTEST] ROOT')
    record, since, root = argv[1], argv[3], os.path.realpath(argv[4])
    problems = []
    if ctest:
        problems += outside(ctest, root)
    if os.path.exists(record):
        problems += [line for line in read(record).decode(errors='replace').splitlines() if line]

    uid = os.getuid()
    ticks = os.sysconf('SC_CLK_TCK')
    with open('/proc/uptime') as file:
        uptime = float(file.read().split()[0])
    try:
        started = (os.stat(since).st_mtime - (time.time() - uptime)) * ticks
    except OSError:
        started = 0
    ancestors = set()
    pid = os.getpid()
    while pid > 1:
        ancestors.add(pid)
        status = status_of(pid)
        if not status:
            break
        pid = status[0]

    table = {}
    for pid in processes():
        try:
            if os.stat('/proc/%d' % pid).st_uid != uid:
                continue
        except OSError:
            continue
        status = status_of(pid)
        if status:
            table[pid] = status
    # The user's service manager adopts orphans in place of the first process.
    managers = {pid for pid in table if read('/proc/%d/comm' % pid).strip() == b'systemd'}

    def orphaned(pid):
        return table[pid][0] in managers or table[pid][0] == 1

    buses = set()
    for pid, (parent, start) in table.items():
        if pid in ancestors or start < started:
            continue
        arguments = read('/proc/%d/cmdline' % pid).split(b'\0')
        line = b' '.join(arguments).decode(errors='replace')
        if (orphaned(pid) and os.path.basename(arguments[0]) == b'dbus-daemon'
                and any(argument.startswith(b'--print-address') for argument in arguments)):
            buses.add(pid)
            problems.append('left a private session bus: %d %s' % (pid, line))
            continue
        try:
            places = [os.readlink('/proc/%d/exe' % pid), os.readlink('/proc/%d/cwd' % pid)]
        except OSError:
            places = []
        under = any(place == root or place.startswith(root + '/') for place in places) or root in line
        if orphaned(pid) and under:
            problems.append('left a process: %d %s' % (pid, line))
    for pid, (parent, start) in table.items():
        if parent in buses:
            line = b' '.join(read('/proc/%d/cmdline' % pid).split(b'\0')).decode(errors='replace')
            problems.append('left a process on a private session bus: %d %s' % (pid, line))

    for problem in problems:
        print(problem)
    if problems:
        return 1
    print('isolation held')
    return 0


def start(argv):
    if len(argv) != 2:
        fail('usage: isolate.py --start DIRECTORY')
    shutil.rmtree(argv[1], ignore_errors=True)
    os.makedirs(argv[1])
    open(os.path.join(argv[1], 'since'), 'w').close()
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--check':
        sys.exit(check(sys.argv[1:]))
    if len(sys.argv) > 1 and sys.argv[1] == '--start':
        sys.exit(start(sys.argv[1:]))
    sys.exit(run(sys.argv[1:]))
