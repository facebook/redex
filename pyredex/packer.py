#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


# pyre-strict


import argparse
import hashlib
import logging
import lzma
import os
import shutil
import tarfile
import tempfile
import timeit
import typing
import zipfile
from abc import ABC, abstractmethod
from contextlib import contextmanager


timer: typing.Callable[[], float] = timeit.default_timer


# TODO: Move to dataclass once minimum Python version increments.
class CompressionEntry(typing.NamedTuple):
    name: str
    filter_fn: typing.Callable[[argparse.Namespace], bool]
    remove_source: bool
    file_list_must: typing.List[str]
    file_list_may: typing.List[str]
    output_name: typing.Optional[str]
    checksum_name: typing.Optional[str]


class _Compressor(ABC):
    def __init__(self, checksum_name: typing.Optional[str]) -> None:
        self.checksum_name = checksum_name
        self.do_checksum: bool = checksum_name is not None
        self.hash: typing.Optional[hashlib._Hash] = None

    def handle_file(self, file_path: str, file_name: str) -> None:
        self._handle_file_impl(file_path, file_name)

        if self.do_checksum:
            hash = self.hash
            if hash is None:
                hash = hashlib.md5()
                self.hash = hash

            # Files should be small enough to read into memory.
            assert os.path.getsize(file_path) < 10000000
            with open(file_path, "rb") as f:
                hash.update(f.read())

    @abstractmethod
    def _handle_file_impl(self, file_path: str, file_name: str) -> None:
        pass

    def finalize(self) -> None:
        if self.do_checksum:
            hash = self.hash
            assert hash is not None
            name = self.checksum_name
            assert name is not None
            self._add_file_content(name, f"{hash.hexdigest()}\n")

        self._finalize_impl()

    @abstractmethod
    def _finalize_impl(self) -> None:
        pass

    @abstractmethod
    def _add_file_content(self, name: str, content: str) -> None:
        pass


class _ZipCompressor(_Compressor):
    def __init__(self, zip_path: str, checksum_name: typing.Optional[str]) -> None:
        super().__init__(checksum_name)
        self.zipfile = zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED)

    def _handle_file_impl(self, file_path: str, file_name: str) -> None:
        self.zipfile.write(filename=file_path, arcname=file_name)

    def _finalize_impl(self) -> None:
        self.zipfile.close()

    def _add_file_content(self, name: str, content: str) -> None:
        self.zipfile.writestr(name, content)


class _TarGzCompressor(_Compressor):
    def __init__(self, targz_path: str, checksum_name: typing.Optional[str]) -> None:
        super().__init__(checksum_name)
        self.tarfile: tarfile.TarFile = tarfile.open(name=targz_path, mode="w:xz")

    def _handle_file_impl(self, file_path: str, file_name: str) -> None:
        # This deals better with symlinks.
        with open(file_path, "rb") as f_in:
            info = self.tarfile.gettarinfo(arcname=file_name, fileobj=f_in)
            self.tarfile.addfile(info, fileobj=f_in)

    def _finalize_impl(self) -> None:
        self.tarfile.close()

    def _add_file_content(self, name: str, content: str) -> None:
        # tarfile does not support writing a buffer in. :-(
        with tempfile.TemporaryDirectory() as tmp_dir:
            checksum_filename = os.path.join(tmp_dir, "checksum.txt")
            with open(checksum_filename, "w") as f:
                f.write(content)
            self.tarfile.add(checksum_filename, name)


def _compress_xz(from_file: str, to_file: str) -> None:
    with lzma.open(filename=to_file, mode="wb", preset=7 | lzma.PRESET_EXTREME) as xz:
        with open(from_file, "rb") as f_in:
            shutil.copyfileobj(f_in, xz)


def _ensure_exists(inputs: typing.List[str], dir: str) -> typing.List[str]:
    for item in inputs:
        assert os.path.exists(os.path.join(dir, item)), f"Did not find {item}"
    return inputs


@contextmanager
def _warning_timer(name: str, threshold: float) -> typing.Generator[int, None, None]:
    start_time = timer()

    try:
        yield 1  # Irrelevant
    finally:
        end_time = timer()
        time_delta = end_time - start_time
        if time_delta > threshold:
            logging.warning("Needed %fs to compress %s.", end_time - start_time, name)
        else:
            logging.debug("Needed %fs to compress.", end_time - start_time)


def compress_entries(
    conf: typing.List[CompressionEntry],
    src_dir: str,
    trg_dir: str,
    args: argparse.Namespace,
) -> None:
    for item in conf:
        logging.debug("Checking %s for compression...", item.name)
        if not item.filter_fn(args):
            continue

        inputs = _ensure_exists(item.file_list_must, src_dir) + [
            f for f in item.file_list_may if os.path.exists(os.path.join(src_dir, f))
        ]
        if not inputs:
            logging.debug("No inputs found.")
            continue

        with _warning_timer(item.name, 1.0) as _:
            logging.info("Compressing %s...", item.name)

            # If an output name is given, use it. If not, ensure that it is only
            # one file.

            if item.output_name is not None:
                name = os.path.join(trg_dir, item.output_name)
                # Compress to archive.
                compressor = None
                if name.endswith(".tar.xz"):
                    compressor = _TarGzCompressor(name, item.checksum_name)
                elif name.endswith(".zip"):
                    compressor = _ZipCompressor(name, item.checksum_name)
                assert compressor is not None

                for f in inputs:
                    f_full = os.path.join(src_dir, f)
                    compressor.handle_file(f_full, f)

                compressor.finalize()
            else:
                assert len(item.file_list_must) + len(item.file_list_may) == 1
                # Can also not use checksum.
                assert not item.checksum_name

                # Compress to .xz
                _compress_xz(
                    os.path.join(src_dir, inputs[0]),
                    os.path.join(trg_dir, item.output_name or (inputs[0] + ".xz")),
                )

            if item.remove_source:
                for f in inputs:
                    os.remove(os.path.join(src_dir, f))
