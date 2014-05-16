/*
** 2011-07-09
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file contains code for the VdbeSorter object, used in concert with
** a VdbeCursor to sort large numbers of keys for CREATE INDEX statements
** or by SELECT statements with ORDER BY clauses that cannot be satisfied
** using indexes and without LIMIT clauses.
**
** The VdbeSorter object implements a multi-threaded external merge sort
** algorithm that is efficient even if the number of elements being sorted
** exceeds the available memory.
**
** Here is the (internal, non-API) interface between this module and the
** rest of the SQLite system:
**
**    sqlite3VdbeSorterInit()       Create a new VdbeSorter object.
**
**    sqlite3VdbeSorterWrite()      Add a single new row to the VdbeSorter
**                                  object.  The row is a binary blob in the
**                                  OP_MakeRecord format that contains both
**                                  the ORDER BY key columns and result columns
**                                  in the case of a SELECT w/ ORDER BY, or
**                                  the complete record for an index entry
**                                  in the case of a CREATE INDEX.
**
**    sqlite3VdbeSorterRewind()     Sort all content previously added.
**                                  Position the read cursor on the
**                                  first sorted element.
**
**    sqlite3VdbeSorterNext()       Advance the read cursor to the next sorted
**                                  element.
**
**    sqlite3VdbeSorterRowkey()     Return the complete binary blob for the
**                                  row currently under the read cursor.
**
**    sqlite3VdbeSorterCompare()    Compare the binary blob for the row
**                                  currently under the read cursor against
**                                  another binary blob X and report if
**                                  X is strictly less than the read cursor.
**                                  Used to enforce uniqueness in a
**                                  CREATE UNIQUE INDEX statement.
**
**    sqlite3VdbeSorterClose()      Close the VdbeSorter object and reclaim
**                                  all resources.
**
**    sqlite3VdbeSorterReset()      Refurbish the VdbeSorter for reuse.  This
**                                  is like Close() followed by Init() only
**                                  much faster.
**
** The interfaces above must be called in a particular order.  Write() can 
** only occur in between Init()/Reset() and Rewind().  Next(), Rowkey(), and
** Compare() can only occur in between Rewind() and Close()/Reset(). i.e.
**
**   Init()
**   for each record: Write()
**   Rewind()
**     Rowkey()/Compare()
**   Next() 
**   Close()
**
** Algorithm:
**
** Records passed to the sorter via calls to Write() are initially held 
** unsorted in main memory. Assuming the amount of memory used never exceeds
** a threshold, when Rewind() is called the set of records is sorted using
** an in-memory merge sort. In this case, no temporary files are required
** and subsequent calls to Rowkey(), Next() and Compare() read records 
** directly from main memory.
**
** If the amount of space used to store records in main memory exceeds the
** threshold, then the set of records currently in memory are sorted and
** written to a temporary file in "Packed Memory Array" (PMA) format.
** A PMA created at this point is known as a "level-0 PMA". Higher levels
** of PMAs may be created by merging existing PMAs together - for example
** merging two or more level-0 PMAs together creates a level-1 PMA.
**
** The threshold for the amount of main memory to use before flushing 
** records to a PMA is roughly the same as the limit configured for the
** page-cache of the main database. Specifically, the threshold is set to 
** the value returned multiplied by "PRAGMA main.page_size" multipled by 
** that returned by "PRAGMA main.cache_size", in bytes.
**
** If the sorter is running in single-threaded mode, then all PMAs generated
** are appended to a single temporary file. Or, if the sorter is running in
** multi-threaded mode then up to (N+1) temporary files may be opened, where
** N is the configured number of worker threads. In this case, instead of
** sorting the records and writing the PMA to a temporary file itself, the
** calling thread usually launches a worker thread to do so. Except, if
** there are already N worker threads running, the main thread does the work
** itself.
**
** The sorter is running in multi-threaded mode if (a) the library was built
** with pre-processor symbol SQLITE_MAX_WORKER_THREADS set to a value greater
** than zero, and (b) worker threads have been enabled at runtime by calling
** sqlite3_config(SQLITE_CONFIG_WORKER_THREADS, ...).
**
** When Rewind() is called, any data remaining in memory is flushed to a 
** final PMA. So at this point the data is stored in some number of sorted
** PMAs within temporary files on disk.
**
** If there are fewer than SORTER_MAX_MERGE_COUNT PMAs in total and the
** sorter is running in single-threaded mode, then these PMAs are merged
** incrementally as keys are retreived from the sorter by the VDBE. See
** comments above object MergeEngine below for details.
**
** Or, if running in multi-threaded mode, then a background thread is
** launched to merge the existing PMAs. Once the background thread has
** merged T bytes of data into a single sorted PMA, the main thread 
** begins reading keys from that PMA while the background thread proceeds
** with merging the next T bytes of data. And so on.
**
** Parameter T is set to half the value of the memory threshold used 
** by Write() above to determine when to create a new PMA.
**
** If there are more than SORTER_MAX_MERGE_COUNT PMAs in total when 
** Rewind() is called, then a hierarchy of incremental-merges is used. 
** First, T bytes of data from the first SORTER_MAX_MERGE_COUNT PMAs on 
** disk are merged together. Then T bytes of data from the second set, and
** so on, such that no operation ever merges more than SORTER_MAX_MERGE_COUNT
** PMAs at a time. This done is to improve locality.
**
** If running in multi-threaded mode and there are more than
** SORTER_MAX_MERGE_COUNT PMAs on disk when Rewind() is called, then more
** than one background thread may be created. Specifically, there may be
** one background thread for each temporary file on disk, and one background
** thread to merge the output of each of the others to a single PMA for
** the main thread to read from.
*/
#include "sqliteInt.h"
#include "vdbeInt.h"

/* 
** If SQLITE_DEBUG_SORTER_THREADS is defined, this module outputs various
** messages to stderr that may be helpful in understanding the performance
** characteristics of the sorter in multi-threaded mode.
*/
#if 0
# define SQLITE_DEBUG_SORTER_THREADS 1
#endif

/*
** Private objects used by the sorter
*/
typedef struct MergeEngine MergeEngine;     /* Merge PMAs together */
typedef struct PmaReader PmaReader;         /* Incrementally read one PMA */
typedef struct PmaWriter PmaWriter;         /* Incrementally write one PMA */
typedef struct SorterRecord SorterRecord;   /* A record being sorted */
typedef struct SortSubtask SortSubtask;     /* A sub-task in the sort process */
typedef struct SorterFile SorterFile;       /* Temporary file object wrapper */
typedef struct SorterList SorterList;       /* In-memory list of records */
typedef struct IncrMerger IncrMerger;       /* Read & merge multiple PMAs */

/*
** A container for a temp file handle and the current amount of data 
** stored in the file.
*/
struct SorterFile {
  sqlite3_file *pFd;              /* File handle */
  i64 iEof;                       /* Bytes of data stored in pFd */
};

/*
** An in-memory list of objects to be sorted.
**
** If aMemory==0 then each object is allocated separately and the objects
** are connected using SorterRecord.u.pNext.  If aMemory!=0 then all objects
** are stored in the aMemory[] bulk memory, one right after the other, and
** are connected using SorterRecord.u.iNext.
*/
struct SorterList {
  SorterRecord *pList;            /* Linked list of records */
  u8 *aMemory;                    /* If non-NULL, bulk memory to hold pList */
  int szPMA;                      /* Size of pList as PMA in bytes */
};

/*
** The MergeEngine object is used to combine two or more smaller PMAs into
** one big PMA using a merge operation.  Separate PMAs all need to be
** combined into one big PMA in order to be able to step through the sorted
** records in order.
**
** The aIter[] array contains a PmaReader object for each of the PMAs being
** merged.  An aIter[] object either points to a valid key or else is at EOF.
** For the purposes of the paragraphs below, we assume that the array is
** actually N elements in size, where N is the smallest power of 2 greater
** to or equal to the number of PMAs being merged. The extra aIter[] elements
** are treated as if they are empty (always at EOF).
**
** The aTree[] array is also N elements in size. The value of N is stored in
** the MergeEngine.nTree variable.
**
** The final (N/2) elements of aTree[] contain the results of comparing
** pairs of PMA keys together. Element i contains the result of 
** comparing aIter[2*i-N] and aIter[2*i-N+1]. Whichever key is smaller, the
** aTree element is set to the index of it. 
**
** For the purposes of this comparison, EOF is considered greater than any
** other key value. If the keys are equal (only possible with two EOF
** values), it doesn't matter which index is stored.
**
** The (N/4) elements of aTree[] that precede the final (N/2) described 
** above contains the index of the smallest of each block of 4 iterators.
** And so on. So that aTree[1] contains the index of the iterator that 
** currently points to the smallest key value. aTree[0] is unused.
**
** Example:
**
**     aIter[0] -> Banana
**     aIter[1] -> Feijoa
**     aIter[2] -> Elderberry
**     aIter[3] -> Currant
**     aIter[4] -> Grapefruit
**     aIter[5] -> Apple
**     aIter[6] -> Durian
**     aIter[7] -> EOF
**
**     aTree[] = { X, 5   0, 5    0, 3, 5, 6 }
**
** The current element is "Apple" (the value of the key indicated by 
** iterator 5). When the Next() operation is invoked, iterator 5 will
** be advanced to the next key in its segment. Say the next key is
** "Eggplant":
**
**     aIter[5] -> Eggplant
**
** The contents of aTree[] are updated first by comparing the new iterator
** 5 key to the current key of iterator 4 (still "Grapefruit"). The iterator
** 5 value is still smaller, so aTree[6] is set to 5. And so on up the tree.
** The value of iterator 6 - "Durian" - is now smaller than that of iterator
** 5, so aTree[3] is set to 6. Key 0 is smaller than key 6 (Banana<Durian),
** so the value written into element 1 of the array is 0. As follows:
**
**     aTree[] = { X, 0   0, 6    0, 3, 5, 6 }
**
** In other words, each time we advance to the next sorter element, log2(N)
** key comparison operations are required, where N is the number of segments
** being merged (rounded up to the next power of 2).
*/
struct MergeEngine {
  int nTree;                 /* Used size of aTree/aIter (power of 2) */
  int *aTree;                /* Current state of incremental merge */
  PmaReader *aIter;          /* Array of iterators to merge data from */
};

/*
** Exactly VdbeSorter.nTask instances of this object are allocated
** as part of each VdbeSorter object. Instances are never allocated any
** other way. VdbeSorter.nTask is set to the number of worker threads allowed
** (see SQLITE_CONFIG_WORKER_THREADS) plus one (the main thread).
**
** Essentially, this structure contains all those fields of the VdbeSorter
** structure for which each thread requires a separate instance. For example,
** each thread requries its own UnpackedRecord object to unpack records in
** as part of comparison operations.
**
** Before a background thread is launched, variable bDone is set to 0. Then, 
** right before it exits, the thread itself sets bDone to 1. This is used for 
** two purposes:
**
**   1. When flushing the contents of memory to a level-0 PMA on disk, to
**      attempt to select a SortSubtask for which there is not already an
**      active background thread (since doing so causes the main thread
**      to block until it finishes).
**
**   2. If SQLITE_DEBUG_SORTER_THREADS is defined, to determine if a call
**      to sqlite3ThreadJoin() is likely to block. Cases that are likely to
**      block provoke debugging output.
**
** In both cases, the effects of the main thread seeing (bDone==0) even
** after the thread has finished are not dire. So we don't worry about
** memory barriers and such here.
*/
struct SortSubtask {
  SQLiteThread *pThread;          /* Background thread, if any */
  int bDone;                      /* Set if thread is finished but not joined */
  VdbeSorter *pSorter;            /* Sorter that owns this sub-task */
  UnpackedRecord *pUnpacked;      /* Space to unpack a record */
  SorterList list;                /* List for thread to write to a PMA */
  int nPMA;                       /* Number of PMAs currently in file */
  SorterFile file;                /* Temp file for level-0 PMAs */
  SorterFile file2;               /* Space for other PMAs */
};

