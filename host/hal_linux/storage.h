/*
 * storage.h - the dock's USB drives as seen by calculator programs.
 *
 * Drives are mounted (by udev) as subdirectories of a root directory such as
 * /media/dock; drive index order is the sorted directory names. Calculator
 * paths are "DRIVE/dir/file" (drive names match case-insensitively) or
 * "dir/file", meaning the first drive.
 *
 * Sandbox: ".." components and absolute paths are rejected, and paths are
 * resolved component by component with openat(O_NOFOLLOW), so symlinks are
 * never followed and nothing outside a drive can be reached.
 *
 * All functions return an enum dock_status.
 */
#ifndef HOST_STORAGE_H
#define HOST_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORAGE_MAX_HANDLES 16u

typedef struct storage storage_t;

storage_t *storage_open(const char *root);
void storage_close(storage_t *s);

int storage_drive(storage_t *s, uint8_t index, char *name, size_t cap);
int storage_fopen(storage_t *s, const char *path, uint8_t mode, uint8_t *handle);
int storage_read(storage_t *s, uint8_t handle, uint8_t *dst, size_t max, size_t *got);
int storage_write(storage_t *s, uint8_t handle, const uint8_t *src, size_t n,
                  size_t *written);
int storage_seek(storage_t *s, uint8_t handle, int32_t offset, uint8_t whence,
                 uint32_t *pos);
int storage_fclose(storage_t *s, uint8_t handle);
int storage_stat(storage_t *s, const char *path, uint32_t *size, bool *is_dir);
int storage_readdir(storage_t *s, const char *path, uint16_t index, char *name,
                    size_t cap, uint32_t *size, bool *is_dir);
int storage_mkdir(storage_t *s, const char *path);
int storage_remove(storage_t *s, const char *path);

/* Close every handle (the calculator session ended). */
void storage_close_all(storage_t *s);

/* Open a regular file read-only through the sandbox (for audio playback).
 * On DOCK_OK *fd is an fd the caller owns. */
int storage_open_ro_fd(storage_t *s, const char *path, int *fd);

#endif /* HOST_STORAGE_H */
