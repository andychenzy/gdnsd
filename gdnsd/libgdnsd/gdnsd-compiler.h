/* Copyright © 2012 Brandon L Black <blblack@gmail.com>
 *
 * This file is part of gdnsd.
 *
 * gdnsd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gdnsd is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gdnsd.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef _GDNSD_COMPILER_H
#define _GDNSD_COMPILER_H

// GCC features we can take advantage of

#ifdef __GNUC__
#  if __GNUC__ < 3
#    error Your GCC is way too old (< 3.x)...
#  endif
#  define likely(x)       __builtin_expect(!!(x), 1)
#  define unlikely(x)     __builtin_expect(!!(x), 0)
#  define V_UNUSED        __attribute__((__unused__))
#  define F_UNUSED        __attribute__((__unused__))
#  define F_CONST         __attribute__((__const__))
#  define F_PURE          __attribute__((__pure__))
#  define F_MALLOC        __attribute__((__malloc__))
#  define F_NORETURN      __attribute__((__noreturn__))
#  if __GNUC__ > 3 || __GNUC_MINOR__ > 2 // gcc 3.3+
#    define F_NONNULLX(...) __attribute__((__nonnull__(__VA_ARGS__)))
#    define F_NONNULL       __attribute__((__nonnull__))
#  else
#    define F_NONNULLX(...)
#    define F_NONNULL
#  endif
#  if __GNUC__ > 3 || __GNUC_MINOR__ > 3 // gcc 3.4+
#    define F_WUNUSED       __attribute__((__warn_unused_result__))
#    define HAVE_BUILTIN_CLZ 1
#  else
#    define F_WUNUSED
#  endif
#else // Other C99 compilers...
#  define likely(x)       (!!(x))
#  define unlikely(x)     (!!(x))
#  define V_UNUSED
#  define F_UNUSED
#  define F_CONST
#  define F_PURE
#  define F_MALLOC
#  define F_NORETURN
#  define F_NONNULLX(...)
#  define F_NONNULL
#  define F_WUNUSED
#endif

// Valgrind hooks for debug builds
#if !defined(NDEBUG) && defined(HAVE_VALGRIND_MEMCHECK_H)
#  include <valgrind/memcheck.h>
#define NOWARN_VALGRIND_MAKE_MEM_NOACCESS(x,y) \
    do { int _x V_UNUSED; _x = VALGRIND_MAKE_MEM_NOACCESS(x,y); } while(0)
#else
#  define RUNNING_ON_VALGRIND 0
#  define VALGRIND_MEMPOOL_ALLOC(x,y,z)   ((void)(0))
#  define VALGRIND_CREATE_MEMPOOL(x,y,z)  ((void)(0))
#  define NOWARN_VALGRIND_MAKE_MEM_NOACCESS(x,y) ((void)(0))
#endif

// And silence some related warnings on gcc 4.6 + valgrind 3.6
#if !defined(NDEBUG) && defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 5))
#  define NOWARN_VALGRIND_CREATE_MEMPOOL(x,y,z) { \
     _Pragma("GCC diagnostic push"); \
     _Pragma("GCC diagnostic ignored \"-Wunused-but-set-variable\""); \
     VALGRIND_CREATE_MEMPOOL(x,y,z); \
     _Pragma("GCC diagnostic pop"); \
}
#  define NOWARN_VALGRIND_MEMPOOL_ALLOC(x,y,z) { \
     _Pragma("GCC diagnostic push"); \
     _Pragma("GCC diagnostic ignored \"-Wunused-but-set-variable\""); \
     VALGRIND_MEMPOOL_ALLOC(x,y,z); \
     _Pragma("GCC diagnostic pop"); \
}
#else
#  define NOWARN_VALGRIND_CREATE_MEMPOOL(x,y,z) \
     VALGRIND_CREATE_MEMPOOL(x,y,z)
#  define NOWARN_VALGRIND_MEMPOOL_ALLOC(x,y,z) \
     VALGRIND_MEMPOOL_ALLOC(x,y,z);
#endif

#endif // _GDNSD_COMPILER_H