/*
** Main sorter structure. A single instance of this is allocated for each 
** sorter cursor created by the VDBE.
**
** mxKeysize:
**   As records are added to the sorter by calls to sqlite3VdbeSorterWrite(),
**   this variable is updated so as to be set to the size on disk of the
**   largest record in the sorter.
*/
struct VdbeSorter {
  int mnPmaSize;                  /* Minimum PMA size, in bytes */
  int mxPmaSize;                  /* Maximum PMA size, in bytes.  0==no limit */
  int mxKeysize;                  /* Largest serialized key seen so far */
  int pgsz;                       /* Main database page size */
  PmaReader *pReader;             /* Read data from here after Rewind() */
  MergeEngine *pMerger;           /* Or here, if bUseThreads==0 */
  sqlite3 *db;                    /* Database connection */
  KeyInfo *pKeyInfo;              /* How to compare records */
  UnpackedRecord *pUnpacked;      /* Used by VdbeSorterCompare() */
  SorterList list;                /* List of in-memory records */
  int iMemory;                    /* Offset of free space in list.aMemory */
  int nMemory;                    /* Size of list.aMemory allocation in bytes */
  u8 bUsePMA;                     /* True if one or more PMAs created */
  u8 bUseThreads;                 /* True to use background threads */
  u8 iPrev;                       /* Previous thread used to flush PMA */
  u8 nTask;                       /* Size of aTask[] array */
  SortSubtask aTask[1];           /* One or more subtasks */
};

/*
** An instance of the following object is used to read records out of a
** PMA, in sorted order.  The next key to be read is cached in nKey/aKey.
** pFile==0 at EOF.
*/
struct PmaReader {
  i64 iReadOff;                   /* Current read offset */
  i64 iEof;                       /* 1 byte past EOF for this iterator */
  int nAlloc;                     /* Bytes of space at aAlloc */
  int nKey;                       /* Number of bytes in key */
  sqlite3_file *pFile;            /* File iterator is reading from */
  u8 *aAlloc;                     /* Allocated space */
  u8 *aKey;                       /* Pointer to current key */
  u8 *aBuffer;                    /* Current read buffer */
  int nBuffer;                    /* Size of read buffer in bytes */
  u8 *aMap;                       /* Pointer to mapping of entire file */
  IncrMerger *pIncr;              /* Incremental merger */
};

/*
** Normally, a PmaReader object iterates through an existing PMA stored 
** within a temp file. However, if the PmaReader.pIncr variable points to
** an object of the following type, it may be used to iterate/merge through
** multiple PMAs simultaneously.
**
** There are two types of IncrMerger object - single (bUseThread==0) and 
** multi-threaded (bUseThread==1). 
**
** A multi-threaded IncrMerger object uses two temporary files - aFile[0] 
** and aFile[1]. Neither file is allowed to grow to more than mxSz bytes in 
** size. When the IncrMerger is initialized, it reads enough data from 
** pMerger to populate aFile[0]. It then sets variables within the 
** corresponding PmaReader object to read from that file and kicks off 
** a background thread to populate aFile[1] with the next mxSz bytes of 
** sorted record data from pMerger. 
**
** When the PmaReader reaches the end of aFile[0], it blocks until the
** background thread has finished populating aFile[1]. It then exchanges
** the contents of the aFile[0] and aFile[1] variables within this structure,
** sets the PmaReader fields to read from the new aFile[0] and kicks off
** another background thread to populate the new aFile[1]. And so on, until
** the contents of pMerger are exhausted.
**
** A single-threaded IncrMerger does not open any temporary files of its
** own. Instead, it has exclusive access to mxSz bytes of space beginning
** at offset iStartOff of file pTask->file2. And instead of using a 
** background thread to prepare data for the PmaReader, with a single
** threaded IncrMerger the allocate part of pTask->file2 is "refilled" with
** keys from pMerger by the calling thread whenever the PmaReader runs out
** of data.
*/
struct IncrMerger {
  SortSubtask *pTask;             /* Task that owns this merger */
  MergeEngine *pMerger;           /* Merge engine thread reads data from */
  i64 iStartOff;                  /* Offset to start writing file at */
  int mxSz;                       /* Maximum bytes of data to store */
  int bEof;                       /* Set to true when merge is finished */
  int bUseThread;                 /* True to use a bg thread for this object */
  SorterFile aFile[2];            /* aFile[0] for reading, [1] for writing */
};

/*
** An instance of this object is used for writing a PMA.
**
** The PMA is written one record at a time.  Each record is of an arbitrary
** size.  But I/O is more efficient if it occurs in page-sized blocks where
** each block is aligned on a page boundary.  This object caches writes to
** the PMA so that aligned, page-size blocks are written.
*/
struct PmaWriter {
  int eFWErr;                     /* Non-zero if in an error state */
  u8 *aBuffer;                    /* Pointer to write buffer */
  int nBuffer;                    /* Size of write buffer in bytes */
  int iBufStart;                  /* First byte of buffer to write */
  int iBufEnd;                    /* Last byte of buffer to write */
  i64 iWriteOff;                  /* Offset of start of buffer in file */
  sqlite3_file *pFile;            /* File to write to */
};

/*
** This object is the header on a single record while that record is being
** held in memory and prior to being written out as part of a PMA.
**
** How the linked list is connected depends on how memory is being managed
** by this module. If using a separate allocation for each in-memory record
** (VdbeSorter.list.aMemory==0), then the list is always connected using the
** SorterRecord.u.pNext pointers.
**
** Or, if using the single large allocation method (VdbeSorter.list.aMemory!=0),
** then while records are being accumulated the list is linked using the
** SorterRecord.u.iNext offset. This is because the aMemory[] array may
** be sqlite3Realloc()ed while records are being accumulated. Once the VM
** has finished passing records to the sorter, or when the in-memory buffer
** is full, the list is sorted. As part of the sorting process, it is
** converted to use the SorterRecord.u.pNext pointers. See function
** vdbeSorterSort() for details.
*/
struct SorterRecord {
  int nVal;                       /* Size of the record in bytes */
  union {
    SorterRecord *pNext;          /* Pointer to next record in list */
    int iNext;                    /* Offset within aMemory of next record */
  } u;
  /* The data for the record immediately follows this header */
};

/* Return a pointer to the buffer containing the record data for SorterRecord
** object p. Should be used as if:
**
**   void *SRVAL(SorterRecord *p) { return (void*)&p[1]; }
*/
#define SRVAL(p) ((void*)((SorterRecord*)(p) + 1))

/* The minimum PMA size is set to this value multiplied by the database
** page size in bytes.  */
#define SORTER_MIN_WORKING 10

/* Maximum number of PMAs that a single MergeEngine can merge */
#define SORTER_MAX_MERGE_COUNT 16

static int vdbeIncrSwap(IncrMerger*);
static void vdbeIncrFree(IncrMerger *);

/*
** Free all memory belonging to the PmaReader object passed as the second
** argument. All structure fields are set to zero before returning.
*/
static void vdbePmaReaderClear(PmaReader *pIter){
  sqlite3_free(pIter->aAlloc);
  sqlite3_free(pIter->aBuffer);
  if( pIter->aMap ) sqlite3OsUnfetch(pIter->pFile, 0, pIter->aMap);
  vdbeIncrFree(pIter->pIncr);
  memset(pIter, 0, sizeof(PmaReader));
}

/*
** Read nByte bytes of data from the stream of data iterated by object p.
** If successful, set *ppOut to point to a buffer containing the data
** and return SQLITE_OK. Otherwise, if an error occurs, return an SQLite
** error code.
**
** The buffer indicated by *ppOut may only be considered valid until the
** next call to this function.
*/
static int vdbePmaReadBlob(
  PmaReader *p,                   /* Iterator */
  int nByte,                      /* Bytes of data to read */
  u8 **ppOut                      /* OUT: Pointer to buffer containing data */
){
  int iBuf;                       /* Offset within buffer to read from */
  int nAvail;                     /* Bytes of data available in buffer */

  if( p->aMap ){
    *ppOut = &p->aMap[p->iReadOff];
    p->iReadOff += nByte;
    return SQLITE_OK;
  }

  assert( p->aBuffer );

  /* If there is no more data to be read from the buffer, read the next 
  ** p->nBuffer bytes of data from the file into it. Or, if there are less
  ** than p->nBuffer bytes remaining in the PMA, read all remaining data.  */
  iBuf = p->iReadOff % p->nBuffer;
  if( iBuf==0 ){
    int nRead;                    /* Bytes to read from disk */
    int rc;                       /* sqlite3OsRead() return code */

    /* Determine how many bytes of data to read. */
    if( (p->iEof - p->iReadOff) > (i64)p->nBuffer ){
      nRead = p->nBuffer;
    }else{
      nRead = (int)(p->iEof - p->iReadOff);
    }
    assert( nRead>0 );

    /* Read data from the file. Return early if an error occurs. */
    rc = sqlite3OsRead(p->pFile, p->aBuffer, nRead, p->iReadOff);
    assert( rc!=SQLITE_IOERR_SHORT_READ );
    if( rc!=SQLITE_OK ) return rc;
  }
  nAvail = p->nBuffer - iBuf; 

  if( nByte<=nAvail ){
    /* The requested data is available in the in-memory buffer. In this
    ** case there is no need to make a copy of the data, just return a 
    ** pointer into the buffer to the caller.  */
    *ppOut = &p->aBuffer[iBuf];
    p->iReadOff += nByte;
  }else{
    /* The requested data is not all available in the in-memory buffer.
    ** In this case, allocate space at p->aAlloc[] to copy the requested
    ** range into. Then return a copy of pointer p->aAlloc to the caller.  */
    int nRem;                     /* Bytes remaining to copy */

    /* Extend the p->aAlloc[] allocation if required. */
    if( p->nAlloc<nByte ){
      u8 *aNew;
      int nNew = MAX(128, p->nAlloc*2);
      while( nByte>nNew ) nNew = nNew*2;
      aNew = sqlite3Realloc(p->aAlloc, nNew);
      if( !aNew ) return SQLITE_NOMEM;
      p->nAlloc = nNew;
      p->aAlloc = aNew;
    }

    /* Copy as much data as is available in the buffer into the start of
    ** p->aAlloc[].  */
    memcpy(p->aAlloc, &p->aBuffer[iBuf], nAvail);
    p->iReadOff += nAvail;
    nRem = nByte - nAvail;

    /* The following loop copies up to p->nBuffer bytes per iteration into
    ** the p->aAlloc[] buffer.  */
    while( nRem>0 ){
      int rc;                     /* vdbePmaReadBlob() return code */
      int nCopy;                  /* Number of bytes to copy */
      u8 *aNext;                  /* Pointer to buffer to copy data from */

      nCopy = nRem;
      if( nRem>p->nBuffer ) nCopy = p->nBuffer;
      rc = vdbePmaReadBlob(p, nCopy, &aNext);
      if( rc!=SQLITE_OK ) return rc;
      assert( aNext!=p->aAlloc );
      memcpy(&p->aAlloc[nByte - nRem], aNext, nCopy);
      nRem -= nCopy;
    }

    *ppOut = p->aAlloc;
  }

  return SQLITE_OK;
}

/*
** Read a varint from the stream of data accessed by p. Set *pnOut to
** the value read.
*/
static int vdbePmaReadVarint(PmaReader *p, u64 *pnOut){
  int iBuf;

  if( p->aMap ){
    p->iReadOff += sqlite3GetVarint(&p->aMap[p->iReadOff], pnOut);
  }else{
    iBuf = p->iReadOff % p->nBuffer;
    if( iBuf && (p->nBuffer-iBuf)>=9 ){
      p->iReadOff += sqlite3GetVarint(&p->aBuffer[iBuf], pnOut);
    }else{
      u8 aVarint[16], *a;
      int i = 0, rc;
      do{
        rc = vdbePmaReadBlob(p, 1, &a);
        if( rc ) return rc;
        aVarint[(i++)&0xf] = a[0];
      }while( (a[0]&0x80)!=0 );
      sqlite3GetVarint(aVarint, pnOut);
    }
  }

  return SQLITE_OK;
}

