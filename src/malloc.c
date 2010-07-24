/*
** 2001 September 15
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
**
** Memory allocation functions used throughout sqlite.
*/
#include "sqliteInt.h"
#include <stdarg.h>

/*
** There are two general-purpose memory allocators:
**
** Simple:
**
**     sqlite3_malloc
**     sqlite3_free
**     sqlite3_realloc
**     sqlite3Malloc
**     sqlite3MallocSize
**     sqlite3_mprintf
**
** Enhanced:
**
**     sqlite3DbMallocRaw
**     sqlite3DbMallocZero
**     sqlite3DbFree
**     sqlite3DbRealloc
**     sqlite3MPrintf
**     sqlite3DbMalloc
**
** All external allocations use the simple memory allocator.
** The enhanced allocator is used internally only, and is not
** available to extensions or applications.
**
** The enhanced allocator is a wrapper around the simple allocator that
** adds the following capabilities:
**
** (1) Access to lookaside memory associated with a database connection.
**
** (2) The ability to link allocations into a hierarchy with automatic
**     deallocation of all elements of the subhierarchy whenever any
**     element within the hierarchy is deallocated.
**
** The two allocators are incompatible in the sense that allocations that
** originate from the simple allocator must be deallocated using the simple
** deallocator and allocations that originate from the enhanced allocator must
** be deallocated using the enhanced deallocator.  You cannot check-out 
** memory from one allocator then return it to the other.
*/

/*
** The automatic hierarchical deallocation feature of the enhanced allocator
** is implemented by adding an instance of the following structure to the
** header of each enhanced allocation.
**
** In order to preserve alignment, this structure must be a multiple of
** 8 bytes in size.
*/
typedef struct EMemHdr EMemHdr;
struct EMemHdr {
  EMemHdr *pEChild;      /* List of children of this node */
  EMemHdr *pESibling;    /* Other nodes that are children of the same parent */
#ifdef SQLITE_MEMDEBUG
  u32 iEMemMagic;        /* Magic number for sanity checking */
  u32 isAChild;          /* True if this allocate is a child of another */
#endif
};

/*
** Macros for querying and setting debugging fields of the EMemHdr object.
*/
#ifdef SQLITE_MEMDEBUG
# define isValidEMem(E)     ((E)->iEMemMagic==0xc0a43fad)
# define setValidEMem(E)    (E)->iEMemMagic = 0xc0a43fad
# define clearValidEMem(E)  (E)->iEMemMagic = 0x12345678
# define isChildEMem(E)     ((E)->isAChild!=0)
# define setChildEMem(E)    (E)->isAChild = 1
# define clearChildEMem(E)  (E)->isAChild = 0
#else
# define isValidEMem(E)
# define setValidEMem(E)
# define clearValidEMem(E)
# define isChildEMem(E)
# define setChildEMem(E)
# define clearChildEMem(E)
#endif

/*
** This routine runs when the memory allocator sees that the
** total memory allocation is about to exceed the soft heap
** limit.
*/
static void softHeapLimitEnforcer(
  void *NotUsed, 
  sqlite3_int64 NotUsed2,
  int allocSize
){
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  sqlite3_release_memory(allocSize);
}

/*
** Set the soft heap-size limit for the library. Passing a zero or 
** negative value indicates no limit.
**
** If the total amount of memory allocated (by all threads) exceeds
** the soft heap limit, then sqlite3_release_memory() is invoked to
** try to free up some memory before proceeding.
*/
void sqlite3_soft_heap_limit(int n){
  sqlite3_uint64 iLimit;
  int overage;
  if( n<0 ){
    iLimit = 0;
  }else{
    iLimit = n;
  }
#ifndef SQLITE_OMIT_AUTOINIT
  sqlite3_initialize();
#endif
  if( iLimit>0 ){
    sqlite3MemoryAlarm(softHeapLimitEnforcer, 0, iLimit);
  }else{
    sqlite3MemoryAlarm(0, 0, 0);
  }
  overage = (int)(sqlite3_memory_used() - (i64)n);
  if( overage>0 ){
    sqlite3_release_memory(overage);
  }
}

