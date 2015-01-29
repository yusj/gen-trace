#define __STDC_FORMAT_MACROS
// C Headers
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
// POSIX Headers
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>
// C++ Headers
#include <new>
#include <vector>
#include "log.h"

#ifdef __ANDROID__
#define CTRACE_FILE_NAME "/sdcard/trace_%d.json"
#else
#define CTRACE_FILE_NAME "trace_%d.json"
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
static const int64_t invalid_time = static_cast<int64_t> (-1);
// frequency in nano.
static const int frequency = 100 * 1000;
static const int ticks = 1;
// time facilities, in microsec.
static const int64_t min_interval = 1000;
static volatile int64_t s_time = 0;

// for WriterThread
pthread_mutex_t writer_waitup_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile bool writer_waitup = false;
pthread_cond_t writer_waitup_cond = PTHREAD_COND_INITIALIZER;
struct Record;
struct Record *pending_records_head;

struct CTraceStruct
{
  int64_t start_time_;
  int64_t end_time_;
  const char *name_;
  void *ret_addr_;
  CTraceStruct (const char *, int64_t start_time, void *ret_addr);
  CTraceStruct ();
};

struct ThreadInfo
{
  static const int max_stack = 1000;
  int pid_;
  int tid_;
  int64_t virtual_time_;
  CTraceStruct stack_[max_stack];
  int stack_end_;
  bool at_work_;
  struct ThreadInfo *prev_;

  ThreadInfo ();
  int64_t UpdateVirtualTime (bool fromStart);
  static ThreadInfo *New ();
  static ThreadInfo *Find ();
  void Push (const char *name, int64_t start_time, void *ret_addr);
  CTraceStruct Pop ();
};

static const int MAX_THREADS = 1000;
char info_store_char[MAX_THREADS * sizeof (ThreadInfo)];

struct FreeListNode
{
  const struct FreeListNode *next_;
};

FreeListNode *free_head;

ThreadInfo *
ThreadInfo::Find ()
{
  return static_cast<ThreadInfo *> (pthread_getspecific (thread_info_key));
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
  virtual_time_ = 0;
  at_work_ = false;
  stack_end_ = 0;
  prev_ = nullptr;
}

void
ThreadInfo::Push (const char *name, int64_t start_time, void *ret_addr)
{
  if (stack_end_ >= max_stack)
    {
      CRASH ();
    }
  stack_[stack_end_++] = CTraceStruct (name, start_time, ret_addr);
}

CTraceStruct
ThreadInfo::Pop ()
{
  if (stack_end_ == 0)
    {
      CRASH ();
    }
  return stack_[--stack_end_];
}

int64_t
ThreadInfo::UpdateVirtualTime (bool fromStart)
{
  int64_t tmp = s_time;
  if (virtual_time_ >= tmp)
    {
      if (fromStart)
        {
          // return the original value.
          return ++virtual_time_;
        }
    }
  else
    {
      virtual_time_ = tmp;
    }
  return virtual_time_;
}

CTraceStruct::CTraceStruct (const char *name, int64_t start_time,
                            void *ret_addr)
{
  name_ = name;
  start_time_ = start_time;
  ret_addr_ = ret_addr;
}

CTraceStruct::CTraceStruct () {}

ThreadInfo *
_GetThreadInfo ()
{
  ThreadInfo *tinfo = ThreadInfo::Find ();
  if (tinfo)
    return tinfo;
  tinfo = ThreadInfo::New ();
  return tinfo;
}

ThreadInfo *
GetThreadInfo ()
{
  ThreadInfo *tinfo = _GetThreadInfo ();
  return tinfo;
}

static void
delete_one_thread_info (ThreadInfo *tinfo)
{
  tinfo->~ThreadInfo ();
  FreeListNode *free_node = reinterpret_cast<FreeListNode *> (tinfo);
  while (true)
    {
      FreeListNode *current_free = free_head;
      free_node->next_ = current_free;
      if (__sync_bool_compare_and_swap (&free_head, current_free, free_node))
        break;
    }
}

static void
delete_thread_infos (void *tinfo)
{
  ThreadInfo *curr = static_cast<ThreadInfo *> (tinfo);
  while (curr)
    {
      ThreadInfo *prev = curr->prev_;
      delete_one_thread_info (curr);
      curr = prev;
    }
}

void *WriterThread (void *);

static void *
timer_func (void *)
{
  pthread_setname_np (pthread_self (), "timer_update");
  struct timespec req;
  req.tv_sec = 0;
  req.tv_nsec = frequency;
  while (true)
    {
      struct timespec rem, *wait_for = &req;
      int nanosleep_ret;

      do
        {
          nanosleep_ret = nanosleep (wait_for, &rem);
          wait_for = &rem;
        }
      while (nanosleep_ret == -1 && EINTR == errno);
      s_time += frequency;
    }
  return nullptr;
}