/*
** Attempt to memory map file pFile. If successful, set *pp to point to the
** new mapping and return SQLITE_OK. If the mapping is not attempted 
** (because the file is too large or the VFS layer is configured not to use
** mmap), return SQLITE_OK and set *pp to NULL.
**
** Or, if an error occurs, return an SQLite error code. The final value of
** *pp is undefined in this case.
*/
static int vdbeSorterMapFile(SortSubtask *pTask, SorterFile *pFile, u8 **pp){
  int rc = SQLITE_OK;
  if( pFile->iEof<=(i64)(pTask->pSorter->db->nMaxSorterMmap) ){
    rc = sqlite3OsFetch(pFile->pFd, 0, (int)pFile->iEof, (void**)pp);
  }
  return rc;
}

/*
** Seek iterator pIter to offset iOff within file pFile. Return SQLITE_OK 
** if successful, or an SQLite error code if an error occurs.
*/
static int vdbePmaReaderSeek(
  SortSubtask *pTask,             /* Task context */
  PmaReader *pIter,               /* Iterate to populate */
  SorterFile *pFile,              /* Sorter file to read from */
  i64 iOff                        /* Offset in pFile */
){
  int rc = SQLITE_OK;

  assert( pIter->pIncr==0 || pIter->pIncr->bEof==0 );

  if( pIter->aMap ){
    sqlite3OsUnfetch(pIter->pFile, 0, pIter->aMap);
    pIter->aMap = 0;
  }
  pIter->iReadOff = iOff;
  pIter->iEof = pFile->iEof;
  pIter->pFile = pFile->pFd;

  rc = vdbeSorterMapFile(pTask, pFile, &pIter->aMap);
  if( rc==SQLITE_OK && pIter->aMap==0 ){
    int pgsz = pTask->pSorter->pgsz;
    int iBuf = pIter->iReadOff % pgsz;
    if( pIter->aBuffer==0 ){
      pIter->aBuffer = (u8*)sqlite3Malloc(pgsz);
      if( pIter->aBuffer==0 ) rc = SQLITE_NOMEM;
      pIter->nBuffer = pgsz;
    }
    if( rc==SQLITE_OK && iBuf ){
      int nRead = pgsz - iBuf;
      if( (pIter->iReadOff + nRead) > pIter->iEof ){
        nRead = (int)(pIter->iEof - pIter->iReadOff);
      }
      rc = sqlite3OsRead(
          pIter->pFile, &pIter->aBuffer[iBuf], nRead, pIter->iReadOff
      );
      assert( rc!=SQLITE_IOERR_SHORT_READ );
    }
  }

  return rc;
}

/*
** Advance iterator pIter to the next key in its PMA. Return SQLITE_OK if
** no error occurs, or an SQLite error code if one does.
*/
static int vdbePmaReaderNext(PmaReader *pIter){
  int rc = SQLITE_OK;             /* Return Code */
  u64 nRec = 0;                   /* Size of record in bytes */


  if( pIter->iReadOff>=pIter->iEof ){
    IncrMerger *pIncr = pIter->pIncr;
    int bEof = 1;
    if( pIncr ){
      rc = vdbeIncrSwap(pIncr);
      if( rc==SQLITE_OK && pIncr->bEof==0 ){
        rc = vdbePmaReaderSeek(
            pIncr->pTask, pIter, &pIncr->aFile[0], pIncr->iStartOff
        );
        bEof = 0;
      }
    }

    if( bEof ){
      /* This is an EOF condition */
      vdbePmaReaderClear(pIter);
      return rc;
    }
  }

  if( rc==SQLITE_OK ){
    rc = vdbePmaReadVarint(pIter, &nRec);
  }
  if( rc==SQLITE_OK ){
    pIter->nKey = (int)nRec;
    rc = vdbePmaReadBlob(pIter, (int)nRec, &pIter->aKey);
  }

  return rc;
}

/*
** Initialize iterator pIter to scan through the PMA stored in file pFile
** starting at offset iStart and ending at offset iEof-1. This function 
** leaves the iterator pointing to the first key in the PMA (or EOF if the 
** PMA is empty).
**
** If the pnByte parameter is NULL, then it is assumed that the file 
** contains a single PMA, and that that PMA omits the initial length varint.
*/
static int vdbePmaReaderInit(
  SortSubtask *pTask,             /* Task context */
  SorterFile *pFile,              /* Sorter file to read from */
  i64 iStart,                     /* Start offset in pFile */
  PmaReader *pIter,               /* Iterator to populate */
  i64 *pnByte                     /* IN/OUT: Increment this value by PMA size */
){
  int rc;

  assert( pFile->iEof>iStart );
  assert( pIter->aAlloc==0 && pIter->nAlloc==0 );
  assert( pIter->aBuffer==0 );
  assert( pIter->aMap==0 );

  rc = vdbePmaReaderSeek(pTask, pIter, pFile, iStart);
  if( rc==SQLITE_OK ){
    u64 nByte;                    /* Size of PMA in bytes */
    rc = vdbePmaReadVarint(pIter, &nByte);
    pIter->iEof = pIter->iReadOff + nByte;
    *pnByte += nByte;
  }

  if( rc==SQLITE_OK ){
    rc = vdbePmaReaderNext(pIter);
  }
  return rc;
}


/*
** Compare key1 (buffer pKey1, size nKey1 bytes) with key2 (buffer pKey2, 
** size nKey2 bytes). Use (pTask->pKeyInfo) for the collation sequences
** used by the comparison. Return the result of the comparison.
**
** Before returning, object (pTask->pUnpacked) is populated with the
** unpacked version of key2. Or, if pKey2 is passed a NULL pointer, then it 
** is assumed that the (pTask->pUnpacked) structure already contains the 
** unpacked key to use as key2.
**
** If an OOM error is encountered, (pTask->pUnpacked->error_rc) is set
** to SQLITE_NOMEM.
*/
static int vdbeSorterCompare(
  SortSubtask *pTask,             /* Subtask context (for pKeyInfo) */
  const void *pKey1, int nKey1,   /* Left side of comparison */
  const void *pKey2, int nKey2    /* Right side of comparison */
){
  UnpackedRecord *r2 = pTask->pUnpacked;
  if( pKey2 ){
    sqlite3VdbeRecordUnpack(pTask->pSorter->pKeyInfo, nKey2, pKey2, r2);
  }
  return sqlite3VdbeRecordCompare(nKey1, pKey1, r2, 0);
}

/*
** This function is called to compare two iterator keys when merging 
** multiple b-tree segments. Parameter iOut is the index of the aTree[] 
** value to recalculate.
*/
static int vdbeSorterDoCompare(
  SortSubtask *pTask, 
  MergeEngine *pMerger, 
  int iOut
){
  int i1;
  int i2;
  int iRes;
  PmaReader *p1;
  PmaReader *p2;

  assert( iOut<pMerger->nTree && iOut>0 );

  if( iOut>=(pMerger->nTree/2) ){
    i1 = (iOut - pMerger->nTree/2) * 2;
    i2 = i1 + 1;
  }else{
    i1 = pMerger->aTree[iOut*2];
    i2 = pMerger->aTree[iOut*2+1];
  }

  p1 = &pMerger->aIter[i1];
  p2 = &pMerger->aIter[i2];

  if( p1->pFile==0 ){
    iRes = i2;
  }else if( p2->pFile==0 ){
    iRes = i1;
  }else{
    int res;
    assert( pTask->pUnpacked!=0 );  /* allocated in vdbeSortSubtaskMain() */
    res = vdbeSorterCompare(
        pTask, p1->aKey, p1->nKey, p2->aKey, p2->nKey
    );
    if( res<=0 ){
      iRes = i1;
    }else{
      iRes = i2;
    }
  }

  pMerger->aTree[iOut] = iRes;
  return SQLITE_OK;
}

/*
** Initialize the temporary index cursor just opened as a sorter cursor.
**
** Usually, the sorter module uses the value of (pCsr->pKeyInfo->nField)
** to determine the number of fields that should be compared from the
** records being sorted. However, if the value passed as argument nField
** is non-zero and the sorter is able to guarantee a stable sort, nField
** is used instead. This is used when sorting records for a CREATE INDEX
** statement. In this case, keys are always delivered to the sorter in
** order of the primary key, which happens to be make up the final part 
** of the records being sorted. So if the sort is stable, there is never
** any reason to compare PK fields and they can be ignored for a small
** performance boost.
**
** The sorter can guarantee a stable sort when running in single-threaded
** mode, but not in multi-threaded mode.
**
** SQLITE_OK is returned if successful, or an SQLite error code otherwise.
*/
int sqlite3VdbeSorterInit(
  sqlite3 *db,                    /* Database connection (for malloc()) */
  int nField,                     /* Number of key fields in each record */
  VdbeCursor *pCsr                /* Cursor that holds the new sorter */
){
  int pgsz;                       /* Page size of main database */
  int i;                          /* Used to iterate through aTask[] */
  int mxCache;                    /* Cache size */
  VdbeSorter *pSorter;            /* The new sorter */
  KeyInfo *pKeyInfo;              /* Copy of pCsr->pKeyInfo with db==0 */
  int szKeyInfo;                  /* Size of pCsr->pKeyInfo in bytes */
  int sz;                         /* Size of pSorter in bytes */
  int rc = SQLITE_OK;
#if SQLITE_MAX_WORKER_THREADS==0
# define nWorker 0
#else
  int nWorker = (sqlite3GlobalConfig.bCoreMutex?sqlite3GlobalConfig.nWorker:0);
#endif

  assert( pCsr->pKeyInfo && pCsr->pBt==0 );
  szKeyInfo = sizeof(KeyInfo) + (pCsr->pKeyInfo->nField-1)*sizeof(CollSeq*);
  sz = sizeof(VdbeSorter) + nWorker * sizeof(SortSubtask);

  pSorter = (VdbeSorter*)sqlite3DbMallocZero(db, sz + szKeyInfo);
  pCsr->pSorter = pSorter;
  if( pSorter==0 ){
    rc = SQLITE_NOMEM;
  }else{
    pSorter->pKeyInfo = pKeyInfo = (KeyInfo*)((u8*)pSorter + sz);
    memcpy(pKeyInfo, pCsr->pKeyInfo, szKeyInfo);
    pKeyInfo->db = 0;
    if( nField && nWorker==0 ) pKeyInfo->nField = nField;
    pSorter->pgsz = pgsz = sqlite3BtreeGetPageSize(db->aDb[0].pBt);
    pSorter->nTask = nWorker + 1;
    pSorter->bUseThreads = (pSorter->nTask>1);
    pSorter->db = db;
    for(i=0; i<pSorter->nTask; i++){
      SortSubtask *pTask = &pSorter->aTask[i];
      pTask->pSorter = pSorter;
    }

    if( !sqlite3TempInMemory(db) ){
      pSorter->mnPmaSize = SORTER_MIN_WORKING * pgsz;
      mxCache = db->aDb[0].pSchema->cache_size;
      if( mxCache<SORTER_MIN_WORKING ) mxCache = SORTER_MIN_WORKING;
      pSorter->mxPmaSize = mxCache * pgsz;

      /* If the application has not configure scratch memory using
      ** SQLITE_CONFIG_SCRATCH then we assume it is OK to do large memory
      ** allocations.  If scratch memory has been configured, then assume
      ** large memory allocations should be avoided to prevent heap
      ** fragmentation.
      */
      if( sqlite3GlobalConfig.pScratch==0 ){
        assert( pSorter->iMemory==0 );
        pSorter->nMemory = pgsz;
        pSorter->list.aMemory = (u8*)sqlite3Malloc(pgsz);
        if( !pSorter->list.aMemory ) rc = SQLITE_NOMEM;
      }
    }
  }

  return rc;
}
#undef nWorker   /* Defined at the top of this function */

/*
** Free the list of sorted records starting at pRecord.
*/
static void vdbeSorterRecordFree(sqlite3 *db, SorterRecord *pRecord){
  SorterRecord *p;
  SorterRecord *pNext;
  for(p=pRecord; p; p=pNext){
    pNext = p->u.pNext;
    sqlite3DbFree(db, p);
  }
}