/*
** Attempt to release up to n bytes of non-essential memory currently
** held by SQLite. An example of non-essential memory is memory used to
** cache database pages that are not currently in use.
*/
int sqlite3_release_memory(int n){
#ifdef SQLITE_ENABLE_MEMORY_MANAGEMENT
  int nRet = 0;
  nRet += sqlite3PcacheReleaseMemory(n-nRet);
  return nRet;
#else
  UNUSED_PARAMETER(n);
  return SQLITE_OK;
#endif
}

/*
** State information local to the memory allocation subsystem.
*/
static SQLITE_WSD struct Mem0Global {
  /* Number of free pages for scratch and page-cache memory */
  u32 nScratchFree;
  u32 nPageFree;

  sqlite3_mutex *mutex;         /* Mutex to serialize access */

  /*
  ** The alarm callback and its arguments.  The mem0.mutex lock will
  ** be held while the callback is running.  Recursive calls into
  ** the memory subsystem are allowed, but no new callbacks will be
  ** issued.
  */
  sqlite3_int64 alarmThreshold;
  void (*alarmCallback)(void*, sqlite3_int64,int);
  void *alarmArg;

  /*
  ** Pointers to the end of sqlite3GlobalConfig.pScratch and
  ** sqlite3GlobalConfig.pPage to a block of memory that records
  ** which pages are available.
  */
  u32 *aScratchFree;
  u32 *aPageFree;
} mem0 = { 0, 0, 0, 0, 0, 0, 0, 0 };

#define mem0 GLOBAL(struct Mem0Global, mem0)

/*
** Initialize the memory allocation subsystem.
*/
int sqlite3MallocInit(void){
  if( sqlite3GlobalConfig.m.xMalloc==0 ){
    sqlite3MemSetDefault();
  }
  memset(&mem0, 0, sizeof(mem0));
  if( sqlite3GlobalConfig.bCoreMutex ){
    mem0.mutex = sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MEM);
  }
  if( sqlite3GlobalConfig.pScratch && sqlite3GlobalConfig.szScratch>=100
      && sqlite3GlobalConfig.nScratch>=0 ){
    int i;
    sqlite3GlobalConfig.szScratch = ROUNDDOWN8(sqlite3GlobalConfig.szScratch-4);
    mem0.aScratchFree = (u32*)&((char*)sqlite3GlobalConfig.pScratch)
                  [sqlite3GlobalConfig.szScratch*sqlite3GlobalConfig.nScratch];
    for(i=0; i<sqlite3GlobalConfig.nScratch; i++){ mem0.aScratchFree[i] = i; }
    mem0.nScratchFree = sqlite3GlobalConfig.nScratch;
  }else{
    sqlite3GlobalConfig.pScratch = 0;
    sqlite3GlobalConfig.szScratch = 0;
  }
  if( sqlite3GlobalConfig.pPage && sqlite3GlobalConfig.szPage>=512
      && sqlite3GlobalConfig.nPage>=1 ){
    int i;
    int overhead;
    int sz = ROUNDDOWN8(sqlite3GlobalConfig.szPage);
    int n = sqlite3GlobalConfig.nPage;
    overhead = (4*n + sz - 1)/sz;
    sqlite3GlobalConfig.nPage -= overhead;
    mem0.aPageFree = (u32*)&((char*)sqlite3GlobalConfig.pPage)
                  [sqlite3GlobalConfig.szPage*sqlite3GlobalConfig.nPage];
    for(i=0; i<sqlite3GlobalConfig.nPage; i++){ mem0.aPageFree[i] = i; }
    mem0.nPageFree = sqlite3GlobalConfig.nPage;
  }else{
    sqlite3GlobalConfig.pPage = 0;
    sqlite3GlobalConfig.szPage = 0;
  }
  return sqlite3GlobalConfig.m.xInit(sqlite3GlobalConfig.m.pAppData);
}

/*
** Deinitialize the memory allocation subsystem.
*/
void sqlite3MallocEnd(void){
  if( sqlite3GlobalConfig.m.xShutdown ){
    sqlite3GlobalConfig.m.xShutdown(sqlite3GlobalConfig.m.pAppData);
  }
  memset(&mem0, 0, sizeof(mem0));
}

/*
** Return the amount of memory currently checked out.
*/
sqlite3_int64 sqlite3_memory_used(void){
  int n, mx;
  sqlite3_int64 res;
  sqlite3_status(SQLITE_STATUS_MEMORY_USED, &n, &mx, 0);
  res = (sqlite3_int64)n;  /* Work around bug in Borland C. Ticket #3216 */
  return res;
}

