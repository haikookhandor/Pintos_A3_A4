#include "threads/thread.h"
#include "devices/timer.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "userprog/syscall.h"
#include "vm/frame.h"
#include "vm/swap.h"
#include "vm/page.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/syscall.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes which are in sleep state */
static struct list sleeping_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };


static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */


#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.
   Also initializes the run queue and the tid lock.
   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().
   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleeping_list);
  // printf("check\n");
  frame_table__init();

  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  initial_thread->parent = NULL;
}

/* Starts the preemptive thread scheduling by enabling the interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Creates the idle thread*/
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Starts preemptive thread scheduling. */
  intr_enable ();

  /* Waits for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforcing preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.
   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.
   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocating the thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initializing the thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Initializing the child thread */
  struct child *baby = child_create (t);
  list_push_back(&thread_current()->children_list, &baby->child_elem);
  t->parent = thread_current();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);
  // printf("uu\n");
  return tid;
}

/* Create child */
struct child *
child_create (struct thread *thread_parent)
{
  struct child *baby = malloc(sizeof(struct child));
  baby->child_thread = thread_parent;
  baby->child_tid = thread_parent->tid;
  baby->exit_status = -2;
  baby->cur_status = CHILD_ALIVE;
  baby->is_first_wait = true;
  baby->loaded = false;
  return baby;
}

/* Return the thread with tid if it exists else return null */
struct thread *
search_thread (tid_t tid)
{
  struct list_elem *e;
  enum intr_level old_level;
  old_level = intr_disable();

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e))
  {
    struct thread *t = list_entry(e, struct thread, allelem);
    if (tid == t->tid) 
    {
      intr_set_level(old_level);
      return t;
    }
  }

  intr_set_level(old_level);
  return NULL;
}

/*Return child with tid if it exists in the list else return null */
struct child *
search_child (tid_t tid, struct list* children_list)
{
  struct list_elem *e;

  for (e = list_begin(children_list); e != list_end(children_list); e = list_next(e))
  {
    struct child *baby = list_entry(e, struct child, child_elem);
    if (tid == baby->child_tid)
    {
      return baby;
    }
  }

  return NULL;
}

/* Return file elem with fd if it exists else return null */
struct file_descriptor *
search_fd (int fd)
{
  struct list_elem *e;
  struct thread *cur = thread_current();
  e = list_begin(&cur->fd_list);

  while (e != list_end(&cur->fd_list))
  {
    struct file_descriptor *fd_element = list_entry(e, struct file_descriptor, fd_elem);
    if (fd == fd_element->fd)
    {
      return fd_element;
    }
    e = list_next(e);
  }

  return NULL;
}

/* Removes all children from children list of thread */
void 
remove_children (struct thread *t)
{
  struct list *child_list = &t->children_list;
  struct list_elem *e = list_begin(child_list);

  while(e != list_end(child_list))
  {
    struct list_elem *next_child = list_next(e);
    struct child *baby = list_entry(e, struct child, child_elem);
    list_remove(e);
    free(baby);
    e = next_child;
  }
}


bool
waketime_compare (struct list_elem *elem1, struct list_elem* elem2, void *aux)
{ 
  struct thread* thread1 = list_entry(elem1, struct thread, elem); 
  struct thread* thread2 = list_entry(elem2, struct thread, elem); 
  return (thread1->wake_time <= thread2->wake_time);
}

/* Puts the thread in sleep for ticks time */
void
thread_sleep (int64_t ticks)
{
  if (ticks <= 0) {
    return;
  }

  struct thread *cur = thread_current(); 
  enum intr_level old_level; 
  
  ASSERT(cur->status == THREAD_RUNNING); 

  cur->wake_time = timer_ticks() + ticks; 
  old_level = intr_disable(); /* Disable interrupt and stores its previous value */ 
  list_insert_ordered(&sleeping_list, &cur->elem, waketime_compare, NULL); 
  thread_block(); /* Blocks the current thread */
  intr_set_level(old_level); /* Set the interrupt to its previous value */
}


void
thread_wakeup ()
{ 
  if (list_empty(&sleeping_list)) {
    return;
  }

  struct list_elem *elem1; 
  struct list_elem *elem2;
  struct thread *t;
  enum intr_level old_level;
  
  elem1 = list_begin(&sleeping_list); /* First element of the sleeping_list */
  while (elem1 != list_end(&sleeping_list)) {
    elem2 = list_next(elem1);
    t = list_entry(elem1, struct thread, elem); 

    if (timer_ticks() >= t->wake_time) { 
      old_level = intr_disable(); /* Disables the interrupts and stores its previous value */
      list_remove(elem1); /* Removes the thread from sleeping_list */
      thread_unblock(t); 
      intr_set_level(old_level); 
      elem1 = elem2; 
    } else {
      break; /* Break if the current iteration thread is not to be woken up as it is in sorted order */ 
    }
  }
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().
   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)
   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it. No return. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

void
thread_set_nice (int nice UNUSED) 
{

}

int
thread_get_nice (void) 
{
  return 0;
}


int
thread_get_load_avg (void) 
{
  return 0;
}

int
thread_get_recent_cpu (void) 
{
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.
   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      intr_disable ();
      thread_block ();
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       
  function (aux);       
  thread_exit ();       
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->magic = THREAD_MAGIC;
  
  list_init(&t->fd_list);
  t->fd_length = 1;
  t->f_execute = NULL;
  list_init(&t->children_list);
  sema_init(&t->sema_load, 0);
  sema_init(&t->sema_wait, 0);
  t->parent = NULL;
  list_init(&t->mmap_list);
  t->mapid = 0;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}


static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Activates the new thread's page tables and, if the old thread is dying, destroys it to complete a thread swap.
Upon calling this procedure, we just changed the thread

Prior to PREV, interrupts were not enabled but the new thread is currently active. Normally, thread schedule() calls this function as its last step before returning, but switch entry() calls it the first time a thread is scheduled (see switch.S).
Calling printf() before the thread switch is complete is not safe. In actuality, this means that printf()s must to be added at the function's end.
The thread switch is finished after this function and its caller have both returned. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* Destroy the struct thread of the thread we moved from if it is dying. This must occur later to prevent thread exit() from tripping over itself. (We don't release initial thread because palloc() was not used to allocate its memory.) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.
   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{ 
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

uint32_t thread_stack_ofs = offsetof (struct thread, stack);

bool thread_alive(int pid){
  struct list_elem *e = list_begin(&all_list);
  while(e!=list_end(&all_list)){
    struct thread* t =list_entry(e,struct thread,allelem);
    if(t->tid == pid) return true;
    e = list_next(e);
  }
  return false;
}
