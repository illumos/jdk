/*
 * Copyright (c) 1998, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef OS_CPU_SOLARIS_X86_BYTES_SOLARIS_X86_HPP
#define OS_CPU_SOLARIS_X86_BYTES_SOLARIS_X86_HPP

extern "C" {
  inline u2 _raw_swap_u2(u2 x) {
    unsigned short int __dest;
    __asm__ ("rorw $8, %w0": "=r" (__dest): "0" (x): "cc");
    return __dest;
  }
  inline u4 _raw_swap_u4(u4 x) {
    unsigned int __dest;
    __asm__ ("bswap %0" : "=r" (__dest) : "0" (x));
    return __dest;
  }
  inline u8 _raw_swap_u8(u8 x) {
    unsigned long  __dest;
    __asm__ ("bswap %q0" : "=r" (__dest) : "0" (x));
    return __dest;
  }
}

// Efficient swapping of data bytes from Java byte
// ordering to native byte ordering and vice versa.
inline u2   Bytes::swap_u2(u2 x) {
  return _raw_swap_u2(x);
}

inline u4   Bytes::swap_u4(u4 x) {
  return _raw_swap_u4(x);
}

inline u8   Bytes::swap_u8(u8 x) {
  return _raw_swap_u8(x);
}

#endif // OS_CPU_SOLARIS_X86_BYTES_SOLARIS_X86_HPP