/*
** Return the maximum amount of memory that has ever been
** checked out since either the beginning of this process
** or since the most recent reset.
*/
sqlite3_int64 sqlite3_memory_highwater(int resetFlag){
  int n, mx;
  sqlite3_int64 res;
  sqlite3_status(SQLITE_STATUS_MEMORY_USED, &n, &mx, resetFlag);
  res = (sqlite3_int64)mx;  /* Work around bug in Borland C. Ticket #3216 */
  return res;
}

/*
** Change the alarm callback
*/
int sqlite3MemoryAlarm(
  void(*xCallback)(void *pArg, sqlite3_int64 used,int N),
  void *pArg,
  sqlite3_int64 iThreshold
){
  sqlite3_mutex_enter(mem0.mutex);
  mem0.alarmCallback = xCallback;
  mem0.alarmArg = pArg;
  mem0.alarmThreshold = iThreshold;
  sqlite3_mutex_leave(mem0.mutex);
  return SQLITE_OK;
}

#ifndef SQLITE_OMIT_DEPRECATED
/*
** Deprecated external interface.  Internal/core SQLite code
** should call sqlite3MemoryAlarm.
*/
int sqlite3_memory_alarm(
  void(*xCallback)(void *pArg, sqlite3_int64 used,int N),
  void *pArg,
  sqlite3_int64 iThreshold
){
  return sqlite3MemoryAlarm(xCallback, pArg, iThreshold);
}
#endif

/*
** Trigger the alarm 
*/
static void sqlite3MallocAlarm(int nByte){
  void (*xCallback)(void*,sqlite3_int64,int);
  sqlite3_int64 nowUsed;
  void *pArg;
  if( mem0.alarmCallback==0 ) return;
  xCallback = mem0.alarmCallback;
  nowUsed = sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED);
  pArg = mem0.alarmArg;
  mem0.alarmCallback = 0;
  sqlite3_mutex_leave(mem0.mutex);
  xCallback(pArg, nowUsed, nByte);
  sqlite3_mutex_enter(mem0.mutex);
  mem0.alarmCallback = xCallback;
  mem0.alarmArg = pArg;
}

/*
** Do a memory allocation with statistics and alarms.  Assume the
** lock is already held.
*/
static int mallocWithAlarm(int n, void **pp){
  int nFull;
  void *p;
  assert( sqlite3_mutex_held(mem0.mutex) );
  nFull = sqlite3GlobalConfig.m.xRoundup(n);
  sqlite3StatusSet(SQLITE_STATUS_MALLOC_SIZE, n);
  if( mem0.alarmCallback!=0 ){
    int nUsed = sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED);
    if( nUsed+nFull >= mem0.alarmThreshold ){
      sqlite3MallocAlarm(nFull);
    }
  }
  p = sqlite3GlobalConfig.m.xMalloc(nFull);
  if( p==0 && mem0.alarmCallback ){
    sqlite3MallocAlarm(nFull);
    p = sqlite3GlobalConfig.m.xMalloc(nFull);
  }
  if( p ){
    nFull = sqlite3MallocSize(p);
    sqlite3StatusAdd(SQLITE_STATUS_MEMORY_USED, nFull);
  }
  *pp = p;
  return nFull;
}

/*
** Allocate memory.  This routine is like sqlite3_malloc() except that it
** assumes the memory subsystem has already been initialized.
*/
void *sqlite3Malloc(int n){
  void *p;
  if( n<=0 || n>=0x7fffff00 ){
    /* A memory allocation of a number of bytes which is near the maximum
    ** signed integer value might cause an integer overflow inside of the
    ** xMalloc().  Hence we limit the maximum size to 0x7fffff00, giving
    ** 255 bytes of overhead.  SQLite itself will never use anything near
    ** this amount.  The only way to reach the limit is with sqlite3_malloc() */
    p = 0;
  }else if( sqlite3GlobalConfig.bMemstat ){
    sqlite3_mutex_enter(mem0.mutex);
    mallocWithAlarm(n, &p);
    sqlite3_mutex_leave(mem0.mutex);
  }else{
    p = sqlite3GlobalConfig.m.xMalloc(n);
  }
  return p;
}