/*
** Free all resources owned by the object indicated by argument pTask. All 
** fields of *pTask are zeroed before returning.
*/
static void vdbeSortSubtaskCleanup(sqlite3 *db, SortSubtask *pTask){
  sqlite3DbFree(db, pTask->pUnpacked);
  pTask->pUnpacked = 0;
  if( pTask->list.aMemory==0 ){
    vdbeSorterRecordFree(0, pTask->list.pList);
  }else{
    sqlite3_free(pTask->list.aMemory);
    pTask->list.aMemory = 0;
  }
  pTask->list.pList = 0;
  if( pTask->file.pFd ){
    sqlite3OsCloseFree(pTask->file.pFd);
    pTask->file.pFd = 0;
    pTask->file.iEof = 0;
  }
  if( pTask->file2.pFd ){
    sqlite3OsCloseFree(pTask->file2.pFd);
    pTask->file2.pFd = 0;
    pTask->file2.iEof = 0;
  }
}

#ifdef SQLITE_DEBUG_SORTER_THREADS
static void vdbeSorterWorkDebug(SortSubtask *pTask, const char *zEvent){
  i64 t;
  int iTask = (pTask - pTask->pSorter->aTask);
  sqlite3OsCurrentTimeInt64(pTask->pSorter->db->pVfs, &t);
  fprintf(stderr, "%lld:%d %s\n", t, iTask, zEvent);
}
static void vdbeSorterRewindDebug(const char *zEvent){
  i64 t;
  sqlite3OsCurrentTimeInt64(sqlite3_vfs_find(0), &t);
  fprintf(stderr, "%lld:X %s\n", t, zEvent);
}
static void vdbeSorterPopulateDebug(
  SortSubtask *pTask,
  const char *zEvent
){
  i64 t;
  int iTask = (pTask - pTask->pSorter->aTask);
  sqlite3OsCurrentTimeInt64(pTask->pSorter->db->pVfs, &t);
  fprintf(stderr, "%lld:bg%d %s\n", t, iTask, zEvent);
}
static void vdbeSorterBlockDebug(
  SortSubtask *pTask,
  int bBlocked,
  const char *zEvent
){
  if( bBlocked ){
    i64 t;
    sqlite3OsCurrentTimeInt64(pTask->pSorter->db->pVfs, &t);
    fprintf(stderr, "%lld:main %s\n", t, zEvent);
  }
}
#else
# define vdbeSorterWorkDebug(x,y)
# define vdbeSorterRewindDebug(y)
# define vdbeSorterPopulateDebug(x,y)
# define vdbeSorterBlockDebug(x,y,z)
#endif

#if SQLITE_MAX_WORKER_THREADS>0
/*
** Join thread pTask->thread.
*/
static int vdbeSorterJoinThread(SortSubtask *pTask){
  int rc = SQLITE_OK;
  if( pTask->pThread ){
#ifdef SQLITE_DEBUG_SORTER_THREADS
    int bDone = pTask->bDone;
#endif
    void *pRet;
    vdbeSorterBlockDebug(pTask, !bDone, "enter");
    rc = sqlite3ThreadJoin(pTask->pThread, &pRet);
    vdbeSorterBlockDebug(pTask, !bDone, "exit");
    if( rc==SQLITE_OK ) rc = SQLITE_PTR_TO_INT(pRet);
    assert( pTask->bDone==1 );
    pTask->bDone = 0;
    pTask->pThread = 0;
  }
  return rc;
}

/*
** Launch a background thread to run xTask(pIn).
*/
static int vdbeSorterCreateThread(
  SortSubtask *pTask,             /* Thread will use this task object */
  void *(*xTask)(void*),          /* Routine to run in a separate thread */
  void *pIn                       /* Argument passed into xTask() */
){
  assert( pTask->pThread==0 && pTask->bDone==0 );
  return sqlite3ThreadCreate(&pTask->pThread, xTask, pIn);
}

/*
** Join all outstanding threads launched by SorterWrite() to create 
** level-0 PMAs.
*/
static int vdbeSorterJoinAll(VdbeSorter *pSorter, int rcin){
  int rc = rcin;
  int i;

  /* This function is always called by the main user thread.
  **
  ** If this function is being called after SorterRewind() has been called, 
  ** it is possible that thread pSorter->aTask[pSorter->nTask-1].pThread
  ** is currently attempt to join one of the other threads. To avoid a race
  ** condition where this thread also attempts to join the same object, join 
  ** thread pSorter->aTask[pSorter->nTask-1].pThread first. */
  for(i=pSorter->nTask-1; i>=0; i--){
    SortSubtask *pTask = &pSorter->aTask[i];
    int rc2 = vdbeSorterJoinThread(pTask);
    if( rc==SQLITE_OK ) rc = rc2;
  }
  return rc;
}
#else
# define vdbeSorterJoinAll(x,rcin) (rcin)
# define vdbeSorterJoinThread(pTask) SQLITE_OK
#endif

/*
** Allocate a new MergeEngine object with space for nIter iterators.
*/
static MergeEngine *vdbeMergeEngineNew(int nIter){
  int N = 2;                      /* Smallest power of two >= nIter */
  int nByte;                      /* Total bytes of space to allocate */
  MergeEngine *pNew;              /* Pointer to allocated object to return */

  assert( nIter<=SORTER_MAX_MERGE_COUNT );

  while( N<nIter ) N += N;
  nByte = sizeof(MergeEngine) + N * (sizeof(int) + sizeof(PmaReader));

  pNew = (MergeEngine*)sqlite3MallocZero(nByte);
  if( pNew ){
    pNew->nTree = N;
    pNew->aIter = (PmaReader*)&pNew[1];
    pNew->aTree = (int*)&pNew->aIter[N];
  }
  return pNew;
}

/*
** Free the MergeEngine object passed as the only argument.
*/
static void vdbeMergeEngineFree(MergeEngine *pMerger){
  int i;
  if( pMerger ){
    for(i=0; i<pMerger->nTree; i++){
      vdbePmaReaderClear(&pMerger->aIter[i]);
    }
  }
  sqlite3_free(pMerger);
}

/*
** Free all resources associated with the IncrMerger object indicated by
** the first argument.
*/
static void vdbeIncrFree(IncrMerger *pIncr){
  if( pIncr ){
#if SQLITE_MAX_WORKER_THREADS>0
    if( pIncr->bUseThread ){
      vdbeSorterJoinThread(pIncr->pTask);
      if( pIncr->aFile[0].pFd ) sqlite3OsCloseFree(pIncr->aFile[0].pFd);
      if( pIncr->aFile[1].pFd ) sqlite3OsCloseFree(pIncr->aFile[1].pFd);
    }
#endif
    vdbeMergeEngineFree(pIncr->pMerger);
    sqlite3_free(pIncr);
  }
}

/*
** Reset a sorting cursor back to its original empty state.
*/
void sqlite3VdbeSorterReset(sqlite3 *db, VdbeSorter *pSorter){
  int i;
  (void)vdbeSorterJoinAll(pSorter, SQLITE_OK);
  assert( pSorter->bUseThreads || pSorter->pReader==0 );
#if SQLITE_MAX_WORKER_THREADS>0
  if( pSorter->pReader ){
    vdbePmaReaderClear(pSorter->pReader);
    sqlite3DbFree(db, pSorter->pReader);
    pSorter->pReader = 0;
  }
#endif
  vdbeMergeEngineFree(pSorter->pMerger);
  pSorter->pMerger = 0;
  for(i=0; i<pSorter->nTask; i++){
    SortSubtask *pTask = &pSorter->aTask[i];
    vdbeSortSubtaskCleanup(db, pTask);
  }
  if( pSorter->list.aMemory==0 ){
    vdbeSorterRecordFree(0, pSorter->list.pList);
  }
  pSorter->list.pList = 0;
  pSorter->list.szPMA = 0;
  pSorter->bUsePMA = 0;
  pSorter->iMemory = 0;
  pSorter->mxKeysize = 0;
  sqlite3DbFree(db, pSorter->pUnpacked);
  pSorter->pUnpacked = 0;
}

/*
** Free any cursor components allocated by sqlite3VdbeSorterXXX routines.
*/
void sqlite3VdbeSorterClose(sqlite3 *db, VdbeCursor *pCsr){
  VdbeSorter *pSorter = pCsr->pSorter;
  if( pSorter ){
    sqlite3VdbeSorterReset(db, pSorter);
    sqlite3_free(pSorter->list.aMemory);
    sqlite3DbFree(db, pSorter);
    pCsr->pSorter = 0;
  }
}

#if SQLITE_MAX_MMAP_SIZE>0
/*
** The first argument is a file-handle open on a temporary file. The file
** is guaranteed to be nByte bytes or smaller in size. This function
** attempts to extend the file to nByte bytes in size and to ensure that
** the VFS has memory mapped it.
**
** Whether or not the file does end up memory mapped of course depends on
** the specific VFS implementation.
*/
static void vdbeSorterExtendFile(sqlite3 *db, sqlite3_file *pFile, i64 nByte){
  if( nByte<=(i64)(db->nMaxSorterMmap) ){
    int rc = sqlite3OsTruncate(pFile, nByte);
    if( rc==SQLITE_OK ){
      void *p = 0;
      sqlite3OsFetch(pFile, 0, (int)nByte, &p);
      sqlite3OsUnfetch(pFile, 0, p);
    }
  }
}
#else
# define vdbeSorterExtendFile(x,y,z) SQLITE_OK
#endif

/*
** Allocate space for a file-handle and open a temporary file. If successful,
** set *ppFile to point to the malloc'd file-handle and return SQLITE_OK.
** Otherwise, set *ppFile to 0 and return an SQLite error code.
*/
static int vdbeSorterOpenTempFile(
  sqlite3 *db,                    /* Database handle doing sort */
  i64 nExtend,                    /* Attempt to extend file to this size */
  sqlite3_file **ppFile
){
  int rc;
  rc = sqlite3OsOpenMalloc(db->pVfs, 0, ppFile,
      SQLITE_OPEN_TEMP_JOURNAL |
      SQLITE_OPEN_READWRITE    | SQLITE_OPEN_CREATE |
      SQLITE_OPEN_EXCLUSIVE    | SQLITE_OPEN_DELETEONCLOSE, &rc
  );
  if( rc==SQLITE_OK ){
    i64 max = SQLITE_MAX_MMAP_SIZE;
    sqlite3OsFileControlHint(*ppFile, SQLITE_FCNTL_MMAP_SIZE, (void*)&max);
    if( nExtend>0 ){
      vdbeSorterExtendFile(db, *ppFile, nExtend);
    }
  }
  return rc;
}

/*
** If it has not already been allocated, allocate the UnpackedRecord 
** structure at pTask->pUnpacked. Return SQLITE_OK if successful (or 
** if no allocation was required), or SQLITE_NOMEM otherwise.
*/
static int vdbeSortAllocUnpacked(SortSubtask *pTask){
  if( pTask->pUnpacked==0 ){
    char *pFree;
    pTask->pUnpacked = sqlite3VdbeAllocUnpackedRecord(
        pTask->pSorter->pKeyInfo, 0, 0, &pFree
    );
    assert( pTask->pUnpacked==(UnpackedRecord*)pFree );
    if( pFree==0 ) return SQLITE_NOMEM;
    pTask->pUnpacked->nField = pTask->pSorter->pKeyInfo->nField;
    pTask->pUnpacked->errCode = 0;
  }
  return SQLITE_OK;
}


/*
** Merge the two sorted lists p1 and p2 into a single list.
** Set *ppOut to the head of the new list.
*/
static void vdbeSorterMerge(
  SortSubtask *pTask,             /* Calling thread context */
  SorterRecord *p1,               /* First list to merge */
  SorterRecord *p2,               /* Second list to merge */
  SorterRecord **ppOut            /* OUT: Head of merged list */
){
  SorterRecord *pFinal = 0;
  SorterRecord **pp = &pFinal;
  void *pVal2 = p2 ? SRVAL(p2) : 0;

  while( p1 && p2 ){
    int res;
    res = vdbeSorterCompare(pTask, SRVAL(p1), p1->nVal, pVal2, p2->nVal);
    if( res<=0 ){
      *pp = p1;
      pp = &p1->u.pNext;
      p1 = p1->u.pNext;
      pVal2 = 0;
    }else{
      *pp = p2;
       pp = &p2->u.pNext;
      p2 = p2->u.pNext;
      if( p2==0 ) break;
      pVal2 = SRVAL(p2);
    }
  }
  *pp = p1 ? p1 : p2;
  *ppOut = pFinal;
}

