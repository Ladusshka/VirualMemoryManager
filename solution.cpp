#ifndef __PROGTEST__
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <semaphore.h>
#include "common.h"
using namespace std;
#endif /* __PROGTEST__ */



//-------------------------------------------------------------------------------------------------
class CMyCpu;

// Per-process slot.
struct TSlot
{
  pthread_t  m_Tid;
  bool       m_Used;
  bool       m_Finished;
  CMyCpu   * m_Cpu;
};

// Global state for one MemMgr invocation.
struct TMemState
{
  uint8_t        * m_MemStart;
  uint32_t         m_TotalPages;
  uint32_t         m_FreePages;
  uint8_t        * m_Bitmap;       // bit=1 => free, bit=0 => used; one bit per page
  uint32_t         m_BitmapHint;
  uint16_t       * m_Refcount;     // refcount per page frame; lives inside managed block

  pthread_mutex_t  m_Mtx;
  pthread_cond_t   m_Cv;

  TSlot            m_Slots [ PROCESS_MAX ];
  uint32_t         m_ActiveCount;
};

static TMemState * g_State = NULL;

// Allocate one free page; caller holds mutex. Returns page index or -1.
static int32_t     allocPage                               ( void )
{
  if ( g_State->m_FreePages == 0 ) return -1;
  uint32_t total = g_State->m_TotalPages;
  for ( uint32_t k = 0; k < total; k++ )
  {
    uint32_t idx = g_State->m_BitmapHint + k;
    if ( idx >= total ) idx -= total;
    if ( g_State->m_Bitmap[idx >> 3] & (uint8_t)(1u << (idx & 7)) )
    {
      g_State->m_Bitmap[idx >> 3] &= (uint8_t) ~(1u << (idx & 7));
      g_State->m_FreePages--;
      g_State->m_BitmapHint = idx + 1;
      if ( g_State->m_BitmapHint >= total ) g_State->m_BitmapHint = 0;
      g_State->m_Refcount[idx] = 1;
      return (int32_t) idx;
    }
  }
  return -1;
}

// Drop one reference to a page; if it was the last, mark frame free.
// Caller holds mutex.
static void        freePage                                ( uint32_t          idx )
{
  if ( --g_State->m_Refcount[idx] == 0 )
  {
    g_State->m_Bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    g_State->m_FreePages++;
  }
}

// Add one reference to an already-allocated page. Caller holds mutex.
static void        sharePage                               ( uint32_t          idx )
{
  g_State->m_Refcount[idx]++;
}

//-------------------------------------------------------------------------------------------------
class CMyCpu : public CCPU
{
  public:
                             CMyCpu                        ( uint8_t         * memStart,
                                                             uint32_t          pageTableRoot )
      : CCPU ( memStart, pageTableRoot ), m_Pages ( 0 ) {}

    virtual uint32_t         GetMemLimit                   ( void ) const;
    virtual bool             SetMemLimit                   ( uint32_t          pages );
    virtual bool             NewProcess                    ( void            * processArg,
                                                             void           (* entryPoint) ( CCPU *, void * ),
                                                             bool              copyMem );

    // Free everything owned by this process. Caller holds mutex.
    void                     freeAllPages                  ( void );

    uint32_t                 m_Pages;

  protected:
    virtual bool             pageFaultHandler              ( uint32_t          address,
                                                             bool              write );

  private:
    bool                     setMemLimitLocked             ( uint32_t          pages );
};

uint32_t           CMyCpu::GetMemLimit                     ( void ) const
{
  return m_Pages;
}

void               CMyCpu::freeAllPages                    ( void )
{
  uint32_t * l1 = (uint32_t *)( m_MemStart + ( m_PageTableRoot & ADDR_MASK ) );
  for ( uint32_t i = 0; i < PAGE_DIR_ENTRIES; i++ )
  {
    if ( l1[i] & BIT_PRESENT )
    {
      uint32_t * l2 = (uint32_t *)( m_MemStart + ( l1[i] & ADDR_MASK ) );
      for ( uint32_t j = 0; j < PAGE_DIR_ENTRIES; j++ )
      {
        if ( l2[j] & BIT_PRESENT )
        {
          freePage ( ( l2[j] & ADDR_MASK ) / PAGE_SIZE );
          l2[j] = 0;
        }
      }
      freePage ( ( l1[i] & ADDR_MASK ) / PAGE_SIZE );
      l1[i] = 0;
    }
  }
  freePage ( ( m_PageTableRoot & ADDR_MASK ) / PAGE_SIZE );
  m_Pages = 0;
}

