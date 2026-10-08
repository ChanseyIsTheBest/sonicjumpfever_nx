/* asset_pack.c -- see asset_pack.h. All stubs; Sonic Jump reads loose files.
 *
 * Copyright (C) 2026, MIT licensed. See LICENSE.
 *
 * The contract libc_shim.c relies on:
 *   *_fd_is / *_dir_is        -> 0, so no descriptor is ever claimed
 *   *_open_path / *_dup*      -> -1, "could not serve this"
 *   *_stat* / *_read_all*     -> 0, "not found in the pack"
 *   *_read/pread/lseek        -> -1, never reached given fd_is is 0
 *
 * Returning anything else here silently diverts real file I/O into a pack that
 * does not exist, so the values matter even though the bodies are trivial.
 */

#include "asset_pack.h"

int asset_pack_open_existing(const char *root) { (void)root; return 0; }
int asset_pack_build(const char *assets_root, const char *root)
{ (void)assets_root; (void)root; return 0; }
int asset_pack_active(void) { return 0; }
const char *asset_pack_error(void) { return "asset packing is disabled"; }

int asset_pack_stat_path(const char *path, uint64_t *size, uint64_t *ino)
{ (void)path; (void)size; (void)ino; return 0; }

int asset_pack_stat_path_info(const char *path, uint64_t *size, uint64_t *ino,
                              int *directory)
{ (void)path; (void)size; (void)ino; (void)directory; return 0; }

int asset_pack_stat_relative(const char *path, uint64_t *size, uint64_t *ino)
{ (void)path; (void)size; (void)ino; return 0; }

int  asset_pack_open_path(const char *path) { (void)path; return -1; }
int  asset_pack_dup_fd(int fd)              { (void)fd; return -1; }
int  asset_pack_dup2_fd(int fd, int target) { (void)fd; (void)target; return -1; }
int  asset_pack_fd_is(int fd)               { (void)fd; return 0; }

long asset_pack_read_fd(int fd, void *buffer, size_t count)
{ (void)fd; (void)buffer; (void)count; return -1; }

long asset_pack_pread_fd(int fd, void *buffer, size_t count, long offset)
{ (void)fd; (void)buffer; (void)count; (void)offset; return -1; }

long asset_pack_lseek_fd(int fd, long offset, int whence)
{ (void)fd; (void)offset; (void)whence; return -1; }

int asset_pack_fstat_fd(int fd, uint64_t *size, uint64_t *ino, int *directory)
{ (void)fd; (void)size; (void)ino; (void)directory; return 0; }

int asset_pack_close_fd(int fd) { (void)fd; return -1; }

int asset_pack_read_all_path(const char *path, void **data, size_t *size)
{ (void)path; (void)data; (void)size; return 0; }

int asset_pack_read_all_relative(const char *path, void **data, size_t *size)
{ (void)path; (void)data; (void)size; return 0; }

size_t      asset_pack_entry_count(void)          { return 0; }
const char *asset_pack_entry_path(size_t index)   { (void)index; return 0; }

void       *asset_pack_opendir_path(const char *path) { (void)path; return 0; }
int         asset_pack_dir_is(const void *dir)        { (void)dir; return 0; }
const char *asset_pack_readdir_path(void *dir, uint8_t *type, uint64_t *ino)
{ (void)dir; (void)type; (void)ino; return 0; }
int         asset_pack_closedir_path(void *dir)       { (void)dir; return -1; }