/*
** Sort the linked list of records headed at pTask->pList. Return 
** SQLITE_OK if successful, or an SQLite error code (i.e. SQLITE_NOMEM) if 
** an error occurs.
*/
static int vdbeSorterSort(SortSubtask *pTask, SorterList *pList){
  int i;
  SorterRecord **aSlot;
  SorterRecord *p;
  int rc;

  rc = vdbeSortAllocUnpacked(pTask);
  if( rc!=SQLITE_OK ) return rc;

  aSlot = (SorterRecord **)sqlite3MallocZero(64 * sizeof(SorterRecord *));
  if( !aSlot ){
    return SQLITE_NOMEM;
  }

  p = pList->pList;
  while( p ){
    SorterRecord *pNext;
    if( pList->aMemory ){
      if( (u8*)p==pList->aMemory ){
        pNext = 0;
      }else{
        assert( p->u.iNext<sqlite3MallocSize(pList->aMemory) );
        pNext = (SorterRecord*)&pList->aMemory[p->u.iNext];
      }
    }else{
      pNext = p->u.pNext;
    }

    p->u.pNext = 0;
    for(i=0; aSlot[i]; i++){
      vdbeSorterMerge(pTask, p, aSlot[i], &p);
      aSlot[i] = 0;
    }
    aSlot[i] = p;
    p = pNext;
  }

  p = 0;
  for(i=0; i<64; i++){
    vdbeSorterMerge(pTask, p, aSlot[i], &p);
  }
  pList->pList = p;

  sqlite3_free(aSlot);
  assert( pTask->pUnpacked->errCode==SQLITE_OK 
       || pTask->pUnpacked->errCode==SQLITE_NOMEM 
  );
  return pTask->pUnpacked->errCode;
}

/*
** Initialize a PMA-writer object.
*/
static void vdbePmaWriterInit(
  sqlite3_file *pFile,            /* File to write to */
  PmaWriter *p,                   /* Object to populate */
  int nBuf,                       /* Buffer size */
  i64 iStart                      /* Offset of pFile to begin writing at */
){
  memset(p, 0, sizeof(PmaWriter));
  p->aBuffer = (u8*)sqlite3Malloc(nBuf);
  if( !p->aBuffer ){
    p->eFWErr = SQLITE_NOMEM;
  }else{
    p->iBufEnd = p->iBufStart = (iStart % nBuf);
    p->iWriteOff = iStart - p->iBufStart;
    p->nBuffer = nBuf;
    p->pFile = pFile;
  }
}

/*
** Write nData bytes of data to the PMA. Return SQLITE_OK
** if successful, or an SQLite error code if an error occurs.
*/
static void vdbePmaWriteBlob(PmaWriter *p, u8 *pData, int nData){
  int nRem = nData;
  while( nRem>0 && p->eFWErr==0 ){
    int nCopy = nRem;
    if( nCopy>(p->nBuffer - p->iBufEnd) ){
      nCopy = p->nBuffer - p->iBufEnd;
    }

    memcpy(&p->aBuffer[p->iBufEnd], &pData[nData-nRem], nCopy);
    p->iBufEnd += nCopy;
    if( p->iBufEnd==p->nBuffer ){
      p->eFWErr = sqlite3OsWrite(p->pFile, 
          &p->aBuffer[p->iBufStart], p->iBufEnd - p->iBufStart, 
          p->iWriteOff + p->iBufStart
      );
      p->iBufStart = p->iBufEnd = 0;
      p->iWriteOff += p->nBuffer;
    }
    assert( p->iBufEnd<p->nBuffer );

    nRem -= nCopy;
  }
}

/*
** Flush any buffered data to disk and clean up the PMA-writer object.
** The results of using the PMA-writer after this call are undefined.
** Return SQLITE_OK if flushing the buffered data succeeds or is not 
** required. Otherwise, return an SQLite error code.
**
** Before returning, set *piEof to the offset immediately following the
** last byte written to the file.
*/
static int vdbePmaWriterFinish(PmaWriter *p, i64 *piEof){
  int rc;
  if( p->eFWErr==0 && ALWAYS(p->aBuffer) && p->iBufEnd>p->iBufStart ){
    p->eFWErr = sqlite3OsWrite(p->pFile, 
        &p->aBuffer[p->iBufStart], p->iBufEnd - p->iBufStart, 
        p->iWriteOff + p->iBufStart
    );
  }
  *piEof = (p->iWriteOff + p->iBufEnd);
  sqlite3_free(p->aBuffer);
  rc = p->eFWErr;
  memset(p, 0, sizeof(PmaWriter));
  return rc;
}

/*
** Write value iVal encoded as a varint to the PMA. Return 
** SQLITE_OK if successful, or an SQLite error code if an error occurs.
*/
static void vdbePmaWriteVarint(PmaWriter *p, u64 iVal){
  int nByte; 
  u8 aByte[10];
  nByte = sqlite3PutVarint(aByte, iVal);
  vdbePmaWriteBlob(p, aByte, nByte);
}

/*
** Write the current contents of in-memory linked-list pList to a level-0
** PMA in the temp file belonging to sub-task pTask. Return SQLITE_OK if 
** successful, or an SQLite error code otherwise.
**
** The format of a PMA is:
**
**     * A varint. This varint contains the total number of bytes of content
**       in the PMA (not including the varint itself).
**
**     * One or more records packed end-to-end in order of ascending keys. 
**       Each record consists of a varint followed by a blob of data (the 
**       key). The varint is the number of bytes in the blob of data.
*/
static int vdbeSorterListToPMA(SortSubtask *pTask, SorterList *pList){
  sqlite3 *db = pTask->pSorter->db;
  int rc = SQLITE_OK;             /* Return code */
  PmaWriter writer;               /* Object used to write to the file */

#ifdef SQLITE_DEBUG
  /* Set iSz to the expected size of file pTask->file after writing the PMA. 
  ** This is used by an assert() statement at the end of this function.  */
  i64 iSz = pList->szPMA + sqlite3VarintLen(pList->szPMA) + pTask->file.iEof;
#endif

  vdbeSorterWorkDebug(pTask, "enter");
  memset(&writer, 0, sizeof(PmaWriter));
  assert( pList->szPMA>0 );

  /* If the first temporary PMA file has not been opened, open it now. */
  if( pTask->file.pFd==0 ){
    rc = vdbeSorterOpenTempFile(db, 0, &pTask->file.pFd);
    assert( rc!=SQLITE_OK || pTask->file.pFd );
    assert( pTask->file.iEof==0 );
    assert( pTask->nPMA==0 );
  }

  /* Try to get the file to memory map */
  if( rc==SQLITE_OK ){
    vdbeSorterExtendFile(db, pTask->file.pFd, pTask->file.iEof+pList->szPMA+9);
  }

  /* Sort the list */
  if( rc==SQLITE_OK ){
    rc = vdbeSorterSort(pTask, pList);
  }

  if( rc==SQLITE_OK ){
    SorterRecord *p;
    SorterRecord *pNext = 0;

    vdbePmaWriterInit(pTask->file.pFd, &writer, pTask->pSorter->pgsz,
                      pTask->file.iEof);
    pTask->nPMA++;
    vdbePmaWriteVarint(&writer, pList->szPMA);
    for(p=pList->pList; p; p=pNext){
      pNext = p->u.pNext;
      vdbePmaWriteVarint(&writer, p->nVal);
      vdbePmaWriteBlob(&writer, SRVAL(p), p->nVal);
      if( pList->aMemory==0 ) sqlite3_free(p);
    }
    pList->pList = p;
    rc = vdbePmaWriterFinish(&writer, &pTask->file.iEof);
  }

  vdbeSorterWorkDebug(pTask, "exit");
  assert( rc!=SQLITE_OK || pList->pList==0 );
  assert( rc!=SQLITE_OK || pTask->file.iEof==iSz );
  return rc;
}

/*
** Advance the MergeEngine iterator passed as the second argument to
** the next entry. Set *pbEof to true if this means the iterator has 
** reached EOF.
**
** Return SQLITE_OK if successful or an error code if an error occurs.
*/
static int vdbeSorterNext(
  SortSubtask *pTask, 
  MergeEngine *pMerger, 
  int *pbEof
){
  int rc;
  int iPrev = pMerger->aTree[1];/* Index of iterator to advance */

  /* Advance the current iterator */
  rc = vdbePmaReaderNext(&pMerger->aIter[iPrev]);

  /* Update contents of aTree[] */
  if( rc==SQLITE_OK ){
    int i;                      /* Index of aTree[] to recalculate */
    PmaReader *pIter1;     /* First iterator to compare */
    PmaReader *pIter2;     /* Second iterator to compare */
    u8 *pKey2;                  /* To pIter2->aKey, or 0 if record cached */

    /* Find the first two iterators to compare. The one that was just
    ** advanced (iPrev) and the one next to it in the array.  */
    pIter1 = &pMerger->aIter[(iPrev & 0xFFFE)];
    pIter2 = &pMerger->aIter[(iPrev | 0x0001)];
    pKey2 = pIter2->aKey;

    for(i=(pMerger->nTree+iPrev)/2; i>0; i=i/2){
      /* Compare pIter1 and pIter2. Store the result in variable iRes. */
      int iRes;
      if( pIter1->pFile==0 ){
        iRes = +1;
      }else if( pIter2->pFile==0 ){
        iRes = -1;
      }else{
        iRes = vdbeSorterCompare(pTask, 
            pIter1->aKey, pIter1->nKey, pKey2, pIter2->nKey
        );
      }

      /* If pIter1 contained the smaller value, set aTree[i] to its index.
      ** Then set pIter2 to the next iterator to compare to pIter1. In this
      ** case there is no cache of pIter2 in pTask->pUnpacked, so set
      ** pKey2 to point to the record belonging to pIter2.
      **
      ** Alternatively, if pIter2 contains the smaller of the two values,
      ** set aTree[i] to its index and update pIter1. If vdbeSorterCompare()
      ** was actually called above, then pTask->pUnpacked now contains
      ** a value equivalent to pIter2. So set pKey2 to NULL to prevent
      ** vdbeSorterCompare() from decoding pIter2 again.
      **
      ** If the two values were equal, then the value from the oldest
      ** PMA should be considered smaller. The VdbeSorter.aIter[] array
      ** is sorted from oldest to newest, so pIter1 contains older values
      ** than pIter2 iff (pIter1<pIter2).  */
      if( iRes<0 || (iRes==0 && pIter1<pIter2) ){
        pMerger->aTree[i] = (int)(pIter1 - pMerger->aIter);
        pIter2 = &pMerger->aIter[ pMerger->aTree[i ^ 0x0001] ];
        pKey2 = pIter2->aKey;
      }else{
        if( pIter1->pFile ) pKey2 = 0;
        pMerger->aTree[i] = (int)(pIter2 - pMerger->aIter);
        pIter1 = &pMerger->aIter[ pMerger->aTree[i ^ 0x0001] ];
      }
    }
    *pbEof = (pMerger->aIter[pMerger->aTree[1]].pFile==0);
  }

  return (rc==SQLITE_OK ? pTask->pUnpacked->errCode : rc);
}

#if SQLITE_MAX_WORKER_THREADS>0
/*
** The main routine for background threads that write level-0 PMAs.
*/
static void *vdbeSorterFlushThread(void *pCtx){
  SortSubtask *pTask = (SortSubtask*)pCtx;
  int rc;                         /* Return code */
  assert( pTask->bDone==0 );
  rc = vdbeSorterListToPMA(pTask, &pTask->list);
  pTask->bDone = 1;
  return SQLITE_INT_TO_PTR(rc);
}
#endif /* SQLITE_MAX_WORKER_THREADS>0 */

