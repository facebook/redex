#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


# pyre-strict


import enum
import itertools
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import typing


IS_WINDOWS: bool = os.name == "nt"


_BACKTRACE_PATTERN: typing.Pattern[str] = re.compile(
    r"^([^(]+)(?:\((.*)\))?\[(0x[0-9a-f]+)\]$"
)


_IGNORE_EXACT_LINES = {
    # No function or line information.
    "??",
    "??:0",
}
_IGNORE_LINES_WITH_SUBSTR = [
    # libc
    "libc.so.6(abort",
    "libc.so.6(gsignal",
    "libc.so.6(__libc_start_main",
    # libc without name
    "libc.so.6(+0x",
    # libstdc++
    "_ZN9__gnu_cxx27__verbose_terminate_handlerEv",
    "_ZSt9terminatev",
    "libstdc++.so.6(__cxa_throw",
    # libstdc++ without name
    "libstdc++.so.6(+0x",
    # Redex crash handler
    "redex-all(_Z23debug_backtrace_handleri",
]


def _should_skip_line(line: str) -> bool:
    global _IGNORE_EXACT_LINES, _IGNORE_LINES_WITH_SUBSTR
    if line in _IGNORE_EXACT_LINES:
        return True
    if any(item in line for item in _IGNORE_LINES_WITH_SUBSTR):
        return True
    return False


ADDR2LINE_PATH: typing.Optional[str] = None


def set_addr2line_path(path: str) -> None:
    global ADDR2LINE_PATH
    ADDR2LINE_PATH = path


