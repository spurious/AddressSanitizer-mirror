//===-- asan_allocator.cc ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of AddressSanitizer, an address sanity checker.
//
// Implementation of ASan's memory allocator.
// Evey piece of memory (AsanChunk) allocated by the allocator
// has a left redzone of REDZONE bytes and
// a right redzone such that the end of the chunk is aligned by REDZONE
// (i.e. the right redzone is between 0 and REDZONE-1).
// The left redzone is always poisoned.
// The right redzone is poisoned on malloc, the body is poisoned on free.
// Once freed, a chunk is moved to a quarantine (fifo list).
// After quarantine, a chunk is returned to freelists.
//
// The left redzone contains ASan's internal data and the stack trace of
// the malloc call.
// Once freed, the body of the chunk contains the stack trace of the free call.
//
//===----------------------------------------------------------------------===//

#include "asan_allocator.h"
#include "asan_int.h"
#include "asan_interceptors.h"
#include "asan_mapping.h"
#include "asan_stats.h"
#include "asan_thread.h"

#include <sys/mman.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>

#define  REDZONE __asan_flag_redzone
static const size_t kMinAllocSize = REDZONE * 2;
static const size_t kMinMmapSize  = kPageSize * 1024;
static const uint64_t kMaxAvailableRam = 32ULL << 30;  // 32G
static const size_t kMaxThreadLocalQuarantine = 1 << 20;  // 1M
static const size_t kMaxSizeForThreadLocalFreeList = 1 << 17;

// Size classes less than kMallocSizeClassStep are powers of two.
// All other size classes are multiples of kMallocSizeClassStep.
static const size_t kMallocSizeClassStepLog = 26;
static const size_t kMallocSizeClassStep = 1UL << kMallocSizeClassStepLog;

#if __WORDSIZE == 32
static const size_t kMaxAllowedMallocSize = 3UL << 30;  // 3G
#else
static const size_t kMaxAllowedMallocSize = 8UL << 30;  // 8G
#endif

static void OutOfMemoryMessage(const char *mem_type, size_t size) {
  Printf("==%d== ERROR: AddressSanitizer failed to allocate "
         "0x%lx (%lu) bytes (%s) in T%d\n",
         getpid(), size, size, mem_type, AsanThread::GetCurrent()->tid());
}

static inline bool IsAligned(uintptr_t a, uintptr_t alignment) {
  return (a & (alignment - 1)) == 0;
}

static inline bool IsPowerOfTwo(size_t x) {
  return (x & (x - 1)) == 0;
}

static inline size_t Log2(size_t x) {
  CHECK(IsPowerOfTwo(x));
  return __builtin_ctzl(x);
}

static inline size_t RoundUpTo(size_t size, size_t boundary) {
  CHECK(IsPowerOfTwo(boundary));
  return (size + boundary - 1) & ~(boundary - 1);
}

static inline size_t RoundUpToPowerOfTwo(size_t size) {
  CHECK(size);
  if (IsPowerOfTwo(size)) return size;
  size_t up = __WORDSIZE - __builtin_clzl(size);
  CHECK(size < (1ULL << up));
  CHECK(size > (1ULL << (up - 1)));
  return 1UL << up;
}

static inline size_t SizeClassToSize(uint8_t size_class) {
  CHECK(size_class < kNumberOfSizeClasses);
  if (size_class <= kMallocSizeClassStepLog) {
    return 1UL << size_class;
  } else {
    return (size_class - kMallocSizeClassStepLog) * kMallocSizeClassStep;
  }
}

static inline uint8_t SizeToSizeClass(size_t size) {
  uint8_t res = 0;
  if (size <= kMallocSizeClassStep) {
    size_t rounded = RoundUpToPowerOfTwo(size);
    res = Log2(rounded);
  } else {
    res = ((size + kMallocSizeClassStep - 1) / kMallocSizeClassStep)
        + kMallocSizeClassStepLog;
  }
  CHECK(res < kNumberOfSizeClasses);
  CHECK(size <= SizeClassToSize(res));
  return res;
}

static void PoisonShadow(uintptr_t mem, size_t size, uint8_t poison) {
  CHECK(IsAligned(mem,        SHADOW_GRANULARITY));
  CHECK(IsAligned(mem + size, SHADOW_GRANULARITY));
  uintptr_t shadow_beg = MemToShadow(mem);
  uintptr_t shadow_end = MemToShadow(mem + size);
  if (poison && SHADOW_GRANULARITY == 128)
    poison = 0xff;
  __asan::real_memset((void*)shadow_beg, poison, shadow_end - shadow_beg);
}