struct Initializer
{
  void
  InitFreeList ()
  {
    ThreadInfo *info_store = reinterpret_cast<ThreadInfo *> (info_store_char);
    free_head
        = reinterpret_cast<FreeListNode *> (&info_store[MAX_THREADS - 1]);
    free_head->next_ = nullptr;
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
    pthread_key_create (&thread_info_key, delete_thread_infos);
    InitFreeList ();

    char buffer[256];
    sprintf (buffer, CTRACE_FILE_NAME, getpid ());
    file_to_write = fopen (buffer, "w");
    fprintf (file_to_write, "{\"traceEvents\": [");
    pthread_t my_writer_thread;
    pthread_create (&my_writer_thread, nullptr, WriterThread, nullptr);
    pthread_detach (my_writer_thread);
    // timer initialize, the timer_func is used to update s_time in a pthread.
    pthread_t thread_timer;
    if (-1 == pthread_create (&thread_timer, nullptr, timer_func, nullptr))
      {
        LOGE ("timer thread fails to start: %s\n", strerror (errno));
        CRASH ();
      }
    // No need to wait or join thread_timer.
    pthread_detach (thread_timer);
  }

  ~Initializer () { fclose (file_to_write); }
};

Initializer __init__;

struct Record
{
  int pid_;
  int tid_;
  int64_t start_time_;
  int64_t dur_;
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
  r->name_ = c->name_;
  r->dur_ = c->end_time_ - c->start_time_;
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
  while (current)
    {
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
               "\"ph\":\"X\", \"name\":\"%.128s\", \"dur\": %" PRIu64 "}",
               "profile", current->pid_, current->tid_, current->start_time_,
               current->name_, current->dur_);
      static int flushCount = 0;
      if (flushCount++ == 5)
        {
          fflush (file_to_write);
          flushCount = 0;
        }
      struct Record *tmp = current;
      current = current->next_;
      free (tmp);
    }
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
              if (record_to_write == nullptr)
                break;
              if (__sync_bool_compare_and_swap (&pending_records_head,
                                                record_to_write, nullptr))
                break;
            }
          if (record_to_write == nullptr)
            break;
          DoWriteRecursive (record_to_write);
        }
    }
  return nullptr;
}
}

extern "C" {
extern void __start_ctrace__ (void *original_ret, const char *name);
extern void *__end_ctrace__ (const char *);
}

#if 0
static void print_data (char *data);
void
print_data (char *data)
{
  char *data_1 = data - 0x40;
  char *data_2 = reinterpret_cast<char *> (
      (reinterpret_cast<intptr_t> (data_1) + 4095UL) & ~(4095UL));
  if (data_2 < data)
    data_1 = data_2;

  int sum = 0;
  int len = 0x90;
  intptr_t data_3 = reinterpret_cast<intptr_t> (data_1);
  if ((data_3 & 4095) + len >= 4096)
    {
      len = 4096 - (data_3 & 4095);
    }
  for (int i = 0; i < len; ++i)
    {
      sum += snprintf (nullptr, 0, "\\x%x", data_1[i]);
    }
  char buf[sum + 1];
  int sum_1 = 0;
  for (int i = 0; i < len; ++i)
    {
      sum_1 += snprintf (buf + sum_1, sum + 1 - sum_1, "\\x%x", data_1[i]);
    }
  LOGI ("%p:%p:%s\n", data_1, data, buf);
}
#endif

void
__start_ctrace__ (void *original_ret, const char *name)
{
  if (file_to_write == 0)
    return;
  ThreadInfo *tinfo = GetThreadInfo ();
  if (tinfo->at_work_)
    {
      ThreadInfo *prev = tinfo;
      tinfo = ThreadInfo::New ();
      tinfo->prev_ = prev;
    }
  tinfo->at_work_ = true;
  int64_t currentTime = tinfo->UpdateVirtualTime (true);

  tinfo->Push (name, currentTime, original_ret);
  tinfo->at_work_ = false;
}

void *
__end_ctrace__ (const char *name)
{
  ThreadInfo *tinfo = GetThreadInfo ();

  if (tinfo->at_work_)
    CRASH ();
  tinfo->at_work_ = true;

  CTraceStruct c;
  while (true)
    {
      c = tinfo->Pop ();
      int64_t currentTime = tinfo->UpdateVirtualTime (false);
      if (file_to_write != nullptr)
        {
          if (c.start_time_ != invalid_time)
            {
              // we should record this
              c.end_time_ = currentTime;
              if (c.end_time_ - c.start_time_ >= min_interval)
                RecordThis (&c, tinfo);
            }
        }
      if (c.name_ == name)
        {
          break;
        }
    }

  void *ret = c.ret_addr_;
  if (tinfo->stack_end_ == 0 && tinfo->prev_)
    {
      // restore to prev
      pthread_setspecific (thread_info_key, tinfo->prev_);
      delete_one_thread_info (tinfo);
      return ret;
    }
  tinfo->at_work_ = false;
  return ret;
}