/*
** This version of the memory allocation is for use by the application.
** First make sure the memory subsystem is initialized, then do the
** allocation.
*/
void *sqlite3_malloc(int n){
#ifndef SQLITE_OMIT_AUTOINIT
  if( sqlite3_initialize() ) return 0;
#endif
  return sqlite3Malloc(n);
}

/*
** Each thread may only have a single outstanding allocation from
** xScratchMalloc().  We verify this constraint in the single-threaded
** case by setting scratchAllocOut to 1 when an allocation
** is outstanding clearing it when the allocation is freed.
*/
#if SQLITE_THREADSAFE==0 && !defined(NDEBUG)
static int scratchAllocOut = 0;
#endif


/*
** Allocate memory that is to be used and released right away.
** This routine is similar to alloca() in that it is not intended
** for situations where the memory might be held long-term.  This
** routine is intended to get memory to old large transient data
** structures that would not normally fit on the stack of an
** embedded processor.
*/
void *sqlite3ScratchMalloc(int n){
  void *p;
  assert( n>0 );

#if SQLITE_THREADSAFE==0 && !defined(NDEBUG)
  /* Verify that no more than two scratch allocation per thread
  ** is outstanding at one time.  (This is only checked in the
  ** single-threaded case since checking in the multi-threaded case
  ** would be much more complicated.) */
  assert( scratchAllocOut<=1 );
#endif

  if( sqlite3GlobalConfig.szScratch<n ){
    goto scratch_overflow;
  }else{  
    sqlite3_mutex_enter(mem0.mutex);
    if( mem0.nScratchFree==0 ){
      sqlite3_mutex_leave(mem0.mutex);
      goto scratch_overflow;
    }else{
      int i;
      i = mem0.aScratchFree[--mem0.nScratchFree];
      i *= sqlite3GlobalConfig.szScratch;
      sqlite3StatusAdd(SQLITE_STATUS_SCRATCH_USED, 1);
      sqlite3StatusSet(SQLITE_STATUS_SCRATCH_SIZE, n);
      sqlite3_mutex_leave(mem0.mutex);
      p = (void*)&((char*)sqlite3GlobalConfig.pScratch)[i];
      assert(  (((u8*)p - (u8*)0) & 7)==0 );
    }
  }
#if SQLITE_THREADSAFE==0 && !defined(NDEBUG)
  scratchAllocOut = p!=0;
#endif

  return p;

scratch_overflow:
  if( sqlite3GlobalConfig.bMemstat ){
    sqlite3_mutex_enter(mem0.mutex);
    sqlite3StatusSet(SQLITE_STATUS_SCRATCH_SIZE, n);
    n = mallocWithAlarm(n, &p);
    if( p ) sqlite3StatusAdd(SQLITE_STATUS_SCRATCH_OVERFLOW, n);
    sqlite3_mutex_leave(mem0.mutex);
  }else{
    p = sqlite3GlobalConfig.m.xMalloc(n);
  }
  sqlite3MemdebugSetType(p, MEMTYPE_SCRATCH);
#if SQLITE_THREADSAFE==0 && !defined(NDEBUG)
  scratchAllocOut = p!=0;
#endif
  return p;    
}
void sqlite3ScratchFree(void *p){
  if( p ){
    if( sqlite3GlobalConfig.pScratch==0
           || p<sqlite3GlobalConfig.pScratch
           || p>=(void*)mem0.aScratchFree ){
      assert( sqlite3MemdebugHasType(p, MEMTYPE_SCRATCH) );
      assert( !sqlite3MemdebugHasType(p, ~MEMTYPE_SCRATCH) );
      sqlite3MemdebugSetType(p, MEMTYPE_HEAP);
      if( sqlite3GlobalConfig.bMemstat ){
        int iSize = sqlite3MallocSize(p);
        sqlite3_mutex_enter(mem0.mutex);
        sqlite3StatusAdd(SQLITE_STATUS_SCRATCH_OVERFLOW, -iSize);
        sqlite3StatusAdd(SQLITE_STATUS_MEMORY_USED, -iSize);
        sqlite3GlobalConfig.m.xFree(p);
        sqlite3_mutex_leave(mem0.mutex);
      }else{
        sqlite3GlobalConfig.m.xFree(p);
      }
    }else{
      int i;
      i = (int)((u8*)p - (u8*)sqlite3GlobalConfig.pScratch);
      i /= sqlite3GlobalConfig.szScratch;
      assert( i>=0 && i<sqlite3GlobalConfig.nScratch );
      sqlite3_mutex_enter(mem0.mutex);
      assert( mem0.nScratchFree<(u32)sqlite3GlobalConfig.nScratch );
      mem0.aScratchFree[mem0.nScratchFree++] = i;
      sqlite3StatusAdd(SQLITE_STATUS_SCRATCH_USED, -1);
      sqlite3_mutex_leave(mem0.mutex);

#if SQLITE_THREADSAFE==0 && !defined(NDEBUG)
    /* Verify that no more than two scratch allocation per thread
    ** is outstanding at one time.  (This is only checked in the
    ** single-threaded case since checking in the multi-threaded case
    ** would be much more complicated.) */
    assert( scratchAllocOut>=1 && scratchAllocOut<=2 );
    scratchAllocOut = 0;
#endif

    }
  }
}