/*
** Flush the current contents of VdbeSorter.list to a new PMA, possibly
** using a background thread.
*/
static int vdbeSorterFlushPMA(VdbeSorter *pSorter){
#if SQLITE_MAX_WORKER_THREADS==0
  pSorter->bUsePMA = 1;
  return vdbeSorterListToPMA(&pSorter->aTask[0], &pSorter->list);
#else
  int rc = SQLITE_OK;
  int i;
  SortSubtask *pTask = 0;    /* Thread context used to create new PMA */
  int nWorker = (pSorter->nTask-1);

  /* Set the flag to indicate that at least one PMA has been written. 
  ** Or will be, anyhow.  */
  pSorter->bUsePMA = 1;

  /* Select a sub-task to sort and flush the current list of in-memory
  ** records to disk. If the sorter is running in multi-threaded mode,
  ** round-robin between the first (pSorter->nTask-1) tasks. Except, if
  ** the background thread from a sub-tasks previous turn is still running,
  ** skip it. If the first (pSorter->nTask-1) sub-tasks are all still busy,
  ** fall back to using the final sub-task. The first (pSorter->nTask-1)
  ** sub-tasks are prefered as they use background threads - the final 
  ** sub-task uses the main thread. */
  for(i=0; i<nWorker; i++){
    int iTest = (pSorter->iPrev + i + 1) % nWorker;
    pTask = &pSorter->aTask[iTest];
    if( pTask->bDone ){
      rc = vdbeSorterJoinThread(pTask);
    }
    if( rc!=SQLITE_OK || pTask->pThread==0 ) break;
  }

  if( rc==SQLITE_OK ){
    if( i==nWorker ){
      /* Use the foreground thread for this operation */
      rc = vdbeSorterListToPMA(&pSorter->aTask[nWorker], &pSorter->list);
    }else{
      /* Launch a background thread for this operation */
      u8 *aMem = pTask->list.aMemory;
      void *pCtx = (void*)pTask;

      assert( pTask->pThread==0 && pTask->bDone==0 );
      assert( pTask->list.pList==0 );
      assert( pTask->list.aMemory==0 || pSorter->list.aMemory!=0 );

      pSorter->iPrev = (pTask - pSorter->aTask);
      pTask->list = pSorter->list;
      pSorter->list.pList = 0;
      pSorter->list.szPMA = 0;
      if( aMem ){
        pSorter->list.aMemory = aMem;
        pSorter->nMemory = sqlite3MallocSize(aMem);
      }else if( pSorter->list.aMemory ){
        pSorter->list.aMemory = sqlite3Malloc(pSorter->nMemory);
        if( !pSorter->list.aMemory ) return SQLITE_NOMEM;
      }

      rc = vdbeSorterCreateThread(pTask, vdbeSorterFlushThread, pCtx);
    }
  }

  return rc;
#endif /* SQLITE_MAX_WORKER_THREADS!=0 */
}

/*
** Add a record to the sorter.
*/
int sqlite3VdbeSorterWrite(
  const VdbeCursor *pCsr,         /* Sorter cursor */
  Mem *pVal                       /* Memory cell containing record */
){
  VdbeSorter *pSorter = pCsr->pSorter;
  int rc = SQLITE_OK;             /* Return Code */
  SorterRecord *pNew;             /* New list element */

  int bFlush;                     /* True to flush contents of memory to PMA */
  int nReq;                       /* Bytes of memory required */
  int nPMA;                       /* Bytes of PMA space required */

  assert( pSorter );

  /* Figure out whether or not the current contents of memory should be
  ** flushed to a PMA before continuing. If so, do so.
  **
  ** If using the single large allocation mode (pSorter->aMemory!=0), then
  ** flush the contents of memory to a new PMA if (a) at least one value is
  ** already in memory and (b) the new value will not fit in memory.
  ** 
  ** Or, if using separate allocations for each record, flush the contents
  ** of memory to a PMA if either of the following are true:
  **
  **   * The total memory allocated for the in-memory list is greater 
  **     than (page-size * cache-size), or
  **
  **   * The total memory allocated for the in-memory list is greater 
  **     than (page-size * 10) and sqlite3HeapNearlyFull() returns true.
  */
  nReq = pVal->n + sizeof(SorterRecord);
  nPMA = pVal->n + sqlite3VarintLen(pVal->n);
  if( pSorter->mxPmaSize ){
    if( pSorter->list.aMemory ){
      bFlush = pSorter->iMemory && (pSorter->iMemory+nReq) > pSorter->mxPmaSize;
    }else{
      bFlush = (
          (pSorter->list.szPMA > pSorter->mxPmaSize)
       || (pSorter->list.szPMA > pSorter->mnPmaSize && sqlite3HeapNearlyFull())
      );
    }
    if( bFlush ){
      rc = vdbeSorterFlushPMA(pSorter);
      pSorter->list.szPMA = 0;
      pSorter->iMemory = 0;
      assert( rc!=SQLITE_OK || pSorter->list.pList==0 );
    }
  }

  pSorter->list.szPMA += nPMA;
  if( nPMA>pSorter->mxKeysize ){
    pSorter->mxKeysize = nPMA;
  }

  if( pSorter->list.aMemory ){
    int nMin = pSorter->iMemory + nReq;

    if( nMin>pSorter->nMemory ){
      u8 *aNew;
      int nNew = pSorter->nMemory * 2;
      while( nNew < nMin ) nNew = nNew*2;
      if( nNew > pSorter->mxPmaSize ) nNew = pSorter->mxPmaSize;
      if( nNew < nMin ) nNew = nMin;

      aNew = sqlite3Realloc(pSorter->list.aMemory, nNew);
      if( !aNew ) return SQLITE_NOMEM;
      pSorter->list.pList = (SorterRecord*)(
          aNew + ((u8*)pSorter->list.pList - pSorter->list.aMemory)
      );
      pSorter->list.aMemory = aNew;
      pSorter->nMemory = nNew;
    }

    pNew = (SorterRecord*)&pSorter->list.aMemory[pSorter->iMemory];
    pSorter->iMemory += ROUND8(nReq);
    pNew->u.iNext = (int)((u8*)(pSorter->list.pList) - pSorter->list.aMemory);
  }else{
    pNew = (SorterRecord *)sqlite3Malloc(nReq);
    if( pNew==0 ){
      return SQLITE_NOMEM;
    }
    pNew->u.pNext = pSorter->list.pList;
  }

  memcpy(SRVAL(pNew), pVal->z, pVal->n);
  pNew->nVal = pVal->n;
  pSorter->list.pList = pNew;

  return rc;
}

/*
** Read keys from pIncr->pMerger and populate pIncr->aFile[1]. The format
** of the data stored in aFile[1] is the same as that used by regular PMAs,
** except that the number-of-bytes varint is omitted from the start.
*/
static int vdbeIncrPopulate(IncrMerger *pIncr){
  int rc = SQLITE_OK;
  int rc2;
  i64 iStart = pIncr->iStartOff;
  SorterFile *pOut = &pIncr->aFile[1];
  SortSubtask *pTask = pIncr->pTask;
  MergeEngine *pMerger = pIncr->pMerger;
  PmaWriter writer;
  assert( pIncr->bEof==0 );

  vdbeSorterPopulateDebug(pTask, "enter");

  vdbePmaWriterInit(pOut->pFd, &writer, pTask->pSorter->pgsz, iStart);
  while( rc==SQLITE_OK ){
    int dummy;
    PmaReader *pReader = &pMerger->aIter[ pMerger->aTree[1] ];
    int nKey = pReader->nKey;
    i64 iEof = writer.iWriteOff + writer.iBufEnd;

    /* Check if the output file is full or if the input has been exhausted.
    ** In either case exit the loop. */
    if( pReader->pFile==0 ) break;
    if( (iEof + nKey + sqlite3VarintLen(nKey))>(iStart + pIncr->mxSz) ) break;

    /* Write the next key to the output. */
    vdbePmaWriteVarint(&writer, nKey);
    vdbePmaWriteBlob(&writer, pReader->aKey, nKey);
    rc = vdbeSorterNext(pTask, pIncr->pMerger, &dummy);
  }

  rc2 = vdbePmaWriterFinish(&writer, &pOut->iEof);
  if( rc==SQLITE_OK ) rc = rc2;
  vdbeSorterPopulateDebug(pTask, "exit");
  return rc;
}

#if SQLITE_MAX_WORKER_THREADS>0
/*
** The main routine for background threads that populate aFile[1] of
** multi-threaded IncrMerger objects.
*/
static void *vdbeIncrPopulateThread(void *pCtx){
  IncrMerger *pIncr = (IncrMerger*)pCtx;
  void *pRet = SQLITE_INT_TO_PTR( vdbeIncrPopulate(pIncr) );
  pIncr->pTask->bDone = 1;
  return pRet;
}

/*
** Launch a background thread to populate aFile[1] of pIncr.
*/
static int vdbeIncrBgPopulate(IncrMerger *pIncr){
  void *p = (void*)pIncr;
  assert( pIncr->bUseThread );
  return vdbeSorterCreateThread(pIncr->pTask, vdbeIncrPopulateThread, p);
}
#endif

/*
** This function is called when the PmaReader corresponding to pIncr has
** finished reading the contents of aFile[0]. Its purpose is to "refill"
** aFile[0] such that the iterator should start rereading it from the
** beginning.
**
** For single-threaded objects, this is accomplished by literally reading 
** keys from pIncr->pMerger and repopulating aFile[0]. 
**
** For multi-threaded objects, all that is required is to wait until the 
** background thread is finished (if it is not already) and then swap 
** aFile[0] and aFile[1] in place. If the contents of pMerger have not
** been exhausted, this function also launches a new background thread
** to populate the new aFile[1].
**
** SQLITE_OK is returned on success, or an SQLite error code otherwise.
*/
static int vdbeIncrSwap(IncrMerger *pIncr){
  int rc = SQLITE_OK;

#if SQLITE_MAX_WORKER_THREADS>0
  if( pIncr->bUseThread ){
    rc = vdbeSorterJoinThread(pIncr->pTask);

    if( rc==SQLITE_OK ){
      SorterFile f0 = pIncr->aFile[0];
      pIncr->aFile[0] = pIncr->aFile[1];
      pIncr->aFile[1] = f0;
    }

    if( rc==SQLITE_OK ){
      if( pIncr->aFile[0].iEof==pIncr->iStartOff ){
        pIncr->bEof = 1;
      }else{
        rc = vdbeIncrBgPopulate(pIncr);
      }
    }
  }else
#endif
  {
    rc = vdbeIncrPopulate(pIncr);
    pIncr->aFile[0] = pIncr->aFile[1];
    if( pIncr->aFile[0].iEof==pIncr->iStartOff ){
      pIncr->bEof = 1;
    }
  }

  return rc;
}

/*
** Allocate and return a new IncrMerger object to read data from pMerger.
**
** If an OOM condition is encountered, return NULL. In this case free the
** pMerger argument before returning.
*/
static int vdbeIncrNew(
  SortSubtask *pTask, 
  MergeEngine *pMerger,
  IncrMerger **ppOut
){
  int rc = SQLITE_OK;
  IncrMerger *pIncr = *ppOut = (IncrMerger*)sqlite3MallocZero(sizeof(*pIncr));
  if( pIncr ){
    pIncr->pMerger = pMerger;
    pIncr->pTask = pTask;
    pIncr->mxSz = MAX(pTask->pSorter->mxKeysize+9,pTask->pSorter->mxPmaSize/2);
    pTask->file2.iEof += pIncr->mxSz;
  }else{
    vdbeMergeEngineFree(pMerger);
    rc = SQLITE_NOMEM;
  }
  return rc;
}

#if SQLITE_MAX_WORKER_THREADS>0
/*
** Set the "use-threads" flag on object pIncr.
*/
static void vdbeIncrSetThreads(IncrMerger *pIncr){
  pIncr->bUseThread = 1;
  pIncr->pTask->file2.iEof -= pIncr->mxSz;
}
#endif /* SQLITE_MAX_WORKER_THREADS>0 */

#define INCRINIT_NORMAL 0
#define INCRINIT_TASK   1
#define INCRINIT_ROOT   2
static int vdbePmaReaderIncrInit(PmaReader *pIter, int eMode);

