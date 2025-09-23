#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


# pyre-strict


import enum
import logging
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import typing
from functools import reduce

from pyredex.logger import get_store_logs_temp_file


IS_WINDOWS: bool = os.name == "nt"
LOGGER: logging.Logger = logging.getLogger(__name__)


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
    "libc.so.6(pthread_kill",
    "libc.so.6(raise",
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

        def _try_run(cmd: typing.Optional[str]) -> typing.Optional[str]:
            if not cmd:
                return None

            # Try directly.  Only if it exists as path.
            if os.path.exists(cmd):
                cmd = os.path.abspath(cmd)
                try:
                    subprocess.check_call(
                        [cmd, "-v"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    LOGGER.debug("Found addr2line at %s", cmd)
                    return cmd
                except Exception:
                    pass

            # Try as PATH reference.
            cmd = shutil.which(cmd)
            if cmd:
                cmd = os.path.abspath(cmd)
                try:
                    subprocess.check_call(
                        [cmd, "-v"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    LOGGER.debug("Found addr2line at %s from `which`", cmd)
                    return cmd
                except Exception:
                    pass

            return None

        path = _try_run(path) or _try_run("addr2line")

        if path:
            ADDR2LINE_PATH = path
            return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass

    return False


_ADDR2LINE_BASE_ARGS = ["-f", "-i", "-C", "-e"]


def _symbolize(filename: str, offset: str) -> typing.Optional[typing.List[str]]:
    # It's good enough not to use server mode.
    try:
        path = ADDR2LINE_PATH
        assert path
        assert os.path.isabs(path)

        output = subprocess.check_output(
            [path] + _ADDR2LINE_BASE_ARGS + [filename, offset],
            stderr=subprocess.DEVNULL,
        )
        return output.decode(sys.stderr.encoding).splitlines()
    except subprocess.CalledProcessError:
        return None


def maybe_addr2line(crash_file: str) -> typing.Optional[typing.List[str]]:
    if not os.path.exists(crash_file) or os.path.getsize(crash_file) == 0:
        return None

    def lines() -> typing.Generator[str, None, None]:
        with open(crash_file, "r") as f:
            for line in f:
                yield line.strip()

    global _BACKTRACE_PATTERN
    backtrace_matches = (
        m
        for line in lines()
        for m in [_BACKTRACE_PATTERN.fullmatch(line)]
        if m is not None
    )

    filter_skips = (m for m in backtrace_matches if not _should_skip_line(m.string))

    checked_addr2line: typing.Optional[bool] = None

    def translate(m: typing.Match[str]) -> typing.Optional[typing.List[str]]:
        nonlocal checked_addr2line
        if checked_addr2line is None:
            checked_addr2line = _has_addr2line()
        if not checked_addr2line:
            return None

        ret = []
        ret.append("%s(%s)[%s]" % (m.group(1), m.group(2), m.group(3)))
        decoded = _symbolize(m.group(1), m.group(3))
        if decoded is not None:
            for idx, line in enumerate(decoded):
                line = line.strip()
                if _should_skip_line(line):
                    continue

                ret.append(f'{"  " * (1 if idx % 2 == 0 else 2)}{line}')

        return ret

    symbolized = (translate(m) for m in filter_skips)

    reduced = reduce(
        lambda a, b: None if a is None or b is None else a + b, symbolized, []
    )

    return reduced


_LINUX_TERMINATE: str = "terminate called"
_MAC_TERMINATE: str = (
    "libc++abi: terminating due to uncaught exception of type boost::exception_detail::error_info_injector<RedexException>"
)


def find_abort_error(lines: typing.Iterable[str]) -> typing.Optional[str]:
    global _LINUX_TERMINATE, _MAC_TERMINATE
    terminate_lines = []
    for line in lines:
        stripped_line = line.rstrip()

        if line.lstrip().startswith((_LINUX_TERMINATE, _MAC_TERMINATE)):
            terminate_lines.append(stripped_line)
            continue

        if len(terminate_lines) > 0:
            terminate_lines.append(stripped_line)

            # Stop on three lines, keep it short.
            if len(terminate_lines) >= 3:
                break
            continue

    if not terminate_lines:
        return None

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

        store_logs = get_store_logs_temp_file()

        for line in stderr:
            try:
                str_line = line.decode(sys.stdout.encoding)
            except UnicodeDecodeError:
                str_line = "<UnicodeDecodeError>\n"
            if line_handler:
                str_line = line_handler(str_line)
            sys.stderr.write(str_line)
            if store_logs:
                store_logs.write(str_line)
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
