#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"

struct bitmap;

void inode_init (void);
bool inode_create (block_sector_t, off_t);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
bool inode_get_symlink (struct inode *inode);
void inode_set_symlink (struct inode *inode, bool is_symlink);

// P4 tryout:
// bool inode_expand (struct inode_disk *disk_inode, off_t new_length);
// off_t inode_extend (struct inode_disk *disk_inode, off_t new_length);
// block_sector_t byte_to_sector(const struct inode *inode, off_t pos);
// bool inode_is_dir(const struct inode *inode);
// struct inode *inode_open_path(const char *path);
// off_t inode_length_physical(const struct inode *inode);
// blkcnt_t inode_get_blocks(const struct inode *inode);
// bool inode_is_removed (const struct inode *);

#endif /* filesys/inode.h */