bool               CMyCpu::setMemLimitLocked               ( uint32_t          newPages )
{
  uint32_t * l1 = (uint32_t *)( m_MemStart + ( m_PageTableRoot & ADDR_MASK ) );
  const uint32_t entryFlags = BIT_PRESENT | BIT_USER | BIT_WRITE;

  if ( newPages > m_Pages )
  {
    // Pre-check: count needed L2 directories + data pages.
    uint32_t needData = newPages - m_Pages;
    uint32_t needL2 = 0;
    for ( uint32_t p = m_Pages; p < newPages; p++ )
    {
      uint32_t l1i = p >> 10;
      uint32_t l2i = p & ( PAGE_DIR_ENTRIES - 1 );
      if ( l2i == 0 || p == m_Pages )
        if ( !( l1[l1i] & BIT_PRESENT ) ) needL2++;
    }
    if ( g_State->m_FreePages < needData + needL2 )
      return false;

    for ( uint32_t p = m_Pages; p < newPages; p++ )
    {
      uint32_t l1i = p >> 10;
      uint32_t l2i = p & ( PAGE_DIR_ENTRIES - 1 );
      if ( !( l1[l1i] & BIT_PRESENT ) )
      {
        int32_t pg = allocPage ();
        memset ( m_MemStart + (uint32_t) pg * PAGE_SIZE, 0, PAGE_SIZE );
        l1[l1i] = ( (uint32_t) pg * PAGE_SIZE ) | entryFlags;
      }
      uint32_t * l2 = (uint32_t *)( m_MemStart + ( l1[l1i] & ADDR_MASK ) );
      int32_t pg = allocPage ();
      l2[l2i] = ( (uint32_t) pg * PAGE_SIZE ) | entryFlags;
    }
  }
  else if ( newPages < m_Pages )
  {
    for ( uint32_t p = newPages; p < m_Pages; p++ )
    {
      uint32_t l1i = p >> 10;
      uint32_t l2i = p & ( PAGE_DIR_ENTRIES - 1 );
      uint32_t * l2 = (uint32_t *)( m_MemStart + ( l1[l1i] & ADDR_MASK ) );
      freePage ( ( l2[l2i] & ADDR_MASK ) / PAGE_SIZE );
      l2[l2i] = 0;
    }
    uint32_t firstFreeL1 = ( newPages + PAGE_DIR_ENTRIES - 1 ) / PAGE_DIR_ENTRIES;
    uint32_t lastL1 = ( m_Pages - 1 ) / PAGE_DIR_ENTRIES;
    for ( uint32_t i = firstFreeL1; i <= lastL1; i++ )
    {
      if ( l1[i] & BIT_PRESENT )
      {
        freePage ( ( l1[i] & ADDR_MASK ) / PAGE_SIZE );
        l1[i] = 0;
      }
    }
  }

  m_Pages = newPages;
  return true;
}

bool               CMyCpu::SetMemLimit                     ( uint32_t          pages )
{
  pthread_mutex_lock ( &g_State->m_Mtx );
  bool r = setMemLimitLocked ( pages );
  pthread_mutex_unlock ( &g_State->m_Mtx );
  return r;
}

//-------------------------------------------------------------------------------------------------
// Page fault handler: handles writes to copy-on-write shared pages.
bool               CMyCpu::pageFaultHandler                ( uint32_t          address,
                                                             bool              write )
{
  if ( !write ) return false;

  pthread_mutex_lock ( &g_State->m_Mtx );

  uint32_t l1i = address >> 22;
  uint32_t l2i = ( address >> OFFSET_BITS ) & ( PAGE_DIR_ENTRIES - 1 );
  uint32_t * l1 = (uint32_t *)( m_MemStart + ( m_PageTableRoot & ADDR_MASK ) );
  if ( !( l1[l1i] & BIT_PRESENT ) )
  {
    pthread_mutex_unlock ( &g_State->m_Mtx );
    return false;
  }
  uint32_t * l2 = (uint32_t *)( m_MemStart + ( l1[l1i] & ADDR_MASK ) );
  uint32_t entry = l2[l2i];
  if ( !( entry & BIT_PRESENT ) )
  {
    pthread_mutex_unlock ( &g_State->m_Mtx );
    return false;
  }
  if ( entry & BIT_WRITE )
  {
    // Already writable; nothing to do (shouldn't really happen).
    pthread_mutex_unlock ( &g_State->m_Mtx );
    return true;
  }

  uint32_t oldIdx = ( entry & ADDR_MASK ) / PAGE_SIZE;
  if ( g_State->m_Refcount[oldIdx] == 1 )
  {
    // Sole owner: just re-enable write.
    l2[l2i] = entry | BIT_WRITE;
    pthread_mutex_unlock ( &g_State->m_Mtx );
    return true;
  }

  // Shared: allocate a private copy.
  int32_t newIdx = allocPage ();
  if ( newIdx < 0 )
  {
    pthread_mutex_unlock ( &g_State->m_Mtx );
    return false;
  }
  memcpy ( m_MemStart + (uint32_t) newIdx * PAGE_SIZE,
           m_MemStart + ( entry & ADDR_MASK ),
           PAGE_SIZE );
  g_State->m_Refcount[oldIdx]--;
  l2[l2i] = ( (uint32_t) newIdx * PAGE_SIZE )
          | BIT_PRESENT | BIT_USER | BIT_WRITE;
  pthread_mutex_unlock ( &g_State->m_Mtx );
  return true;
}

