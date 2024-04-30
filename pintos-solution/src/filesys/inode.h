#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include <list.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "filesys/filesys.h"

struct bitmap;

#define DIRECT_BLOCKS 122
#define INDIRECT_BLOCKS 128
#define DOUBLY_INDIRECT_BLOCKS 128 * 128

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  // block_sector_t start; /* First data sector. */
  block_sector_t direct_blocks[DIRECT_BLOCKS];  /* Direct block pointers. */
  block_sector_t indirect_block;      /* Indirect block pointer. */
  block_sector_t doubly_indirect_block; /* Doubly indirect block pointer. */

  bool is_dir;
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  bool is_symlink;      /* True if symbolic link, false otherwise. */
  // uint32_t unused[124]; /* Not used. */
};

struct inode_indirect_block_sector {
  block_sector_t blocks[INDIRECT_BLOCKS];
};


/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};


void inode_init (void);
bool inode_create (block_sector_t, off_t, bool is_dir);
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

bool inode_expand (struct inode_disk *disk_inode, off_t new_length);

off_t inode_extend (struct inode_disk *disk_inode, off_t new_length);
block_sector_t byte_to_sector(const struct inode *inode, off_t pos);
bool inode_is_dir(const struct inode *inode);
struct inode *inode_open_path(const char *path);
off_t inode_length_physical(const struct inode *inode);
blkcnt_t inode_get_blocks(const struct inode *inode);
bool inode_is_removed (const struct inode *);

#endif /* filesys/inode.h */