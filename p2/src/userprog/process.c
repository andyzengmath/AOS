#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "lib/string.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include "vm/frame.h"

#define ALIGN(ADDR) ((void *) ((uintptr_t) ADDR - (uintptr_t) ADDR % 4))

static thread_func start_process NO_RETURN;

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t process_execute (const char *file_name)
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  // Get executable name for thread create
  char *save_ptr;
  const char delimiter[7] = " \t\r\n\f\v";
  int len = strlen (file_name);
  char args_copy[len + 1];
  strlcpy (args_copy, file_name, len + 1);
  char *exec_name = strtok_r ((char *) args_copy, delimiter, &save_ptr);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (exec_name, PRI_DEFAULT, start_process, fn_copy);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process (void *filename)
{
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (filename, &if_.eip, &if_.esp);

  if (!is_kernel_vaddr (filename))
    palloc_free_page (filename);

  thread_current ()->parent->success = success;
  sema_up (&thread_current ()->parent->child_created);

  /* If load failed, quit. */
  palloc_free_page (filename);
  if (!success)
    {
      exit (-1);
    }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait (tid_t child_tid UNUSED)
{
  struct thread *parent = thread_current ();
  struct list *children = &parent->children;

  if (list_empty (children))
    {
      return -1;
    }

  // Find child if in children list and wait on its exit returning status
  struct list_elem *curr;
  for (curr = list_front (children); curr != list_end (children);
       curr = list_next (curr))
    {
      struct child *curr_item = list_entry (curr, struct child, elem);
      if (curr_item->pid == child_tid)
        {
          sema_down (&curr_item->exited);
          list_remove (curr);
          int status = curr_item->exit_status;
          free (curr_item);
          return status;
        }
    }
  return -1;
}

/* Free the current process's resources. */
void process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack (void **esp, const char *filename);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load (const char *args, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int argc;

  char *save_ptr;
  const char delimiter[7] = " \t\r\n\f\v";
  int len = strlen (args);
  char args_copy[len + 1];
  strlcpy (args_copy, args, len + 1);
  char *filename = strtok_r ((char *) args_copy, delimiter, &save_ptr);

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (filename);
  if (file == NULL)
    {
      printf ("load: %s: open failed\n", filename);
      goto done;
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 ||
      ehdr.e_machine != 3 || ehdr.e_version != 1 ||
      ehdr.e_phentsize != sizeof (struct Elf32_Phdr) || ehdr.e_phnum > 1024)
    {
      printf ("load: %s: error loading executable\n", filename);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (argc = 0; argc < ehdr.e_phnum; argc++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      
      // [placeholder] - it is turtles all the way down
      
      switch (phdr.p_type)
        {
          case PT_NULL:
          case PT_NOTE:
          case PT_PHDR:
          case PT_STACK:
          default:
            /* Ignore this segment. */
            break;
          case PT_DYNAMIC:
          case PT_INTERP:
          case PT_SHLIB:
            goto done;
          case PT_LOAD:
            if (validate_segment (&phdr, file))
              {
                bool writable = (phdr.p_flags & PF_W) != 0;
                uint32_t file_page = phdr.p_offset & ~PGMASK;
                uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint32_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0)
                  {
                    /* Normal segment.
                       Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes =
                        (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE) -
                         read_bytes);
                  }
                else
                  {
                    /* Entirely zero.
                       Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                  }
                if (!load_segment (file, file_page, (void *) mem_page,
                                   read_bytes, zero_bytes, writable))
                  goto done;
              }
            else
              goto done;
            break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, args))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable)
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false;
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable))
        {
          palloc_free_page (kpage);
          return false;
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

static bool decrement_stack (char **char_esp, void *stackpg, char **tokens,
                             int d)
{
  *char_esp -= d;
  bool valid_dec =
      ((uintptr_t) *char_esp >= (uintptr_t) PHYS_BASE - (uintptr_t) PGSIZE);
  if (!valid_dec)
    {
      palloc_free_page (tokens);
      palloc_free_page (stackpg);
    }
  return valid_dec;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack (void **esp, const char *filename)
{
  uint8_t *kpage;
  bool success = false;
  char *file_name = (char *) filename;

  char **tokens = palloc_get_page (0);
  if (tokens == NULL)
    return false;
  char *save_ptr;
  const char delimiter[7] = " \t\r\n\f\v";
  tokens[0] = strtok_r (file_name, delimiter, &save_ptr);
  int argc = 0;
  while (tokens[argc])
    {
      argc++;
      if (argc > 128)
        {
          return false;
        }
      tokens[argc] = strtok_r (NULL, delimiter, &save_ptr);
    }

  kpage = frame_alloc_and_lock (PAL_USER | PAL_ZERO);
  if (kpage != NULL)
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        {
          *esp = PHYS_BASE;
          char *char_esp = (char *) *esp;
          int i = argc - 1;
          while (i >= 0)
            {
              int len = strlen (tokens[i]) + 1;
              if (!decrement_stack (&char_esp, kpage, tokens, len))
                {
                  return false;
                }
              memcpy (char_esp, tokens[i], len);
              i--;
            }

          char_esp = ALIGN (char_esp); // align down to nearest multiple of 4

          if (!decrement_stack (&char_esp, kpage, tokens, 4))
            {
              return false;
            }
          int *arg_end = (int *) char_esp;
          *arg_end = 0;

          i = argc - 1;
          char *top = *esp;
          while (i >= 0)
            {
              if (!decrement_stack (&char_esp, kpage, tokens, 4))
                {
                  return false;
                }
              top -= strlen (tokens[i]) + 1;
              memcpy (char_esp, &top, 4);
              i--;
            }
          char *arg_start = char_esp;
          if (!decrement_stack (&char_esp, kpage, tokens, 4))
            {
              return false;
            }
          memcpy (char_esp, &arg_start, 4);
          if (!decrement_stack (&char_esp, kpage, tokens, 4))
            {
              return false;
            }
          memcpy (char_esp, &argc, 4);
          if (!decrement_stack (&char_esp, kpage, tokens, 4))
            {
              return false;
            }
          *esp = char_esp;
          palloc_free_page (tokens);
        }
      else
        {
          palloc_free_page (tokens);
          frame_unlock(kpage);
        }
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL &&
          pagedir_set_page (t->pagedir, upage, kpage, writable));
}