/*
** TRUE if p is a lookaside memory allocation from db
*/
#ifndef SQLITE_OMIT_LOOKASIDE
static int isLookaside(sqlite3 *db, void *p){
  return db && p && p>=db->lookaside.pStart && p<db->lookaside.pEnd;
}
#else
#define isLookaside(A,B) 0
#endif

/*
** Return the size of a memory allocation previously obtained from
** sqlite3Malloc() or sqlite3_malloc().
**
** The size returned is the usable size and does not include any
** bookkeeping overhead or sentinals at the end of the allocation.
*/
int sqlite3MallocSize(void *p){
  assert( sqlite3MemdebugHasType(p, MEMTYPE_HEAP) );
  assert( !sqlite3MemdebugHasType(p, MEMTYPE_RECURSIVE) );
  return sqlite3GlobalConfig.m.xSize(p);
}
int sqlite3DbMallocSize(sqlite3 *db, void *pObj){
  EMemHdr *p = (EMemHdr*)pObj;
  assert( db==0 || sqlite3_mutex_held(db->mutex) );
  if( p ){
    p--;
    assert( isValidEMem(p) );
  }
  if( isLookaside(db, p) ){
    return db->lookaside.sz - sizeof(EMemHdr);
  }else{
    assert( sqlite3MemdebugHasType(p, MEMTYPE_RECURSIVE) );
    assert( sqlite3MemdebugHasType(p,
             db ? (MEMTYPE_DB|MEMTYPE_HEAP) : MEMTYPE_HEAP) );
    return sqlite3GlobalConfig.m.xSize(p) - sizeof(EMemHdr);
  }
}

/*
** Free memory previously obtained from sqlite3Malloc().
*/
void sqlite3_free(void *p){
  if( p==0 ) return;
  assert( !sqlite3MemdebugHasType(p, MEMTYPE_RECURSIVE) );
  assert( sqlite3MemdebugHasType(p, MEMTYPE_HEAP) );
  if( sqlite3GlobalConfig.bMemstat ){
    sqlite3_mutex_enter(mem0.mutex);
    sqlite3StatusAdd(SQLITE_STATUS_MEMORY_USED, -sqlite3MallocSize(p));
    sqlite3GlobalConfig.m.xFree(p);
    sqlite3_mutex_leave(mem0.mutex);
  }else{
    sqlite3GlobalConfig.m.xFree(p);
  }
}

/*
** Free memory that might be associated with a particular database
** connection.  All child allocations are also freed.
*/
void sqlite3DbFree(sqlite3 *db, void *pObj){
  EMemHdr *p = (EMemHdr*)pObj;
  assert( db==0 || sqlite3_mutex_held(db->mutex) );
  if( p ) p--;
  while( p ){
    EMemHdr *pNext = p->pESibling;
    assert( isValidEMem(p) );
    if( p->pEChild ) sqlite3DbFree(db, (void*)&p->pEChild[1]);
    if( isLookaside(db, p) ){
      LookasideSlot *pBuf = (LookasideSlot*)p;
      clearValidEMem(p);
      pBuf->pNext = db->lookaside.pFree;
      db->lookaside.pFree = pBuf;
      db->lookaside.nOut--;
    }else{
      assert( sqlite3MemdebugHasType(p, MEMTYPE_RECURSIVE) );
      assert( sqlite3MemdebugHasType(p,
                         db ? (MEMTYPE_DB|MEMTYPE_HEAP) : MEMTYPE_HEAP) );
      sqlite3MemdebugSetType(p, MEMTYPE_HEAP);
      clearValidEMem(p);
      sqlite3_free(p);
    }
    p = pNext;
  }
}

