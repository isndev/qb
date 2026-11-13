#!/usr/bin/env python3
"""mutants.py -- lot T7-TLS part 4 (QB-1270): one plant at a time in a COPY of the candidate's qb sources, the plant
asserted to land exactly once, the catchers rebuilt and run, the copy restored. The control C00 (nothing planted) runs
first and last; a C00 that is not green stops the battery.

    python3 mutants.py <qb copy root> <build dir> <logs dir> [ids...]     (Linux / macOS; the build dir configured)
    python3 mutants.py --list

Verdict per plant: CAUGHT <cases> | CAUGHT-AT-COMPILE-TIME | SURVIVED | NOT-JUDGED.
"""
import os
import re
import shutil
import subprocess
import sys

SSL = 'src/qb/io/tcp/ssl/socket.cpp'
IOH = 'src/qb/io/async/io.h'

READ_TAIL = ('    _read_wants_write = true;\n'
             '    qb::io::socket::set_last_errno(EWOULDBLOCK);\n'
             '    return -1;\n'
             '}\n')
HS_GATE = '    } else if (ret != 0 || !SSL_want_write(ssl_handle())) {\n'
WANT_WRITE = '            case SSL_ERROR_WANT_WRITE:\n                break; // the read must write first'
SYS_GATE = '                if (!qb::io::socket::not_send_error(qb::io::socket::get_last_errno())) {\n                    quiet_shutdown();\n'
SYS_ERR = ('                if (const char *state = SSL_state_string(ssl_handle()); state && std::string_view(state) == "SSLERR") {\n'
           '                    qb::io::socket::set_last_errno(tls_session_aborted);\n'
           '                    return -1;\n'
           '                }\n'
           '                // Kept:')
SYS_KEPT = '                // ended a connection OpenSSL let go on.\n                break;\n'
CLEAR = '        _read_wants_write = false; // anything but data from here on'
W_WANT_WRITE = '                case SSL_ERROR_WANT_WRITE:\n                    _write_wants_read = false;\n'
W_WANT_READ = '                    _write_wants_read = true;\n                    return 0;\n'
W_HS = '        _write_wants_read = !SSL_want_write(ssl_handle());\n'

ARM = ('                    if constexpr (detail::tls_io<_Derived>) {\n'
       '                        if (Derived.transport().read_wants_write())\n'
       '                            watch_write_for_read();\n'
       '                    }\n')
IN_HOOK = ('            if (event._revents & EV_WRITE) {\n'
           '                read_after_write();\n'
           '                return;\n'
           '            }\n')
IN_AFTER = ('    read_after_write() noexcept {\n'
            '        this->_async_event.set(EV_READ);\n'
            '        this->_async_event.feed_event(EV_READ);\n'
            '    }\n')
WATCH_FN = '    watch_write_for_read() noexcept {\n'
WATCH = WATCH_FN + '        this->_async_event.set(EV_WRITE);\n'
HW_TEST = '        if (unlikely(raw_ret < (detail::tls_io<_Derived> ? 1 : 0))) {\n'
TOOK_GUARD = '        if (_reason || !_protocol->ok())\n            return false;\n        if (!Derived.pendingWrite()) {\n'
TOOK_EMPTY = ('            if (!Derived.transport().read_wants_write())\n'
              '                return false;\n'
              '            this->_async_event.set(EV_READ);\n'
              '            this->_async_event.feed_event(EV_READ);\n'
              '            return true;\n')
TOOK_PEER = ('        if (!Derived.transport().write_wants_read())\n'
             '            return false;\n'
             '        this->_async_event.set(EV_READ);\n')
REARM = ('                if ((this->_async_event.events & (EV_READ | EV_WRITE)) != (EV_READ | EV_WRITE))\n'
         '                    this->_async_event.set(EV_READ | EV_WRITE);\n')
OUT_TEST = '                if (unlikely(ret < (detail::tls_io<_Derived> ? 1 : 0))) {\n'
OUT_WAIT = ('                        if (ret == 0 && Derived.pendingWrite() && Derived.transport().write_wants_read()) {\n'
            '                            wait_read_for_write();\n')
OUT_HOOK = ('            if (event._revents & EV_READ) {\n'
            '                write_after_read();\n'
            '                return;\n'
            '            }\n')
OUT_WAIT_SET = '    wait_read_for_write() noexcept {\n        this->_async_event.set(EV_READ);\n'
OUT_AFTER = ('    write_after_read() noexcept {\n'
             '        this->_async_event.set(EV_WRITE);\n'
             '    }\n')