//-------------------------------------------------------------------------------------------------
struct TThreadArg
{
  CMyCpu      * m_Cpu;
  void       (* m_Entry) ( CCPU *, void * );
  void        * m_Arg;
  int           m_SlotIdx;
};

static void      * threadFn                                ( void            * p )
{
  TThreadArg * ta = (TThreadArg *) p;
  ta->m_Entry ( ta->m_Cpu, ta->m_Arg );

  pthread_mutex_lock ( &g_State->m_Mtx );
  g_State->m_Slots[ ta->m_SlotIdx ].m_Finished = true;
  pthread_cond_broadcast ( &g_State->m_Cv );
  pthread_mutex_unlock ( &g_State->m_Mtx );

  delete ta;
  return NULL;
}

// Reap finished slots; caller holds mutex (which may be temporarily released).
static bool        reapFinishedLocked                      ( void )
{
  bool any = false;
  for ( int i = 0; i < (int) PROCESS_MAX; i++ )
  {
    if ( g_State->m_Slots[i].m_Used && g_State->m_Slots[i].m_Finished )
    {
      pthread_t t = g_State->m_Slots[i].m_Tid;
      CMyCpu * cpu = g_State->m_Slots[i].m_Cpu;
      g_State->m_Slots[i].m_Used = false;
      g_State->m_Slots[i].m_Finished = false;
      g_State->m_Slots[i].m_Cpu = NULL;
      g_State->m_ActiveCount--;
      pthread_mutex_unlock ( &g_State->m_Mtx );
      pthread_join ( t, NULL );
      pthread_mutex_lock ( &g_State->m_Mtx );
      cpu->freeAllPages ();
      delete cpu;
      any = true;
    }
  }
  return any;
}

bool               CMyCpu::NewProcess                      ( void            * processArg,
                                                             void           (* entryPoint) ( CCPU *, void * ),
                                                             bool              copyMem )
{
  pthread_mutex_lock ( &g_State->m_Mtx );

  // Find a free slot, reaping/waiting as needed.
  int slot = -1;
  while ( slot < 0 )
  {
    reapFinishedLocked ();
    for ( int i = 0; i < (int) PROCESS_MAX; i++ )
      if ( !g_State->m_Slots[i].m_Used ) { slot = i; break; }
    if ( slot < 0 )
      pthread_cond_wait ( &g_State->m_Cv, &g_State->m_Mtx );
  }

  // Allocate root page directory for the new process.
  int32_t pgDir = allocPage ();
  if ( pgDir < 0 )
  {
    pthread_mutex_unlock ( &g_State->m_Mtx );
    return false;
  }
  memset ( g_State->m_MemStart + (uint32_t) pgDir * PAGE_SIZE, 0, PAGE_SIZE );

  CMyCpu * newCpu = new CMyCpu ( g_State->m_MemStart, (uint32_t) pgDir * PAGE_SIZE );

  if ( copyMem && m_Pages > 0 )
  {
    // Copy-on-write: share data pages with parent, copy only L2 directories.
    uint32_t needL2 = ( m_Pages + PAGE_DIR_ENTRIES - 1 ) / PAGE_DIR_ENTRIES;
    if ( g_State->m_FreePages < needL2 )
    {
      freePage ( (uint32_t) pgDir );
      delete newCpu;
      pthread_mutex_unlock ( &g_State->m_Mtx );
      return false;
    }
    const uint32_t dirFlags = BIT_PRESENT | BIT_USER | BIT_WRITE;
    const uint32_t roMask   = ~ (uint32_t) BIT_WRITE;
    uint32_t * srcL1 = (uint32_t *)( m_MemStart + ( m_PageTableRoot & ADDR_MASK ) );
    uint32_t * dstL1 = (uint32_t *)( m_MemStart + (uint32_t) pgDir * PAGE_SIZE );
    for ( uint32_t p = 0; p < m_Pages; p++ )
    {
      uint32_t l1i = p >> 10;
      uint32_t l2i = p & ( PAGE_DIR_ENTRIES - 1 );
      if ( !( dstL1[l1i] & BIT_PRESENT ) )
      {
        int32_t pg = allocPage ();
        memset ( m_MemStart + (uint32_t) pg * PAGE_SIZE, 0, PAGE_SIZE );
        dstL1[l1i] = ( (uint32_t) pg * PAGE_SIZE ) | dirFlags;
      }
      uint32_t * dstL2 = (uint32_t *)( m_MemStart + ( dstL1[l1i] & ADDR_MASK ) );
      uint32_t * srcL2 = (uint32_t *)( m_MemStart + ( srcL1[l1i] & ADDR_MASK ) );
      // Share the data page; clear BIT_WRITE in both parent's and child's PTE.
      uint32_t srcEntry = srcL2[l2i];
      uint32_t dataIdx = ( srcEntry & ADDR_MASK ) / PAGE_SIZE;
      sharePage ( dataIdx );
      uint32_t roEntry = srcEntry & roMask;
      srcL2[l2i] = roEntry;
      dstL2[l2i] = roEntry;
    }
    newCpu->m_Pages = m_Pages;
  }

  g_State->m_Slots[slot].m_Used = true;
  g_State->m_Slots[slot].m_Finished = false;
  g_State->m_Slots[slot].m_Cpu = newCpu;
  g_State->m_ActiveCount++;

  TThreadArg * ta = new TThreadArg;
  ta->m_Cpu = newCpu;
  ta->m_Entry = entryPoint;
  ta->m_Arg = processArg;
  ta->m_SlotIdx = slot;

  if ( pthread_create ( &g_State->m_Slots[slot].m_Tid, NULL, threadFn, ta ) != 0 )
  {
    delete ta;
    g_State->m_Slots[slot].m_Used = false;
    g_State->m_Slots[slot].m_Cpu = NULL;
    g_State->m_ActiveCount--;
    newCpu->freeAllPages ();
    delete newCpu;
    pthread_mutex_unlock ( &g_State->m_Mtx );
    return false;
  }

  pthread_mutex_unlock ( &g_State->m_Mtx );
  return true;
}