/*
** Change the size of an existing memory allocation.
**
** This is the same as sqlite3_realloc() except that it assumes that
** the memory subsystem has already been initialized.
*/
void *sqlite3Realloc(void *pOld, int nBytes){
  int nOld, nNew;
  void *pNew;
  if( pOld==0 ){
    return sqlite3Malloc(nBytes);
  }
  if( nBytes<=0 ){
    sqlite3_free(pOld);
    return 0;
  }
  if( nBytes>=0x7fffff00 ){
    /* The 0x7ffff00 limit term is explained in comments on sqlite3Malloc() */
    return 0;
  }
  nOld = sqlite3MallocSize(pOld);
  nNew = sqlite3GlobalConfig.m.xRoundup(nBytes);
  if( nOld==nNew ){
    pNew = pOld;
  }else if( sqlite3GlobalConfig.bMemstat ){
    sqlite3_mutex_enter(mem0.mutex);
    sqlite3StatusSet(SQLITE_STATUS_MALLOC_SIZE, nBytes);
    if( sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED)+nNew-nOld >= 
          mem0.alarmThreshold ){
      sqlite3MallocAlarm(nNew-nOld);
    }
    assert( sqlite3MemdebugHasType(pOld, MEMTYPE_HEAP) );
    assert( !sqlite3MemdebugHasType(pOld, ~MEMTYPE_HEAP) );
    pNew = sqlite3GlobalConfig.m.xRealloc(pOld, nNew);
    if( pNew==0 && mem0.alarmCallback ){
      sqlite3MallocAlarm(nBytes);
      pNew = sqlite3GlobalConfig.m.xRealloc(pOld, nNew);
    }
    if( pNew ){
      nNew = sqlite3MallocSize(pNew);
      sqlite3StatusAdd(SQLITE_STATUS_MEMORY_USED, nNew-nOld);
    }
    sqlite3_mutex_leave(mem0.mutex);
  }else{
    pNew = sqlite3GlobalConfig.m.xRealloc(pOld, nNew);
  }
  return pNew;
}

/*
** The public interface to sqlite3Realloc.  Make sure that the memory
** subsystem is initialized prior to invoking sqliteRealloc.
*/
void *sqlite3_realloc(void *pOld, int n){
#ifndef SQLITE_OMIT_AUTOINIT
  if( sqlite3_initialize() ) return 0;
#endif
  return sqlite3Realloc(pOld, n);
}


/*
** Allocate and zero memory.
*/ 
void *sqlite3MallocZero(int n){
  void *p = sqlite3Malloc(n);
  if( p ){
    memset(p, 0, n);
  }
  return p;
}

/*
** Allocate and zero memory.  If the allocation fails, make
** the mallocFailed flag in the connection pointer.
*/
void *sqlite3DbMallocZero(sqlite3 *db, int n){
  void *p = sqlite3DbMallocRaw(db, n);
  if( p ){
    memset(p, 0, n);
  }
  return p;
}