// Given REDZONE bytes, we need to mark first size bytes
// as addressable and the rest REDZONE-size bytes as unaddressable.
static void PoisonMemoryPartialRightRedzone(uintptr_t mem, size_t size) {
  CHECK(size <= REDZONE);
  CHECK(IsAligned(mem, REDZONE));
  CHECK(IsPowerOfTwo(SHADOW_GRANULARITY));
  CHECK(IsPowerOfTwo(REDZONE));
  CHECK(REDZONE >= SHADOW_GRANULARITY);
  uint8_t *shadow = (uint8_t*)MemToShadow(mem);
  PoisonShadowPartialRightRedzone(shadow, size,
                                  REDZONE, SHADOW_GRANULARITY,
                                  kAsanHeapRightRedzoneMagic);
}

static size_t total_mmaped = 0;

static uint8_t *MmapNewPagesAndPoisonShadow(size_t size) {
  CHECK(IsAligned(size, kPageSize));
  uint8_t *res = (uint8_t*)__asan_mmap(0, size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
  total_mmaped += size;
  if (res == (uint8_t*)-1) {
    OutOfMemoryMessage(__FUNCTION__, size);
    PRINT_CURRENT_STACK();
    abort();
  }
  PoisonShadow((uintptr_t)res, size, kAsanHeapLeftRedzoneMagic);
  if (__asan_flag_debug) {
    Printf("ASAN_MMAP: ["PP", "PP")\n", res, res + size);
  }
  return res;
}

// Every chunk of memory allocated by this allocator can be in one of 3 states:
// CHUNK_AVAILABLE: the chunk is in the free list and ready to be allocated.
// CHUNK_ALLOCATED: the chunk is allocated and not yet freed.
// CHUNK_QUARANTINE: the chunk was freed and put into quarantine zone.
//
// The pseudo state CHUNK_MEMALIGN is used to mark that the address is not
// the beginning of a AsanChunk (in which case 'next' contains the address
// of the AsanChunk).
//
// The magic numbers for the enum values are taken randomly.
enum {
  CHUNK_AVAILABLE  = 0x573B,
  CHUNK_ALLOCATED  = 0x3204,
  CHUNK_QUARANTINE = 0x1978,
  CHUNK_MEMALIGN   = 0xDC68,
};

struct ChunkBase {
  uint16_t   chunk_state;
  uint8_t    size_class;
  uint32_t   offset;  // User-visible memory starts at this+offset (beg()).
  int32_t    alloc_tid;
  int32_t    free_tid;
  size_t     used_size;  // Size requested by the user.
  AsanChunk *next;

  uintptr_t   beg() { return (uintptr_t)this + offset; }
  size_t Size() { return SizeClassToSize(size_class); }
  uint8_t SizeClass() { return size_class; }
};

struct AsanChunk: public ChunkBase {
  uint32_t *compressed_alloc_stack() {
    CHECK(REDZONE >= sizeof(ChunkBase));
    return (uint32_t*)((uintptr_t)this + sizeof(ChunkBase));
  }
  uint32_t *compressed_free_stack() {
    CHECK(REDZONE >= sizeof(ChunkBase));
    return (uint32_t*)((uintptr_t)this + REDZONE);
  }

  // The left redzone after the ChunkBase is given to the alloc stack trace.
  size_t compressed_alloc_stack_size() {
    return (REDZONE - sizeof(ChunkBase)) / sizeof(uint32_t);
  }
  size_t compressed_free_stack_size() {
    return (REDZONE) / sizeof(uint32_t);
  }

  bool AddrIsInside(uintptr_t addr, size_t access_size, size_t *offset) {
    if (addr >= beg() && (addr + access_size) <= (beg() + used_size)) {
      *offset = addr - beg();
      return true;
    }
    return false;
  }

  bool AddrIsAtLeft(uintptr_t addr, size_t access_size, size_t *offset) {
    if (addr >= (uintptr_t)this && addr < beg()) {
      *offset = beg() - addr;
      return true;
    }
    return false;
  }

  bool AddrIsAtRight(uintptr_t addr, size_t access_size, size_t *offset) {
    if (addr + access_size >= beg() + used_size &&
        addr < (uintptr_t)this + Size() + REDZONE) {
      if (addr <= beg() + used_size)
        *offset = 0;
      else
        *offset = addr - (beg() + used_size);
      return true;
    }
    return false;
  }

  void DescribeAddress(uintptr_t addr, size_t access_size) {
    size_t offset;
    Printf(""PP" is located ", addr);
    if (AddrIsInside(addr, access_size, &offset)) {
      Printf("%ld bytes inside of", offset);
    } else if (AddrIsAtLeft(addr, access_size, &offset)) {
      Printf("%ld bytes to the left of", offset);
    } else if (AddrIsAtRight(addr, access_size, &offset)) {
      Printf("%ld bytes to the right of", offset);
    } else {
      Printf(" somewhere around (this is AddressSanitizer bug!)");
    }
    Printf(" %lu-byte region ["PP","PP")\n",
           used_size, beg(), beg() + used_size);
  }
};

static AsanChunk *PtrToChunk(uintptr_t ptr) {
  AsanChunk *m = (AsanChunk*)(ptr - REDZONE);
  if (m->chunk_state == CHUNK_MEMALIGN) {
    m = m->next;
  }
  return m;
}


void AsanChunkFifoList::PushList(AsanChunkFifoList *q) {
  if (last_) {
    CHECK(first_);
    CHECK(!last_->next);
    last_->next = q->first_;
    last_ = q->last_;
  } else {
    CHECK(!first_);
    last_ = q->last_;
    first_ = q->first_;
  }
  size_ += q->size();
  q->clear();
}

void AsanChunkFifoList::Push(AsanChunk *n) {
  CHECK(n->next == NULL);
  if (last_) {
    CHECK(first_);
    CHECK(!last_->next);
    last_->next = n;
    last_ = n;
  } else {
    CHECK(!first_);
    last_ = first_ = n;
  }
  size_ += n->Size();
}

AsanChunk *AsanChunkFifoList::Pop() {
  CHECK(first_);
  AsanChunk *res = first_;
  first_ = first_->next;
  if (first_ == NULL)
    last_ = NULL;
  CHECK(size_ >= res->Size());
  size_ -= res->Size();
  if (last_) {
    CHECK(!last_->next);
  }
  return res;
}

namespace {

// All pages we ever allocated.
struct PageGroup {
  uintptr_t beg;
  uintptr_t end;
  size_t size_of_chunk;
  bool InRange(uintptr_t addr) {
    return addr >= beg && addr < end;
  }
};



class MallocInfo {
 public:
  AsanChunk *AllocateChunks(uint8_t size_class, size_t n_chunks) {
    AsanChunk *m = NULL;
    AsanChunk **fl = &free_lists_[size_class];
    {
      ScopedLock lock(&mu_);
      for (size_t i = 0; i < n_chunks; i++) {
        if (!(*fl)) {
          *fl = GetNewChunks(size_class);
        }
        AsanChunk *t = *fl;
        *fl = t->next;
        t->next = m;
        CHECK(t->chunk_state == CHUNK_AVAILABLE);
        m = t;
      }
    }
    return m;
  }

  void SwallowThreadLocalMallocStorage(AsanThreadLocalMallocStorage *x,
                                       bool eat_free_lists) {
    CHECK(__asan_flag_quarantine_size > 0);
    ScopedLock lock(&mu_);
    AsanChunkFifoList *q = &x->quarantine_;
    if (q->size() > 0) {
      quarantine_.PushList(q);
      while (quarantine_.size() > __asan_flag_quarantine_size) {
        Pop();
      }
    }
    if (eat_free_lists) {
      for (size_t size_class = 0; size_class < kNumberOfSizeClasses;
           size_class++) {
        AsanChunk *m = x->free_lists_[size_class];
        while (m) {
          AsanChunk *t = m->next;
          m->next = free_lists_[size_class];
          free_lists_[size_class] = m;
          m = t;
        }
        x->free_lists_[size_class] = 0;
      }
    }
  }

  void BypassThreadLocalQuarantine(AsanChunk *chunk) {
    ScopedLock lock(&mu_);
    quarantine_.Push(chunk);
  }

  AsanChunk *FindMallocedOrFreed(uintptr_t addr, size_t access_size) {
    ScopedLock lock(&mu_);
    return FindChunkByAddr(addr);
  }

  // TODO(glider): AllocationSize() may become very slow if the size of
  // page_groups_ grows. This can be fixed by increasing kMinMmapSize,
  // but a better solution is to speed up the search somehow.
  size_t AllocationSize(uintptr_t ptr) {
    ScopedLock lock(&mu_);

    // first, check if this is our memory
    PageGroup *g = FindPageGroupUnlocked(ptr);
    if (!g) return 0;
    AsanChunk *m = PtrToChunk(ptr);
    if (m->chunk_state == CHUNK_ALLOCATED) {
      return m->used_size;
    } else {
      return 0;
    }
  }

  void PrintStatus() {
    ScopedLock lock(&mu_);
    size_t malloced = 0;

    Printf(" MallocInfo: in quarantine: %ld malloced: %ld; ",
           quarantine_.size() >> 20, malloced >> 20);
    for (size_t j = 1; j < kNumberOfSizeClasses; j++) {
      AsanChunk *i = free_lists_[j];
      if (!i) continue;
      size_t t = 0;
      for (; i; i = i->next) {
        t += i->Size();
      }
      Printf("%ld:%ld ", j, t >> 20);
    }
    Printf("\n");
  }

  PageGroup *FindPageGroup(uintptr_t addr) {
    ScopedLock lock(&mu_);
    return FindPageGroupUnlocked(addr);
  }

 private:
  PageGroup *FindPageGroupUnlocked(uintptr_t addr) {
    for (int i = 0; i < n_page_groups_; i++) {
      PageGroup *g = page_groups_[i];
      if (g->InRange(addr)) {
        return g;
      }
    }
    return NULL;
  }

  AsanChunk *FindChunkByAddr(uintptr_t addr) {
    PageGroup *g = FindPageGroupUnlocked(addr);
    if (!g) return 0;
    CHECK(g->size_of_chunk);
    uintptr_t offset_from_beg = addr - g->beg;
    uintptr_t this_chunk_addr = g->beg +
        (offset_from_beg / g->size_of_chunk) * g->size_of_chunk;
    CHECK(g->InRange(this_chunk_addr));
    AsanChunk *m = (AsanChunk*)this_chunk_addr;
    CHECK(m->chunk_state == CHUNK_ALLOCATED ||
          m->chunk_state == CHUNK_AVAILABLE ||
          m->chunk_state == CHUNK_QUARANTINE);
    uintptr_t offset = 0;
    if (m->AddrIsInside(addr, 1, &offset) ||
        m->AddrIsAtRight(addr, 1, &offset))
      return m;
    bool is_at_left = m->AddrIsAtLeft(addr, 1, &offset);
    CHECK(is_at_left);
    if (this_chunk_addr == g->beg) {
      // leftmost chunk
      return m;
    }
    uintptr_t left_chunk_addr = this_chunk_addr - g->size_of_chunk;
    CHECK(g->InRange(left_chunk_addr));
    AsanChunk *l = (AsanChunk*)left_chunk_addr;
    uintptr_t l_offset = 0;
    bool is_at_right = l->AddrIsAtRight(addr, 1, &l_offset);
    CHECK(is_at_right);
    if (l_offset < offset) {
      return l;
    }
    return m;
  }

  void Pop() {
    CHECK(quarantine_.size() > 0);
    AsanChunk *m = quarantine_.Pop();
    CHECK(m);
    // if (F_v >= 2) Printf("MallocInfo::pop %p\n", m);

    CHECK(m->chunk_state == CHUNK_QUARANTINE);
    m->chunk_state = CHUNK_AVAILABLE;
    CHECK(m->alloc_tid >= 0);
    CHECK(m->free_tid >= 0);

    size_t size_class = m->SizeClass();
    m->next = free_lists_[size_class];
    free_lists_[size_class] = m;

    if (__asan_flag_stats) {
      __asan_stats.real_frees++;
      __asan_stats.really_freed += m->used_size;
      __asan_stats.really_freed_by_size[Log2(m->Size())]++;
    }
  }

  // Get a list of newly allocated chunks.
  AsanChunk *GetNewChunks(uint8_t size_class) {
    size_t size = SizeClassToSize(size_class);
    CHECK(IsPowerOfTwo(kMinMmapSize));
    CHECK(size < kMinMmapSize || (size % kMinMmapSize) == 0);
    size_t mmap_size = std::max(size, kMinMmapSize);
    size_t n_chunks = mmap_size / size;
    CHECK(n_chunks * size == mmap_size);
    if (size < kPageSize) {
      // Size is small, just poison the last chunk.
      n_chunks--;
    } else {
      // Size is large, allocate an extra page at right and poison it.
      mmap_size += kPageSize;
    }
    CHECK(n_chunks > 0);
    uint8_t *mem = MmapNewPagesAndPoisonShadow(mmap_size);
    if (__asan_flag_stats) {
      __asan_stats.mmaps++;
      __asan_stats.mmaped += mmap_size;
      __asan_stats.mmaped_by_size[Log2(size)] += n_chunks;
    }
    AsanChunk *res = NULL;
    for (size_t i = 0; i < n_chunks; i++) {
      AsanChunk *m = (AsanChunk*)(mem + i * size);
      m->chunk_state = CHUNK_AVAILABLE;
      m->size_class = size_class;
      m->next = res;
      res = m;
    }
    PageGroup *pg = (PageGroup*)(mem + n_chunks * size);
    // This memory is already poisoned, no need to poison it again.
    pg->beg = (uintptr_t)mem;
    pg->end = pg->beg + mmap_size;
    pg->size_of_chunk = size;
    int page_group_idx = AtomicInc(&n_page_groups_) - 1;
    CHECK(page_group_idx < (int)ASAN_ARRAY_SIZE(page_groups_));
    page_groups_[page_group_idx] = pg;
    return res;
  }

  AsanChunk *free_lists_[kNumberOfSizeClasses];
  AsanChunkFifoList quarantine_;
  AsanLock mu_;

  PageGroup *page_groups_[kMaxAvailableRam / kMinMmapSize];
  int n_page_groups_;  // atomic
};

static MallocInfo malloc_info;

}  // namespace

void AsanThreadLocalMallocStorage::CommitBack() {
  malloc_info.SwallowThreadLocalMallocStorage(this, true);
}

static void Describe(uintptr_t addr, size_t access_size) {
  AsanChunk *m = malloc_info.FindMallocedOrFreed(addr, access_size);
  if (!m) return;
  m->DescribeAddress(addr, access_size);
  CHECK(m->alloc_tid >= 0);
  AsanThreadSummary *alloc_thread = AsanThread::FindByTid(m->alloc_tid);
  AsanStackTrace alloc_stack;
  AsanStackTrace::UncompressStack(&alloc_stack, m->compressed_alloc_stack(),
                                  m->compressed_alloc_stack_size());

  if (m->free_tid >= 0) {
    AsanThreadSummary *free_thread = AsanThread::FindByTid(m->free_tid);
    Printf("freed by thread T%d here:\n", free_thread->tid());
    AsanStackTrace free_stack;
    AsanStackTrace::UncompressStack(&free_stack, m->compressed_free_stack(),
                                    m->compressed_free_stack_size());
    free_stack.PrintStack();
    Printf("previously allocated by thread T%d here:\n",
           alloc_thread->tid());

    alloc_stack.PrintStack();
    AsanThread::GetCurrent()->summary()->Announce();
    free_thread->Announce();
    alloc_thread->Announce();
  } else {
    Printf("allocated by thread T%d here:\n", alloc_thread->tid());
    alloc_stack.PrintStack();
    AsanThread::GetCurrent()->summary()->Announce();
    alloc_thread->Announce();
  }
}

static uint8_t *Allocate(size_t alignment, size_t size, AsanStackTrace *stack) {
  __asan_init();
  CHECK(stack);
  if (size == 0) {
    size = 1;  // TODO(kcc): do something smarter
  }
  CHECK(IsPowerOfTwo(alignment));
  size_t rounded_size = RoundUpTo(size, REDZONE);
  size_t needed_size = rounded_size + REDZONE;
  if (alignment > REDZONE) {
    needed_size += alignment;
  }
  CHECK(IsAligned(needed_size, REDZONE));
  if (needed_size > kMaxAllowedMallocSize) {
    OutOfMemoryMessage(__FUNCTION__, size);
    stack->PrintStack();
    abort();
  }

  uint8_t size_class = SizeToSizeClass(needed_size);
  size_t size_to_allocate = SizeClassToSize(size_class);
  CHECK(size_to_allocate >= kMinAllocSize);
  CHECK(size_to_allocate >= needed_size);
  CHECK(IsAligned(size_to_allocate, REDZONE));

  if (__asan_flag_v >= 2) {
    Printf("Allocate align: %ld size: %ld class: %d real: %ld\n",
         alignment, size, size_class, size_to_allocate);
  }

  if (__asan_flag_stats) {
    __asan_stats.allocated_since_last_stats += size;
    __asan_stats.mallocs++;
    __asan_stats.malloced += size;
    __asan_stats.malloced_redzones += size_to_allocate - size;
    __asan_stats.malloced_by_size[Log2(size_to_allocate)]++;
    if (__asan_stats.allocated_since_last_stats > (1U << __asan_flag_stats)) {
      __asan_stats.PrintStats();
      malloc_info.PrintStatus();
      __asan_stats.allocated_since_last_stats = 0;
    }
  }

  AsanThread *t = AsanThread::GetCurrent();
  AsanChunk *m = NULL;
  if (!t || size_to_allocate >= kMaxSizeForThreadLocalFreeList) {
    // get directly from global storage.
    m = malloc_info.AllocateChunks(size_class, 1);
    if (__asan_flag_stats)  __asan_stats.malloc_large++;
  } else {
    // get from the thread-local storage.
    AsanChunk **fl = &t->malloc_storage().free_lists_[size_class];
    if (!*fl) {
      size_t n_new_chunks = kMaxSizeForThreadLocalFreeList / size_to_allocate;
      // n_new_chunks = std::min((size_t)32, n_new_chunks);
      *fl = malloc_info.AllocateChunks(size_class, n_new_chunks);
      if (__asan_flag_stats) __asan_stats.malloc_small_slow++;
    }
    m = *fl;
    *fl = (*fl)->next;
  }
  CHECK(m);
  CHECK(m->chunk_state == CHUNK_AVAILABLE);
  m->chunk_state = CHUNK_ALLOCATED;
  m->next = NULL;
  CHECK(m->Size() == size_to_allocate);
  uintptr_t addr = (uintptr_t)m + REDZONE;
  CHECK(addr == (uintptr_t)m->compressed_free_stack());

  if (alignment > REDZONE && (addr & (alignment - 1))) {
    addr = RoundUpTo(addr, alignment);
    CHECK((addr & (alignment - 1)) == 0);
    AsanChunk *p = (AsanChunk*)(addr - REDZONE);
    p->chunk_state = CHUNK_MEMALIGN;
    p->next = m;
  }
  CHECK(m == PtrToChunk(addr));
  m->used_size = size;
  m->offset = addr - (uintptr_t)m;
  CHECK(m->beg() == addr);
  m->alloc_tid = t ? t->tid() : 0;
  m->free_tid   = AsanThread::kInvalidTid;
  AsanStackTrace::CompressStack(stack, m->compressed_alloc_stack(),
                                m->compressed_alloc_stack_size());
  PoisonShadow(addr, rounded_size, 0);
  if (size < rounded_size) {
    PoisonMemoryPartialRightRedzone(addr + rounded_size - REDZONE,
                                    size & (REDZONE - 1));
  }
  return (uint8_t*)addr;
}

static void Deallocate(uint8_t *ptr, AsanStackTrace *stack) {
  if (!ptr) return;
  CHECK(stack);

  if (__asan_flag_debug) {
    CHECK(malloc_info.FindPageGroup((uintptr_t)ptr));
  }

  // Printf("Deallocate "PP"\n", ptr);
  AsanChunk *m = PtrToChunk((uintptr_t)ptr);
  if (m->chunk_state == CHUNK_QUARANTINE) {
    Printf("attempting double-free on %p:\n", ptr);
    stack->PrintStack();
    m->DescribeAddress((uintptr_t)ptr, 1);
    __asan_show_stats_and_abort();
  } else if (m->chunk_state != CHUNK_ALLOCATED) {
    Printf("attempting free on address which was not malloc()-ed: %p\n", ptr);
    stack->PrintStack();
    __asan_show_stats_and_abort();
  }
  CHECK(m->chunk_state == CHUNK_ALLOCATED);
  CHECK(m->free_tid == AsanThread::kInvalidTid);
  CHECK(m->alloc_tid >= 0);
  AsanThread *t = AsanThread::GetCurrent();
  m->free_tid = t ? t->tid() : 0;
  AsanStackTrace::CompressStack(stack, m->compressed_free_stack(),
                                m->compressed_free_stack_size());
  size_t rounded_size = RoundUpTo(m->used_size, REDZONE);
  PoisonShadow((uintptr_t)ptr, rounded_size, kAsanHeapFreeMagic);

  if (__asan_flag_stats) {
    __asan_stats.frees++;
    __asan_stats.freed += m->used_size;
    __asan_stats.freed_by_size[Log2(m->Size())]++;
  }

  m->chunk_state = CHUNK_QUARANTINE;
  if (t) {
    AsanThreadLocalMallocStorage *ms = &t->malloc_storage();
    CHECK(!m->next);
    ms->quarantine_.Push(m);

    if (ms->quarantine_.size() > kMaxThreadLocalQuarantine) {
      malloc_info.SwallowThreadLocalMallocStorage(ms, false);
    }
  } else {
    CHECK(!m->next);
    malloc_info.BypassThreadLocalQuarantine(m);
  }
}

static uint8_t *Reallocate(uint8_t *old_ptr, size_t new_size,
                           AsanStackTrace *stack) {
  if (!old_ptr) {
    return Allocate(0, new_size, stack);
  }
  if (new_size == 0) {
    return NULL;
  }
  if (__asan_flag_stats) {
    __asan_stats.reallocs++;
    __asan_stats.realloced += new_size;
  }
  AsanChunk *m = PtrToChunk((uintptr_t)old_ptr);
  CHECK(m->chunk_state == CHUNK_ALLOCATED);
  size_t old_size = m->used_size;
  size_t memcpy_size = std::min(new_size, old_size);
  uint8_t *new_ptr = Allocate(0, new_size, stack);
  __asan::real_memcpy(new_ptr, old_ptr, memcpy_size);
  Deallocate(old_ptr, stack);
  return new_ptr;
}



void *__asan_memalign(size_t alignment, size_t size, AsanStackTrace *stack) {
  return (void*)Allocate(alignment, size, stack);
}

void __asan_free(void *ptr, AsanStackTrace *stack) {
  Deallocate((uint8_t*)ptr, stack);
}

void *__asan_malloc(size_t size, AsanStackTrace *stack) {
  return (void*)Allocate(0, size, stack);
}

void *__asan_calloc(size_t nmemb, size_t size, AsanStackTrace *stack) {
  uint8_t *res = Allocate(0, nmemb * size, stack);
  __asan::real_memset(res, 0, nmemb * size);
  return (void*)res;
}

void *__asan_realloc(void *p, size_t size, AsanStackTrace *stack) {
  return Reallocate((uint8_t*)p, size, stack);
}

void *__asan_valloc(size_t size, AsanStackTrace *stack) {
  return Allocate(kPageSize, size, stack);
}

void *__asan_pvalloc(size_t size, AsanStackTrace *stack) {
  size = RoundUpTo(size, kPageSize);
  if (size == 0) {
    // pvalloc(0) should allocate one page.
    size = kPageSize;
  }
  return Allocate(kPageSize, size, stack);
}

int __asan_posix_memalign(void **memptr, size_t alignment, size_t size,
                          AsanStackTrace *stack) {
  *memptr = Allocate(alignment, size, stack);
  CHECK(IsAligned((uintptr_t)*memptr, alignment));
  return 0;
}

size_t __asan_mz_size(const void *ptr) {
  return malloc_info.AllocationSize((uintptr_t)ptr);
}

void __asan_describe_heap_address(uintptr_t addr, uintptr_t access_size) {
  Describe(addr, access_size);
}
size_t __asan_total_mmaped() {
  return total_mmaped;
}

// ---------------------- Fake stack-------------------- {{{1
AsanFakeStack::AsanFakeStack()
  : stack_size_(0), alive_(false) {
  __asan::real_memset(allocated_size_classes_,
                      0, sizeof(allocated_size_classes_));
  __asan::real_memset(size_classes_, 0, sizeof(size_classes_));
}

bool AsanFakeStack::AddrIsInSizeClass(uintptr_t addr, size_t size_class) {
  uintptr_t mem = allocated_size_classes_[size_class];
  uintptr_t size = ClassMmapSize(size_class);
  bool res = mem && addr >= mem && addr < mem + size;
  return res;
}

uintptr_t AsanFakeStack::AddrIsInFakeStack(uintptr_t addr) {
  for (size_t i = 0; i < kNumberOfSizeClasses; i++) {
    if (AddrIsInSizeClass(addr, i)) return allocated_size_classes_[i];
  }
  return 0;
}

// We may want to compute this during compilation.
inline size_t AsanFakeStack::ComputeSizeClass(size_t alloc_size) {
  size_t rounded_size = RoundUpToPowerOfTwo(alloc_size);
  size_t log = Log2(rounded_size);
  CHECK(alloc_size <= (1UL << log));
  CHECK(alloc_size > (1UL << (log-1)));
  size_t res = log < kMinStackFrameSizeLog ? 0 : log - kMinStackFrameSizeLog;
  CHECK(res < kNumberOfSizeClasses);
  CHECK(ClassSize(res) >= rounded_size);
  return res;
}

void AsanFakeStack::FifoList::FifoPush(uintptr_t a) {
  // Printf("T%d push "PP"\n", AsanThread::GetCurrent()->tid(), a);
  FifoNode *node = (FifoNode*)a;
  CHECK(node);
  node->next = 0;
  if (first == 0 && last == 0) {
    first = last = node;
  } else {
    CHECK(first);
    CHECK(last);
    last->next = node;
    last = node;
  }
}

uintptr_t AsanFakeStack::FifoList::FifoPop() {
  CHECK(first && last && "Exhausted fake stack");
  FifoNode *res = 0;
  if (first == last) {
    res = first;
    first = last = 0;
  } else {
    res = first;
    first = first->next;
  }
  return (uintptr_t)res;
}

void AsanFakeStack::Init(size_t stack_size) {
  stack_size_ = stack_size;
  alive_ = true;
}

void AsanFakeStack::Cleanup() {
  alive_ = false;
  for (size_t i = 0; i < kNumberOfSizeClasses; i++) {
    uintptr_t mem = allocated_size_classes_[i];
    if (mem) {
      PoisonShadow(mem, ClassMmapSize(i), 0);
      allocated_size_classes_[i] = 0;
      int munmap_res = munmap((void*)mem, ClassMmapSize(i));
      CHECK(munmap_res == 0);
    }
  }
}

size_t AsanFakeStack::ClassMmapSize(size_t size_class) {
  return RoundUpToPowerOfTwo(stack_size_);
}

void AsanFakeStack::AllocateOneSizeClass(size_t size_class) {
  CHECK(ClassMmapSize(size_class) >= kPageSize);
  uintptr_t new_mem = (uintptr_t)__asan_mmap(0, ClassMmapSize(size_class),
                                             PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANON, -1, 0);
  CHECK(new_mem != (uintptr_t)-1);
  // Printf("T%d new_mem[%ld]: "PP"-"PP" mmap %ld\n",
  //       AsanThread::GetCurrent()->tid(),
  //       size_class, new_mem, new_mem + ClassMmapSize(size_class),
  //       ClassMmapSize(size_class));
  size_t i;
  for (i = 0; i < ClassMmapSize(size_class);
       i += ClassSize(size_class)) {
    size_classes_[size_class].FifoPush(new_mem + i);
  }
  CHECK(i == ClassMmapSize(size_class));
  allocated_size_classes_[size_class] = new_mem;
}

uintptr_t AsanFakeStack::AllocateStack(size_t size) {
  CHECK(alive_);
  CHECK(size <= kMaxStackMallocSize);
  size_t size_class = ComputeSizeClass(size);
  if (!allocated_size_classes_[size_class]) {
    AllocateOneSizeClass(size_class);
  }
  uintptr_t ptr = size_classes_[size_class].FifoPop();
  CHECK(ptr);
  PoisonShadow(ptr, size, 0);
  return ptr;
}

void AsanFakeStack::DeallocateStack(uintptr_t ptr, size_t size) {
  CHECK(alive_);
  size_t size_class = ComputeSizeClass(size);
  CHECK(allocated_size_classes_[size_class]);
  CHECK(AddrIsInSizeClass(ptr, size_class));
  CHECK(AddrIsInSizeClass(ptr + size - 1, size_class));
  PoisonShadow(ptr, size, kAsanStackAfterReturnMagic);
  size_classes_[size_class].FifoPush(ptr);
}

size_t __asan_stack_malloc(size_t size, size_t real_stack) {
  AsanThread *t = AsanThread::GetCurrent();
  if (!t) {
    // TSD is gone, use the real stack.
    return real_stack;
  }
  size_t ptr = t->FakeStack().AllocateStack(size);
  // Printf("__asan_stack_malloc "PP" %ld "PP"\n", ptr, size, real_stack);
  return ptr;
}

void __asan_stack_free(size_t ptr, size_t size, size_t real_stack) {
  if (ptr == real_stack) {
    // we returned the real stack in __asan_stack_malloc, so do nothing now.
    return;
  }
  AsanThread *t = AsanThread::GetCurrent();
  if (!t) {
    // TSD is gone between __asan_stack_malloc and here.
    // The whole thread fake stack has been destructed anyway.
    return;
  }
  // Printf("__asan_stack_free   "PP" %ld "PP"\n", ptr, size, real_stack);
  t->FakeStack().DeallocateStack(ptr, size);
}
