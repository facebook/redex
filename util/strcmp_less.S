/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * strcmp_less: A customized strcmp implementation on x86 SSE42 machines.
 *
 * bool strcmp_less(const char *, const char *);
 * Return true (1) when str1 < str2. Otherwise return false (0).
 *
 * This function is not 100% safe. One movdqu instruction could read 16 bytes
 * data. It could read data beyond the boundary of the string, which is usually
 * fine because reading data does not change the memory. One exception is that
 * when the string is stored at the very very end of the data section, the
 * movdqu instruction may try to read from non-existing address. This case
 * should happen rarely.
*/

#if defined(__SSE4_2__) && defined(__linux__)
        .file       "strcmp_less.S"
        .text

        .align      16
        .globl      strcmp_less
        .type       strcmp_less, @function
strcmp_less:
        .cfi_startproc

        // rdi stores str1*
        // rsi stores str2*
        // both rdi and rsi are scratch registers
        mov         $-16, %rax           // offset
strcmp_less_loop:
        add         $16, %rax
        movdqu      (%rsi, %rax), %xmm1
        pcmpistri   $0x18, (%rdi, %rax), %xmm1
        ja          strcmp_less_loop
        jc          strcmp_less_diff
strcmp_less_ge:
        // str1 >= str2
        xor         %eax, %eax
        ret

strcmp_less_diff:
        add         %rax, %rcx
        movzbl      (%rdi, %rcx), %eax  // str1
        movzbl      (%rsi, %rcx), %edx  // str2
 		    sub         %edx, %eax          // str1 - str2
        jge         strcmp_less_ge
        // str1 < str2
        mov         $1, %eax
        ret

        .cfi_endproc
        .size strcmp_less, .-strcmp_less
#endif
