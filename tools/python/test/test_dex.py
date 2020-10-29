# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import io
import os
import unittest
import zipfile

import dex


class TestDex(unittest.TestCase):
    @staticmethod
    def _open_file():
        filename = os.environ["DEX_FILE"]
        if filename.endswith(".dex"):
            f = open(filename, "rb")  # noqa: P201
            return dex.File(filename, f)
        if filename.endswith(".apk") or filename.endswith(".jar"):
            with zipfile.ZipFile(filename, "r") as zip_file:
                names = set(zip_file.namelist())
                if "classes.dex" not in names:
                    raise ValueError(f'No "classes.dex" in "{filename}"')
                info = zip_file.getinfo("classes.dex")
                data = zip_file.read(info)
                return dex.File(f"{filename}!classes.dex", io.BytesIO(data))
        raise ValueError(f'Unknown format: "{filename}"')

    def test_find_string_idx(self):
        dex_file = TestDex._open_file()

        strings = dex_file.get_strings()

        for i, s_data_item in enumerate(strings):
            idx = dex_file.find_string_idx(s_data_item)
            self.assertEquals(i, idx, f'Different index for "{s_data_item.data}"')

            idx = dex_file.find_string_idx(s_data_item.data)
            self.assertEquals(i, idx, f'Different index for "{s_data_item.data}"')

        # Synthesize some strings.
        for s_data_item in strings:
            if s_data_item.data:
                input = s_data_item.data
                before = chr(ord(input[0]) - 1) + input[1:]
                idx = dex_file.find_string_idx(before)
                self.assertEquals(idx, -1, f'Found "{before}"')

            after = s_data_item.data + "X"
            idx = dex_file.find_string_idx(after)
            self.assertEquals(idx, -1, f'Found "{after}"')
