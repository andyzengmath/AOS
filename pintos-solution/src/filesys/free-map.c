#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

static struct file *free_map_file; /* Free map file. */
static struct bitmap *free_map;    /* Free map, one bit per sector. */

// P2 solution:

/* Initializes the free map. */
void free_map_init (void)
{
  free_map = bitmap_create (block_size (fs_device));
  if (free_map == NULL)
    PANIC ("bitmap creation failed--file system device is too large");
  bitmap_mark (free_map, FREE_MAP_SECTOR);
  bitmap_mark (free_map, ROOT_DIR_SECTOR);
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool free_map_allocate (size_t cnt, block_sector_t *sectorp)
{
  block_sector_t sector = bitmap_scan_and_flip (free_map, 0, cnt, false);
  if (sector != BITMAP_ERROR && free_map_file != NULL &&
      !bitmap_write (free_map, free_map_file))
    {
      bitmap_set_multiple (free_map, sector, cnt, false);
      sector = BITMAP_ERROR;
    }
  if (sector != BITMAP_ERROR)
    *sectorp = sector;
  return sector != BITMAP_ERROR;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void free_map_release (block_sector_t sector, size_t cnt)
{
  ASSERT (bitmap_all (free_map, sector, cnt));
  bitmap_set_multiple (free_map, sector, cnt, false);
  bitmap_write (free_map, free_map_file);
}

/* Opens the free map file and reads it from disk. */
void free_map_open (void)
{
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_read (free_map, free_map_file))
    PANIC ("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
void free_map_close (void) { file_close (free_map_file); }

/* Creates a new free map file on disk and writes the free map to
   it. */
void free_map_create (void)
{
  /* Create inode. */
  if (!inode_create (FREE_MAP_SECTOR, bitmap_file_size (free_map)))
    PANIC ("free map creation failed");

  /* Write bitmap to file. */
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_write (free_map, free_map_file))
    PANIC ("can't write free map");
}


// P4 tryout:
// struct lock free_map_lock;

// /* Initializes the free map. */
// void free_map_init (void)
// {
//   free_map = bitmap_create (block_size (fs_device));
//   if (free_map == NULL)
//     PANIC ("bitmap creation failed--file system device is too large");
//   bitmap_mark (free_map, FREE_MAP_SECTOR);
//   bitmap_mark (free_map, ROOT_DIR_SECTOR);

//   lock_init (&free_map_lock);
// }

// /* Allocates CNT consecutive sectors from the free map and stores
//    the first into *SECTORP.
//    Returns true if successful, false if not enough consecutive
//    sectors were available or if the free_map file could not be
//    written. */
// bool free_map_allocate (size_t cnt, block_sector_t *sectorp)
// {
//   lock_acquire (&free_map_lock);
//   block_sector_t sector = bitmap_scan_and_flip (free_map, 0, cnt, false);
//   if (sector != BITMAP_ERROR && free_map_file != NULL &&
//       !bitmap_write (free_map, free_map_file))
//     {
//       bitmap_set_multiple (free_map, sector, cnt, false);
//       sector = BITMAP_ERROR;
//     }
//   lock_release (&free_map_lock);

//   if (sector != BITMAP_ERROR)
//     *sectorp = sector;
  
//   // Adjust free map file size after allocation
//   // off_t new_size = bitmap_file_size(free_map);
//   // if (new_size > inode_length(file_get_inode(free_map_file)))
//   //   inode_extend(&file_get_inode(free_map_file)->data, new_size);

//   return sector != BITMAP_ERROR;
// }

// // /* Makes CNT sectors starting at SECTOR available for use. */
// void free_map_release (block_sector_t sector, size_t cnt)
// {
//   ASSERT (bitmap_all (free_map, sector, cnt));

//   lock_acquire (&free_map_lock);
//   bitmap_set_multiple (free_map, sector, cnt, false);
//   bitmap_write (free_map, free_map_file);
//   lock_release (&free_map_lock);
// }

// /* Opens the free map file and reads it from disk. */
// void free_map_open (void)
// {
//   lock_acquire (&free_map_lock);
//   free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
//   lock_release (&free_map_lock);

//   if (free_map_file == NULL)
//     PANIC ("can't open free map");
//   if (!bitmap_read (free_map, free_map_file))
//     PANIC ("can't read free map");
// }

// /* Writes the free map to disk and closes the free map file. */
// void free_map_close (void) {
//   lock_acquire (&free_map_lock);
//   file_close (free_map_file);
//   lock_release (&free_map_lock);
// }

// /* Creates a new free map file on disk and writes the free map to
//    it. */
// void free_map_create (void)
// {
//   /* Create inode. */
//   // if (!inode_create_2 (FREE_MAP_SECTOR, bitmap_file_size (free_map), false))
//   if (!inode_create (FREE_MAP_SECTOR, bitmap_file_size (free_map), false))
//     PANIC ("free map creation failed");

//   /* Write bitmap to file. */
//   lock_acquire (&free_map_lock);
//   free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
//   lock_release (&free_map_lock);

//   if (free_map_file == NULL)
//     PANIC ("can't open free map");
//   if (!bitmap_write (free_map, free_map_file))
//     PANIC ("can't write free map");
// }
