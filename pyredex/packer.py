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
import multiprocessing
import os
import shutil
import subprocess
import tarfile
import tempfile
import timeit
import typing
import zipfile
from abc import ABC, abstractmethod
from contextlib import contextmanager
from enum import Enum

from pyredex.utils import get_xz_path

timer: typing.Callable[[], float] = timeit.default_timer


class CompressionLevel(Enum):
    FAST = 1
    DEFAULT = 2
    BETTER = 3


# TODO: Move to dataclass once minimum Python version increments.
class CompressionEntry(typing.NamedTuple):
    name: str
    filter_fn: typing.Optional[typing.Callable[[argparse.Namespace], bool]]
    remove_source: bool
    file_list_must: typing.List[str]
    file_list_may: typing.List[str]
    output_name: typing.Optional[str]
    checksum_name: typing.Optional[str]
    compression_level: CompressionLevel

    # For pickling.
    def without_filter(self) -> "CompressionEntry":
        return CompressionEntry(
            self.name,
            None,
            self.remove_source,
            self.file_list_must,
            self.file_list_may,
            self.output_name,
            self.checksum_name,
            self.compression_level,
        )


class _Compressor(ABC):
    _BUF_SIZE: int = 128 * 1024

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

            # Gotta read this block-wise, as some files may be
            # quite large.
            with open(file_path, "rb") as f:
                while True:
                    data = f.read(_Compressor._BUF_SIZE)
                    if not data:
                        break
                    hash.update(data)

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
    def __init__(
        self,
        zip_path: str,
        checksum_name: typing.Optional[str],
        compression_level: CompressionLevel,
    ) -> None:
        super().__init__(checksum_name)
        self.zipfile = zipfile.ZipFile(
            zip_path,
            "w",
            compression=zipfile.ZIP_DEFLATED,
            compresslevel=_ZipCompressor._get_compress_level(compression_level),
        )

    @staticmethod
    def _get_compress_level(compression_level: CompressionLevel) -> int:
        if compression_level == CompressionLevel.FAST:
            return 0
        elif compression_level == CompressionLevel.BETTER:
            return 9
        else:
            return 6  # This is the default.

    def _handle_file_impl(self, file_path: str, file_name: str) -> None:
        self.zipfile.write(filename=file_path, arcname=file_name)

    def _finalize_impl(self) -> None:
        self.zipfile.close()

    def _add_file_content(self, name: str, content: str) -> None:
        self.zipfile.writestr(name, content)


class _TarGzCompressor(_Compressor):
    def __init__(
        self,
        targz_path: str,
        checksum_name: typing.Optional[str],
        compression_level: CompressionLevel,
    ) -> None:
        super().__init__(checksum_name)
        self.tarfile: tarfile.TarFile = tarfile.open(
            name=targz_path,
            mode="w:xz",
            compresslevel=_TarGzCompressor._get_compress_level(compression_level),
        )

    @staticmethod
    def _get_compress_level(compression_level: CompressionLevel) -> int:
        if compression_level == CompressionLevel.FAST:
            return 0
        elif compression_level == CompressionLevel.BETTER:
            return 9
        else:
            return 9  # This is the default.

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


def _get_xz_compress_level(
    compression_level: CompressionLevel,
) -> typing.Tuple[str, int]:
    if compression_level == CompressionLevel.FAST:
        return ("-5", 5)
    elif compression_level == CompressionLevel.BETTER:
        return ("-9e", 9 | lzma.PRESET_EXTREME)
    else:
        return ("-7e", 7 | lzma.PRESET_EXTREME)


def _compress_xz(
    from_file: str, to_file: str, compression_level: CompressionLevel
) -> None:
    comp = _get_xz_compress_level(compression_level)
    xz = get_xz_path()
    if xz is not None:
        cmd = [xz, "-z", comp[0], "-T10"]
        with open(from_file, "rb") as fin:
            with open(to_file, "wb") as fout:
                subprocess.check_call(cmd, stdin=fin, stdout=fout)
                return

    with lzma.open(filename=to_file, mode="wb", preset=comp[1]) as xz:
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


def _compress(
    data: typing.Tuple[CompressionEntry, str, str, argparse.Namespace]
) -> None:
    item, src_dir, trg_dir, args = data
    logging.debug("Checking %s for compression...", item.name)

    inputs = _ensure_exists(item.file_list_must, src_dir) + [
        f for f in item.file_list_may if os.path.exists(os.path.join(src_dir, f))
    ]
    if not inputs:
        logging.debug("No inputs found.")
        return

    with _warning_timer(item.name, 1.0) as _:
        logging.debug("Compressing %s...", item.name)

        # If an output name is given, use it. If not, ensure that it is only
        # one file.

        if item.output_name is not None:
            name = os.path.join(trg_dir, item.output_name)
            # Compress to archive.
            compressor = None
            if name.endswith(".tar.xz"):
                compressor = _TarGzCompressor(
                    name, item.checksum_name, item.compression_level
                )
            elif name.endswith(".zip"):
                compressor = _ZipCompressor(
                    name, item.checksum_name, item.compression_level
                )
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
                item.compression_level,
            )

        if item.remove_source:
            for f in inputs:
                os.remove(os.path.join(src_dir, f))
    pass


def compress_entries(
    conf: typing.List[CompressionEntry],
    src_dir: str,
    trg_dir: str,
    args: argparse.Namespace,
    processes: int = 4,
) -> None:
    if processes == 1:
        for item in conf:
            _compress((item, src_dir, trg_dir, args))
    else:
        with multiprocessing.Pool(processes=processes) as pool:

            def _is_selected(item: CompressionEntry) -> bool:
                filter_fn = item.filter_fn
                return filter_fn is None or filter_fn(args)

            imap_iter = pool.imap_unordered(
                _compress,
                (
                    (item.without_filter(), src_dir, trg_dir, args)
                    for item in conf
                    if _is_selected(item)  # Can't pickle lambdas so test here.
                ),
            )

            for _ in imap_iter:
                pass
