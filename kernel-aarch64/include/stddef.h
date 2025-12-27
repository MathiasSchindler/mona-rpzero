#pragma once

/* Minimal stddef.h for freestanding AArch64 builds. */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long size_t;
typedef long ptrdiff_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#ifdef __cplusplus
}
#endif
