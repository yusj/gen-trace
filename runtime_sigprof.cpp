#define __STDC_FORMAT_MACROS
// C Headers
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
// POSIX Headers
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/syscall.h>
// C++ Headers
#include <new>

#ifndef CTRACE_FILE_NAME
#define CTRACE_FILE_NAME "/sdcard/trace.json"
#endif // CTRACE_FILE_NAME
#define CRASH()                                                               \
  do                                                                          \
    {                                                                         \
      (*(int *)0xeadbaddc = 0);                                               \
    }                                                                         \
  while (0)

#ifdef CTRACE_ENABLE_STAT
int stat_find_miss = 0;
#endif // CTRACE_ENABLE_STAT
namespace
{
pthread_key_t thread_info_key;
FILE *file_to_write;
static const uint64_t invalid_time = static_cast<uint64_t> (-1);
static const int frequency = 100;
static const int ticks = 1;
static const int max_idle_times = 1000;

// for WriterThread
pthread_mutex_t writer_waitup_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile bool writer_waitup = false;
pthread_cond_t writer_waitup_cond = PTHREAD_COND_INITIALIZER;
struct Record;
struct Record *pending_records_head;

#ifdef __ARM_EABI__

struct sigcontext
{
  unsigned long trap_no;
  unsigned long error_code;
  unsigned long oldmask;
  unsigned long arm_r0;
  unsigned long arm_r1;
  unsigned long arm_r2;
  unsigned long arm_r3;
  unsigned long arm_r4;
  unsigned long arm_r5;
  unsigned long arm_r6;
  unsigned long arm_r7;
  unsigned long arm_r8;
  unsigned long arm_r9;
  unsigned long arm_r10;
  unsigned long arm_fp;
  unsigned long arm_ip;
  unsigned long arm_sp;
  unsigned long arm_lr;
  unsigned long arm_pc;
  unsigned long arm_cpsr;
  unsigned long fault_address;
};

#endif

struct ucontext
{
  unsigned long uc_flags;
  struct ucontext *uc_link;
  stack_t uc_stack;
  struct sigcontext uc_mcontext;
  sigset_t uc_sigmask; /* mask last for extensibility */
};

struct CTraceStruct
{
  uint64_t start_time_;
  uint64_t min_end_time_;
  uint64_t start_time_thread_;
  uint64_t min_end_time_thread_;
  const char *name_;
  CTraceStruct (const char *);
};

struct ThreadInfo
{
  static const int max_stack = 1000;
  int pid_;
  int tid_;
  CTraceStruct *stack_[max_stack];
  int stack_end_;
  uint64_t current_time_;
  uint64_t current_time_thread_;
  int idle_times_;
  bool blocked_;
  ThreadInfo ();
  void UpdateCurrentTime ();
  void UpdateCurrentTimeThread ();
  void SetBlocked ();
  static ThreadInfo *New ();
  static ThreadInfo *Find ();
};

static const int MAX_THREADS = 100;
char info_store_char[MAX_THREADS * sizeof (ThreadInfo)];

struct FreeListNode
{
  const struct FreeListNode *next_;
};

FreeListNode *free_head;

uint64_t GetTimesFromClock ();

void
ThreadInfo::SetBlocked ()
{
  blocked_ = true;
  idle_times_ = 0;
}

ThreadInfo *
ThreadInfo::Find ()
{
  return static_cast<ThreadInfo *> (pthread_getspecific (thread_info_key));
}

void
ThreadInfo::UpdateCurrentTime ()
{
  current_time_ = GetTimesFromClock ();
}

void
ThreadInfo::UpdateCurrentTimeThread ()
{
  current_time_thread_ += ticks * frequency;
}

ThreadInfo *
ThreadInfo::New ()
{
  ThreadInfo *free_thread_info;
  while (true)
    {
      FreeListNode *current_free = free_head;
      if (current_free == NULL)
        CRASH ();
      if (!__sync_bool_compare_and_swap (&free_head, current_free,
                                         current_free->next_))
        continue;
      free_thread_info = reinterpret_cast<ThreadInfo *> (current_free);
      break;
    }
  if (free_thread_info == NULL)
    CRASH ();
  pthread_setspecific (thread_info_key, free_thread_info);
  return new (free_thread_info) ThreadInfo ();
}

ThreadInfo::ThreadInfo ()
{
  pid_ = getpid ();
  tid_ = syscall (__NR_gettid, 0);
  stack_end_ = 0;
  idle_times_ = 0;
  current_time_thread_ = 0;
  blocked_ = true;
}

CTraceStruct::CTraceStruct (const char *name)
{
  start_time_ = invalid_time;
  name_ = name;
}

ThreadInfo *
_GetThreadInfo ()
{
  ThreadInfo *tinfo = ThreadInfo::Find ();
  if (tinfo)
    return tinfo;
  tinfo = ThreadInfo::New ();
  return tinfo;
}

uint64_t
GetTimesFromClock ()
{
  static const int64_t kMillisecondsPerSecond = 1000;
  static const int64_t kMicrosecondsPerMillisecond = 1000;
  static const int64_t kMicrosecondsPerSecond = kMicrosecondsPerMillisecond
                                                * kMillisecondsPerSecond;
  static const int64_t kNanosecondsPerMicrosecond = 1000;

  struct timespec ts_thread;
  clock_gettime (CLOCK_MONOTONIC, &ts_thread);
  return (static_cast<uint64_t> (ts_thread.tv_sec) * kMicrosecondsPerSecond)
         + (static_cast<uint64_t> (ts_thread.tv_nsec)
            / kNanosecondsPerMicrosecond);
}

ThreadInfo *
GetThreadInfo ()
{
  ThreadInfo *tinfo = _GetThreadInfo ();
  if (tinfo->blocked_)
    {
      tinfo->UpdateCurrentTime ();
      sigset_t unblock_set;
      sigemptyset (&unblock_set);
      sigaddset (&unblock_set, SIGPROF);
      sigprocmask (SIG_UNBLOCK, &unblock_set, 0);
      tinfo->blocked_ = false;
    }
  return tinfo;
}

void
DeleteThreadInfo (void *tinfo)
{
  FreeListNode *free_node = static_cast<FreeListNode *> (tinfo);
  while (true)
    {
      FreeListNode *current_free = free_head;
      free_node->next_ = current_free;
      if (__sync_bool_compare_and_swap (&free_head, current_free, free_node))
        break;
    }
}

void
MyHandler (int, siginfo_t *, void *context)
{
  // we don't use GetThreadInfo , because
  // it make no sense to deal
  // with the thread without this structure
  // created in __start_ctrace__.
  ThreadInfo *tinfo = ThreadInfo::Find ();
  if (!tinfo)
    {
      // block this signal if it does not belong to
      // the profiling threads.
      sigaddset (&static_cast<ucontext *> (context)->uc_sigmask, SIGPROF);
      return;
    }
  uint64_t old_time = tinfo->current_time_;
  tinfo->UpdateCurrentTime ();
  uint64_t current_time = tinfo->current_time_;

  uint64_t old_time_thread = tinfo->current_time_thread_;
  tinfo->UpdateCurrentTimeThread ();
  uint64_t current_time_thread = tinfo->current_time_thread_;

  if (tinfo->stack_end_ >= ThreadInfo::max_stack)
    {
      CRASH ();
    }
  for (int i = 0; i < tinfo->stack_end_;
       ++i, old_time += ticks, old_time_thread += ticks)
    {
      CTraceStruct *cur = tinfo->stack_[i];
      if (cur->start_time_ != invalid_time)
        continue;
      cur->start_time_ = old_time;
      cur->start_time_thread_ = old_time_thread;
    }
  if (tinfo->stack_end_ != 0)
    {
      tinfo->stack_[tinfo->stack_end_ - 1]->min_end_time_thread_
          = current_time_thread + ticks;

      tinfo->stack_[tinfo->stack_end_ - 1]->min_end_time_ = current_time
                                                            + ticks;
    }
  else
    {
      tinfo->idle_times_++;
      if (tinfo->idle_times_ >= max_idle_times)
        {
          // will block SIGPROF
          sigaddset (&static_cast<ucontext *> (context)->uc_sigmask, SIGPROF);
          tinfo->SetBlocked ();
          tinfo->idle_times_ = 0;
        }
    }
}

void *WriterThread (void *);

struct Initializer
{
  void
  InitFreeList ()
  {
    ThreadInfo *info_store = reinterpret_cast<ThreadInfo *> (info_store_char);
    free_head
        = reinterpret_cast<FreeListNode *> (&info_store[MAX_THREADS - 1]);
    free_head->next_ = NULL;
    for (int i = MAX_THREADS - 2; i >= 0; --i)
      {
        FreeListNode *current
            = reinterpret_cast<FreeListNode *> (&info_store[i]);
        current->next_ = free_head;
        free_head = current;
      }
  }