//-------------------------------------------------------------------------------------------------
void               MemMgr                                  ( void            * mem,
                                                             uint32_t          totalPages,
                                                             void            * processArg,
                                                             void           (* mainProcess) ( CCPU *, void * ))
{
  TMemState st;
  memset ( &st, 0, sizeof ( st ) );
  st.m_MemStart    = (uint8_t *) mem;
  st.m_TotalPages  = totalPages;
  st.m_FreePages   = totalPages;

  uint32_t bmBytes = ( totalPages + 7 ) / 8;
  st.m_Bitmap = new uint8_t [ bmBytes ];
  memset ( st.m_Bitmap, 0xFF, bmBytes );
  for ( uint32_t i = totalPages; i < bmBytes * 8; i++ )
    st.m_Bitmap[i >> 3] &= (uint8_t) ~(1u << (i & 7));
  st.m_BitmapHint = 0;

  // Refcount array lives inside the managed memory block (a few pages).
  uint32_t rcBytes = totalPages * (uint32_t) sizeof ( uint16_t );
  uint32_t rcPages = ( rcBytes + CCPU::PAGE_SIZE - 1 ) / CCPU::PAGE_SIZE;
  st.m_Refcount = (uint16_t *) st.m_MemStart;
  memset ( st.m_Refcount, 0, rcPages * CCPU::PAGE_SIZE );
  for ( uint32_t i = 0; i < rcPages; i++ )
  {
    st.m_Bitmap[i >> 3] &= (uint8_t) ~(1u << (i & 7));
    st.m_Refcount[i] = 1;
  }
  st.m_FreePages -= rcPages;
  st.m_BitmapHint = rcPages;

  pthread_mutex_init ( &st.m_Mtx, NULL );
  pthread_cond_init  ( &st.m_Cv,  NULL );

  g_State = &st;

  pthread_mutex_lock ( &st.m_Mtx );
  int32_t pgDir = allocPage ();
  pthread_mutex_unlock ( &st.m_Mtx );
  memset ( st.m_MemStart + (uint32_t) pgDir * CCPU::PAGE_SIZE, 0, CCPU::PAGE_SIZE );
  CMyCpu * initCpu = new CMyCpu ( st.m_MemStart, (uint32_t) pgDir * CCPU::PAGE_SIZE );

  mainProcess ( initCpu, processArg );

  pthread_mutex_lock ( &st.m_Mtx );
  while ( st.m_ActiveCount > 0 )
  {
    if ( !reapFinishedLocked () )
      pthread_cond_wait ( &st.m_Cv, &st.m_Mtx );
  }
  initCpu->freeAllPages ();
  pthread_mutex_unlock ( &st.m_Mtx );

  delete initCpu;
  pthread_mutex_destroy ( &st.m_Mtx );
  pthread_cond_destroy  ( &st.m_Cv );
  delete [] st.m_Bitmap;
  g_State = NULL;
}
