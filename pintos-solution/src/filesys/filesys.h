#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include <stddef.h>
#include "filesys/off_t.h"
#include "devices/block.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0 /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1 /* Root directory file inode sector. */

/* Block device that contains the file system. */
extern struct block *fs_device;

/* Count of allocated blocks. */
typedef uint32_t blkcnt_t;

/* Struct containing file status. */
struct stat
{
    size_t logical_size;            /* The logical file size of a file. */
    size_t physical_size;           /* The physical file size of a file. */
    block_sector_t inode_number;    /* The inode number of a file. */
    blkcnt_t blocks;                /* Number of blocks allocated. */
};

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);

/* Symbolic link creation */
bool filesys_symlink (char *target, char *linkpath);

// P4 tryout:
// bool filesys_create (const char *name, off_t initial_size, bool is_dir);
// bool filesys_create_dir (const char *name);
// struct dir *filesys_open_dir (const char *name);
// struct inode *filesys_open_inode (const char *name);
// bool filesys_chdir (const char *name);
// bool valid_filename(const char *name);

#endif /* filesys/filesys.h */