# id: (what, file, a, b, count): `a` must occur `count` times (default 1); every occurrence is replaced.
MUTANTS = {
    # ---- ssl::socket::read() -----------------------------------------------------------------------------------------
    'R01': ('WANT_WRITE answered 0, "nothing yet", as before', SSL, WANT_WRITE,
            '            case SSL_ERROR_WANT_WRITE:\n                return 0; // the read must write first', 1),
    'R02': ('a socket error the io layer retries keeps the base gate (ENOBUFS taken for a reset)', SSL, SYS_GATE,
            SYS_GATE.replace('not_send_error', 'not_recv_error'), 1),
    'R03': ('a kept socket error answered -1 as is, the read not marked as one that must write first', SSL, SYS_KEPT,
            '                // ended a connection OpenSSL let go on.\n                return -1;\n', 1),
    'R04': ('the read that must write first is not marked so', SSL, READ_TAIL,
            READ_TAIL.replace('_read_wants_write = true;', '_read_wants_write = false;'), 1),
    'R05': ('the read that must write first leaves the error it met (no would-block)', SSL, READ_TAIL,
            READ_TAIL.replace('    qb::io::socket::set_last_errno(EWOULDBLOCK);\n', ''), 1),
    'R06': ('a handshake flight the read drives is never one that must write first', SSL, HS_GATE,
            '    } else if (true) {\n', 1),
    'R07': ('OpenSSL\'s error state not read on a kept socket error (OpenSSL 4 reports nothing aborted)', SSL, SYS_ERR,
            '                // Kept:', 1),
    'R08': ('the mark not cleared when the read answers anything but data', SSL, CLEAR, '        // (mutant R08)', 1),
    # ---- ssl::socket::write() ----------------------------------------------------------------------------------------
    'W01': ('a write waiting for the peer is not marked so', SSL, W_WANT_READ,
            '                    _write_wants_read = false;\n                    return 0;\n', 1),
    'W02': ('a write refused by a full socket is marked as waiting for the peer', SSL, W_WANT_WRITE,
            W_WANT_WRITE.replace('= false;', '= true;'), 1),
    'W03': ('a handshake step a write drives always waits for the peer', SSL, W_HS,
            '        _write_wants_read = true;\n', 1),
    'W04': ('a handshake step a write drives never waits for the peer', SSL, W_HS,
            '        _write_wants_read = false;\n', 1),
    # ---- io.h: input<> and io<> read passes -------------------------------------------------------------------------
    'I01': ('input<>: its read pass never watches writability for a read', IOH, ARM, '', 2, 0),
    'I01b': ('io<>: its read pass never watches writability for a read', IOH, ARM, '', 2, 1),
    'I02': ('input<>: readability watched too (a read that waits to write)', IOH, WATCH, WATCH_FN + '        this->_async_event.set(EV_READ | EV_WRITE);\n', 2, 0),
    'I02b': ('io<>: readability watched too (a read that waits to write)', IOH, WATCH, WATCH_FN + '        this->_async_event.set(EV_READ | EV_WRITE);\n', 2, 1),
    'I03': ('input<>: readability alone watched (nothing armed)', IOH, WATCH, WATCH_FN + '        this->_async_event.set(EV_READ);\n', 2, 0),
    'I03b': ('io<>: readability alone watched (nothing armed)', IOH, WATCH, WATCH_FN + '        this->_async_event.set(EV_READ);\n', 2, 1),
    'I04': ('input<>: the writability a read asked for falls to the error path', IOH, IN_HOOK, '', 1),
    'I05': ('input<>: the read not made again when writability came', IOH, IN_AFTER,
            IN_AFTER.replace('        this->_async_event.feed_event(EV_READ);\n', ''), 1),
    'I06': ('io<>: a TLS write pass that took nothing not looked at (< 0 as before)', IOH, HW_TEST,
            '        if (unlikely(raw_ret < 0)) {\n', 1),
    'I07': ('io<>: the read not made again when the writability it asked for came', IOH, TOOK_EMPTY,
            TOOK_EMPTY.replace('            this->_async_event.feed_event(EV_READ);\n', ''), 1),
    'I08': ('io<>: every empty write pass makes a read, asked for or not', IOH, TOOK_EMPTY,
            TOOK_EMPTY.replace('            if (!Derived.transport().read_wants_write())\n                return false;\n', ''), 1),
    'I09': ('io<>: a connection being ended or drained is treated as any other', IOH, TOOK_GUARD,
            '        if (!Derived.pendingWrite()) {\n', 1),
    'I10': ('io<>: a write waiting for the peer keeps writability watched', IOH, TOOK_PEER,
            TOOK_PEER.replace('        this->_async_event.set(EV_READ);\n', ''), 1),
    'I11': ('io<>: a write pass that leaves output queued watches nothing more', IOH, REARM, '', 1),
    'I12': ('io<>: a write pass that leaves output queued adds writability alone (readability not watched again)', IOH, REARM,
            '                    this->ready_to_write();\n', 1),
    # ---- io.h: output<> ---------------------------------------------------------------------------------------------
    'O01': ('output<>: a TLS write that took nothing not looked at (< 0 as before)', IOH, OUT_TEST,
            '                if (unlikely(ret < 0)) {\n', 1),
    'O02': ('output<>: a write waiting for the peer keeps writability watched', IOH, OUT_WAIT_SET,
            '    wait_read_for_write() noexcept {\n', 1),
    'O03': ('output<>: the readability it asked for falls to the error path', IOH, OUT_HOOK, '', 1),
    'O04': ('output<>: writability not watched again when readability came', IOH, OUT_AFTER,
            OUT_AFTER.replace('        this->_async_event.set(EV_WRITE);\n', ''), 1),
}

