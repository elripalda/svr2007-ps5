/**
 * @file        rex/ps5_compat.h
 * @brief       [ps5 port] Linux names the POSIX code uses, mapped to the PS5's
 *              FreeBSD equivalents. Force-included on PS5 builds
 *              (-include rex/ps5_compat.h); a no-op everywhere else.
 *
 * Macros only, no includes: it is force-included ahead of the precompiled
 * headers, and declaring anything here (even via <stdio.h>) would declare
 * clang's __va_list_tag twice and break va_list in PCH'd files.
 */
#pragma once

#if defined(__PROSPERO__)

// FreeBSD's off_t is always 64-bit: the *64 large-file names are the plain ones.
#define fseeko64 fseeko
#define ftello64 ftello
#define ftruncate64 ftruncate
#define lseek64 lseek
#define mmap64 mmap
#define pread64 pread
#define pwrite64 pwrite
#define fopen64 fopen
#define open64 open
#define fstat64 fstat
#define stat64 stat
#define lstat64 lstat
#define off64_t off_t

// Linux-only clock; the monotonic clock is the closest PS5 equivalent.
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC

#endif  // __PROSPERO__
