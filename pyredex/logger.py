#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


# pyre-strict


import os
import sys
import typing


trace: typing.Optional[typing.Dict[str, int]] = None
trace_fp: typing.Optional[typing.TextIO] = None

ALL = "__ALL__"


def parse_trace_string(trace: typing.Optional[str]) -> typing.Dict[str, int]:
    """
    The trace string is of the form KEY1:VALUE1,KEY2:VALUE2,...

    We convert it into a dict here.
    """
    if not trace:
        return {}
    rv = {}
    for t in trace.split(","):
        try:
            module, level = t.split(":")
            rv[module] = int(level)
        except ValueError:
            rv[ALL] = int(t)
    return rv


def get_trace() -> typing.Dict[str, int]:
    global trace
    local_trace = trace
    if local_trace is not None:
        return local_trace
    if "TRACE" in os.environ:
        local_trace = parse_trace_string(os.environ["TRACE"])
    else:
        local_trace = {}
    trace = local_trace
    return local_trace


def get_log_level() -> int:
    trace = get_trace()
    return max(trace.get("REDEX", 0), trace.get(ALL, 0))


def strip_trace_tag(env: typing.Dict[str, str]) -> None:
    """
    Remove the "REDEX:N" component from the trace string
    """
    try:
        trace = parse_trace_string(env["TRACE"])
        trace.pop("REDEX")
        if ALL in trace:
            trace_str = str(trace[ALL])
            trace.pop(ALL)
        else:
            trace_str = ""
        trace_str += ",".join(k + ":" + str(v) for k, v in trace.items())
        env["TRACE"] = trace_str
    except KeyError:
        pass


def get_trace_file() -> typing.TextIO:
    global trace_fp
    local_trace_fp = trace_fp
    if local_trace_fp is not None:
        return local_trace_fp

    trace_file = os.environ.get("TRACEFILE")
    if trace_file:
        sys.stderr.write("Trace output will go to %s\n" % trace_file)
        local_trace_fp = open(trace_file, "w")  # noqa: P201
    else:
        local_trace_fp = sys.stderr
    trace_fp = local_trace_fp
    return local_trace_fp


def update_trace_file(env: typing.Dict[str, str]) -> None:
    """
    If TRACEFILE is specified, update it to point to the file descriptor
    instead of the filename. (redex-all will treat integer TRACEFILE values as
    file descriptors.) This allows the redex-all subprocess to append to
    the file instead of calling open() on it again, which would overwrite its
    contents.

    Note that having redex-all open() the file under append mode is not a
    desirable solution as we still want to overwrite the file when redex-all is
    run outside of the wrapper script.
    """
    trace_fp = get_trace_file()
    if trace_fp is not sys.stderr:
        env["TRACEFILE"] = str(trace_fp.fileno())


def setup_trace_for_child(env: typing.Dict[str, str]) -> typing.Dict[str, str]:
    """
    Change relevant environment variables so that tracing in the redex-all
    subprocess works
    """
    env = env.copy()
    strip_trace_tag(env)
    update_trace_file(env)
    return env


def flush() -> None:
    get_trace_file().flush()


def log(*stuff: typing.Any) -> None:
    if get_log_level() > 0:
        print(*stuff, file=get_trace_file())
