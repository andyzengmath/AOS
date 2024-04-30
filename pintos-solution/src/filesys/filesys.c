#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/malloc.h"
#include "threads/thread.h"

// /* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done (void) { free_map_close (); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create (const char *name, off_t initial_size, bool is_dir)
{
  block_sector_t inode_sector = 0;
  // struct dir *dir = dir_open_root ();
  // split path and name
  char directory[ strlen(name) ];
  char file_name[ strlen(name) ];
  split_path_filename(name, directory, file_name);
  struct dir *dir = dir_open_path (directory);

  bool success = (dir != NULL && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, is_dir)
                  && dir_add (dir, file_name, inode_sector, is_dir));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  dir_close (dir);

  return success;
}

// bool filesys_create(const char *name, off_t initial_size) {
//   block_sector_t inode_sector = 0;
//   struct dir *dir = dir_open_root();
//   bool success = (dir != NULL && free_map_allocate(1, &inode_sector) &&
//                   inode_create(inode_sector, initial_size) &&
//                   dir_add(dir, name, inode_sector));
//   if (!success && inode_sector != 0)
//     free_map_release(inode_sector, 1);

//   dir_close(dir);

//   return success;
// }

// bool filesys_create_2(const char *name, off_t initial_size, bool is_dir) {
//   block_sector_t inode_sector = 0;
//   struct dir *dir = dir_open_root();
//   bool success = (dir != NULL && free_map_allocate(1, &inode_sector) &&
//                   inode_create_2(inode_sector, initial_size, is_dir) &&
//                   dir_add_2(dir, name, inode_sector, is_dir));
//   if (!success && inode_sector != 0)
//     free_map_release(inode_sector, 1);

//   dir_close(dir);

//   return success;
// }


/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
// struct file *filesys_open (const char *name)
// {
//   struct dir *dir = dir_open_root ();
//   struct inode *inode = NULL;

//   if (dir != NULL)
//     dir_lookup (dir, name, &inode);
//   dir_close (dir);

//   if (inode == NULL)
//     return NULL;

//   if (inode_get_symlink (inode))
//     {
//       char target[15];
//       inode_read_at (inode, target, NAME_MAX + 1, 0);
//       struct dir *root = dir_open_root ();
//       if (!dir_lookup (root, target, &inode))
//         {
//           return NULL;
//         }
//       dir_close (root);
//     }

//   return file_open (inode);
// }

struct file *filesys_open(const char *name) {
  // struct dir *dir = dir_open_root();
  // char directory[ strlen(name) ];
  // char file_name[ strlen(name) ];
  int l = strlen(name);
  if (l == 0) return NULL;

  char directory[ l + 1 ];
  char file_name[ l + 1 ];
  split_path_filename(name, directory, file_name);
  struct dir *dir = dir_open_path (directory);

  struct inode *inode = NULL;

  // removed directory handling
  if (dir == NULL) return NULL;

  if (strlen(file_name) > 0) {
    dir_lookup (dir, file_name, &inode);
    dir_close (dir);
  } else { // empty filename : just return the directory
    inode = dir_get_inode (dir);
  }

  // removed file handling
  if (inode == NULL || inode_is_removed (inode))
    return NULL;

  if (inode_get_symlink(inode)) {
    char target[NAME_MAX + 1];
    inode_read_at(inode, target, NAME_MAX + 1, 0);
    return filesys_open(target); // Recursively open the target of the symbolic link
  }

  return file_open(inode);
}


/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove (const char *name)
{
  // struct dir *dir = dir_open_root ();
  // bool success = dir != NULL && dir_remove (dir, name);
  char directory[ strlen(name) ];
  char file_name[ strlen(name) ];
  split_path_filename(name, directory, file_name);
  struct dir *dir = dir_open_path (directory);

  bool success = (dir != NULL && dir_remove (dir, file_name));
  dir_close (dir);

  return success;
}

/* Creates symbolic link LINKPATH to target file TARGET
   Returns true if symbolic link created successfully,
   false otherwise. */
bool filesys_symlink (char *target, char *linkpath)
{
  ASSERT (target != NULL && linkpath != NULL);
  bool success = filesys_create (linkpath, 15, false);
  struct file *symlink = filesys_open (linkpath);
  inode_set_symlink (file_get_inode (symlink), true);
  inode_write_at (file_get_inode (symlink), target, NAME_MAX + 1, 0);
  file_close (symlink);
  return success;
}

/* Formats the file system. */
static void do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

bool valid_filename(const char *name) {
    if (name == NULL) {
        return false;  // NULL filename is invalid
    }

    while (*name != '\0') {
        if (*name == '/') {
            return false;  // '/' character found, invalid filename
        }
        name++;
    }

    return true;  // No invalid characters found, filename is valid
}

// /* Create a directory with the given name. */
// bool filesys_create_dir (const char *name)
// {
//   block_sector_t inode_sector = 0;
//   struct dir *parent_dir = NULL;
//   char *dir_name = NULL;
//   bool success = false;

//   if (!valid_filename (name) || strchr (name, '/') != NULL)
//     return false;

//   parent_dir = dir_open_parent (name, &dir_name);
//   if (parent_dir == NULL)
//     return false;

//   success = (dir != NULL && dir_add (parent_dir, dir_name, inode_sector));
//   if (success) {
//     struct dir *new_dir = dir_open (inode_open (inode_sector));
//     if (new_dir != NULL) {
//       success = dir_add (new_dir, ".", inode_sector) && dir_add (new_dir, "..", inode_get_inumber (dir_get_inode (parent_dir)));
//       dir_close (new_dir);
//     } else {
//       success = false;
//     }
//   }

//   dir_close (parent_dir);
//   free (dir_name);
//   return success;
// }

/* Open a directory given its name. */
struct dir *filesys_open_dir (const char *name)
{
  if (!valid_filename (name) || strchr (name, '/') != NULL)
    return NULL;

  struct dir *dir = dir_open_path (name);
  return dir;
}

/* Open an inode given its path. */
struct inode *filesys_open_inode (const char *name)
{
  if (!valid_filename (name))
    return NULL;

  struct inode *inode = inode_open_path (name);
  return inode;
}

/* Change CWD for the current thread. */
bool
filesys_chdir (const char *name)
{
  struct dir *dir = dir_open_path (name);

  if(dir == NULL) {
    return false;
  }

  // switch CWD
  dir_close (thread_current()->cwd);
  thread_current()->cwd = dir;
  return true;
}