# Check whether addr2line is available
def _has_addr2line() -> bool:
    try:
        global ADDR2LINE_PATH
        path = ADDR2LINE_PATH
        if path:
            if os.path.exists(path) or shutil.which(path):
                return True

        subprocess.check_call(
            ["addr2line", "-v"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        ADDR2LINE_PATH = "addr2line"
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


_ADDR2LINE_BASE_ARGS = ["-f", "-i", "-C", "-e"]


def _symbolize(filename: str, offset: str) -> typing.List[str]:
    # It's good enough not to use server mode.
    try:
        path = ADDR2LINE_PATH
        assert path
        output = subprocess.check_output(
            [path] + _ADDR2LINE_BASE_ARGS + [filename, offset]
        )
        return output.decode(sys.stderr.encoding).splitlines()
    except subprocess.CalledProcessError:
        return ["<addr2line error>"]


def maybe_addr2line(lines: typing.Iterable[str]) -> typing.Optional[typing.List[str]]:
    global _BACKTRACE_PATTERN

    # Generate backtrace lines.
    def find_matches() -> typing.Generator[typing.Match[str], None, None]:
        for line in lines:
            stripped_line = line.strip()
            m = _BACKTRACE_PATTERN.fullmatch(stripped_line)
            if m is not None:
                yield m

    # Check whether there is anything to do.
    matches_gen = find_matches()
    first_elem = next(matches_gen, None)
    if first_elem is None:
        return None
    matches_gen = itertools.chain([first_elem], matches_gen)

    if not _has_addr2line():
        sys.stderr.write("Addr2line not found!\n")
        return None
    ret = []

    for m in matches_gen:
        if _should_skip_line(m.string):
            continue

        ret.append("%s(%s)[%s]" % (m.group(1), m.group(2), m.group(3)))
        decoded = _symbolize(m.group(1), m.group(3))
        for idx, line in enumerate(decoded):
            line = line.strip()
            if _should_skip_line(line):
                continue

            ret.append(f'{"  " * (1 if idx % 2 == 0 else 2)}{line}')

    return ret


def find_abort_error(lines: typing.Iterable[str]) -> typing.Optional[str]:
    terminate_lines = []
    for line in lines:
        stripped_line = line.rstrip()

        if stripped_line.startswith("terminate called"):
            terminate_lines.append(stripped_line)
            continue

        if len(terminate_lines) > 0:
            terminate_lines.append(stripped_line)

            # Stop on ten lines.
            if len(terminate_lines) >= 10:
                break
            continue

    if not terminate_lines:
        return None

    if len(terminate_lines) >= 3:
        # Try to find the first line matching a backtrace.
        backtrace_idx = None
        global _BACKTRACE_PATTERN
        for i in range(2, len(terminate_lines)):
            m = _BACKTRACE_PATTERN.fullmatch(terminate_lines[i])
            if m is not None:
                backtrace_idx = i
                break

        if backtrace_idx:
            terminate_lines = terminate_lines[:backtrace_idx]
        else:
            # Probably not one of ours, or with a very detailed error, just
            # print two lines.
            terminate_lines = terminate_lines[0:2]

    # Remove trailing newlines.
    while terminate_lines:
        if terminate_lines[-1] == "":
            terminate_lines.pop()
        else:
            break

    # Add space to offset.
    return "\n".join(" " + line for line in terminate_lines)


# pyre-fixme[24] # Cannot express the generics required pre-3.9
PopenType = subprocess.Popen


def run_and_stream_stderr(
    args: typing.List[str],
    env: typing.Dict[str, str],
    pass_fds: typing.Optional[typing.List[int]],
) -> typing.Tuple[
    PopenType,
    typing.Callable[
        [typing.Optional[typing.Callable[[str], str]]],
        typing.Tuple[int, typing.List[str]],
    ],
]:
    if IS_WINDOWS:
        # Windows does not support `pass_fds` parameter.
        proc: PopenType = subprocess.Popen(args, env=env, stderr=subprocess.PIPE)
    else:
        proc: PopenType = subprocess.Popen(
            args,
            env=env,
            # pyre-fixme[6]: For 3rd argument expected `Collection[int]` but got
            #  `Optional[List[int]]`.
            pass_fds=pass_fds,
            stderr=subprocess.PIPE,
        )

    def stream_and_return(
        line_handler: typing.Optional[typing.Callable[[str], str]] = None
    ) -> typing.Tuple[int, typing.List[str]]:
        err_out: typing.List[str] = []
        # Copy and stash the output.
        stderr = proc.stderr
        assert stderr is not None
        for line in stderr:
            try:
                str_line = line.decode(sys.stdout.encoding)
            except UnicodeDecodeError:
                str_line = "<UnicodeDecodeError>\n"
            if line_handler:
                str_line = line_handler(str_line)
            sys.stderr.write(str_line)
            err_out.append(str_line)
            if len(err_out) > 1000:
                err_out = err_out[100:]

        returncode = proc.wait()

        return (returncode, err_out)

    return (proc, stream_and_return)


# Signal handlers.
# A SIGINT handler gives the process some time to wait for redex to terminate
# and symbolize a backtrace. The SIGINT handler uninstalls itself so that a
# second SIGINT really kills redex.py and install a SIGALRM handler. The SIGALRM
# handler either sends a SIGINT to redex, waits some more, or terminates
# redex.py.
class BinaryState(enum.Enum):
    UNSTARTED = 1
    STARTED = 2
    POSTPROCESSING = 3
    FINISHED = 4


class SigIntHandler:
    def __init__(self) -> None:
        # pyre-fixme[4]
        self._old_handler: typing.Optional[typing.Any] = None
        self._state: BinaryState = BinaryState.UNSTARTED
        self._proc: typing.Optional[PopenType] = None

    def __enter__(self) -> "SigIntHandler":
        self.install()
        return self

    # pyre-fixme[2]
    def __exit__(self, *args, **kwargs) -> None:
        self.set_finished()

    def install(self) -> None:
        # On Linux, support ctrl-c.
        # Note: must be on the main thread. Add checks. Portability is an issue.
        if platform.system() != "Linux":
            return

        self._old_handler = signal.getsignal(signal.SIGINT)
        signal.signal(signal.SIGINT, self._sigint_handler)

    def uninstall(self) -> None:
        if self._old_handler is not None:
            signal.signal(signal.SIGINT, self._old_handler)

    def set_state(self, new_state: BinaryState) -> None:
        self._state = new_state

    def set_started(self, new_proc: PopenType) -> None:
        self.set_proc(new_proc)
        self.set_state(BinaryState.STARTED)

    def set_postprocessing(self) -> None:
        self.set_state(BinaryState.POSTPROCESSING)

    def set_finished(self) -> None:
        self.set_state(BinaryState.FINISHED)
        self.uninstall()

    def set_proc(self, new_proc: PopenType) -> None:
        self._proc = new_proc

    def _sigalrm_handler(self, _signum, _frame) -> None:
        signal.signal(signal.SIGALRM, signal.SIG_DFL)
        if self._state == BinaryState.STARTED:
            # Send SIGINT in case redex-all was not in the same process
            # group and wait some more.
            proc = self._proc
            assert proc is not None
            proc.send_signal(signal.SIGINT)
            signal.alarm(3)
            return
        if self._state == BinaryState.POSTPROCESSING:
            # Maybe symbolization took a while. Give it some more time.
            signal.alarm(3)
            return
        # Kill ourselves.
        os.kill(os.getpid(), signal.SIGINT)

    def _sigint_handler(self, _signum, _frame) -> None:
        signal.signal(signal.SIGINT, self._old_handler)
        if self._state == BinaryState.UNSTARTED or self._state == BinaryState.FINISHED:
            os.kill(os.getpid(), signal.SIGINT)

        # This is the first SIGINT, schedule some waiting period. redex-all is
        # likely in the same process group and already got a SIGINT delivered.
        signal.signal(signal.SIGALRM, self._sigalrm_handler)
        signal.alarm(3)
