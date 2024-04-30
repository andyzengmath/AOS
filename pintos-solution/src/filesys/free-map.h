#ifndef FILESYS_FREE_MAP_H
#define FILESYS_FREE_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/block.h"

/* Define the maximum number of direct and indirect block pointers per inode. */
#define DIRECT_PTRS 10
#define INDIRECT_PTRS 128
#define DOUBLY_INDIRECT_PTRS (128 * 128)

void free_map_init (void);
void free_map_read (void);
void free_map_create (void);
void free_map_open (void);
void free_map_close (void);

bool free_map_allocate (size_t, block_sector_t *);
void free_map_release (block_sector_t, size_t);

#endif /* filesys/free-map.h */