/*
** Allocate and zero memory.  If the allocation fails, make
** the mallocFailed flag in the connection pointer.
**
** If db!=0 and db->mallocFailed is true (indicating a prior malloc
** failure on the same database connection) then always return 0.
** Hence for a particular database connection, once malloc starts
** failing, it fails consistently until mallocFailed is reset.
** This is an important assumption.  There are many places in the
** code that do things like this:
**
**         int *a = (int*)sqlite3DbMallocRaw(db, 100);
**         int *b = (int*)sqlite3DbMallocRaw(db, 200);
**         if( b ) a[10] = 9;
**
** In other words, if a subsequent malloc (ex: "b") worked, it is assumed
** that all prior mallocs (ex: "a") worked too.
*/
void *sqlite3DbMallocRaw(sqlite3 *db, int n){
  EMemHdr *p;
  assert( db==0 || sqlite3_mutex_held(db->mutex) );
  n += sizeof(EMemHdr);
#ifndef SQLITE_OMIT_LOOKASIDE
  if( db ){
    LookasideSlot *pBuf;
    if( db->mallocFailed ){
      return 0;
    }
    if( db->lookaside.bEnabled && n<=db->lookaside.sz
         && (pBuf = db->lookaside.pFree)!=0 ){
      db->lookaside.pFree = pBuf->pNext;
      db->lookaside.nOut++;
      if( db->lookaside.nOut>db->lookaside.mxOut ){
        db->lookaside.mxOut = db->lookaside.nOut;
      }
      p = (EMemHdr*)pBuf;
      goto finish_emalloc_raw;
    }
  }
#else
  if( db && db->mallocFailed ){
    return 0;
  }
#endif
  p = sqlite3Malloc(n);
  if( !p ){
    if( db ) db->mallocFailed = 1;
    return 0;
  }
  sqlite3MemdebugSetType(p, MEMTYPE_RECURSIVE |
            ((db && db->lookaside.bEnabled) ? MEMTYPE_DB : MEMTYPE_HEAP));

finish_emalloc_raw:
  memset(p, 0, sizeof(EMemHdr));
  setValidEMem(p);
  return (void*)&p[1];
}

/*
** Resize the block of memory pointed to by p to n bytes. If the
** resize fails, set the mallocFailed flag in the connection object.
**
** The pOld memory block must not be linked into an allocation hierarchy
** as a child.  It is OK for the allocation to be the root of a hierarchy
** of allocations; the only restriction is that there must be no other
** allocations above the pOld allocation in the hierarchy.  To resize 
** an allocation that is a child within a hierarchy, first
** unlink the allocation, resize it, then relink it.  
*/
void *sqlite3DbRealloc(sqlite3 *db, void *pOld, int n){
  EMemHdr *p = (EMemHdr*)pOld;
  EMemHdr *pNew = 0;
  assert( db!=0 );
  assert( sqlite3_mutex_held(db->mutex) );
  if( db->mallocFailed==0 ){
    if( p==0 ){
      return sqlite3DbMallocRaw(db, n);
    }
    p--;
    assert( isValidEMem(p) );    /* pOld obtained from extended allocator */
    assert( !isChildEMem(p) );   /* pOld must not be a child allocation */
    if( isLookaside(db, p) ){
      if( n+sizeof(EMemHdr)<=db->lookaside.sz ){
        return pOld;
      }
      pNew = sqlite3DbMallocRaw(db, n);
      if( pNew ){
        memcpy(pNew-1, p, db->lookaside.sz);
        setValidEMem(pNew-1);
        sqlite3DbFree(db, pOld);
      }
    }else{
      assert( sqlite3MemdebugHasType(p, MEMTYPE_RECURSIVE) );
      assert( sqlite3MemdebugHasType(p, MEMTYPE_DB|MEMTYPE_HEAP) );
      sqlite3MemdebugSetType(p, MEMTYPE_HEAP);
      pNew = sqlite3_realloc(p, n+sizeof(EMemHdr));
      if( !pNew ){
        sqlite3MemdebugSetType(p, MEMTYPE_RECURSIVE|MEMTYPE_HEAP);
        db->mallocFailed = 1;
      }else{
        sqlite3MemdebugSetType(pNew, MEMTYPE_RECURSIVE | 
              (db->lookaside.bEnabled ? MEMTYPE_DB : MEMTYPE_HEAP));
        setValidEMem(pNew);
        pNew++;
      }
    }
  }
  return (void*)pNew;
}

/*
** Attempt to reallocate p.  If the reallocation fails, then free p
** and set the mallocFailed flag in the database connection.
*/
void *sqlite3DbReallocOrFree(sqlite3 *db, void *p, int n){
  void *pNew;
  pNew = sqlite3DbRealloc(db, p, n);
  if( !pNew ){
    sqlite3DbFree(db, p);
  }
  return pNew;
}

