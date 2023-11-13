#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


# pyre-strict


import json
import logging
import os
import platform
import typing
from contextlib import contextmanager
from io import TextIOWrapper

from pyredex.utils import time_it


# Data class for a running step.
class _RunningPart:
    def __init__(self, event_id: int, name: str, desc: str) -> None:
        self.event_id = event_id
        self.name = name
        self.desc = desc


# Handle connection to buck. Manage running parts (assumed to have stack discipline).
class BuckConnection:
    EVENT_TYPE_STEP = '{"eventType": "STEP_EVENT"}'

    def __init__(self) -> None:
        self.has_buck: typing.Optional[bool] = None
        self.action_id = ""
        self.event_id = 0
        self.event_pipe: typing.Optional[TextIOWrapper] = None
        self.running_parts: typing.List[_RunningPart] = []

    def connect(self) -> None:
        assert self.has_buck is None
        if (
            "BUCK_EVENT_PIPE" not in os.environ
            or "BUCK_EVENT_PIPE" not in os.environ
            or "BUCK_ACTION_ID" not in os.environ
            # TODO(T103482589) Work around an issue on macs.
            or platform.system() == "Darwin"
        ):
            self.has_buck = False
            return

        self.has_buck = True
        self.action_id = os.environ["BUCK_ACTION_ID"]

        try:
            self.__open_pipe()
            self.__init_message()
        except BaseException as e:
            logging.warning("Failed to connect to buck: %s", e)
            self.has_buck = False

    def is_connected(self) -> bool:
        return self.has_buck is not None

    def disconnect(self) -> None:
        local = self.event_pipe
        if local:
            local.close()

    def __open_pipe(self) -> None:
        event_path = os.path.abspath(os.environ["BUCK_EVENT_PIPE"])
        # Need to go low-level for non-blocking connection.
        fd = os.open(event_path, os.O_WRONLY | os.O_NONBLOCK)
        if fd < 0:
            raise RuntimeError(f"Could not open pipe to {event_path}")
        self.event_pipe = open(fd, mode="w")  # noqa(P201)

    def __init_message(self) -> None:
        local = self.event_pipe
        assert local
        local.write("j")
        local.write(os.linesep)
        local.flush()

    def __create_step_message(self, event: _RunningPart, status: str) -> str:
        message = {
            "event_id": event.event_id,
            "step_status": status,
            "step_type": event.name,
            "description": event.desc,
            "action_id": self.action_id,
        }

        return json.dumps(message)

    def __send_step(self, event: _RunningPart, status: str) -> None:
        message = self.__create_step_message(event, status)
        local = self.event_pipe
        if not local:
            return
        try:
            local.write(str(len(BuckConnection.EVENT_TYPE_STEP)))
            local.write(os.linesep)
            local.write(BuckConnection.EVENT_TYPE_STEP)
            local.write(str(len(message)))
            local.write(os.linesep)
            local.write(message)
            local.flush()
        except (BrokenPipeError, BlockingIOError) as e:
            logging.error("Buck pipe is broken! %s", e)
            self.has_buck = False
            self.event_pipe = None

    def start(self, name: str, desc: str) -> None:
        if not self.has_buck:
            return

        part = _RunningPart(self.event_id, name, desc)
        self.event_id += 1

        self.__send_step(part, "STARTED")

        self.running_parts.append(part)

    def end(self) -> None:
        if not self.has_buck:
            return

        if not self.running_parts:
            return

        part = self.running_parts.pop()
        self.__send_step(part, "FINISHED")

    def size(self) -> int:
        return len(self.running_parts)

    def end_all(self, down_to: typing.Optional[int] = None) -> None:
        if not self.has_buck:
            return

        left = 0 if not down_to else max(0, down_to)

        while len(self.running_parts) > left:
            part = self.running_parts.pop()
            self.__send_step(part, "FINISHED")


_BUCK_CONNECTION = BuckConnection()


def get_buck_connection() -> BuckConnection:
    return _BUCK_CONNECTION


class BuckConnectionScope:
    def __init__(self) -> None:
        self.was_connected = False
        self.num_parts = 0
        pass

    def __enter__(self) -> BuckConnection:
        self.was_connected = _BUCK_CONNECTION.is_connected()
        if not self.was_connected:
            _BUCK_CONNECTION.connect()

        self.num_parts = _BUCK_CONNECTION.size()

        return _BUCK_CONNECTION

    def __exit__(self, *args: typing.Any) -> None:
        _BUCK_CONNECTION.end_all(self.num_parts)
        if not self.was_connected:
            _BUCK_CONNECTION.disconnect()


@contextmanager
def BuckPartScope(
    name: str, desc: str, timed: bool = True, timed_start: bool = True
) -> typing.Generator[int, None, None]:
    if timed:
        with time_it(
            f"{desc} took {{time:.2f}} seconds",
            **({"start": desc} if timed_start else {}),
        ):
            _BUCK_CONNECTION.start(name, desc)
            try:
                yield 1  # Irrelevant
            finally:
                _BUCK_CONNECTION.end()
    else:
        _BUCK_CONNECTION.start(name, desc)
        try:
            yield 1  # Irrelevant
        finally:
            _BUCK_CONNECTION.end()
