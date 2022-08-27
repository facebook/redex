/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

use std::string::ToString;

const POINTER_SIZE_IN_BITS: usize = std::mem::size_of::<usize>() * 8;

union PointerOrBits {
    pointer: *mut usize, // TODO: When len > pointer size, allocate and store pointer here.
    bits: usize,
}

pub struct BitVec {
    actual_storage: PointerOrBits,
    len: usize,
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
        BitVec {
            actual_storage: PointerOrBits { bits: 0 },
            len: 0,
        }
    }

    pub fn from_int(v: usize) -> Self {
        BitVec {
            actual_storage: PointerOrBits { bits: v },
            len: POINTER_SIZE_IN_BITS,
        }
    }

    pub fn from_int_with_len(v: usize, len: usize) -> Self {
        assert!(len <= POINTER_SIZE_IN_BITS);

        let mask = make_mask(len);
        BitVec {
            actual_storage: PointerOrBits { bits: v & mask },
            len,
        }
    }

    pub fn get(&self, idx: usize) -> bool {
        assert!(idx < self.len, "idx: {}, len: {}", idx, self.len);

        let storage_unit;
        let intraunit_offset;

        if self.len <= POINTER_SIZE_IN_BITS {
            // Actual vector is stored in where it's supposed to be a pointer.
            storage_unit = unsafe { self.actual_storage.bits };
            intraunit_offset = idx;
        } else {
            todo!("Long bitvec not implemented");
        }

        let mask: usize = 1 << intraunit_offset;
        (storage_unit & mask) != 0
    }

    fn push(&mut self, val: bool) {
        let storage_unit: &mut usize;
        let intraunit_offset: usize;

        if self.len < POINTER_SIZE_IN_BITS {
            storage_unit = unsafe { &mut self.actual_storage.bits };
            intraunit_offset = self.len;
        } else {
            todo!("Long bitvec not implemented");
        }

        if val {
            *storage_unit |= 1 << intraunit_offset;
        } else {
            *storage_unit &= !(1 << intraunit_offset);
        }

        self.len += 1;
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn begins_with(&self, prefix: &BitVec) -> bool {
        if self.len() < prefix.len() {
            return false;
        }

        if self.len() <= POINTER_SIZE_IN_BITS {
            let compare_mask = make_mask(prefix.len());
            let compare_left = unsafe { self.actual_storage.bits & compare_mask };
            let compare_right = unsafe { prefix.actual_storage.bits & compare_mask };
            compare_left == compare_right
        } else {
            todo!("Long bitvec not implemented");
        }
    }

    pub fn common_prefix(v1: &BitVec, v2: &BitVec) -> BitVec {
        let min_len = std::cmp::min(v1.len(), v2.len());

        if min_len > POINTER_SIZE_IN_BITS {
            todo!("Long bitvec not implemented");
        }

        let cmp1 = unsafe { v1.actual_storage.bits };
        let cmp2 = unsafe { v2.actual_storage.bits };

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

impl Clone for BitVec {
    fn clone(&self) -> Self {
        if self.len <= POINTER_SIZE_IN_BITS {
            Self {
                actual_storage: PointerOrBits {
                    bits: unsafe { self.actual_storage.bits },
                },
                len: self.len,
            }
        } else {
            todo!("Long bitvec not implemented");
        }
    }
}

impl ToString for BitVec {
    fn to_string(&self) -> String {
        let bits = unsafe { self.actual_storage.bits };
        format!("({} bits {:#b} aka {})", self.len, bits, bits)
    }
}

impl PartialEq for BitVec {
    fn eq(&self, other: &Self) -> bool {
        if self.len != other.len {
            return false;
        }

        if self.len > POINTER_SIZE_IN_BITS {
            todo!("Long bitvec not implemented");
        }

        let compare_left = unsafe { self.actual_storage.bits };
        let compare_right = unsafe { other.actual_storage.bits };
        compare_left == compare_right
    }
}

impl Eq for BitVec {}

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
                        let bits = unsafe { bv.actual_storage.bits };
                        (bits & make_mask(bv.len())) as $type
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
