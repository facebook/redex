#! /usr/bin/env python

# Copyright (c) 2016-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.


from __future__ import absolute_import
from __future__ import division
from __future__ import print_function


class LookupDictionary(dict):
    """
    a dictionary which can lookup value by key, or keys by value
    """

    def __init__(self, items=[]):
        """items can be a list of pair_lists or a dictionary"""
        dict.__init__(self, items)

    def get_keys_for_value(self, value, fail_value=None):
        """find the key(s) as a list given a value"""
        list_result = [item[0] for item in self.items() if item[1] == value]
        if len(list_result) > 0:
            return list_result
        return fail_value

    def get_first_key_for_value(self, value, fail_value=None):
        """return the first key of this dictionary given the value"""
        list_result = [item[0] for item in self.items() if item[1] == value]
        if len(list_result) > 0:
            return list_result[0]
        return fail_value

    def get_value(self, key, fail_value=None):
        """find the value given a key"""
        if key in self:
            return self[key]
        return fail_value


class Enum(LookupDictionary):

    def __init__(self, initial_value=0, items=[]):
        """items can be a list of pair_lists or a dictionary"""
        LookupDictionary.__init__(self, items)
        self.set_value(initial_value)

    @classmethod
    def max_width(cls):
        max_key_len = 0
        for key in cls.enum:
            key_len = len(key)
            if key_len > max_key_len:
                max_key_len = key_len
        return max_key_len

    def set_value(self, v):
        if type(v) == str:
            self.value = self.get_value(v, -1)
        else:
            self.value = v

    def get_enum_value(self):
        return self.value

    def get_enum_name(self):
        return self.__str__()

    def __lt__(self, other):
        if other is None:
            return False
        if type(other) is int:
            return self.value < other
        else:
            return self.value < other.value

    def __le__(self, other):
        if other is None:
            return False
        if type(other) is int:
            return self.value <= other
        else:
            return self.value <= other.value

    def __gt__(self, other):
        if other is None:
            return False
        if type(other) is int:
            return self.value > other
        else:
            return self.value > other.value

    def __ge__(self, other):
        if other is None:
            return False
        if type(other) is int:
            return self.value >= other
        else:
            return self.value >= other.value

    def __eq__(self, other):
        if other is None:
            return False
        if type(other) is int:
            return self.value == other
        else:
            return self.value == other.value

    def __ne__(self, other):
        if other is None:
            return True
        if type(other) is int:
            return self.value != other
        else:
            return self.value != other.value

    def __int__(self):
        return self.get_enum_value()

    def __hash__(self):
        return hash(self.get_enum_value())

    def __str__(self):
        s = self.get_first_key_for_value(self.value, None)
        if s is None:
            s = "%#8.8x" % self.value
        return s

    def __repr__(self):
        return self.__str__()
