#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

// P4 tryout:
// #define DIRECT_BLOCKS 122
// #define INDIRECT_BLOCKS 128
// #define DOUBLY_INDIRECT_BLOCKS 128 * 128

// P2 soltuion:
/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
{
  block_sector_t start; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  bool is_symlink;      /* True if symbolic link, false otherwise. */
  uint32_t unused[124]; /* Not used. */

// P4 tryout:
//   block_sector_t direct_blocks[DIRECT_BLOCKS];  /* Direct block pointers. */
//   block_sector_t indirect_block;      /* Indirect block pointer. */
//   block_sector_t doubly_indirect_block; /* Doubly indirect block pointer. */
//   bool is_dir;
};

// struct inode_indirect_block_sector {
//   block_sector_t blocks[INDIRECT_BLOCKS];
// };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

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

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector (const struct inode *inode, off_t pos)
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init (void) { list_init (&open_inodes); }

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_symlink = false;
      if (free_map_allocate (sectors, &disk_inode->start))
        {
          block_write (fs_device, sector, disk_inode);
          if (sectors > 0)
            {
              static char zeros[BLOCK_SECTOR_SIZE];
              size_t i;

              for (i = 0; i < sectors; i++)
                block_write (fs_device, disk_inode->start + i, zeros);
            }
          success = true;
        }
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk. (Does it?  Check code.)
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          free_map_release (inode->sector, 1);
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length));
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at (struct inode *inode, void *buffer_, off_t size,
                     off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                      off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length (const struct inode *inode) { return inode->data.length; }

bool inode_get_symlink (struct inode *inode) { 
  ASSERT (inode != NULL);
  return inode->data.is_symlink; 
}

void inode_set_symlink (struct inode *inode, bool is_symlink)
{
  inode->data.is_symlink = is_symlink;
  block_write (fs_device, inode->sector, &inode->data);
}



// P4 tryout:

// static block_sector_t
// index_to_sector (const struct inode_disk *idisk, off_t index)
// {
//   off_t index_base = 0, index_limit = 0;   // base, limit for sector index
//   block_sector_t ret;

//   // (1) direct blocks
//   index_limit += DIRECT_BLOCKS * 1;
//   if (index < index_limit) {
//     return idisk->direct_blocks[index];
//   }
//   index_base = index_limit;

//   // (2) a single indirect block
//   index_limit += 1 * INDIRECT_BLOCKS;
//   if (index < index_limit) {
//     struct inode_indirect_block_sector *indirect_idisk;
//     indirect_idisk = calloc(1, sizeof(struct inode_indirect_block_sector));
//     block_read (fs_device, idisk->indirect_block, indirect_idisk);

//     ret = indirect_idisk->blocks[ index - index_base ];
//     free(indirect_idisk);

//     return ret;
//   }
//   index_base = index_limit;

//   // (3) a single doubly indirect block
//   index_limit += 1 * INDIRECT_BLOCKS * INDIRECT_BLOCKS;
//   if (index < index_limit) {
//     // first and second level block index, respecitvely
//     off_t index_first =  (index - index_base) / INDIRECT_BLOCKS;
//     off_t index_second = (index - index_base) % INDIRECT_BLOCKS;

//     // fetch two indirect block sectors
//     struct inode_indirect_block_sector *indirect_idisk;
//     indirect_idisk = calloc(1, sizeof(struct inode_indirect_block_sector));

//     block_read (fs_device, idisk->doubly_indirect_block, indirect_idisk);
//     block_read (fs_device, indirect_idisk->blocks[index_first], indirect_idisk);
//     ret = indirect_idisk->blocks[index_second];

//     free(indirect_idisk);
//     return ret;
//   }

//   // (4) what up?
//   return -1;
// }

// block_sector_t byte_to_sector(const struct inode *inode, off_t pos)
// {
//   ASSERT(inode != NULL);
//   if (0 <= pos && pos < inode->data.length) {
//     // sector index
//     off_t index = pos / BLOCK_SECTOR_SIZE;
//     return index_to_sector (&inode->data, index);
//   }
//   else
//     return -1;
// }

// /* List of open inodes, so that opening a single inode twice
//    returns the same `struct inode'. */
// static struct list open_inodes;
// struct lock inode_lock;

// /* Initializes the inode module. */
// void inode_init (void) { list_init (&open_inodes); }

// /* Initializes an inode with LENGTH bytes of data and
//    writes the new inode to sector SECTOR on the file system
//    device.
//    Returns true if successful.
//    Returns false if memory or disk allocation fails. */
// bool inode_create (block_sector_t sector, off_t length, bool is_dir)
// {
//   struct inode_disk *disk_inode = NULL;
//   bool success = false;

//   ASSERT (length >= 0);

//   /* If this assertion fails, the inode structure is not exactly
//      one sector in size, and you should fix that. */
//   ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

//   disk_inode = calloc (1, sizeof *disk_inode);
//   if (disk_inode != NULL)
//     {
//       size_t sectors = bytes_to_sectors (length);
//       disk_inode->length = length;
//       disk_inode->magic = INODE_MAGIC;
//       disk_inode->is_dir = is_dir;
//       disk_inode->is_symlink = false;
//       if (inode_allocate (disk_inode))
//         {
//           block_write (fs_device, sector, disk_inode);
//           success = true;
//         }
//       free (disk_inode);
//     }
//   return success;
// }

// /* Reads an inode from SECTOR
//    and returns a `struct inode' that contains it.
//    Returns a null pointer if memory allocation fails. */
// struct inode *inode_open (block_sector_t sector)
// {
//   struct list_elem *e;
//   struct inode *inode;

//   /* Check whether this inode is already open. */
//   for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
//        e = list_next (e))
//     {
//       inode = list_entry (e, struct inode, elem);
//       if (inode->sector == sector)
//         {
//           inode_reopen (inode);
//           return inode;
//         }
//     }

//   /* Allocate memory. */
//   inode = malloc (sizeof *inode);
//   if (inode == NULL)
//     return NULL;

//   /* Initialize. */
//   list_push_front (&open_inodes, &inode->elem);
//   inode->sector = sector;
//   inode->open_cnt = 1;
//   inode->deny_write_cnt = 0;
//   inode->removed = false;
//   block_read (fs_device, inode->sector, &inode->data);
//   return inode;
// }

// /* Reopens and returns INODE. */
// struct inode *inode_reopen (struct inode *inode)
// {
//   if (inode != NULL)
//     inode->open_cnt++;
//   return inode;
// }

// /* Returns INODE's inode number. */
// block_sector_t inode_get_inumber (const struct inode *inode)
// {
//   return inode->sector;
// }

// /* Closes INODE and writes it to disk. (Does it?  Check code.)
//    If this was the last reference to INODE, frees its memory.
//    If INODE was also a removed inode, frees its blocks. */
// void inode_close (struct inode *inode)
// {
//   /* Ignore null pointer. */
//   if (inode == NULL)
//     return;

//   /* Release resources if this was the last opener. */
//   if (--inode->open_cnt == 0)
//     {
//       /* Remove from inode list and release lock. */
//       list_remove (&inode->elem);

//       /* Deallocate blocks if removed. */
//       if (inode->removed)
//         {
//           free_map_release (inode->sector, 1);
//           inode_deallocate (inode);
//         }

//       free (inode);
//     }
// }


// /* Marks INODE to be deleted when it is closed by the last caller who
//    has it open. */
// void inode_remove (struct inode *inode)
// {
//   ASSERT (inode != NULL);
//   inode->removed = true;
// }

// /* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
//    Returns the number of bytes actually read, which may be less
//    than SIZE if an error occurs or end of file is reached. */
// off_t inode_read_at (struct inode *inode, void *buffer_, off_t size,
//                      off_t offset)
// {
//   uint8_t *buffer = buffer_;
//   off_t bytes_read = 0;
//   uint8_t *bounce = NULL;

//   while (size > 0)
//     {
//       /* Disk sector to read, starting byte offset within sector. */
//       block_sector_t sector_idx = byte_to_sector (inode, offset);
//       int sector_ofs = offset % BLOCK_SECTOR_SIZE;

//       /* Bytes left in inode, bytes left in sector, lesser of the two. */
//       off_t inode_left = inode_length (inode) - offset;
//       int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
//       int min_left = inode_left < sector_left ? inode_left : sector_left;

//       /* Number of bytes to actually copy out of this sector. */
//       int chunk_size = size < min_left ? size : min_left;
//       if (chunk_size <= 0)
//         break;

//       if (sector_idx == (block_sector_t)-1) {
//         if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
//           {
//             /* Read full sector directly into caller's buffer. */
//             block_read (fs_device, sector_idx, buffer + bytes_read);
//           }
//         else
//           {
//             /* Read sector into bounce buffer, then partially copy
//               into caller's buffer. */
//             if (bounce == NULL)
//               {
//                 bounce = malloc (BLOCK_SECTOR_SIZE);
//                 if (bounce == NULL)
//                   break;
//               }
//             block_read (fs_device, sector_idx, bounce);
//             memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
//           }
//       } else {
//         // Reading from unallocated block, fill buffer with zeros
//         memset(buffer + bytes_read, 0, chunk_size);
//       }

//       /* Advance. */
//       size -= chunk_size;
//       offset += chunk_size;
//       bytes_read += chunk_size;
//     }
//   free (bounce);

//   return bytes_read;
// }

// /* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
//    Returns the number of bytes actually written, which may be
//    less than SIZE if end of file is reached or an error occurs.
//    (Normally a write at end of file would extend the inode, but
//    growth is not yet implemented.) */
// off_t inode_write_at (struct inode *inode, const void *buffer_, off_t size,
//                       off_t offset)
// {
//   const uint8_t *buffer = buffer_;
//   off_t bytes_written = 0;
//   uint8_t *bounce = NULL;

//   if (inode->deny_write_cnt)
//     return 0;

//   // beyond the EOF: extend the file
//   if( byte_to_sector(inode, offset + size - 1) == -1u ) {
//     // extend and reserve up to [offset + size] bytes
//     bool success;
//     success = inode_reserve (& inode->data, offset + size);
//     if (!success) return 0;  // fail?

//     // write back the (extended) file size
//     inode->data.length = offset + size;
//     block_write (fs_device, inode->sector, & inode->data);
//   }

//   while (size > 0)
//     {
//       /* Sector to write, starting byte offset within sector. */
//       block_sector_t sector_idx = byte_to_sector (inode, offset);
//       int sector_ofs = offset % BLOCK_SECTOR_SIZE;

//       /* Bytes left in inode, bytes left in sector, lesser of the two. */
//       off_t inode_left = inode_length (inode) - offset;
//       int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
//       int min_left = inode_left < sector_left ? inode_left : sector_left;

//       /* Number of bytes to actually write into this sector. */
//       int chunk_size = size < min_left ? size : min_left;
//       if (chunk_size <= 0)
//         break;

//       if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
//         {
//           /* Write full sector directly to disk. */
//           block_write (fs_device, sector_idx, buffer + bytes_written);
//         }
//       else
//         {
//           /* We need a bounce buffer. */
//           if (bounce == NULL)
//             {
//               bounce = malloc (BLOCK_SECTOR_SIZE);
//               if (bounce == NULL)
//                 break;
//             }

//           /* If the sector contains data before or after the chunk
//              we're writing, then we need to read in the sector
//              first.  Otherwise we start with a sector of all zeros. */
//           if (sector_ofs > 0 || chunk_size < sector_left)
//             block_read (fs_device, sector_idx, bounce);
//           else
//             memset (bounce, 0, BLOCK_SECTOR_SIZE);
//           memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
//           block_write (fs_device, sector_idx, bounce);
//         }

//       /* Advance. */
//       size -= chunk_size;
//       offset += chunk_size;
//       bytes_written += chunk_size;
//     }
//   free (bounce);

//   return bytes_written;
// }

// /* Disables writes to INODE.
//    May be called at most once per inode opener. */
// void inode_deny_write (struct inode *inode)
// {
//   inode->deny_write_cnt++;
//   ASSERT (inode->deny_write_cnt <= inode->open_cnt);
// }

// /* Re-enables writes to INODE.
//    Must be called once by each inode opener who has called
//    inode_deny_write() on the inode, before closing the inode. */
// void inode_allow_write (struct inode *inode)
// {
//   ASSERT (inode->deny_write_cnt > 0);
//   ASSERT (inode->deny_write_cnt <= inode->open_cnt);
//   inode->deny_write_cnt--;
// }

// /* Returns the length, in bytes, of INODE's data. */
// off_t inode_length (const struct inode *inode) { return inode->data.length; }

// bool inode_get_symlink (struct inode *inode) { 
//   ASSERT (inode != NULL);
//   return inode->data.is_symlink; 
// }

// void inode_set_symlink (struct inode *inode, bool is_symlink)
// {
//   inode->data.is_symlink = is_symlink;
//   block_write (fs_device, inode->sector, &inode->data);
// }

// bool inode_expand (struct inode_disk *disk_inode, off_t new_length)
// {
//   off_t old_length = disk_inode->length;
//   off_t sectors_needed = bytes_to_sectors(new_length) - bytes_to_sectors(old_length);
//   if (sectors_needed <= 0)
//     return true; // No expansion needed

//   size_t i;

//   // Direct blocks
//   for (i = 0; i < DIRECT_BLOCKS && sectors_needed > 0; ++i)
//   {
//     if (disk_inode->direct_blocks[i] == 0)
//     {
//       if (!free_map_allocate(1, &disk_inode->direct_blocks[i]))
//         return false;
//       --sectors_needed;
//     }
//   }

//   // Indirect block
//   if (sectors_needed > 0 && disk_inode->indirect_block == 0)
//   {
//     if (!free_map_allocate(1, &disk_inode->indirect_block))
//       return false;
//     --sectors_needed;
//   }

//   // Doubly indirect block
//   if (sectors_needed > 0 && disk_inode->doubly_indirect_block == 0)
//   {
//     if (!free_map_allocate(1, &disk_inode->doubly_indirect_block))
//       return false;
//     --sectors_needed;
//   }

//   return sectors_needed <= 0;
// }

// off_t inode_extend (struct inode_disk *disk_inode, off_t new_length)
// {
//   if (new_length > disk_inode->length)
//   {
//     if (!inode_expand(disk_inode, new_length))
//       return disk_inode->length;
//     disk_inode->length = new_length;
//   }
//   return disk_inode->length;
// }

// bool inode_is_dir(const struct inode *inode)
// {
//     return inode != NULL && inode->data.is_dir;
// }

// /* Open an inode given its path. */
// struct inode *inode_open_path(const char *path)
// {
//     struct inode *inode = NULL;
//     struct dir *dir = NULL;
//     char *path_copy = NULL;
//     char *token, *save_ptr;

//     /* Check if path is not null */
//     if (path == NULL)
//         return NULL;

//     /* Make a copy of path for tokenizing */
//     path_copy = malloc(strlen(path) + 1);
//     if (path_copy == NULL)
//         return NULL;
//     strlcpy(path_copy, path, strlen(path) + 1);

//     /* Tokenize the path to traverse directories */
//     token = strtok_r(path_copy, "/", &save_ptr);
//     if (token == NULL) {
//         free(path_copy);
//         return NULL; // Empty path
//     }

//     /* Open the root directory initially */
//     dir = dir_open_root();
//     if (dir == NULL) {
//         free(path_copy);
//         return NULL; // Failed to open root directory
//     }

//     /* Traverse through each token in the path */
//     while (token != NULL && dir != NULL) {
//         /* Lookup the entry corresponding to the token */
//         if (!dir_lookup(dir, token, &inode)) {
//             dir_close(dir);
//             free(path_copy);
//             return NULL; // Entry not found
//         }

//         /* Check if the inode is a directory */
//         if (inode_is_dir(inode)) {
//             /* Close the current directory and open the subdirectory */
//             dir_close(dir);
//             dir = dir_open(inode);
//         } else {
//             /* Not a directory, close the directory */
//             dir_close(dir);
//             dir = NULL;
//         }

//         /* Move to the next token */
//         token = strtok_r(NULL, "/", &save_ptr);
//     }

//     free(path_copy);
//     return inode;
// }

// /* Returns the physical length of the inode's data. */
// off_t inode_length_physical(const struct inode *inode) {
//     lock_acquire(&inode_lock);
//     off_t length = inode->data.length;
//     lock_release(&inode_lock);
//     return length;
// }

// /* Returns the number of blocks allocated to the inode. */
// blkcnt_t inode_get_blocks(const struct inode *inode) {
//     lock_acquire(&inode_lock);
//     off_t length = inode->data.length;
//     blkcnt_t blocks = (length + BLOCK_SECTOR_SIZE - 1) / BLOCK_SECTOR_SIZE;
//     lock_release(&inode_lock);
//     return blocks;
// }

// /* Returns whether the file is removed or not. */
// bool
// inode_is_removed (const struct inode *inode)
// {
//   return inode->removed;
// }

// static inline size_t
// min (size_t a, size_t b)
// {
//   return a < b ? a : b;
// }

// static
// bool inode_allocate (struct inode_disk *disk_inode)
// {
//   return inode_reserve (disk_inode, disk_inode->length);
// }

// static bool
// inode_reserve_indirect (block_sector_t* p_entry, size_t num_sectors, int level)
// {
//   static char zeros[BLOCK_SECTOR_SIZE];

//   // only supports 2-level indirect block scheme as of now
//   ASSERT (level <= 2);

//   if (level == 0) {
//     // base case : allocate a single sector if necessary and put it into the block
//     if (*p_entry == 0) {
//       if(! free_map_allocate (1, p_entry))
//         return false;
//       block_write (fs_device, *p_entry, zeros);
//     }
//     return true;
//   }

//   struct inode_indirect_block_sector indirect_block;
//   if(*p_entry == 0) {
//     // not yet allocated: allocate it, and fill with zero
//     free_map_allocate (1, p_entry);
//     block_write (fs_device,*p_entry, zeros);
//   }
//   block_read (fs_device, *p_entry, &indirect_block);

//   size_t unit = (level == 1 ? 1 : INDIRECT_BLOCKS);
//   size_t i, l = DIV_ROUND_UP (num_sectors, unit);

//   for (i = 0; i < l; ++ i) {
//     size_t subsize = min(num_sectors, unit);
//     // inode_reserve_indirect (& indirect_block.blocks[i], subsize, level - 1);
//     if(! inode_reserve_indirect (& indirect_block.blocks[i], subsize, level - 1))
//       return false;
//     num_sectors -= subsize;
//   }

//   ASSERT (num_sectors == 0);
//   block_write (fs_device, *p_entry, &indirect_block);
//   return true;
// }

// static bool
// inode_reserve (struct inode_disk *disk_inode, off_t length)
// {
//   static char zeros[BLOCK_SECTOR_SIZE];

//   if (length < 0) return false;

//   // (remaining) number of sectors, occupied by this file.
//   size_t num_sectors = bytes_to_sectors(length);
//   size_t i, l;

//   // (1) direct blocks
//   l = min(num_sectors, DIRECT_BLOCKS * 1);
//   for (i = 0; i < l; ++ i) {
//     if (disk_inode->direct_blocks[i] == 0) { // unoccupied
//       // free_map_allocate (1, &disk_inode->direct_blocks[i]);
//       if(! free_map_allocate (1, &disk_inode->direct_blocks[i]))
//         return false;
//       block_write (fs_device, disk_inode->direct_blocks[i], zeros);
//     }
//   }
//   num_sectors -= l;
//   if(num_sectors == 0) return true;

//   // (2) a single indirect block
//   l = min(num_sectors, 1 * INDIRECT_BLOCKS);
//   // inode_reserve_indirect (& disk_inode->indirect_block, l, 1);
//   if(! inode_reserve_indirect (& disk_inode->indirect_block, l, 1))
//     return false;
//   num_sectors -= l;
//   if(num_sectors == 0) return true;

//   // (3) a single doubly indirect block
//   l = min(num_sectors, 1 * INDIRECT_BLOCKS * INDIRECT_BLOCKS);
//   // inode_reserve_indirect (& disk_inode->doubly_indirect_block, l, 2);
//   if(! inode_reserve_indirect (& disk_inode->doubly_indirect_block, l, 2))
//     return false;
//   num_sectors -= l;
//   if(num_sectors == 0) return true;

//   ASSERT (num_sectors == 0);
//   return false;
// }

// static void
// inode_deallocate_indirect (block_sector_t entry, size_t num_sectors, int level)
// {
//   // only supports 2-level indirect block scheme as of now
//   ASSERT (level <= 2);

//   if (level == 0) {
//     free_map_release (entry, 1);
//     return;
//   }

//   struct inode_indirect_block_sector indirect_block;
//   block_read (fs_device, entry, &indirect_block);

//   size_t unit = (level == 1 ? 1 : INDIRECT_BLOCKS);
//   size_t i, l = DIV_ROUND_UP (num_sectors, unit);

//   for (i = 0; i < l; ++ i) {
//     size_t subsize = min(num_sectors, unit);
//     inode_deallocate_indirect (indirect_block.blocks[i], subsize, level - 1);
//     num_sectors -= subsize;
//   }

//   ASSERT (num_sectors == 0);
//   free_map_release (entry, 1);
// }

// static
// bool inode_deallocate (struct inode *inode)
// {
//   off_t file_length = inode->data.length; // bytes
//   if(file_length < 0) return false;

//   // (remaining) number of sectors, occupied by this file.
//   size_t num_sectors = bytes_to_sectors(file_length);
//   size_t i, l;

//   // (1) direct blocks
//   l = min(num_sectors, DIRECT_BLOCKS * 1);
//   for (i = 0; i < l; ++ i) {
//     free_map_release (inode->data.direct_blocks[i], 1);
//   }
//   num_sectors -= l;

//   // (2) a single indirect block
//   l = min(num_sectors, 1 * INDIRECT_BLOCKS);
//   if(l > 0) {
//     inode_deallocate_indirect (inode->data.indirect_block, l, 1);
//     num_sectors -= l;
//   }

//   // (3) a single doubly indirect block
//   l = min(num_sectors, 1 * INDIRECT_BLOCKS * INDIRECT_BLOCKS);
//   if(l > 0) {
//     inode_deallocate_indirect (inode->data.doubly_indirect_block, l, 2);
//     num_sectors -= l;
//   }

//   ASSERT (num_sectors == 0);
//   return true;
// }