/*
** Make a copy of a string in memory obtained from sqliteMalloc(). These 
** functions call sqlite3MallocRaw() directly instead of sqliteMalloc(). This
** is because when memory debugging is turned on, these two functions are 
** called via macros that record the current file and line number in the
** ThreadData structure.
*/
char *sqlite3DbStrDup(sqlite3 *db, const char *z){
  char *zNew;
  size_t n;
  if( z==0 ){
    return 0;
  }
  n = sqlite3Strlen30(z) + 1;
  assert( (n&0x7fffffff)==n );
  zNew = sqlite3DbMallocRaw(db, (int)n);
  if( zNew ){
    memcpy(zNew, z, n);
  }
  return zNew;
}
char *sqlite3DbStrNDup(sqlite3 *db, const char *z, int n){
  char *zNew;
  if( z==0 ){
    return 0;
  }
  assert( (n&0x7fffffff)==n );
  zNew = sqlite3DbMallocRaw(db, n+1);
  if( zNew ){
    memcpy(zNew, z, n);
    zNew[n] = 0;
  }
  return zNew;
}

/*
** Link extended allocation nodes such that deallocating the parent
** causes the child to be automatically deallocated.
*/
void sqlite3MemLink(void *pParentObj, void *pChildObj){
  EMemHdr *pParent = (EMemHdr*)pParentObj;
  EMemHdr *pChild = (EMemHdr*)pChildObj;
  if( pParent && pChild ){
    pParent--;
    assert( isValidEMem(pParent) );  /* pParentObj is an extended allocation */ 
    pChild--;
    assert( isValidEMem(pChild) );   /* pChildObj is an extended allocation */
    assert( !isChildEMem(pChild) );  /* pChildObj not a child of another obj */
    pChild->pESibling = pParent->pEChild;
    pParent->pEChild = pChild;
    setChildEMem(pChild);
  }
}

/*
** pChildObj is a child object of pParentObj due to a prior call
** to sqlite3MemLink().  This routine breaks that linkage, making
** pChildObj an independent node that is not a child of any other node.
*/
void sqlite3MemUnlink(void *pParentObj, void *pChildObj){
  EMemHdr *pParent = (EMemHdr*)pParentObj;
  EMemHdr *pChild = (EMemHdr*)pChildObj;
  EMemHdr **pp;

  assert( pParentObj!=0 );
  assert( pChildObj!=0 );
  pParent--;
  assert( isValidEMem(pParent) );  /* pParentObj is an extended allocation */ 
  pChild--;
  assert( isValidEMem(pChild) );   /* pChildObj is an extended allocation */
  assert( isChildEMem(pChild) );   /* pChildObj a child of something */
  for(pp=&pParent->pEChild; (*pp)!=pChild; pp = &(*pp)->pESibling){
    assert( *pp );                /* pChildObj is a child of pParentObj */
    assert( isValidEMem(*pp) );   /* All children of pParentObj are valid */
    assert( isChildEMem(*pp) );   /* All children of pParentObj are children */
  }
  *pp = pChild->pESibling;
  pChild->pESibling = 0;
  clearChildEMem(pChild);
}


/*
** Create a string from the zFromat argument and the va_list that follows.
** Store the string in memory obtained from sqliteMalloc() and make *pz
** point to that string.
*/
void sqlite3SetString(char **pz, sqlite3 *db, const char *zFormat, ...){
  va_list ap;
  char *z;

  va_start(ap, zFormat);
  z = sqlite3VMPrintf(db, zFormat, ap);
  va_end(ap);
  sqlite3DbFree(db, *pz);
  *pz = z;
}


/*
** This function must be called before exiting any API function (i.e. 
** returning control to the user) that has called sqlite3_malloc or
** sqlite3_realloc.
**
** The returned value is normally a copy of the second argument to this
** function. However, if a malloc() failure has occurred since the previous
** invocation SQLITE_NOMEM is returned instead. 
**
** If the first argument, db, is not NULL and a malloc() error has occurred,
** then the connection error-code (the value returned by sqlite3_errcode())
** is set to SQLITE_NOMEM.
*/
int sqlite3ApiExit(sqlite3* db, int rc){
  /* If the db handle is not NULL, then we must hold the connection handle
  ** mutex here. Otherwise the read (and possible write) of db->mallocFailed 
  ** is unsafe, as is the call to sqlite3Error().
  */
  assert( !db || sqlite3_mutex_held(db->mutex) );
  if( db && (db->mallocFailed || rc==SQLITE_IOERR_NOMEM) ){
    sqlite3Error(db, SQLITE_NOMEM, 0);
    db->mallocFailed = 0;
    rc = SQLITE_NOMEM;
  }
  return rc & (db ? db->errMask : 0xff);
}