/*
** Initialize the merger argument passed as the second argument. Once this
** function returns, the first key of merged data may be read from the merger
** object in the usual fashion.
**
** If argument eMode is INCRINIT_ROOT, then it is assumed that any IncrMerge
** objects attached to the PmaReader objects that the merger reads from have
** already been populated, but that they have not yet populated aFile[0] and
** set the PmaReader objects up to read from it. In this case all that is
** required is to call vdbePmaReaderNext() on each iterator to point it at
** its first key.
**
** Otherwise, if eMode is any value other than INCRINIT_ROOT, then use 
** vdbePmaReaderIncrInit() to initialize each PmaReader that feeds data 
** to pMerger.
**
** SQLITE_OK is returned if successful, or an SQLite error code otherwise.
*/
static int vdbeIncrInitMerger(
  SortSubtask *pTask, 
  MergeEngine *pMerger, 
  int eMode                       /* One of the INCRINIT_XXX constants */
){
  int rc = SQLITE_OK;             /* Return code */
  int i;                          /* For iterating through PmaReader objects */
  int nTree = pMerger->nTree;

  for(i=0; rc==SQLITE_OK && i<nTree; i++){
    if( eMode==INCRINIT_ROOT ){
      /* Iterators should be normally initialized in order, as if they are
      ** reading from the same temp file this makes for more linear file IO.
      ** However, in the INCRINIT_ROOT case, if iterator aIter[nTask-1] is
      ** in use it will block the vdbePmaReaderNext() call while it uses
      ** the main thread to fill its buffer. So calling PmaReaderNext()
      ** on this iterator before any of the multi-threaded iterators takes
      ** better advantage of multi-processor hardware. */
      rc = vdbePmaReaderNext(&pMerger->aIter[nTree-i-1]);
    }else{
      rc = vdbePmaReaderIncrInit(&pMerger->aIter[i], INCRINIT_NORMAL);
    }
  }

  for(i=pMerger->nTree-1; rc==SQLITE_OK && i>0; i--){
    rc = vdbeSorterDoCompare(pTask, pMerger, i);
  }

  return (rc==SQLITE_OK ? pTask->pUnpacked->errCode : rc);
}

/*
** If the PmaReader passed as the first argument is not an incremental-reader
** (if pIter->pIncr==0), then this function is a no-op. Otherwise, it serves
** to open and/or initialize the temp file related fields of the IncrMerge
** object at (pIter->pIncr).
**
** If argument eMode is set to INCRINIT_NORMAL, then PmaReader iterators
** in the sub-tree headed by pIter are also initialized. Data is then loaded
** into the buffers belonging to this iterator, pIter, and it is set to
** point to the first key in its range.
**
** If argument eMode is set to INCRINIT_TASK, then PmaReader is guaranteed
** to be a multi-threaded iterator and this function is being called in a
** background thread. In this case all iterators in the sub-tree are 
** initialized as for INCRINIT_NORMAL and the aFile[1] buffer belonging to
** pIter is populated. However, the iterator itself is not set up to point
** to its first key. A call to vdbePmaReaderNext() is still required to do
** that. 
**
** The reason this function does not call vdbePmaReaderNext() immediately 
** in the INCRINIT_TASK case is that vdbePmaReaderNext() assumes that has
** to block on thread (pTask->thread) before accessing aFile[1]. But, since
** this entire function is being run by thread (pTask->thread), that will
** lead to the current background thread attempting to join itself.
**
** Finally, if argument eMode is set to INCRINIT_ROOT, it may be assumed
** that pIter->pIncr is a multi-threaded IncrMerge objects, and that all
** child-trees have already been initialized using IncrInit(INCRINIT_TASK).
** In this case vdbePmaReaderNext() is called on all child iterators and
** the current iterator set to point to the first key in its range.
**
** SQLITE_OK is returned if successful, or an SQLite error code otherwise.
*/
static int vdbePmaReaderIncrInit(PmaReader *pIter, int eMode){
  int rc = SQLITE_OK;
  IncrMerger *pIncr = pIter->pIncr;
  if( pIncr ){
    SortSubtask *pTask = pIncr->pTask;
    sqlite3 *db = pTask->pSorter->db;

    rc = vdbeIncrInitMerger(pTask, pIncr->pMerger, eMode);

    /* Set up the required files for pIncr. A multi-theaded IncrMerge object
    ** requires two temp files to itself, whereas a single-threaded object
    ** only requires a region of pTask->file2. */
    if( rc==SQLITE_OK ){
      int mxSz = pIncr->mxSz;
#if SQLITE_MAX_WORKER_THREADS>0
      if( pIncr->bUseThread ){
        rc = vdbeSorterOpenTempFile(db, mxSz, &pIncr->aFile[0].pFd);
        if( rc==SQLITE_OK ){
          rc = vdbeSorterOpenTempFile(db, mxSz, &pIncr->aFile[1].pFd);
        }
      }else
#endif
      /*if( !pIncr->bUseThread )*/{
        if( pTask->file2.pFd==0 ){
          assert( pTask->file2.iEof>0 );
          rc = vdbeSorterOpenTempFile(db, pTask->file2.iEof, &pTask->file2.pFd);
          pTask->file2.iEof = 0;
        }
        if( rc==SQLITE_OK ){
          pIncr->aFile[1].pFd = pTask->file2.pFd;
          pIncr->iStartOff = pTask->file2.iEof;
          pTask->file2.iEof += mxSz;
        }
      }
    }

#if SQLITE_MAX_WORKER_THREADS>0
    if( rc==SQLITE_OK && pIncr->bUseThread ){
      /* Use the current thread to populate aFile[1], even though this
      ** iterator is multi-threaded. The reason being that this function
      ** is already running in background thread pIncr->pTask->thread. */
      assert( eMode==INCRINIT_ROOT || eMode==INCRINIT_TASK );
      rc = vdbeIncrPopulate(pIncr);
    }
#endif

    if( rc==SQLITE_OK && eMode!=INCRINIT_TASK ){
      rc = vdbePmaReaderNext(pIter);
    }
  }
  return rc;
}

#if SQLITE_MAX_WORKER_THREADS>0
/*
** The main routine for vdbePmaReaderIncrInit() operations run in 
** background threads.
*/
static void *vdbePmaReaderBgInit(void *pCtx){
  PmaReader *pReader = (PmaReader*)pCtx;
  void *pRet = SQLITE_INT_TO_PTR(vdbePmaReaderIncrInit(pReader,INCRINIT_TASK));
  pReader->pIncr->pTask->bDone = 1;
  return pRet;
}

/*
** Use a background thread to invoke vdbePmaReaderIncrInit(INCRINIT_TASK) 
** on the the PmaReader object passed as the first argument.
**
** This call will initialize the various fields of the pIter->pIncr 
** structure and, if it is a multi-threaded IncrMerger, launch a 
** background thread to populate aFile[1].
*/
static int vdbePmaReaderBgIncrInit(PmaReader *pIter){
  void *pCtx = (void*)pIter;
  return vdbeSorterCreateThread(pIter->pIncr->pTask, vdbePmaReaderBgInit, pCtx);
}
#endif

/*
** Allocate a new MergeEngine object to merge the contents of nPMA level-0
** PMAs from pTask->file. If no error occurs, set *ppOut to point to
** the new object and return SQLITE_OK. Or, if an error does occur, set *ppOut
** to NULL and return an SQLite error code.
**
** When this function is called, *piOffset is set to the offset of the
** first PMA to read from pTask->file. Assuming no error occurs, it is 
** set to the offset immediately following the last byte of the last
** PMA before returning. If an error does occur, then the final value of
** *piOffset is undefined.
*/
static int vdbeMergeEngineLevel0(
  SortSubtask *pTask,             /* Sorter task to read from */
  int nPMA,                       /* Number of PMAs to read */
  i64 *piOffset,                  /* IN/OUT: Read offset in pTask->file */
  MergeEngine **ppOut             /* OUT: New merge-engine */
){
  MergeEngine *pNew;              /* Merge engine to return */
  i64 iOff = *piOffset;
  int i;
  int rc = SQLITE_OK;

  *ppOut = pNew = vdbeMergeEngineNew(nPMA);
  if( pNew==0 ) rc = SQLITE_NOMEM;

  for(i=0; i<nPMA && rc==SQLITE_OK; i++){
    i64 nDummy;
    PmaReader *pIter = &pNew->aIter[i];
    rc = vdbePmaReaderInit(pTask, &pTask->file, iOff, pIter, &nDummy);
    iOff = pIter->iEof;
  }

  if( rc!=SQLITE_OK ){
    vdbeMergeEngineFree(pNew);
    *ppOut = 0;
  }
  *piOffset = iOff;
  return rc;
}

/*
** Return the depth of a tree comprising nPMA PMAs, assuming a fanout of
** SORTER_MAX_MERGE_COUNT. The returned value does not include leaf nodes.
**
** i.e.
**
**   nPMA<=16    -> TreeDepth() == 0
**   nPMA<=256   -> TreeDepth() == 1
**   nPMA<=65536 -> TreeDepth() == 2
*/
static int vdbeSorterTreeDepth(int nPMA){
  int nDepth = 0;
  i64 nDiv = SORTER_MAX_MERGE_COUNT;
  while( nDiv < (i64)nPMA ){
    nDiv = nDiv * SORTER_MAX_MERGE_COUNT;
    nDepth++;
  }
  return nDepth;
}

/*
** pRoot is the root of an incremental merge-tree with depth nDepth (according
** to vdbeSorterTreeDepth()). pLeaf is the iSeq'th leaf to be added to the
** tree, counting from zero. This function adds pLeaf to the tree.
**
** If successful, SQLITE_OK is returned. If an error occurs, an SQLite error
** code is returned and pLeaf is freed.
*/
static int vdbeSorterAddToTree(
  SortSubtask *pTask,             /* Task context */
  int nDepth,                     /* Depth of tree according to TreeDepth() */
  int iSeq,                       /* Sequence number of leaf within tree */
  MergeEngine *pRoot,             /* Root of tree */
  MergeEngine *pLeaf              /* Leaf to add to tree */
){
  int rc = SQLITE_OK;
  int nDiv = 1;
  int i;
  MergeEngine *p = pRoot;
  IncrMerger *pIncr;

  rc = vdbeIncrNew(pTask, pLeaf, &pIncr);

  for(i=1; i<nDepth; i++){
    nDiv = nDiv * SORTER_MAX_MERGE_COUNT;
  }

  for(i=1; i<nDepth && rc==SQLITE_OK; i++){
    int iIter = (iSeq / nDiv) % SORTER_MAX_MERGE_COUNT;
    PmaReader *pIter = &p->aIter[iIter];

    if( pIter->pIncr==0 ){
      MergeEngine *pNew = vdbeMergeEngineNew(SORTER_MAX_MERGE_COUNT);
      if( pNew==0 ){
        rc = SQLITE_NOMEM;
      }else{
        rc = vdbeIncrNew(pTask, pNew, &pIter->pIncr);
      }
    }
    if( rc==SQLITE_OK ){
      p = pIter->pIncr->pMerger;
      nDiv = nDiv / SORTER_MAX_MERGE_COUNT;
    }
  }

  if( rc==SQLITE_OK ){
    p->aIter[iSeq % SORTER_MAX_MERGE_COUNT].pIncr = pIncr;
  }else{
    vdbeIncrFree(pIncr);
  }
  return rc;
}