  Initializer ()
  {
    pthread_key_create (&thread_info_key, DeleteThreadInfo);
    InitFreeList ();
    struct sigaction myaction = { 0 };
    struct itimerval timer;
    myaction.sa_sigaction = MyHandler;
    myaction.sa_flags = SA_SIGINFO;
    sigaction (SIGPROF, &myaction, NULL);

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = frequency;
    timer.it_interval = timer.it_value;
    setitimer (ITIMER_PROF, &timer, NULL);
    file_to_write = fopen (CTRACE_FILE_NAME, "w");
    fprintf (file_to_write, "{\"traceEvents\": [");
    pthread_t my_writer_thread;
    pthread_create (&my_writer_thread, NULL, WriterThread, NULL);
  }

  ~Initializer () { fclose (file_to_write); }
};

Initializer __init__;

struct Record
{
  int pid_;
  int tid_;
  uint64_t start_time_;
  uint64_t dur_;
  uint64_t start_time_thread_;
  uint64_t dur_thread_;
  const char *name_;
  struct Record *next_;
};

struct Lock
{
  Lock (pthread_mutex_t *mutex) : mutex_ (mutex)
  {
    pthread_mutex_lock (mutex_);
  }
  ~Lock () { pthread_mutex_unlock (mutex_); }
  pthread_mutex_t *mutex_;
};

void
RecordThis (CTraceStruct *c, ThreadInfo *tinfo)
{
  Record *r = static_cast<Record *> (malloc (sizeof (Record)));
  if (!r)
    CRASH ();
  r->pid_ = tinfo->pid_;
  r->tid_ = tinfo->tid_;
  r->start_time_ = c->start_time_;
  r->start_time_thread_ = c->start_time_thread_;
  r->name_ = c->name_;
  r->dur_ = c->min_end_time_ - c->start_time_;
  r->dur_thread_ = c->min_end_time_thread_ - c->start_time_thread_;
  while (true)
    {
      Record *current_head = pending_records_head;
      r->next_ = current_head;
      if (__sync_bool_compare_and_swap (&pending_records_head, current_head,
                                        r))
        break;
    }
  {
    Lock lock (&writer_waitup_mutex);
    writer_waitup = true;
    pthread_cond_signal (&writer_waitup_cond);
  }
}

void
DoWriteRecursive (struct Record *current)
{
  if (current->next_)
    DoWriteRecursive (current->next_);

  static bool needComma = false;
  if (!needComma)
    {
      needComma = true;
    }
  else
    {
      fprintf (file_to_write, ", ");
    }
  fprintf (file_to_write,
           "{\"cat\":\"%s\", \"pid\":%d, \"tid\":%d, \"ts\":%" PRIu64 ", "
           "\"ph\":\"X\", \"name\":\"%s\", \"dur\": %" PRIu64
           ", \"tts\":%" PRIu64 ", \"tdur\":%" PRIu64 "}",
           "profile", current->pid_, current->tid_, current->start_time_,
           current->name_, current->dur_, current->start_time_thread_,
           current->dur_thread_);
  static int flushCount = 0;
  if (flushCount++ == 5)
    {
      fflush (file_to_write);
      flushCount = 0;
    }
  free (current);
}

void *
WriterThread (void *)
{
  pthread_setname_np (pthread_self (), "WriterThread");

  while (true)
    {
      Record *record_to_write;

      {
        Lock lock (&writer_waitup_mutex);
        if (writer_waitup == false)
          pthread_cond_wait (&writer_waitup_cond, &writer_waitup_mutex);
        assert (writer_waitup == true);
        writer_waitup = false;
      }
      while (pending_records_head)
        {
          while (true)
            {
              record_to_write = pending_records_head;
              if (record_to_write == NULL)
                break;
              if (__sync_bool_compare_and_swap (&pending_records_head,
                                                record_to_write, NULL))
                break;
            }
          if (record_to_write == NULL)
            break;
          DoWriteRecursive (record_to_write);
        }
    }
  return NULL;
}
}

