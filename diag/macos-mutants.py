#!/usr/bin/env python3
"""SCRATCH (Huly QB-1270 part 3), never to merge: the mutants of the lot's product lines, on the system and the OpenSSL
that decide them -- macOS for the shutdown of both directions, OpenSSL 4 for a read OpenSSL refuses. One build
directory, one plant at a time in the source, the plant asserted to have landed (the text occurs once), the witnesses
run, the source restored; the control C00 (nothing planted) first and last.

    diag/macos-mutants.py <logs directory> <build directory>   (the build directory configured already)
"""
import pathlib
import re
import subprocess
import sys

out = pathlib.Path(sys.argv[1]).resolve()
build = pathlib.Path(sys.argv[2]).resolve()
out.mkdir(parents=True, exist_ok=True)
jobs = subprocess.run(['sysctl', '-n', 'hw.logicalcpu'], capture_output=True, text=True).stdout.strip()
targets = ['qb-io-test-system-tls-close-notify', 'qb-io-test-system-tcp-socket-loopback']

NL = chr(10)
SOCKET = 'src/qb/io/system/sys__socket.cpp'
TLS = 'src/qb/io/tcp/ssl/socket.cpp'
READ_ELSE = NL + ' ' * 16 + 'else if (const char *state = SSL_state_string(ssl_handle()); state && std::string_view(state) == "SSLERR")' + NL
READ_SET = ' ' * 20 + 'qb::io::socket::set_last_errno(tls_session_aborted);' + NL
WRITE_ELSE = NL + ' ' * 20 + 'else if (const char *state = SSL_state_string(ssl_handle()); state && std::string_view(state) == "SSLERR")' + NL
WRITE_SET = ' ' * 24 + 'qb::io::socket::set_last_errno(tls_session_aborted);' + NL
RETURN = '        return ::shutdown(this->fd, SHUT_RDWR) == 0 ? 0 : ::shutdown(this->fd, SHUT_WR);' + NL
PLANTS = [
    # the fix taken out: XNU's SHUT_RDWR alone
    ('A1', SOCKET, '    if (how == SHUT_RDWR)' + NL, '    if (false)' + NL),
    # the receiving side alone where XNU refused: no FIN
    ('A2', SOCKET, RETURN, RETURN.replace('? 0 : ::shutdown(this->fd, SHUT_WR)', '? 0 : ::shutdown(this->fd, SHUT_RD)')),
    # each side on its own call, the sending side first (the lot's first draft): input unread, a reset
    ('A3', SOCKET, RETURN,
     '        { const int w = ::shutdown(this->fd, SHUT_WR); const int r = ::shutdown(this->fd, SHUT_RD); '
     'return w == 0 || r == 0 ? 0 : -1; }' + NL),
    # the fallback answers success whatever it did
    ('A4', SOCKET, RETURN, '        return ::shutdown(this->fd, SHUT_RDWR) == 0 ? 0 : (::shutdown(this->fd, SHUT_WR), 0);' + NL),
    # the fallback taken after a SHUT_RDWR that succeeded
    ('A5', SOCKET, RETURN, RETURN.replace('== 0 ? 0 :', '!= 0 ? 0 :')),
    # the read's report taken out: a socket error OpenSSL ended the session on reads as "try again"
    ('B1', TLS, READ_ELSE + READ_SET, NL),
    # ...or reported as a "try again" of its own
    ('B2', TLS, READ_ELSE + READ_SET, READ_ELSE + ' ' * 20 + 'qb::io::socket::set_last_errno(EAGAIN);' + NL),
    # the write's report taken out
    ('B3', TLS, WRITE_ELSE + WRITE_SET, NL),
    # ...or reported as a "try again" of its own
    ('B4', TLS, WRITE_ELSE + WRITE_SET, WRITE_ELSE + ' ' * 24 + 'qb::io::socket::set_last_errno(ENOBUFS);' + NL),
    # the error state read as in init on OpenSSL 4 (round 2's gate): a KeyUpdate answer reported aborted
    ('B5', TLS, WRITE_ELSE, NL + ' ' * 20 + 'else if (OPENSSL_version_major() >= 4 && SSL_in_init(ssl_handle()))' + NL),
    ('B6', TLS, READ_ELSE, NL + ' ' * 16 + 'else if (OPENSSL_version_major() >= 4 && SSL_in_init(ssl_handle()))' + NL),
    # the read's backstop taken out: a read OpenSSL refuses keeps a would-block an earlier call left
    ('B9', TLS, ' ' * 16 + 'if (err == SSL_ERROR_SSL && qb::io::socket::not_recv_error(qb::io::socket::get_last_errno()))' + NL,
     ' ' * 16 + 'if (false)' + NL),
]


def run(pid):
    log = out / f'mutant-{pid}-build.log'
    with open(log, 'w') as f:
        rc = subprocess.run(['cmake', '--build', str(build), '--parallel', jobs, '--target', *targets],
                            stdout=f, stderr=subprocess.STDOUT).returncode
    if rc:
        return None
    failed = set()
    for t in targets:
        txt = out / f'mutant-{pid}-{t}.txt'
        with open(txt, 'w') as f:
            subprocess.run([str(build / 'bin' / 'tests' / t), '--gtest_color=no', '--gtest_repeat=3'],
                           stdout=f, stderr=subprocess.STDOUT)
        failed |= set(re.findall(r'^\[  FAILED  \] (\w+\.\w+) \(', txt.read_text(errors='replace'), re.M))
    return sorted(failed)


def control(pid):
    failed = run(pid)
    print(f'{pid}: ' + ('BUILD-FAILED' if failed is None else 'GREEN' if not failed else 'RED ' + ' '.join(failed)), flush=True)


control('C00')
for pid, path, old, new in PLANTS:
    source = pathlib.Path(path)
    text = source.read_text()
    if text.count(old) != 1:
        print(f'{pid}: NOT PLANTED ({text.count(old)} occurrences of its text)', flush=True)
        continue
    source.write_text(text.replace(old, new))
    try:
        failed = run(pid)
    finally:
        source.write_text(text)
    verdict = 'BUILD-FAILED' if failed is None else ('CAUGHT by ' + ' '.join(failed)) if failed else 'SURVIVED'
    print(f'{pid}: {verdict}', flush=True)
control('C00-after')
print(subprocess.run(['git', 'status', '--short', 'src'], capture_output=True, text=True).stdout or 'src clean')
