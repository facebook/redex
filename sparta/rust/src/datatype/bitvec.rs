/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::fmt::Debug;
use std::fmt::Formatter;
use std::fmt::Result;
use std::string::ToString;

const POINTER_SIZE_IN_BITS: usize = std::mem::size_of::<usize>() * 8;

/// A bit string of at most `POINTER_SIZE_IN_BITS` bits, indexed from the least
/// significant bit.
///
/// This is used both for leaf keys, where `len` is the full width of the key
/// type, and for branch prefixes, where `len` is the branching bit position and
/// is therefore variable.
#[derive(Clone, PartialEq, Eq)]
pub struct BitVec {
    bits: usize,
    len: usize,
}

impl Debug for BitVec {
    fn fmt(&self, f: &mut Formatter<'_>) -> Result {
        f.debug_struct("BitVec")
            .field("bits", &format_args!("{:#b}", self.bits))
            .field("len", &self.len)
            .finish()
    }
}

fn make_mask(len: usize) -> usize {
    // avoid integer overflow
    assert!(len <= POINTER_SIZE_IN_BITS);
    if len == POINTER_SIZE_IN_BITS {
        usize::MAX
    } else {
        (1 << len) - 1
    }
}

impl BitVec {
    pub fn new() -> Self {
        BitVec { bits: 0, len: 0 }
    }

    pub fn from_int(v: usize) -> Self {
        BitVec {
            bits: v,
            len: POINTER_SIZE_IN_BITS,
        }
    }

    pub fn from_int_with_len(v: usize, len: usize) -> Self {
        let mask = make_mask(len);
        BitVec {
            bits: v & mask,
            len,
        }
    }

    pub fn get(&self, idx: usize) -> bool {
        assert!(idx < self.len, "idx: {}, len: {}", idx, self.len);

        let mask: usize = 1 << idx;
        (self.bits & mask) != 0
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn begins_with(&self, prefix: &BitVec) -> bool {
        if self.len() < prefix.len() {
            return false;
        }

        let compare_mask = make_mask(prefix.len());
        (self.bits & compare_mask) == (prefix.bits & compare_mask)
    }

    pub fn common_prefix(v1: &BitVec, v2: &BitVec) -> BitVec {
        let min_len = std::cmp::min(v1.len(), v2.len());

        let cmp1 = v1.bits;
        let cmp2 = v2.bits;

        let mask = make_mask(min_len);

        let mut same_bits = (!(cmp1 ^ cmp2)) & mask;
        let mut count_prefix: usize = 0;

        while same_bits % 2 == 1 {
            same_bits >>= 1;
            count_prefix += 1;
        }

        assert!(
            count_prefix <= min_len,
            "count_prefix: {}, min_len: {}",
            count_prefix,
            min_len
        );

        BitVec::from_int_with_len(cmp1, count_prefix)
    }

    pub fn back(&self) -> bool {
        self.get(self.len() - 1)
    }
}

impl Default for BitVec {
    fn default() -> Self {
        Self::new()
    }
}

impl ToString for BitVec {
    fn to_string(&self) -> String {
        format!("({} bits {:#b} aka {})", self.len, self.bits, self.bits)
    }
}

macro_rules! impl_bitvec_convert_for_integral_type {
    ( $ ($type:ty), * ) => {
        $(
            impl From<$type> for BitVec {
                fn from(u: $type) -> BitVec {
                    const SIZE: usize = std::mem::size_of::<$type>() * 8;
                    BitVec::from_int_with_len(u as usize, SIZE)
                }
            }

            impl From<&BitVec> for $type {
                fn from(bv: &BitVec) -> $type {
                    const LIMIT: usize = std::mem::size_of::<$type>() * 8;
                    if bv.len() > LIMIT {
                        panic!("BitVec too long for $type")
                    } else {
                        (bv.bits & make_mask(bv.len())) as $type
                    }
                }
            }
        )*
    };
}

impl_bitvec_convert_for_integral_type!(u8, u16, u32, u64, i8, i16, i32, i64, usize);

#[cfg(test)]
mod tests {

    use crate::datatype::bitvec::*;

    #[test]
    fn test_make_mask() {
        assert_eq!(make_mask(0), 0b0);
        assert_eq!(make_mask(1), 0b1);
        assert_eq!(make_mask(2), 0b11);
        assert_eq!(make_mask(3), 0b111);
        assert_eq!(make_mask(42), 0x3FFFFFFFFFF);
    }

    #[test]
    fn test_common_prefix() {
        let bv1 = BitVec::from_int_with_len(0b1010010, 8);
        let bv2 = BitVec::from_int_with_len(0b1110010, 8);
        let bv3 = BitVec::from_int_with_len(0b10010, 5);

        assert!(
            BitVec::common_prefix(&bv1, &bv2) == bv3,
            "bv1: {}, bv2: {}, bv3: {}",
            bv1.to_string(),
            bv2.to_string(),
            bv3.to_string()
        );
    }

    #[test]
    fn test_create_bitvec() {
        let bv: BitVec = BitVec::from(10);
        assert!(bv == bv);

        let bv8: BitVec = BitVec::from_int_with_len(0xFEFE, 8);
        let bv12: BitVec = BitVec::from_int_with_len(0xABFEFE, 12);

        assert!(bv12.begins_with(&bv8));
    }
}