CATCHERS = ['qb-io-test-system-tls-read-wants-write', 'qb-io-test-system-tls-close-notify',
            'qb-io-test-system-ssl-socket-loopback', 'qb-io-test-system-tls-text-roundtrip',
            'qb-io-test-system-close-after-peer-tls', 'qb-io-test-system-session-close-after-peer']


def log(msg):
    print(msg, flush=True)


def build(build_dir, logs, tag):
    path = os.path.join(logs, f'{tag}-build.log')
    with open(path, 'w') as f:
        rc = subprocess.run(['cmake', '--build', build_dir, '-j', os.environ.get('JOBS', '6'), '--target'] + CATCHERS,
                            stdout=f, stderr=subprocess.STDOUT).returncode
    return rc == 0


def run(build_dir, logs, tag):
    failed = []
    for t in CATCHERS:
        exe = None
        for root, _, files in os.walk(build_dir):
            if t in files and os.access(os.path.join(root, t), os.X_OK):
                exe = os.path.join(root, t)
                break
        if not exe:
            failed.append(t + '(missing)')
            continue
        path = os.path.join(logs, f'{tag}.{t}.log')
        with open(path, 'w') as f:
            try:
                rc = subprocess.run([exe, '--gtest_color=no'], cwd=os.path.dirname(exe), stdout=f, stderr=subprocess.STDOUT,
                                    timeout=240).returncode
            except subprocess.TimeoutExpired:
                rc = -999
        text = open(path, errors='replace').read()
        cases = sorted(set(re.findall(r'^\[  FAILED  \] ([A-Za-z0-9_]+\.[A-Za-z0-9_]+)', text, re.M)))
        if not cases and rc != 0:
            cases = [f'{t}(exit {rc})']
        failed += cases
    return failed


def main():
    if sys.argv[1:2] == ['--list']:
        for k, v in MUTANTS.items():
            print(f'{k}\t{v[0]}')
        return 0
    root, build_dir, logs = (os.path.abspath(a) for a in sys.argv[1:4])
    ids = sys.argv[4:] or list(MUTANTS)
    os.makedirs(logs, exist_ok=True)
    pristine = {}
    for f in {v[1] for v in MUTANTS.values()}:
        pristine[f] = open(os.path.join(root, f), encoding='utf-8', newline='').read()
    # Every plant must land in the pristine text before anything is run.
    for k in ids:
        what, f, a, b, count = MUTANTS[k][:5]
        n = pristine[f].count(a)
        if n != count:
            log(f'PLANT-DOES-NOT-LAND {k}: {n} occurrence(s) of its text, {count} expected')
            return 2

    def restore():
        for f, text in pristine.items():
            p = os.path.join(root, f)
            if open(p, encoding='utf-8', newline='').read() != text:
                open(p, 'w', encoding='utf-8', newline='').write(text)

    def control(tag):
        restore()
        if not build(build_dir, logs, tag):
            log(f'{tag}: BUILD FAILED')
            return False
        failed = run(build_dir, logs, tag)
        log(f'{tag}: {"GREEN" if not failed else "RED " + " ".join(failed)}')
        return not failed

    if not control('C00-first'):
        return 1
    tally = {'CAUGHT': 0, 'SURVIVED': 0, 'OTHER': 0}
    for k in ids:
        what, f, a, b, count = MUTANTS[k][:5]
        which = MUTANTS[k][5] if len(MUTANTS[k]) > 5 else None
        restore()
        p = os.path.join(root, f)
        if which is None:
            text = pristine[f].replace(a, b)
        else:
            at = -1
            for _ in range(which + 1):
                at = pristine[f].index(a, at + 1)
            text = pristine[f][:at] + b + pristine[f][at + len(a):]
        assert text != pristine[f]
        open(p, 'w', encoding='utf-8', newline='').write(text)
        if not build(build_dir, logs, k):
            log(f'{k} CAUGHT-AT-COMPILE-TIME (or NOT-JUDGED: read {k}-build.log) -- {what}')
            tally['OTHER'] += 1
            continue
        failed = run(build_dir, logs, k)
        if failed:
            tally['CAUGHT'] += 1
            log(f'{k} CAUGHT {" ".join(failed)} -- {what}')
        else:
            tally['SURVIVED'] += 1
            log(f'{k} SURVIVED -- {what}')
    ok = control('C00-last')
    log(f'planted {len(ids)}: caught {tally["CAUGHT"]}, survived {tally["SURVIVED"]}, other {tally["OTHER"]}; C00 last {"green" if ok else "RED"}')
    restore()
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