extern "C" {
extern void __start_ctrace__ (void *c, const char *name);
extern void __end_ctrace__ (CTraceStruct *c, const char *name);
}

void
__start_ctrace__ (void *c, const char *name)
{
  if (file_to_write == 0)
    return;
  CTraceStruct *cs = new (c) CTraceStruct (name);
  ThreadInfo *tinfo = GetThreadInfo ();
  if (tinfo->stack_end_ == 0)
    {
      // always update the time in the first entry.
      // Or if it sleep too long, will make this entry looks
      // very time consuming.
      tinfo->UpdateCurrentTime ();
    }
  if (tinfo->stack_end_ < ThreadInfo::max_stack)
    {
      tinfo->stack_[tinfo->stack_end_] = cs;
    }
  tinfo->stack_end_++;
}

void
__end_ctrace__ (CTraceStruct *c, const char *name)
{
  if (file_to_write == 0)
    return;
  ThreadInfo *tinfo = GetThreadInfo ();
  tinfo->stack_end_--;
  if (tinfo->stack_end_ < ThreadInfo::max_stack)
    {
      if (c->start_time_ != invalid_time)
        {
          if (tinfo->stack_end_ == 0)
            {
              tinfo->UpdateCurrentTime ();
              c->min_end_time_ = tinfo->current_time_ + ticks;
            }
          // we should record this
          RecordThis (c, tinfo);
          if (tinfo->stack_end_ != 0)
            {
              // propagate the back's mini end time
              tinfo->stack_[tinfo->stack_end_ - 1]->min_end_time_
                  = c->min_end_time_ + ticks;
              tinfo->stack_[tinfo->stack_end_ - 1]->min_end_time_thread_
                  = c->min_end_time_thread_ + ticks;
              tinfo->current_time_ += ticks;
              tinfo->current_time_thread_ += ticks;
            }
        }
    }
}
