#pragma once

#include "proc.h"
#include "stdint.h"

int user_range_ok(uint64_t user_ptr, uint64_t len);

uint64_t cstr_len_u64(const char *s);
uint64_t cstr_len(const char *s);
int cstr_eq_u64(const char *a, const char *b);

int normalize_abs_path(const char *in, char *out, uint64_t outsz);
int resolve_path(proc_t *p, const char *in, char *out, uint64_t outsz);

/*
 * Path helpers.
 *
 * - abs_path_to_no_slash_trim(): converts "/a/b" -> "a/b" and trims trailing slashes.
 * - abs_path_parent_dir(): converts "/a/b" -> "/a" ("/a" -> "/", "/" -> "/").
 *
 * Returns 0 on success, or a negative errno value on error.
 */
int abs_path_to_no_slash_trim(const char *abs_path, char *out_no_slash, uint64_t outsz);
int abs_path_parent_dir(const char *abs_path, char *out_parent_abs, uint64_t outsz);

int copy_cstr_from_user(char *dst, uint64_t dstsz, uint64_t user_ptr);
int read_u64_from_user(uint64_t user_ptr, uint64_t *out);
int write_bytes_to_user(uint64_t user_dst, const void *src, uint64_t len);
int write_u64_to_user(uint64_t user_dst, uint64_t v);
int write_u16_to_user(uint64_t user_dst, uint16_t v);

uint64_t align_down_u64(uint64_t x, uint64_t a);
uint64_t align_up_u64(uint64_t x, uint64_t a);