/*
** This function is called as part of a SorterRewind() operation on a sorter
** that has already written two or more level-0 PMAs to one or more temp
** files. It builds a tree of MergeEngine/IncrMerger/PmaReader objects that 
** can be used to incrementally merge all PMAs on disk.
**
** If successful, SQLITE_OK is returned and *ppOut set to point to the
** MergeEngine object at the root of the tree before returning. Or, if an
** error occurs, an SQLite error code is returned and the final value 
** of *ppOut is undefined.
*/
static int vdbeSorterMergeTreeBuild(VdbeSorter *pSorter, MergeEngine **ppOut){
  MergeEngine *pMain = 0;
  int rc = SQLITE_OK;
  int iTask;

#if SQLITE_MAX_WORKER_THREADS>0
  /* If the sorter uses more than one task, then create the top-level 
  ** MergeEngine here. This MergeEngine will read data from exactly 
  ** one PmaReader per sub-task.  */
  assert( pSorter->bUseThreads || pSorter->nTask==1 );
  if( pSorter->nTask>1 ){
    pMain = vdbeMergeEngineNew(pSorter->nTask);
    if( pMain==0 ) rc = SQLITE_NOMEM;
  }
#endif

  for(iTask=0; iTask<pSorter->nTask && rc==SQLITE_OK; iTask++){
    SortSubtask *pTask = &pSorter->aTask[iTask];
    if( pTask->nPMA ){
      MergeEngine *pRoot = 0;     /* Root node of tree for this task */
      int nDepth = vdbeSorterTreeDepth(pTask->nPMA);
      i64 iReadOff = 0;

      if( pTask->nPMA<=SORTER_MAX_MERGE_COUNT ){
        rc = vdbeMergeEngineLevel0(pTask, pTask->nPMA, &iReadOff, &pRoot);
      }else{
        int i;
        int iSeq = 0;
        pRoot = vdbeMergeEngineNew(SORTER_MAX_MERGE_COUNT);
        if( pRoot==0 ) rc = SQLITE_NOMEM;
        for(i=0; i<pTask->nPMA && rc==SQLITE_OK; i += SORTER_MAX_MERGE_COUNT){
          MergeEngine *pMerger = 0; /* New level-0 PMA merger */
          int nReader;              /* Number of level-0 PMAs to merge */

          nReader = MIN(pTask->nPMA - i, SORTER_MAX_MERGE_COUNT);
          rc = vdbeMergeEngineLevel0(pTask, nReader, &iReadOff, &pMerger);
          if( rc==SQLITE_OK ){
            rc = vdbeSorterAddToTree(pTask, nDepth, iSeq++, pRoot, pMerger);
          }
        }
      }

      if( rc==SQLITE_OK ){
        if( pMain==0 ){
          pMain = pRoot;
        }else{
          rc = vdbeIncrNew(pTask, pRoot, &pMain->aIter[iTask].pIncr);
        }
      }else{
        vdbeMergeEngineFree(pRoot);
      }
    }
  }

  if( rc!=SQLITE_OK ){
    vdbeMergeEngineFree(pMain);
    pMain = 0;
  }
  *ppOut = pMain;
  return rc;
}

/*
** This function is called as part of an sqlite3VdbeSorterRewind() operation
** on a sorter that has written two or more PMAs to temporary files. It sets
** up either VdbeSorter.pMerger (for single threaded sorters) or pReader
** (for multi-threaded sorters) so that it can be used to iterate through
** all records stored in the sorter.
**
** SQLITE_OK is returned if successful, or an SQLite error code otherwise.
*/
static int vdbeSorterSetupMerge(VdbeSorter *pSorter){
  int rc;                         /* Return code */
  SortSubtask *pTask0 = &pSorter->aTask[0];
  MergeEngine *pMain = 0;
#if SQLITE_MAX_WORKER_THREADS
  sqlite3 *db = pTask0->pSorter->db;
#endif

  rc = vdbeSorterMergeTreeBuild(pSorter, &pMain);
  if( rc==SQLITE_OK ){
#if SQLITE_MAX_WORKER_THREADS
    assert( pSorter->bUseThreads==0 || pSorter->nTask>1 );
    if( pSorter->bUseThreads ){
      int iTask;
      PmaReader *pIter;
      SortSubtask *pLast = &pSorter->aTask[pSorter->nTask-1];
      rc = vdbeSortAllocUnpacked(pLast);
      if( rc==SQLITE_OK ){
        pIter = (PmaReader*)sqlite3DbMallocZero(db, sizeof(PmaReader));
        pSorter->pReader = pIter;
        if( pIter==0 ) rc = SQLITE_NOMEM;
      }
      if( rc==SQLITE_OK ){
        rc = vdbeIncrNew(pLast, pMain, &pIter->pIncr);
        if( rc==SQLITE_OK ){
          vdbeIncrSetThreads(pIter->pIncr);
          for(iTask=0; iTask<(pSorter->nTask-1); iTask++){
            IncrMerger *pIncr;
            if( (pIncr = pMain->aIter[iTask].pIncr) ){
              vdbeIncrSetThreads(pIncr);
              assert( pIncr->pTask!=pLast );
            }
          }
          for(iTask=0; rc==SQLITE_OK && iTask<pSorter->nTask; iTask++){
            PmaReader *p = &pMain->aIter[iTask];
            assert( p->pIncr==0 || p->pIncr->pTask==&pSorter->aTask[iTask] );
            if( p->pIncr ){ 
              if( iTask==pSorter->nTask-1 ){
                rc = vdbePmaReaderIncrInit(p, INCRINIT_TASK);
              }else{
                rc = vdbePmaReaderBgIncrInit(p);
              }
            }
          }
        }
        pMain = 0;
      }
      if( rc==SQLITE_OK ){
        rc = vdbePmaReaderIncrInit(pIter, INCRINIT_ROOT);
      }
    }else
#endif
    {
      rc = vdbeIncrInitMerger(pTask0, pMain, INCRINIT_NORMAL);
      pSorter->pMerger = pMain;
      pMain = 0;
    }
  }

  if( rc!=SQLITE_OK ){
    vdbeMergeEngineFree(pMain);
  }
  return rc;
}


/*
** Once the sorter has been populated by calls to sqlite3VdbeSorterWrite,
** this function is called to prepare for iterating through the records
** in sorted order.
*/
int sqlite3VdbeSorterRewind(const VdbeCursor *pCsr, int *pbEof){
  VdbeSorter *pSorter = pCsr->pSorter;
  int rc = SQLITE_OK;             /* Return code */

  assert( pSorter );

  /* If no data has been written to disk, then do not do so now. Instead,
  ** sort the VdbeSorter.pRecord list. The vdbe layer will read data directly
  ** from the in-memory list.  */
  if( pSorter->bUsePMA==0 ){
    if( pSorter->list.pList ){
      *pbEof = 0;
      rc = vdbeSorterSort(&pSorter->aTask[0], &pSorter->list);
    }else{
      *pbEof = 1;
    }
    return rc;
  }

  /* Write the current in-memory list to a PMA. When the VdbeSorterWrite() 
  ** function flushes the contents of memory to disk, it immediately always
  ** creates a new list consisting of a single key immediately afterwards.
  ** So the list is never empty at this point.  */
  assert( pSorter->list.pList );
  rc = vdbeSorterFlushPMA(pSorter);

  /* Join all threads */
  rc = vdbeSorterJoinAll(pSorter, rc);

  vdbeSorterRewindDebug("rewind");

  /* Assuming no errors have occurred, set up a merger structure to 
  ** incrementally read and merge all remaining PMAs.  */
  assert( pSorter->pReader==0 );
  if( rc==SQLITE_OK ){
    rc = vdbeSorterSetupMerge(pSorter);
    *pbEof = 0;
  }

  vdbeSorterRewindDebug("rewinddone");
  return rc;
}

/*
** Advance to the next element in the sorter.
*/
int sqlite3VdbeSorterNext(sqlite3 *db, const VdbeCursor *pCsr, int *pbEof){
  VdbeSorter *pSorter = pCsr->pSorter;
  int rc;                         /* Return code */

  assert( pSorter->bUsePMA || (pSorter->pReader==0 && pSorter->pMerger==0) );
  if( pSorter->bUsePMA ){
    assert( pSorter->pReader==0 || pSorter->pMerger==0 );
    assert( pSorter->bUseThreads==0 || pSorter->pReader );
    assert( pSorter->bUseThreads==1 || pSorter->pMerger );
#if SQLITE_MAX_WORKER_THREADS>0
    if( pSorter->bUseThreads ){
      rc = vdbePmaReaderNext(pSorter->pReader);
      *pbEof = (pSorter->pReader->pFile==0);
    }else
#endif
    /*if( !pSorter->bUseThreads )*/ {
      rc = vdbeSorterNext(&pSorter->aTask[0], pSorter->pMerger, pbEof);
    }
  }else{
    SorterRecord *pFree = pSorter->list.pList;
    pSorter->list.pList = pFree->u.pNext;
    pFree->u.pNext = 0;
    if( pSorter->list.aMemory==0 ) vdbeSorterRecordFree(db, pFree);
    *pbEof = !pSorter->list.pList;
    rc = SQLITE_OK;
  }
  return rc;
}

/*
** Return a pointer to a buffer owned by the sorter that contains the 
** current key.
*/
static void *vdbeSorterRowkey(
  const VdbeSorter *pSorter,      /* Sorter object */
  int *pnKey                      /* OUT: Size of current key in bytes */
){
  void *pKey;
  if( pSorter->bUsePMA ){
    PmaReader *pReader;
#if SQLITE_MAX_WORKER_THREADS>0
    if( pSorter->bUseThreads ){
      pReader = pSorter->pReader;
    }else
#endif
    /*if( !pSorter->bUseThreads )*/{
      pReader = &pSorter->pMerger->aIter[pSorter->pMerger->aTree[1]];
    }
    *pnKey = pReader->nKey;
    pKey = pReader->aKey;
  }else{
    *pnKey = pSorter->list.pList->nVal;
    pKey = SRVAL(pSorter->list.pList);
  }
  return pKey;
}

/*
** Copy the current sorter key into the memory cell pOut.
*/
int sqlite3VdbeSorterRowkey(const VdbeCursor *pCsr, Mem *pOut){
  VdbeSorter *pSorter = pCsr->pSorter;
  void *pKey; int nKey;           /* Sorter key to copy into pOut */

  pKey = vdbeSorterRowkey(pSorter, &nKey);
  if( sqlite3VdbeMemGrow(pOut, nKey, 0) ){
    return SQLITE_NOMEM;
  }
  pOut->n = nKey;
  MemSetTypeFlag(pOut, MEM_Blob);
  memcpy(pOut->z, pKey, nKey);

  return SQLITE_OK;
}

/*
** Compare the key in memory cell pVal with the key that the sorter cursor
** passed as the first argument currently points to. For the purposes of
** the comparison, ignore the rowid field at the end of each record.
**
** If the sorter cursor key contains any NULL values, consider it to be
** less than pVal. Even if pVal also contains NULL values.
**
** If an error occurs, return an SQLite error code (i.e. SQLITE_NOMEM).
** Otherwise, set *pRes to a negative, zero or positive value if the
** key in pVal is smaller than, equal to or larger than the current sorter
** key.
**
** This routine forms the core of the OP_SorterCompare opcode, which in
** turn is used to verify uniqueness when constructing a UNIQUE INDEX.
*/
int sqlite3VdbeSorterCompare(
  const VdbeCursor *pCsr,         /* Sorter cursor */
  Mem *pVal,                      /* Value to compare to current sorter key */
  int nIgnore,                    /* Ignore this many fields at the end */
  int *pRes                       /* OUT: Result of comparison */
){
  VdbeSorter *pSorter = pCsr->pSorter;
  UnpackedRecord *r2 = pSorter->pUnpacked;
  KeyInfo *pKeyInfo = pCsr->pKeyInfo;
  int i;
  void *pKey; int nKey;           /* Sorter key to compare pVal with */

  if( r2==0 ){
    char *p;
    r2 = pSorter->pUnpacked = sqlite3VdbeAllocUnpackedRecord(pKeyInfo,0,0,&p);
    assert( pSorter->pUnpacked==(UnpackedRecord*)p );
    if( r2==0 ) return SQLITE_NOMEM;
    r2->nField = pKeyInfo->nField-nIgnore;
  }
  assert( r2->nField>=pKeyInfo->nField-nIgnore );

  pKey = vdbeSorterRowkey(pSorter, &nKey);
  sqlite3VdbeRecordUnpack(pKeyInfo, nKey, pKey, r2);
  for(i=0; i<r2->nField; i++){
    if( r2->aMem[i].flags & MEM_Null ){
      *pRes = -1;
      return SQLITE_OK;
    }
  }

  *pRes = sqlite3VdbeRecordCompare(pVal->n, pVal->z, r2, 0);
  return SQLITE_OK;
}
