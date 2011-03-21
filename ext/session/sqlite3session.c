
#ifdef SQLITE_ENABLE_SESSION

#include "sqlite3session.h"
#include <assert.h>
#include <string.h>

#include "sqliteInt.h"
#include "vdbeInt.h"

typedef struct SessionTable SessionTable;
typedef struct SessionChange SessionChange;
typedef struct SessionBuffer SessionBuffer;

/*
** Session handle structure.
*/
struct sqlite3_session {
  sqlite3 *db;                    /* Database handle session is attached to */
  char *zDb;                      /* Name of database session is attached to */
  int bEnable;                    /* True if currently recording */
  int rc;                         /* Non-zero if an error has occurred */
  sqlite3_session *pNext;         /* Next session object on same db. */
  SessionTable *pTable;           /* List of attached tables */
};

/*
** Structure for changeset iterators.
*/
struct sqlite3_changeset_iter {
  u8 *aChangeset;                 /* Pointer to buffer containing changeset */
  int nChangeset;                 /* Number of bytes in aChangeset */
  u8 *pNext;                      /* Pointer to next change within aChangeset */
  int rc;                         /* Iterator error code */
  sqlite3_stmt *pConflict;        /* Points to conflicting row, if any */
  char *zTab;                     /* Current table */
  int nCol;                       /* Number of columns in zTab */
  int op;                         /* Current operation */
  sqlite3_value **apValue;        /* old.* and new.* values */
};

/*
** Each session object maintains a set of the following structures, one
** for each table the session object is monitoring. The structures are
** stored in a linked list starting at sqlite3_session.pTable.
**
** The keys of the SessionTable.aChange[] hash table are all rows that have
** been modified in any way since the session object was attached to the
** table.
**
** The data associated with each hash-table entry is a structure containing
** a subset of the initial values that the modified row contained at the
** start of the session. Or no initial values if the row was inserted.
*/
struct SessionTable {
  SessionTable *pNext;
  char *zName;                    /* Local name of table */
  int nCol;                       /* Number of columns in table zName */
  const char **azCol;             /* Column names */
  u8 *abPK;                       /* Array of primary key flags */
  int nEntry;                     /* Total number of entries in hash table */
  int nChange;                    /* Size of apChange[] array */
  SessionChange **apChange;       /* Hash table buckets */
};

/* 
** RECORD FORMAT:
**
** The following record format is similar to (but not compatible with) that 
** used in SQLite database files. This format is used as part of the 
** change-set binary format, and so must be architecture independent.
**
** Unlike the SQLite database record format, each field is self-contained -
** there is no separation of header and data. Each field begins with a
** single byte describing its type, as follows:
**
**       0x00: Undefined value.
**       0x01: Integer value.
**       0x02: Real value.
**       0x03: Text value.
**       0x04: Blob value.
**       0x05: SQL NULL value.
**
** Note that the above match the definitions of SQLITE_INTEGER, SQLITE_TEXT
** and so on in sqlite3.h. For undefined and NULL values, the field consists
** only of the single type byte. For other types of values, the type byte
** is followed by:
**
**   Text values:
**     A varint containing the number of bytes in the value (encoded using
**     UTF-8). Followed by a buffer containing the UTF-8 representation
**     of the text value. There is no nul terminator.
**
**   Blob values:
**     A varint containing the number of bytes in the value, followed by
**     a buffer containing the value itself.
**
**   Integer values:
**     An 8-byte big-endian integer value.
**
**   Real values:
**     An 8-byte big-endian IEEE 754-2008 real value.
**
** Varint values are encoded in the same way as varints in the SQLite 
** record format.
**
** CHANGESET FORMAT:
**
** A changeset is a collection of DELETE, UPDATE and INSERT operations on
** one or more tables. Operations on a single table are grouped together,
** but may occur in any order (i.e. deletes, updates and inserts are all
** mixed together).
**
** Each group of changes begins with a table header:
**
**   1 byte: Constant 0x54 (capital 'T')
**   Varint: Big-endian integer set to the number of columns in the table.
**   N bytes: Unqualified table name (encoded using UTF-8). Nul-terminated.
**
** Followed by one or more changes to the table.
**
**   1 byte: Either SQLITE_INSERT, UPDATE or DELETE.
**   old.* record: (delete and update only)
**   new.* record: (insert and update only)
*/

/*
** For each row modified during a session, there exists a single instance of
** this structure stored in a SessionTable.aChange[] hash table.
*/
struct SessionChange {
  int bInsert;                    /* True if row was inserted this session */
  int nRecord;                    /* Number of bytes in buffer aRecord[] */
  u8 *aRecord;                    /* Buffer containing old.* record */
  SessionChange *pNext;           /* For hash-table collisions */
};

/*
** Instances of this structure are used to build strings or binary records.
*/
struct SessionBuffer {
  u8 *aBuf;                       /* Pointer to changeset buffer */
  int nBuf;                       /* Size of buffer aBuf */
  int nAlloc;                     /* Size of allocation containing aBuf */
};

/*
** Write a varint with value iVal into the buffer at aBuf. Return the 
** number of bytes written.
*/
static int sessionVarintPut(u8 *aBuf, int iVal){
  return putVarint32(aBuf, iVal);
}

/*
** Return the number of bytes required to store value iVal as a varint.
*/
static int sessionVarintLen(int iVal){
  return sqlite3VarintLen(iVal);
}

/*
** Read a varint value from aBuf[] into *piVal. Return the number of 
** bytes read.
*/
static int sessionVarintGet(u8 *aBuf, int *piVal){
  return getVarint32(aBuf, *piVal);
}

/*
** Read a 64-bit big-endian integer value from buffer aRec[]. Return
** the value read.
*/
static sqlite3_int64 sessionGetI64(u8 *aRec){
  return (((sqlite3_int64)aRec[0]) << 56)
       + (((sqlite3_int64)aRec[1]) << 48)
       + (((sqlite3_int64)aRec[2]) << 40)
       + (((sqlite3_int64)aRec[3]) << 32)
       + (((sqlite3_int64)aRec[4]) << 24)
       + (((sqlite3_int64)aRec[5]) << 16)
       + (((sqlite3_int64)aRec[6]) <<  8)
       + (((sqlite3_int64)aRec[7]) <<  0);
}

/*
** Write a 64-bit big-endian integer value to the buffer aBuf[].
*/
static void sessionPutI64(u8 *aBuf, sqlite3_int64 i){
  aBuf[0] = (i>>56) & 0xFF;
  aBuf[1] = (i>>48) & 0xFF;
  aBuf[2] = (i>>40) & 0xFF;
  aBuf[3] = (i>>32) & 0xFF;
  aBuf[4] = (i>>24) & 0xFF;
  aBuf[5] = (i>>16) & 0xFF;
  aBuf[6] = (i>> 8) & 0xFF;
  aBuf[7] = (i>> 0) & 0xFF;
}

/*
** This function is used to serialize the contents of value pValue (see
** comment titled "RECORD FORMAT" above).
**
** If it is non-NULL, the serialized form of the value is written to 
** buffer aBuf. *pnWrite is set to the number of bytes written before
** returning. Or, if aBuf is NULL, the only thing this function does is
** set *pnWrite.
**
** If no error occurs, SQLITE_OK is returned. Or, if an OOM error occurs
** within a call to sqlite3_value_text() (may fail if the db is utf-16)) 
** SQLITE_NOMEM is returned.
*/
static int sessionSerializeValue(
  u8 *aBuf,                       /* If non-NULL, write serialized value here */
  sqlite3_value *pValue,          /* Value to serialize */
  int *pnWrite                    /* IN/OUT: Increment by bytes written */
){
  int eType;                      /* Value type (SQLITE_NULL, TEXT etc.) */
  int nByte;                      /* Size of serialized value in bytes */

  eType = sqlite3_value_type(pValue);
  if( aBuf ) aBuf[0] = eType;

  switch( eType ){
    case SQLITE_NULL: 
      nByte = 1;
      break;

    case SQLITE_INTEGER: 
    case SQLITE_FLOAT:
      if( aBuf ){
        /* TODO: SQLite does something special to deal with mixed-endian
        ** floating point values (e.g. ARM7). This code probably should
        ** too.  */
        u64 i;
        if( eType==SQLITE_INTEGER ){
          i = (u64)sqlite3_value_int64(pValue);
        }else{
          double r;
          assert( sizeof(double)==8 && sizeof(u64)==8 );
          r = sqlite3_value_double(pValue);
          memcpy(&i, &r, 8);
        }
        sessionPutI64(&aBuf[1], i);
      }
      nByte = 9; 
      break;

    default: {
      int n = sqlite3_value_bytes(pValue);
      int nVarint = sessionVarintLen(n);
      assert( eType==SQLITE_TEXT || eType==SQLITE_BLOB );
      if( aBuf ){
        sessionVarintPut(&aBuf[1], n);
        memcpy(&aBuf[nVarint + 1], eType==SQLITE_TEXT ? 
            sqlite3_value_text(pValue) : sqlite3_value_blob(pValue), n
        );
      }

      nByte = 1 + nVarint + n;
      break;
    }
  }

  *pnWrite += nByte;
  return SQLITE_OK;
}

#define HASH_APPEND(hash, add) ((hash) << 3) ^ (hash) ^ (unsigned int)(add)
static unsigned int sessionHashAppendI64(unsigned int h, i64 i){
  h = HASH_APPEND(h, i & 0xFFFFFFFF);
  return HASH_APPEND(h, (i>>32)&0xFFFFFFFF);
}
static unsigned int sessionHashAppendBlob(unsigned int h, int n, const u8 *z){
  int i;
  for(i=0; i<n; i++) h = HASH_APPEND(h, z[i]);
  return h;
}

/*
** This function may only be called from within a pre-update callback.
** It calculates a hash based on the primary key values of the old.* or 
** new.* row currently available. The value returned is guaranteed to
** be less than pTab->nBucket.
*/
static unsigned int sessionPreupdateHash(
  sqlite3 *db,                    /* Database handle */
  SessionTable *pTab,             /* Session table handle */
  int bNew,                       /* True to hash the new.* PK */
  int *piHash                     /* OUT: Hash value */
){
  unsigned int h = 0;             /* Hash value to return */
  int i;                          /* Used to iterate through columns */

  assert( pTab->nCol==sqlite3_preupdate_count(db) );
  for(i=0; i<pTab->nCol; i++){
    if( pTab->abPK[i] ){
      int rc;
      int eType;
      sqlite3_value *pVal;

      if( bNew ){
        rc = sqlite3_preupdate_new(db, i, &pVal);
      }else{
        rc = sqlite3_preupdate_old(db, i, &pVal);
      }

      eType = sqlite3_value_type(pVal);
      h = HASH_APPEND(h, eType);
      switch( eType ){
        case SQLITE_INTEGER: 
        case SQLITE_FLOAT: {
          i64 iVal;
          if( eType==SQLITE_INTEGER ){
            iVal = sqlite3_value_int64(pVal);
          }else{
            double rVal = sqlite3_value_double(pVal);
            assert( sizeof(iVal)==8 && sizeof(rVal)==8 );
            memcpy(&iVal, &rVal, 8);
          }
          h = sessionHashAppendI64(h, iVal);
          break;
        }

        case SQLITE_TEXT: 
        case SQLITE_BLOB: {
          int n = sqlite3_value_bytes(pVal);
          const u8 *z = eType==SQLITE_TEXT ?
            sqlite3_value_text(pVal) : sqlite3_value_blob(pVal);
          h = sessionHashAppendBlob(h, n, z);
          break;
        }
      }
    }
  }

  *piHash = (h % pTab->nChange);
  return SQLITE_OK;
}

/*
** Based on the primary key values stored in change pChange, calculate a
** hash key, assuming the has table has nBucket buckets. The hash keys
** calculated by this function are compatible with those calculated by
** sessionPreupdateHash().
*/
static unsigned int sessionChangeHash(
  sqlite3 *db,                    /* Database handle */
  SessionTable *pTab,             /* Table handle */
  SessionChange *pChange,         /* Change handle */
  int nBucket                     /* Assume this many buckets in hash table */
){
  unsigned int h = 0;             /* Value to return */
  int i;                          /* Used to iterate through columns */
  u8 *a = pChange->aRecord;       /* Used to iterate through change record */

  for(i=0; i<pTab->nCol; i++){
    int eType = *a++;
    int isPK = pTab->abPK[i];

    if( isPK ) h = HASH_APPEND(h, eType);
    switch( eType ){
      case SQLITE_INTEGER:
      case SQLITE_FLOAT: {
        if( isPK ){
          i64 iVal = sessionGetI64(a);
          h = sessionHashAppendI64(h, iVal);
        }
        a += 8;
        break;
      }
      case SQLITE_TEXT:
      case SQLITE_BLOB: {
        int n;
        a += sessionVarintGet(a, &n);
        if( isPK ){
          h = sessionHashAppendBlob(h, n, a);
        }
        a += n;
        break;
      }
    }
  }
  return (h % nBucket);
}

static int sessionPreupdateEqual(
  sqlite3 *db,
  SessionTable *pTab,
  SessionChange *pChange,
  int bNew,
  int *pbEqual
){
  int i;
  u8 *a = pChange->aRecord;

  *pbEqual = 0;

  for(i=0; i<pTab->nCol; i++){
    int eType = *a++;
    if( !pTab->abPK[i] ){
      switch( eType ){
        case SQLITE_INTEGER: 
        case SQLITE_FLOAT:
          a += 8;
          break;

        case SQLITE_TEXT: 
        case SQLITE_BLOB: {
          int n;
          a += sessionVarintGet(a, &n);
          a += n;
          break;
        }
      }
    }else{
      sqlite3_value *pVal;
      int rc;
      if( bNew ){
        rc = sqlite3_preupdate_new(db, i, &pVal);
      }else{
        rc = sqlite3_preupdate_old(db, i, &pVal);
      }
      if( rc!=SQLITE_OK || sqlite3_value_type(pVal)!=eType ) return rc;

      switch( eType ){
        case SQLITE_INTEGER:
        case SQLITE_FLOAT: {
          i64 iVal = sessionGetI64(a);
          a += 8;
          if( eType==SQLITE_INTEGER ){
            if( sqlite3_value_int64(pVal)!=iVal ) return SQLITE_OK;
          }else{
            double rVal;
            assert( sizeof(iVal)==8 && sizeof(rVal)==8 );
            memcpy(&rVal, &iVal, 8);
            if( sqlite3_value_double(pVal)!=rVal ) return SQLITE_OK;
          }
          break;
        }
        case SQLITE_TEXT:
        case SQLITE_BLOB: {
          int n;
          const u8 *z;
          a += sessionVarintGet(a, &n);
          if( sqlite3_value_bytes(pVal)!=n ) return SQLITE_OK;
          z = eType==SQLITE_TEXT ? 
            sqlite3_value_text(pVal) : sqlite3_value_blob(pVal);
          if( memcmp(a, z, n) ) return SQLITE_OK;
          a += n;
          break;
        }
      }
    }
  }

  *pbEqual = 1;
  return SQLITE_OK;
}

/*
** If required, grow the hash table used to store changes on table pTab 
** (part of the session pSession). If a fatal OOM error occurs, set the
** session object to failed and return SQLITE_ERROR. Otherwise, return
** SQLITE_OK.
**
** It is possible that a non-fatal OOM error occurs in this function. In
** that case the hash-table does not grow, but SQLITE_OK is returned anyway.
** Growing the hash table in this case is a performance optimization only,
** it is not required for correct operation.
*/
static int sessionGrowHash(sqlite3_session *pSession, SessionTable *pTab){
  if( pTab->nChange==0 || pTab->nEntry>=(pTab->nChange/2) ){
    int i;
    SessionChange **apNew;
    int nNew = (pTab->nChange ? pTab->nChange : 128) * 2;

    apNew = (SessionChange **)sqlite3_malloc(sizeof(SessionChange *) * nNew);
    if( apNew==0 ){
      if( pTab->nChange==0 ){
        pSession->rc = SQLITE_NOMEM;
        return SQLITE_ERROR;
      }
      return SQLITE_OK;
    }
    memset(apNew, 0, sizeof(SessionChange *) * nNew);

    for(i=0; i<pTab->nChange; i++){
      SessionChange *p;
      SessionChange *pNext;
      for(p=pTab->apChange[i]; p; p=pNext){
        int iHash = sessionChangeHash(pSession->db, pTab, p, nNew);
        pNext = p->pNext;
        p->pNext = apNew[iHash];
        apNew[iHash] = p;
      }
    }

    sqlite3_free(pTab->apChange);
    pTab->nChange = nNew;
    pTab->apChange = apNew;
  }

  return SQLITE_OK;
}

/*
** This function queries the database for the names of the columns of table
** zThis, in schema zDb. It is expected that the table has nCol columns. If
** not, SQLITE_SCHEMA is returned and none of the output variables are
** populated.
**
** Otherwise, if it is not NULL, variable *pzTab is set to point to a
** nul-terminated copy of the table name. *pazCol (if not NULL) is set to
** point to an array of pointers to column names. And *pabPK (again, if not
** NULL) is set to point to an array of booleans - true if the corresponding
** column is part of the primary key.
**
** For example, if the table is declared as:
**
**     CREATE TABLE tbl1(w, x, y, z, PRIMARY KEY(w, z));
**
** Then the three output variables are populated as follows:
**
**     *pzTab  = "tbl1"
**     *pazCol = {"w", "x", "y", "z"}
**     *pabPK  = {1, 0, 0, 1}
**
** All returned buffers are part of the same single allocation, which must
** be freed using sqlite3_free() by the caller. If pazCol was not NULL, then
** pointer *pazCol should be freed to release all memory. Otherwise, pointer
** *pabPK. It is illegal for both pazCol and pabPK to be NULL.
*/
static int sessionTableInfo(
  sqlite3 *db,                    /* Database connection */
  const char *zDb,                /* Name of attached database (e.g. "main") */
  const char *zThis,              /* Table name */
  int nCol,                       /* Expected number of columns */
  const char **pzTab,             /* OUT: Copy of zThis */
  const char ***pazCol,           /* OUT: Array of column names for table */
  u8 **pabPK                      /* OUT: Array of booleans - true for PK col */
){
  char *zPragma;
  sqlite3_stmt *pStmt;
  int rc;
  int nByte;
  int nDbCol = 0;
  int nThis;
  int i;
  u8 *pAlloc;
  u8 *pFree = 0;
  char **azCol;
  u8 *abPK;

  assert( pazCol || pabPK );

  nThis = strlen(zThis);
  zPragma = sqlite3_mprintf("PRAGMA '%q'.table_info('%q')", zDb, zThis);
  if( !zPragma ) return SQLITE_NOMEM;

  rc = sqlite3_prepare_v2(db, zPragma, -1, &pStmt, 0);
  sqlite3_free(zPragma);
  if( rc!=SQLITE_OK ) return rc;

  nByte = nThis + 1;
  while( SQLITE_ROW==sqlite3_step(pStmt) ){
    nByte += sqlite3_column_bytes(pStmt, 1);
    nDbCol++;
  }
  rc = sqlite3_reset(pStmt);

  if( nDbCol!=nCol ){
    rc = SQLITE_SCHEMA;
  }
  if( rc==SQLITE_OK ){
    nByte += nDbCol * (sizeof(const char *) + sizeof(u8) + 1);
    pAlloc = sqlite3_malloc(nByte);
    if( pAlloc==0 ){
      rc = SQLITE_NOMEM;
    }
  }
  if( rc==SQLITE_OK ){
    pFree = pAlloc;
    if( pazCol ){
      azCol = (char **)pAlloc;
      pAlloc = (u8 *)&azCol[nCol];
    }
    if( pabPK ){
      abPK = (u8 *)pAlloc;
      pAlloc = &abPK[nCol];
    }
    if( pzTab ){
      memcpy(pAlloc, zThis, nThis+1);
      *pzTab = (char *)pAlloc;
      pAlloc += nThis+1;
    }
  
    i = 0;
    while( SQLITE_ROW==sqlite3_step(pStmt) ){
      int nName = sqlite3_column_bytes(pStmt, 1);
      const unsigned char *zName = sqlite3_column_text(pStmt, 1);
      if( zName==0 ) break;
      if( pazCol ){
        memcpy(pAlloc, zName, nName+1);
        azCol[i] = (char *)pAlloc;
        pAlloc += nName+1;
      }
      if( pabPK ) abPK[i] = sqlite3_column_int(pStmt, 5);
      i++;
    }
    rc = sqlite3_reset(pStmt);
  
  }

  /* If successful, populate the output variables. Otherwise, zero them and
  ** free any allocation made. An error code will be returned in this case.
  */
  if( rc==SQLITE_OK ){
    if( pazCol ) *pazCol = (const char **)azCol;
    if( pabPK ) *pabPK = abPK;
  }else{
    if( pazCol ) *pazCol = 0;
    if( pabPK ) *pabPK = 0;
    if( pzTab ) *pzTab = 0;
    sqlite3_free(pFree);
  }
  sqlite3_finalize(pStmt);
  return rc;
}

/*
** This function is only called from within a pre-update handler for a
** write to table pTab, part of session pSession. If this is the first
** write to this table, set the SessionTable.nCol variable to the number
** of columns in the table.
**
** Otherwise, if this is not the first time this table has been written
** to, check that the number of columns in the table has not changed. If
** it has not, return zero.
**
** If the number of columns in the table has changed since the last write
** was recorded, set the session error-code to SQLITE_SCHEMA and return
** non-zero. Users are not allowed to change the number of columns in a table
** for which changes are being recorded by the session module. If they do so, 
** it is an error.
*/
static int sessionInitTable(sqlite3_session *pSession, SessionTable *pTab){
  if( pTab->nCol==0 ){
    assert( pTab->azCol==0 || pTab->abPK==0 );
    pTab->nCol = sqlite3_preupdate_count(pSession->db);
    pSession->rc = sessionTableInfo(pSession->db, pSession->zDb, 
        pTab->zName, pTab->nCol, 0, &pTab->azCol, &pTab->abPK
    );
  }else if( pTab->nCol!=sqlite3_preupdate_count(pSession->db) ){
    pSession->rc = SQLITE_SCHEMA;
  }
  return pSession->rc;
}

static void sessionPreupdateOneChange(
  int op,
  sqlite3_session *pSession,
  SessionTable *pTab
){
  sqlite3 *db = pSession->db;
  SessionChange *pChange;
  SessionChange *pC;
  int iHash; 
  int rc = SQLITE_OK;

  if( pSession->rc ) return;

  /* Load table details if required */
  if( sessionInitTable(pSession, pTab) ) return;

  /* Grow the hash table if required */
  if( sessionGrowHash(pSession, pTab) ) return;

  /* Search the hash table for an existing entry for rowid=iKey2. If
   ** one is found, store a pointer to it in pChange and unlink it from
   ** the hash table. Otherwise, set pChange to NULL.
   */
  rc = sessionPreupdateHash(db, pTab, op==SQLITE_INSERT, &iHash);
  for(pC=pTab->apChange[iHash]; rc==SQLITE_OK && pC; pC=pC->pNext){
    int bEqual;
    rc = sessionPreupdateEqual(db, pTab, pC, op==SQLITE_INSERT, &bEqual);
    if( bEqual ) break;
  }
  if( pC==0 ){
    /* Create a new change object containing all the old values (if
     ** this is an SQLITE_UPDATE or SQLITE_DELETE), or just the PK
     ** values (if this is an INSERT). */
    int nByte;              /* Number of bytes to allocate */
    int i;                  /* Used to iterate through columns */

    pTab->nEntry++;

    /* Figure out how large an allocation is required */
    nByte = sizeof(SessionChange);
    for(i=0; i<pTab->nCol && rc==SQLITE_OK; i++){
      sqlite3_value *p = 0;
      if( op!=SQLITE_INSERT ){
        rc = sqlite3_preupdate_old(pSession->db, i, &p);
      }else if( 1 || pTab->abPK[i] ){
        rc = sqlite3_preupdate_new(pSession->db, i, &p);
      }
      if( p && rc==SQLITE_OK ){
        rc = sessionSerializeValue(0, p, &nByte);
      }
    }

    /* Allocate the change object */
    pChange = (SessionChange *)sqlite3_malloc(nByte);
    if( !pChange ){
      rc = SQLITE_NOMEM;
    }else{
      memset(pChange, 0, sizeof(SessionChange));
      pChange->aRecord = (u8 *)&pChange[1];
    }

    /* Populate the change object */
    nByte = 0;
    for(i=0; i<pTab->nCol && rc==SQLITE_OK; i++){
      sqlite3_value *p = 0;
      if( op!=SQLITE_INSERT ){
        rc = sqlite3_preupdate_old(pSession->db, i, &p);
      }else if( 1 || pTab->abPK[i] ){
        rc = sqlite3_preupdate_new(pSession->db, i, &p);
      }
      if( p && rc==SQLITE_OK ){
        rc = sessionSerializeValue(&pChange->aRecord[nByte], p, &nByte);
      }
    }
    pChange->nRecord = nByte;

    /* If an error has occurred, mark the session object as failed. */
    if( rc!=SQLITE_OK ){
      sqlite3_free(pChange);
      pSession->rc = rc;
    }else{
      /* Add the change back to the hash-table */
      pChange->bInsert = (op==SQLITE_INSERT);
      pChange->pNext = pTab->apChange[iHash];
      pTab->apChange[iHash] = pChange;
    }
  }
}

/*
** The 'pre-update' hook registered by this module with SQLite databases.
*/
static void xPreUpdate(
  void *pCtx,                     /* Copy of third arg to preupdate_hook() */
  sqlite3 *db,                    /* Database handle */
  int op,                         /* SQLITE_UPDATE, DELETE or INSERT */
  char const *zDb,                /* Database name */
  char const *zName,              /* Table name */
  sqlite3_int64 iKey1,            /* Rowid of row about to be deleted/updated */
  sqlite3_int64 iKey2             /* New rowid value (for a rowid UPDATE) */
){
  sqlite3_session *pSession;
  int nDb = strlen(zDb);
  int nName = strlen(zDb);

  assert( sqlite3_mutex_held(db->mutex) );

  for(pSession=(sqlite3_session *)pCtx; pSession; pSession=pSession->pNext){
    SessionTable *pTab;

    /* If this session is attached to a different database ("main", "temp" 
    ** etc.), or if it is not currently enabled, there is nothing to do. Skip 
    ** to the next session object attached to this database. */
    if( pSession->bEnable==0 ) continue;
    if( pSession->rc ) continue;
    if( sqlite3_strnicmp(zDb, pSession->zDb, nDb+1) ) continue;

    for(pTab=pSession->pTable; pTab; pTab=pTab->pNext){
      if( 0==sqlite3_strnicmp(pTab->zName, zName, nName+1) ){
        sessionPreupdateOneChange(op, pSession, pTab);
        if( op==SQLITE_UPDATE ){
          sessionPreupdateOneChange(SQLITE_INSERT, pSession, pTab);
        }
        break;
      }
    }
  }
}

/*
** Create a session object. This session object will record changes to
** database zDb attached to connection db.
*/
int sqlite3session_create(
  sqlite3 *db,                    /* Database handle */
  const char *zDb,                /* Name of db (e.g. "main") */
  sqlite3_session **ppSession     /* OUT: New session object */
){
  sqlite3_session *pNew;          /* Newly allocated session object */
  sqlite3_session *pOld;          /* Session object already attached to db */
  int nDb = strlen(zDb);          /* Length of zDb in bytes */

  /* Zero the output value in case an error occurs. */
  *ppSession = 0;

  /* Allocate and populate the new session object. */
  pNew = (sqlite3_session *)sqlite3_malloc(sizeof(sqlite3_session) + nDb + 1);
  if( !pNew ) return SQLITE_NOMEM;
  memset(pNew, 0, sizeof(sqlite3_session));
  pNew->db = db;
  pNew->zDb = (char *)&pNew[1];
  pNew->bEnable = 1;
  memcpy(pNew->zDb, zDb, nDb+1);

  /* Add the new session object to the linked list of session objects 
  ** attached to database handle $db. Do this under the cover of the db
  ** handle mutex.  */
  sqlite3_mutex_enter(sqlite3_db_mutex(db));
  pOld = (sqlite3_session*)sqlite3_preupdate_hook(db, xPreUpdate, (void*)pNew);
  pNew->pNext = pOld;
  sqlite3_mutex_leave(sqlite3_db_mutex(db));

  *ppSession = pNew;
  return SQLITE_OK;
}

/*
** Delete a session object previously allocated using sqlite3session_create().
*/
void sqlite3session_delete(sqlite3_session *pSession){
  sqlite3 *db = pSession->db;
  sqlite3_session *pHead;
  sqlite3_session **pp;

  /* Unlink the session from the linked list of sessions attached to the
  ** database handle. Hold the db mutex while doing so.  */
  sqlite3_mutex_enter(sqlite3_db_mutex(db));
  pHead = (sqlite3_session*)sqlite3_preupdate_hook(db, 0, 0);
  for(pp=&pHead; (*pp)!=pSession; pp=&((*pp)->pNext));
  *pp = (*pp)->pNext;
  if( pHead ) sqlite3_preupdate_hook(db, xPreUpdate, (void *)pHead);
  sqlite3_mutex_leave(sqlite3_db_mutex(db));

  /* Delete all attached table objects. And the contents of their 
  ** associated hash-tables. */
  while( pSession->pTable ){
    int i;
    SessionTable *pTab = pSession->pTable;
    pSession->pTable = pTab->pNext;
    for(i=0; i<pTab->nChange; i++){
      SessionChange *p;
      SessionChange *pNext;
      for(p=pTab->apChange[i]; p; p=pNext){
        pNext = p->pNext;
        sqlite3_free(p);
      }
    }
    sqlite3_free(pTab->azCol);
    sqlite3_free(pTab->apChange);
    sqlite3_free(pTab);
  }

  /* Free the session object itself. */
  sqlite3_free(pSession);
}

/*
** Attach a table to a session. All subsequent changes made to the table
** while the session object is enabled will be recorded.
**
** Only tables that have a PRIMARY KEY defined may be attached. It does
** not matter if the PRIMARY KEY is an "INTEGER PRIMARY KEY" (rowid alias)
** or not.
*/
int sqlite3session_attach(
  sqlite3_session *pSession,      /* Session object */
  const char *zName               /* Table name */
){
  SessionTable *pTab;             /* New table object (if required) */
  int nName;                      /* Number of bytes in string zName */
  int rc = SQLITE_OK;

  sqlite3_mutex_enter(sqlite3_db_mutex(pSession->db));

  /* First search for an existing entry. If one is found, this call is
  ** a no-op. Return early. */
  nName = strlen(zName);
  for(pTab=pSession->pTable; pTab; pTab=pTab->pNext){
    if( 0==sqlite3_strnicmp(pTab->zName, zName, nName+1) ) break;
  }

  if( !pTab ){
    /* Allocate new SessionTable object. */
    pTab = (SessionTable *)sqlite3_malloc(sizeof(SessionTable) + nName + 1);
    if( !pTab ){
      rc = SQLITE_NOMEM;
    }else{
      /* Populate the new SessionTable object and link it into the list. */
      memset(pTab, 0, sizeof(SessionTable));
      pTab->zName = (char *)&pTab[1];
      memcpy(pTab->zName, zName, nName+1);
      pTab->pNext = pSession->pTable;
      pSession->pTable = pTab;
    }
  }

  sqlite3_mutex_leave(sqlite3_db_mutex(pSession->db));
  return rc;
}

/*
** Ensure that there is room in the buffer to append nByte bytes of data.
** If not, use sqlite3_realloc() to grow the buffer so that there is.
**
** If successful, return zero. Otherwise, if an OOM condition is encountered,
** set *pRc to SQLITE_NOMEM and return non-zero.
*/
static int sessionBufferGrow(SessionBuffer *p, int nByte, int *pRc){
  if( p->nAlloc-p->nBuf<nByte ){
    u8 *aNew;
    int nNew = p->nAlloc ? p->nAlloc : 128;
    do {
      nNew = nNew*2;
    }while( nNew<(p->nAlloc+nByte) );

    aNew = (u8 *)sqlite3_realloc(p->aBuf, nNew);
    if( 0==aNew ){
      *pRc = SQLITE_NOMEM;
      return 1;
    }
    p->aBuf = aNew;
    p->nAlloc = nNew;
  }
  return 0;
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a single byte to the buffer. 
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendByte(SessionBuffer *p, u8 v, int *pRc){
  if( *pRc==SQLITE_OK && 0==sessionBufferGrow(p, 1, pRc) ){
    p->aBuf[p->nBuf++] = v;
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a single varint to the buffer. 
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendVarint(SessionBuffer *p, sqlite3_int64 v, int *pRc){
  if( *pRc==SQLITE_OK && 0==sessionBufferGrow(p, 9, pRc) ){
    p->nBuf += sessionVarintPut(&p->aBuf[p->nBuf], v);
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a blob of data to the buffer. 
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendBlob(
  SessionBuffer *p, 
  const u8 *aBlob, 
  int nBlob, 
  int *pRc
){
  if( *pRc==SQLITE_OK && 0==sessionBufferGrow(p, nBlob, pRc) ){
    memcpy(&p->aBuf[p->nBuf], aBlob, nBlob);
    p->nBuf += nBlob;
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append a string to the buffer. All bytes in the string
** up to (but not including) the nul-terminator are written to the buffer.
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendStr(
  SessionBuffer *p, 
  const char *zStr, 
  int *pRc
){
  int nStr = strlen(zStr);
  if( *pRc==SQLITE_OK && 0==sessionBufferGrow(p, nStr, pRc) ){
    memcpy(&p->aBuf[p->nBuf], zStr, nStr);
    p->nBuf += nStr;
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append the string representation of integer iVal
** to the buffer. No nul-terminator is written.
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendInteger(
  SessionBuffer *p,               /* Buffer to append to */
  int iVal,                       /* Value to write the string rep. of */
  int *pRc                        /* IN/OUT: Error code */
){
  char aBuf[24];
  sqlite3_snprintf(sizeof(aBuf)-1, aBuf, "%d", iVal);
  sessionAppendStr(p, aBuf, pRc);
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is 
** called. Otherwise, append the string zStr enclosed in quotes (") and
** with any embedded quote characters escaped to the buffer. No 
** nul-terminator byte is written.
**
** If an OOM condition is encountered, set *pRc to SQLITE_NOMEM before
** returning.
*/
static void sessionAppendIdent(
  SessionBuffer *p,               /* Buffer to a append to */
  const char *zStr,               /* String to quote, escape and append */
  int *pRc                        /* IN/OUT: Error code */
){
  int nStr = strlen(zStr)*2 + 2 + 1;
  if( *pRc==SQLITE_OK && 0==sessionBufferGrow(p, nStr, pRc) ){
    char *zOut = (char *)&p->aBuf[p->nBuf];
    const char *zIn = zStr;
    *zOut++ = '"';
    while( *zIn ){
      if( *zIn=='"' ) *zOut++ = '"';
      *zOut++ = *(zIn++);
    }
    *zOut++ = '"';
    p->nBuf = ((u8 *)zOut - p->aBuf);
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is
** called. Otherwse, it appends the serialized version of the value stored
** in column iCol of the row that SQL statement pStmt currently points
** to to the buffer.
*/
static void sessionAppendCol(
  SessionBuffer *p,               /* Buffer to append to */
  sqlite3_stmt *pStmt,            /* Handle pointing to row containing value */
  int iCol,                       /* Column to read value from */
  int *pRc                        /* IN/OUT: Error code */
){
  if( *pRc==SQLITE_OK ){
    int eType = sqlite3_column_type(pStmt, iCol);
    sessionAppendByte(p, (u8)eType, pRc);
    if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
      sqlite3_int64 i;
      u8 aBuf[8];
      if( eType==SQLITE_INTEGER ){
        i = sqlite3_column_int64(pStmt, iCol);
      }else{
        double r = sqlite3_column_double(pStmt, iCol);
        memcpy(&i, &r, 8);
      }
      sessionPutI64(aBuf, i);
      sessionAppendBlob(p, aBuf, 8, pRc);
    }
    if( eType==SQLITE_BLOB || eType==SQLITE_TEXT ){
      int nByte = sqlite3_column_bytes(pStmt, iCol);
      sessionAppendVarint(p, nByte, pRc);
      sessionAppendBlob(p, eType==SQLITE_BLOB ? 
        sqlite3_column_blob(pStmt, iCol) : sqlite3_column_text(pStmt, iCol),
        nByte, pRc
      );
    }
  }
}

/*
** This function is a no-op if *pRc is other than SQLITE_OK when it is
** called. 
**
** Otherwse, if *pRc is SQLITE_OK, then it appends an update change to
** the buffer (see the comments under "CHANGESET FORMAT" at the top of the
** file). An update change consists of:
**
**   1 byte:  SQLITE_UPDATE (0x17)
**   n bytes: old.* record (see RECORD FORMAT)
**   m bytes: new.* record (see RECORD FORMAT)
**
** The SessionChange object passed as the third argument contains the
** values that were stored in the row when the session began (the old.*
** values). The statement handle passed as the second argument points
** at the current version of the row (the new.* values).
**
** If all of the old.* values are equal to their corresponding new.* value
** (i.e. nothing has changed), then no data at all is appended to the buffer.
**
** Otherwise, the old.* record contains all primary key values and the 
** original values of any fields that have been modified. The new.* record 
** contains the new values of only those fields that have been modified.
*/ 
static void sessionAppendUpdate(
  SessionBuffer *pBuf,            /* Buffer to append to */
  sqlite3_stmt *pStmt,            /* Statement handle pointing at new row */
  SessionChange *p,               /* Object containing old values */
  u8 *abPK,                       /* Boolean array - true for PK columns */
  int *pRc                        /* IN/OUT: Error code */
){
  if( *pRc==SQLITE_OK ){
    SessionBuffer buf2 = {0,0,0}; /* Buffer to accumulate new.* record in */
    int bNoop = 1;                /* Set to zero if any values are modified */
    int nRewind = pBuf->nBuf;     /* Set to zero if any values are modified */
    int i;                        /* Used to iterate through columns */
    u8 *pCsr = p->aRecord;        /* Used to iterate through old.* values */

    sessionAppendByte(pBuf, SQLITE_UPDATE, pRc);
    for(i=0; i<sqlite3_column_count(pStmt); i++){
      int bChanged = 0;
      int nAdvance;
      int eType = *pCsr;
      switch( eType ){
        case SQLITE_NULL:
          nAdvance = 1;
          if( sqlite3_column_type(pStmt, i)!=SQLITE_NULL ){
            bChanged = 1;
          }
          break;

        case SQLITE_FLOAT:
        case SQLITE_INTEGER: {
          nAdvance = 9;
          if( eType==sqlite3_column_type(pStmt, i) ){
            sqlite3_int64 iVal = sessionGetI64(&pCsr[1]);
            if( eType==SQLITE_INTEGER ){
              if( iVal==sqlite3_column_int64(pStmt, i) ) break;
            }else{
              double dVal;
              memcpy(&dVal, &iVal, 8);
              if( dVal==sqlite3_column_double(pStmt, i) ) break;
            }
          }
          bChanged = 1;
          break;
        }

        case SQLITE_TEXT:
        case SQLITE_BLOB: {
          int nByte;
          int nHdr = 1 + sessionVarintGet(&pCsr[1], &nByte);
          nAdvance = nHdr + nByte;
          if( eType==sqlite3_column_type(pStmt, i) 
           && nByte==sqlite3_column_bytes(pStmt, i) 
           && 0==memcmp(&pCsr[nHdr], sqlite3_column_blob(pStmt, i), nByte)
          ){
            break;
          }
          bChanged = 1;
        }
      }

      if( bChanged || abPK[i] ){
        sessionAppendBlob(pBuf, pCsr, nAdvance, pRc);
      }else{
        sessionAppendByte(pBuf, 0, pRc);
      }

      if( bChanged ){
        sessionAppendCol(&buf2, pStmt, i, pRc);
        bNoop = 0;
      }else{
        sessionAppendByte(&buf2, 0, pRc);
      }

      pCsr += nAdvance;
    }

    if( bNoop ){
      pBuf->nBuf = nRewind;
    }else{
      sessionAppendBlob(pBuf, buf2.aBuf, buf2.nBuf, pRc);
    }
    sqlite3_free(buf2.aBuf);
  }
}

static int sessionSelectStmt(
  sqlite3 *db,                    /* Database handle */
  const char *zDb,                /* Database name */
  const char *zTab,               /* Table name */
  int nCol,
  const char **azCol,
  u8 *abPK,
  sqlite3_stmt **ppStmt
){
  int rc = SQLITE_OK;
  int i;
  const char *zSep = "";
  SessionBuffer buf = {0, 0, 0};

  sessionAppendStr(&buf, "SELECT * FROM ", &rc);
  sessionAppendIdent(&buf, zDb, &rc);
  sessionAppendStr(&buf, ".", &rc);
  sessionAppendIdent(&buf, zTab, &rc);
  sessionAppendStr(&buf, " WHERE ", &rc);
  for(i=0; i<nCol; i++){
    if( abPK[i] ){
      sessionAppendStr(&buf, zSep, &rc);
      sessionAppendIdent(&buf, azCol[i], &rc);
      sessionAppendStr(&buf, " = ?", &rc);
      sessionAppendInteger(&buf, i+1, &rc);
      zSep = " AND ";
    }
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, (char *)buf.aBuf, buf.nBuf, ppStmt, 0);
  }
  sqlite3_free(buf.aBuf);
  return rc;
}

static int sessionSelectBind(
  sqlite3_stmt *pSelect,
  int nCol,
  u8 *abPK,
  u8 *aRecord,
  int nRecord
){
  int i;
  int rc = SQLITE_OK;
  u8 *a = aRecord;

  for(i=0; i<nCol && rc==SQLITE_OK; i++){
    int eType = *a++;

    switch( eType ){
      case SQLITE_NULL:
        if( abPK[i] ) rc = sqlite3_bind_null(pSelect, i+1);
        break;

      case SQLITE_INTEGER: {
        if( abPK[i] ){
          i64 iVal = sessionGetI64(a);
          rc = sqlite3_bind_int64(pSelect, i+1, iVal);
        }
        a += 8;
        break;
      }

      case SQLITE_FLOAT: {
        if( abPK[i] ){
          double rVal;
          i64 iVal = sessionGetI64(a);
          memcpy(&rVal, &iVal, 8);
          rc = sqlite3_bind_double(pSelect, i+1, rVal);
        }
        a += 8;
        break;
      }

      case SQLITE_TEXT: {
        int n;
        a += sessionVarintGet(a, &n);
        if( abPK[i] ){
          rc = sqlite3_bind_text(pSelect, i+1, (char *)a, n, SQLITE_TRANSIENT);
        }
        a += n;
        break;
      }

      case SQLITE_BLOB: {
        int n;
        a += sessionVarintGet(a, &n);
        if( abPK[i] ){
          rc = sqlite3_bind_blob(pSelect, i+1, a, n, SQLITE_TRANSIENT);
        }
        a += n;
        break;
      }
    }
  }

  return rc;
}

/*
** Obtain a changeset object containing all changes recorded by the 
** session object passed as the first argument.
**
** It is the responsibility of the caller to eventually free the buffer 
** using sqlite3_free().
*/
int sqlite3session_changeset(
  sqlite3_session *pSession,      /* Session object */
  int *pnChangeset,               /* OUT: Size of buffer at *ppChangeset */
  void **ppChangeset              /* OUT: Buffer containing changeset */
){
  sqlite3 *db = pSession->db;     /* Source database handle */
  SessionTable *pTab;             /* Used to iterate through attached tables */
  SessionBuffer buf = {0,0,0};    /* Buffer in which to accumlate changeset */
  int rc;                         /* Return code */

  sqlite3_mutex_enter(sqlite3_db_mutex(db));

  /* Zero the output variables in case an error occurs. If this session
  ** object is already in the error state (sqlite3_session.rc != SQLITE_OK),
  ** this call will be a no-op.  */
  *pnChangeset = 0;
  *ppChangeset = 0;
  rc = pSession->rc;

  for(pTab=pSession->pTable; rc==SQLITE_OK && pTab; pTab=pTab->pNext){
    if( pTab->nEntry ){
      const char *zName = pTab->zName;
      int nCol = pTab->nCol;      /* Local copy of member variable */
      u8 *abPK = pTab->abPK;      /* Local copy of member variable */
      int i;                      /* Used to iterate through hash buckets */
      sqlite3_stmt *pSel = 0;     /* SELECT statement to query table pTab */
      int nRewind = buf.nBuf;     /* Initial size of write buffer */
      int nNoop;                  /* Size of buffer after writing tbl header */

      /* Write a table header */
      sessionAppendByte(&buf, 'T', &rc);
      sessionAppendVarint(&buf, nCol, &rc);
      sessionAppendBlob(&buf, (u8 *)zName, strlen(zName)+1, &rc);

      /* Build and compile a statement to execute: */
      if( rc==SQLITE_OK ){
        rc = sessionSelectStmt(
            db, pSession->zDb, zName, nCol, pTab->azCol, abPK, &pSel);
      }

      if( rc==SQLITE_OK && nCol!=sqlite3_column_count(pSel) ){
        rc = SQLITE_SCHEMA;
      }

      nNoop = buf.nBuf;
      for(i=0; i<pTab->nChange; i++){
        SessionChange *p;         /* Used to iterate through changes */

        for(p=pTab->apChange[i]; rc==SQLITE_OK && p; p=p->pNext){
          rc = sessionSelectBind(pSel, nCol, abPK, p->aRecord, p->nRecord);
          if( rc==SQLITE_OK ){
            if( sqlite3_step(pSel)==SQLITE_ROW ){
              int iCol;
              if( p->bInsert ){
                sessionAppendByte(&buf, SQLITE_INSERT, &rc);
                for(iCol=0; iCol<nCol; iCol++){
                  sessionAppendCol(&buf, pSel, iCol, &rc);
                }
              }else{
                sessionAppendUpdate(&buf, pSel, p, abPK, &rc);
              }
            }else if( !p->bInsert ){
              /* A DELETE change */
              sessionAppendByte(&buf, SQLITE_DELETE, &rc);
              sessionAppendBlob(&buf, p->aRecord, p->nRecord, &rc);
            }
            rc = sqlite3_reset(pSel);
          }
        }
      }

      sqlite3_finalize(pSel);

      if( buf.nBuf==nNoop ){
        buf.nBuf = nRewind;
      }
    }
  }

  if( rc==SQLITE_OK ){
    *pnChangeset = buf.nBuf;
    *ppChangeset = buf.aBuf;
  }else{
    sqlite3_free(buf.aBuf);
  }

  sqlite3_mutex_leave(sqlite3_db_mutex(db));
  return rc;
}

/*
** Enable or disable the session object passed as the first argument.
*/
int sqlite3session_enable(sqlite3_session *pSession, int bEnable){
  int ret;
  sqlite3_mutex_enter(sqlite3_db_mutex(pSession->db));
  if( bEnable>=0 ){
    pSession->bEnable = bEnable;
  }
  ret = pSession->bEnable;
  sqlite3_mutex_leave(sqlite3_db_mutex(pSession->db));
  return ret;
}

/*
** Create an iterator used to iterate through the contents of a changeset.
*/
int sqlite3changeset_start(
  sqlite3_changeset_iter **pp,    /* OUT: Changeset iterator handle */
  int nChangeset,                 /* Size of buffer pChangeset in bytes */
  void *pChangeset                /* Pointer to buffer containing changeset */
){
  sqlite3_changeset_iter *pRet;   /* Iterator to return */
  int nByte;                      /* Number of bytes to allocate for iterator */

  /* Zero the output variable in case an error occurs. */
  *pp = 0;

  /* Allocate and initialize the iterator structure. */
  nByte = sizeof(sqlite3_changeset_iter);
  pRet = (sqlite3_changeset_iter *)sqlite3_malloc(nByte);
  if( !pRet ) return SQLITE_NOMEM;
  memset(pRet, 0, sizeof(sqlite3_changeset_iter));
  pRet->aChangeset = (u8 *)pChangeset;
  pRet->nChangeset = nChangeset;
  pRet->pNext = pRet->aChangeset;

  /* Populate the output variable and return success. */
  *pp = pRet;
  return SQLITE_OK;
}

/*
** Deserialize a single record from a buffer in memory. See "RECORD FORMAT"
** for details.
**
** When this function is called, *paChange points to the start of the record
** to deserialize. Assuming no error occurs, *paChange is set to point to
** one byte after the end of the same record before this function returns.
**
** If successful, each element of the apOut[] array (allocated by the caller)
** is set to point to an sqlite3_value object containing the value read
** from the corresponding position in the record. If that value is not
** included in the record (i.e. because the record is part of an UPDATE change
** and the field was not modified), the corresponding element of apOut[] is
** set to NULL.
**
** It is the responsibility of the caller to free all sqlite_value structures
** using sqlite3_free().
**
** If an error occurs, an SQLite error code (e.g. SQLITE_NOMEM) is returned.
** The apOut[] array may have been partially populated in this case.
*/
static int sessionReadRecord(
  u8 **paChange,                  /* IN/OUT: Pointer to binary record */
  int nCol,                       /* Number of values in record */
  sqlite3_value **apOut           /* Write values to this array */
){
  int i;                          /* Used to iterate through columns */
  u8 *aRec = *paChange;           /* Cursor for the serialized record */

  for(i=0; i<nCol; i++){
    int eType = *aRec++;          /* Type of value (SQLITE_NULL, TEXT etc.) */
    assert( !apOut || apOut[i]==0 );
    if( eType ){
      if( apOut ){
        apOut[i] = sqlite3ValueNew(0);
        if( !apOut[i] ) return SQLITE_NOMEM;
      }

      if( eType==SQLITE_TEXT || eType==SQLITE_BLOB ){
        int nByte;
        int enc = (eType==SQLITE_TEXT ? SQLITE_UTF8 : 0);
        aRec += sessionVarintGet(aRec, &nByte);
        if( apOut ){
          sqlite3ValueSetStr(apOut[i], nByte, aRec, enc, SQLITE_STATIC);
        }
        aRec += nByte;
      }
      if( eType==SQLITE_INTEGER || eType==SQLITE_FLOAT ){
        if( apOut ){
          sqlite3_int64 v = sessionGetI64(aRec);
          if( eType==SQLITE_INTEGER ){
            sqlite3VdbeMemSetInt64(apOut[i], v);
          }else{
            double d;
            memcpy(&d, &v, 8);
            sqlite3VdbeMemSetDouble(apOut[i], d);
          }
        }
        aRec += 8;
      }
    }
  }

  *paChange = aRec;
  return SQLITE_OK;
}

/*
** Advance an iterator created by sqlite3changeset_start() to the next
** change in the changeset. This function may return SQLITE_ROW, SQLITE_DONE
** or SQLITE_CORRUPT.
**
** This function may not be called on iterators passed to a conflict handler
** callback by changeset_apply().
*/
int sqlite3changeset_next(sqlite3_changeset_iter *p){
  u8 *aChange;
  int i;
  u8 c;

  /* If the iterator is in the error-state, return immediately. */
  if( p->rc!=SQLITE_OK ) return p->rc;

  /* Free the current contents of p->apValue[]. */
  if( p->apValue ){
    for(i=0; i<p->nCol*2; i++){
      sqlite3ValueFree(p->apValue[i]);
    }
    memset(p->apValue, 0, sizeof(sqlite3_value*)*p->nCol*2);
  }

  /* If the iterator is already at the end of the changeset, return DONE. */
  if( p->pNext>=&p->aChangeset[p->nChangeset] ){
    return SQLITE_DONE;
  }
  aChange = p->pNext;

  c = *(aChange++);
  if( c=='T' ){
    int nByte;                    /* Bytes to allocate for apValue */
    aChange += sessionVarintGet(aChange, &p->nCol);
    p->zTab = (char *)aChange;
    aChange += (strlen((char *)aChange) + 1);
    p->op = *(aChange++);
    sqlite3_free(p->apValue);
    nByte = sizeof(sqlite3_value *) * p->nCol * 2;
    p->apValue = (sqlite3_value **)sqlite3_malloc(nByte);
    if( !p->apValue ){
      return (p->rc = SQLITE_NOMEM);
    }
    memset(p->apValue, 0, sizeof(sqlite3_value*)*p->nCol*2);
  }else{
    p->op = c;
  }
  if( p->op!=SQLITE_UPDATE && p->op!=SQLITE_DELETE && p->op!=SQLITE_INSERT ){
    return (p->rc = SQLITE_CORRUPT);
  }

  /* If this is an UPDATE or DELETE, read the old.* record. */
  if( p->op!=SQLITE_INSERT ){
    p->rc = sessionReadRecord(&aChange, p->nCol, p->apValue);
    if( p->rc!=SQLITE_OK ) return p->rc;
  }

  /* If this is an INSERT or UPDATE, read the new.* record. */
  if( p->op!=SQLITE_DELETE ){
    p->rc = sessionReadRecord(&aChange, p->nCol, &p->apValue[p->nCol]);
    if( p->rc!=SQLITE_OK ) return p->rc;
  }

  p->pNext = aChange;
  return SQLITE_ROW;
}

/*
** The following three functions extract information on the current change
** from a changeset iterator. They may only be called after changeset_next()
** has returned SQLITE_ROW.
*/
int sqlite3changeset_op(
  sqlite3_changeset_iter *pIter,  /* Iterator handle */
  const char **pzTab,             /* OUT: Pointer to table name */
  int *pnCol,                     /* OUT: Number of columns in table */
  int *pOp                        /* OUT: SQLITE_INSERT, DELETE or UPDATE */
){
  *pOp = pIter->op;
  *pnCol = pIter->nCol;
  *pzTab = pIter->zTab;
  return SQLITE_OK;
}

/*
** This function may only be called while the iterator is pointing to an
** SQLITE_UPDATE or SQLITE_DELETE change (see sqlite3changeset_op()).
** Otherwise, SQLITE_MISUSE is returned.
**
** It sets *ppValue to point to an sqlite3_value structure containing the
** iVal'th value in the old.* record. Or, if that particular value is not
** included in the record (because the change is an UPDATE and the field
** was not modified and is not a PK column), set *ppValue to NULL.
**
** If value iVal is out-of-range, SQLITE_RANGE is returned and *ppValue is
** not modified. Otherwise, SQLITE_OK.
*/
int sqlite3changeset_old(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int iVal,                       /* Index of old.* value to retrieve */
  sqlite3_value **ppValue         /* OUT: Old value (or NULL pointer) */
){
  if( pIter->op!=SQLITE_UPDATE && pIter->op!=SQLITE_DELETE ){
    return SQLITE_MISUSE;
  }
  if( iVal<0 || iVal>=pIter->nCol ){
    return SQLITE_RANGE;
  }
  *ppValue = pIter->apValue[iVal];
  return SQLITE_OK;
}

/*
** This function may only be called while the iterator is pointing to an
** SQLITE_UPDATE or SQLITE_INSERT change (see sqlite3changeset_op()).
** Otherwise, SQLITE_MISUSE is returned.
**
** It sets *ppValue to point to an sqlite3_value structure containing the
** iVal'th value in the new.* record. Or, if that particular value is not
** included in the record (because the change is an UPDATE and the field
** was not modified), set *ppValue to NULL.
**
** If value iVal is out-of-range, SQLITE_RANGE is returned and *ppValue is
** not modified. Otherwise, SQLITE_OK.
*/
int sqlite3changeset_new(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int iVal,                       /* Index of new.* value to retrieve */
  sqlite3_value **ppValue         /* OUT: New value (or NULL pointer) */
){
  if( pIter->op!=SQLITE_UPDATE && pIter->op!=SQLITE_INSERT ){
    return SQLITE_MISUSE;
  }
  if( iVal<0 || iVal>=pIter->nCol ){
    return SQLITE_RANGE;
  }
  *ppValue = pIter->apValue[pIter->nCol+iVal];
  return SQLITE_OK;
}

/*
** This function may only be called with a changeset iterator that has been
** passed to an SQLITE_CHANGESET_DATA or SQLITE_CHANGESET_CONFLICT 
** conflict-handler function. Otherwise, SQLITE_MISUSE is returned.
**
** If successful, *ppValue is set to point to an sqlite3_value structure
** containing the iVal'th value of the conflicting record.
**
** If value iVal is out-of-range or some other error occurs, an SQLite error
** code is returned. Otherwise, SQLITE_OK.
*/
int sqlite3changeset_conflict(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int iVal,                       /* Index of conflict record value to fetch */
  sqlite3_value **ppValue         /* OUT: Value from conflicting row */
){
  if( !pIter->pConflict ){
    return SQLITE_MISUSE;
  }
  if( iVal<0 || iVal>=sqlite3_column_count(pIter->pConflict) ){
    return SQLITE_RANGE;
  }
  *ppValue = sqlite3_column_value(pIter->pConflict, iVal);
  return SQLITE_OK;
}

/*
** Finalize an iterator allocated with sqlite3changeset_start().
**
** This function may not be called on iterators passed to a conflict handler
** callback by changeset_apply().
*/
int sqlite3changeset_finalize(sqlite3_changeset_iter *p){
  int i;                          /* Used to iterate through p->apValue[] */
  int rc = p->rc;                 /* Return code */
  for(i=0; i<p->nCol*2; i++) sqlite3ValueFree(p->apValue[i]);
  sqlite3_free(p->apValue);
  sqlite3_free(p);
  return rc;
}

/*
** Invert a changeset object.
*/
int sqlite3changeset_invert(
  int nChangeset,                 /* Number of bytes in input */
  void *pChangeset,               /* Input changeset */
  int *pnInverted,                /* OUT: Number of bytes in output changeset */
  void **ppInverted               /* OUT: Inverse of pChangeset */
){
  u8 *aOut;
  u8 *aIn;
  int i;
  int nCol = 0;

  /* Zero the output variables in case an error occurs. */
  *ppInverted = 0;
  *pnInverted = 0;
  if( nChangeset==0 ) return SQLITE_OK;

  aOut = (u8 *)sqlite3_malloc(nChangeset);
  if( !aOut ) return SQLITE_NOMEM;
  aIn = (u8 *)pChangeset;

  i = 0;
  while( i<nChangeset ){
    u8 eType = aIn[i];
    switch( eType ){
      case 'T': {
        int nByte = 1 + sessionVarintGet(&aIn[i+1], &nCol);
        nByte += 1 + strlen((char *)&aIn[i+nByte]);
        memcpy(&aOut[i], &aIn[i], nByte);
        i += nByte;
        break;
      }

      case SQLITE_INSERT:
      case SQLITE_DELETE: {
        int nByte;
        u8 *aEnd = &aIn[i+1];

        sessionReadRecord(&aEnd, nCol, 0);
        aOut[i] = (eType==SQLITE_DELETE ? SQLITE_INSERT : SQLITE_DELETE);
        nByte = aEnd - &aIn[i+1];
        memcpy(&aOut[i+1], &aIn[i+1], nByte);
        i += 1 + nByte;
        break;
      }

      case SQLITE_UPDATE: {
        int nByte1;              /* Size of old.* record in bytes */
        int nByte2;              /* Size of new.* record in bytes */
        u8 *aEnd = &aIn[i+1];    

        sessionReadRecord(&aEnd, nCol, 0);
        nByte1 = aEnd - &aIn[i+1];
        sessionReadRecord(&aEnd, nCol, 0);
        nByte2 = aEnd - &aIn[i+1] - nByte1;

        aOut[i] = SQLITE_UPDATE;
        memcpy(&aOut[i+1], &aIn[i+1+nByte1], nByte2);
        memcpy(&aOut[i+1+nByte2], &aIn[i+1], nByte1);

        i += 1 + nByte1 + nByte2;
        break;
      }

      default:
        sqlite3_free(aOut);
        return SQLITE_CORRUPT;
    }
  }

  *pnInverted = nChangeset;
  *ppInverted = (void *)aOut;
  return SQLITE_OK;
}

typedef struct SessionApplyCtx SessionApplyCtx;
struct SessionApplyCtx {
  sqlite3 *db;
  sqlite3_stmt *pDelete;          /* DELETE statement */
  sqlite3_stmt *pUpdate;          /* DELETE statement */
  sqlite3_stmt *pInsert;          /* INSERT statement */
  sqlite3_stmt *pSelect;          /* SELECT statement */
  int nCol;                       /* Size of azCol[] and abPK[] arrays */
  const char **azCol;             /* Array of column names */
  u8 *abPK;                       /* Boolean array - true if column is in PK */
};

/*
** Formulate a statement to DELETE a row from database db. Assuming a table
** structure like this:
**
**     CREATE TABLE x(a, b, c, d, PRIMARY KEY(a, c));
**
** The DELETE statement looks like this:
**
**     DELETE FROM x WHERE a = :1 AND c = :3 AND :5 OR (b IS :2 AND d IS :4)
**
** Variable :5 (nCol+1) is a boolean. It should be set to 0 if we require
** matching b and d values, or 1 otherwise. The second case comes up if the
** conflict handler is invoked with NOTFOUND and returns CHANGESET_REPLACE.
**
** If successful, SQLITE_OK is returned and SessionApplyCtx.pDelete is left
** pointing to the prepared version of the SQL statement.
*/
static int sessionDeleteRow(
  sqlite3 *db,                    /* Database handle */
  const char *zTab,               /* Table name */
  SessionApplyCtx *p              /* Session changeset-apply context */
){
  int i;
  const char *zSep = "";
  int rc = SQLITE_OK;
  SessionBuffer buf = {0, 0, 0};
  int nPk = 0;

  sessionAppendStr(&buf, "DELETE FROM ", &rc);
  sessionAppendIdent(&buf, zTab, &rc);
  sessionAppendStr(&buf, " WHERE ", &rc);

  for(i=0; i<p->nCol; i++){
    if( p->abPK[i] ){
      nPk++;
      sessionAppendStr(&buf, zSep, &rc);
      sessionAppendIdent(&buf, p->azCol[i], &rc);
      sessionAppendStr(&buf, " = ?", &rc);
      sessionAppendInteger(&buf, i+1, &rc);
      zSep = " AND ";
    }
  }

  if( nPk<p->nCol ){
    sessionAppendStr(&buf, " AND (?", &rc);
    sessionAppendInteger(&buf, p->nCol+1, &rc);
    sessionAppendStr(&buf, " OR ", &rc);

    zSep = "";
    for(i=0; i<p->nCol; i++){
      if( !p->abPK[i] ){
        sessionAppendStr(&buf, zSep, &rc);
        sessionAppendIdent(&buf, p->azCol[i], &rc);
        sessionAppendStr(&buf, " IS ?", &rc);
        sessionAppendInteger(&buf, i+1, &rc);
        zSep = "AND ";
      }
    }
    sessionAppendStr(&buf, ")", &rc);
  }

  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, (char *)buf.aBuf, buf.nBuf, &p->pDelete, 0);
  }
  sqlite3_free(buf.aBuf);

  return rc;
}

/*
** Formulate and prepare a statement to UPDATE a row from database db. 
** Assuming a table structure like this:
**
**     CREATE TABLE x(a, b, c, d, PRIMARY KEY(a, c));
**
** The UPDATE statement looks like this:
**
**     UPDATE x SET
**     a = CASE WHEN ?2  THEN ?3  ELSE a END,
**     b = CASE WHEN ?5  THEN ?6  ELSE a END,
**     c = CASE WHEN ?8  THEN ?9  ELSE a END,
**     d = CASE WHEN ?11 THEN ?12 ELSE a END
**     WHERE a = ?1 AND c = ?7 AND (?13 OR 
**       (?5==0 OR b IS ?4) AND (?11==0 OR b IS ?10) AND
**     )
**
** For each column in the table, there are three variables to bind:
**
**     ?(i*3+1)    The old.* value of the column, if any.
**     ?(i*3+2)    A boolean flag indicating that the value is being modified.
**     ?(i*3+3)    The new.* value of the column, if any.
**
** Also, a boolean flag that, if set to true, causes the statement to update
** a row even if the non-PK values do not match. This is required if the
** conflict-handler is invoked with CHANGESET_DATA and returns
** CHANGESET_REPLACE. This is variable "?(nCol*3+1)".
**
** If successful, SQLITE_OK is returned and SessionApplyCtx.pUpdate is left
** pointing to the prepared version of the SQL statement.
*/
static int sessionUpdateRow(
  sqlite3 *db,                    /* Database handle */
  const char *zTab,               /* Table name */
  SessionApplyCtx *p              /* Session changeset-apply context */
){
  int rc = SQLITE_OK;
  int i;
  const char *zSep = "";
  SessionBuffer buf = {0, 0, 0};

  /* Append "UPDATE tbl SET " */
  sessionAppendStr(&buf, "UPDATE ", &rc);
  sessionAppendIdent(&buf, zTab, &rc);
  sessionAppendStr(&buf, " SET ", &rc);

  /* Append the assignments */
  for(i=0; i<p->nCol; i++){
    sessionAppendStr(&buf, zSep, &rc);
    sessionAppendIdent(&buf, p->azCol[i], &rc);
    sessionAppendStr(&buf, " = CASE WHEN ?", &rc);
    sessionAppendInteger(&buf, i*3+2, &rc);
    sessionAppendStr(&buf, " THEN ?", &rc);
    sessionAppendInteger(&buf, i*3+3, &rc);
    sessionAppendStr(&buf, " ELSE ", &rc);
    sessionAppendIdent(&buf, p->azCol[i], &rc);
    sessionAppendStr(&buf, " END", &rc);
    zSep = ", ";
  }

  /* Append the PK part of the WHERE clause */
  sessionAppendStr(&buf, " WHERE ", &rc);
  for(i=0; i<p->nCol; i++){
    if( p->abPK[i] ){
      sessionAppendIdent(&buf, p->azCol[i], &rc);
      sessionAppendStr(&buf, " = ?", &rc);
      sessionAppendInteger(&buf, i*3+1, &rc);
      sessionAppendStr(&buf, " AND ", &rc);
    }
  }

  /* Append the non-PK part of the WHERE clause */
  sessionAppendStr(&buf, " (?", &rc);
  sessionAppendInteger(&buf, p->nCol*3+1, &rc);
  sessionAppendStr(&buf, " OR 1", &rc);
  for(i=0; i<p->nCol; i++){
    if( !p->abPK[i] ){
      sessionAppendStr(&buf, " AND (?", &rc);
      sessionAppendInteger(&buf, i*3+2, &rc);
      sessionAppendStr(&buf, "=0 OR ", &rc);
      sessionAppendIdent(&buf, p->azCol[i], &rc);
      sessionAppendStr(&buf, " IS ?", &rc);
      sessionAppendInteger(&buf, i*3+1, &rc);
      sessionAppendStr(&buf, ")", &rc);
    }
  }
  sessionAppendStr(&buf, ")", &rc);

  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, (char *)buf.aBuf, buf.nBuf, &p->pUpdate, 0);
  }
  sqlite3_free(buf.aBuf);

  return rc;
}

/*
** Formulate and prepare an SQL statement to query table zTab by primary
** key. Assuming the following table structure:
**
**     CREATE TABLE x(a, b, c, d, PRIMARY KEY(a, c));
**
** The SELECT statement looks like this:
**
**     SELECT * FROM x WHERE a = ?1 AND c = ?3
**
** If successful, SQLITE_OK is returned and SessionApplyCtx.pSelect is left
** pointing to the prepared version of the SQL statement.
*/
static int sessionSelectRow(
  sqlite3 *db,                    /* Database handle */
  const char *zTab,               /* Table name */
  SessionApplyCtx *p              /* Session changeset-apply context */
){
  return sessionSelectStmt(
      db, "main", zTab, p->nCol, p->azCol, p->abPK, &p->pSelect);
}

/*
** Formulate and prepare an INSERT statement to add a record to table zTab.
** For example:
**
**     INSERT INTO main."zTab" VALUES(?1, ?2, ?3 ...);
**
** If successful, SQLITE_OK is returned and SessionApplyCtx.pInsert is left
** pointing to the prepared version of the SQL statement.
*/
static int sessionInsertRow(
  sqlite3 *db,                    /* Database handle */
  const char *zTab,               /* Table name */
  SessionApplyCtx *p              /* Session changeset-apply context */
){
  int rc = SQLITE_OK;
  int i;
  SessionBuffer buf = {0, 0, 0};

  sessionAppendStr(&buf, "INSERT INTO main.", &rc);
  sessionAppendIdent(&buf, zTab, &rc);
  sessionAppendStr(&buf, " VALUES(?", &rc);
  for(i=1; i<p->nCol; i++){
    sessionAppendStr(&buf, ", ?", &rc);
  }
  sessionAppendStr(&buf, ")", &rc);

  if( rc==SQLITE_OK ){
    rc = sqlite3_prepare_v2(db, (char *)buf.aBuf, buf.nBuf, &p->pInsert, 0);
  }
  sqlite3_free(buf.aBuf);
  return rc;
}

/*
** SQL statement pSelect is as generated by the sessionSelectRow() function.
** This function binds the primary key values from the change that changeset
** iterator pIter points to to the SELECT and attempts to seek to the table
** entry. If a row is found, the SELECT statement left pointing at the row 
** and SQLITE_ROW is returned. Otherwise, if no row is found and no error
** has occured, the statement is reset and SQLITE_OK is returned. If an
** error occurs, an SQLite error code is returned.
**
** If the iterator currently points to an INSERT record, bind values from the
** new.* record to the SELECT statement. Or, if it points to a DELETE, bind
** values from the old.* record. If the changeset iterator points to an
** UPDATE, bind values from the new.* record, but use old.* values in place
** of any undefined new.* values.
*/
static int sessionSeekToRow(
  sqlite3 *db,                    /* Database handle */
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  u8 *abPK,                       /* Primary key flags array */
  sqlite3_stmt *pSelect           /* SELECT statement from sessionSelectRow() */
){
  int rc = SQLITE_OK;             /* Return code */
  int i;                          /* Used to iterate through table columns */
  int nCol;                       /* Number of columns in table */
  int op;                         /* Changset operation (SQLITE_UPDATE etc.) */
  const char *zDummy;             /* Unused */

  sqlite3changeset_op(pIter, &zDummy, &nCol, &op);

  for(i=0; rc==SQLITE_OK && i<nCol; i++){
    if( abPK[i] ){
      sqlite3_value *pVal = 0;
      if( op!=SQLITE_DELETE ){
        rc = sqlite3changeset_new(pIter, i, &pVal);
      }
      if( pVal==0 ){
        rc = sqlite3changeset_old(pIter, i, &pVal);
      }
      if( rc==SQLITE_OK ){
        rc = sqlite3_bind_value(pSelect, i+1, pVal);
      }
    }
  }

  if( rc==SQLITE_OK ){
    rc = sqlite3_step(pSelect);
    if( rc!=SQLITE_ROW ) rc = sqlite3_reset(pSelect);
  }

  return rc;
}

/*
** Invoke the conflict handler for the change that the changeset iterator
** currently points to.
**
** Argument eType must be either CHANGESET_DATA or CHANGESET_CONFLICT.
** If argument pbReplace is NULL, then the type of conflict handler invoked
** depends solely on eType, as follows:
**
**    eType value                 Value passed to xConflict
**    -------------------------------------------------
**    CHANGESET_DATA              CHANGESET_NOTFOUND
**    CHANGESET_CONFLICT          CHANGESET_CONSTRAINT
**
** Or, if pbReplace is not NULL, then an attempt is made to find an existing
** record with the same primary key as the record about to be deleted, updated
** or inserted. If such a record can be found, it is available to the conflict
** handler as the "conflicting" record. In this case the type of conflict
** handler invoked is as follows:
**
**    eType value         PK Record found?   Value passed to xConflict
**    ----------------------------------------------------------------
**    CHANGESET_DATA      Yes                CHANGESET_DATA
**    CHANGESET_DATA      No                 CHANGESET_NOTFOUND
**    CHANGESET_CONFLICT  Yes                CHANGESET_CONFLICT
**    CHANGESET_CONFLICT  No                 CHANGESET_CONSTRAINT
**
** If pbReplace is not NULL, and a record with a matching PK is found, and
** the conflict handler function returns SQLITE_CHANGESET_REPLACE, *pbReplace
** is set to non-zero before returning SQLITE_OK.
**
** If the conflict handler returns SQLITE_CHANGESET_ABORT, SQLITE_ABORT is
** returned. Or, if the conflict handler returns an invalid value, 
** SQLITE_MISUSE. If the conflict handler returns SQLITE_CHANGESET_OMIT,
** this function returns SQLITE_OK.
*/
static int sessionConflictHandler(
  int eType,                      /* Either CHANGESET_DATA or CONFLICT */
  SessionApplyCtx *p,             /* changeset_apply() context */
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  int(*xConflict)(void *, int, sqlite3_changeset_iter*),
  void *pCtx,                     /* First argument for conflict handler */
  int *pbReplace                  /* OUT: Set to true if PK row is found */
){
  int res;                        /* Value returned by conflict handler */
  int rc;
  int nCol;
  int op;
  const char *zDummy;

  sqlite3changeset_op(pIter, &zDummy, &nCol, &op);

  assert( eType==SQLITE_CHANGESET_CONFLICT || eType==SQLITE_CHANGESET_DATA );
  assert( SQLITE_CHANGESET_CONFLICT+1==SQLITE_CHANGESET_CONSTRAINT );
  assert( SQLITE_CHANGESET_DATA+1==SQLITE_CHANGESET_NOTFOUND );

  /* Bind the new.* PRIMARY KEY values to the SELECT statement. */
  if( pbReplace ){
    rc = sessionSeekToRow(p->db, pIter, p->abPK, p->pSelect);
  }else{
    rc = SQLITE_DONE;
  }

  if( rc==SQLITE_ROW ){
    /* There exists another row with the new.* primary key. */
    pIter->pConflict = p->pSelect;
    res = xConflict(pCtx, eType, pIter);
    pIter->pConflict = 0;
    rc = sqlite3_reset(p->pSelect);
  }else{
    /* No other row with the new.* primary key. */
    rc = sqlite3_reset(p->pSelect);
    if( rc==SQLITE_OK ){
      res = xConflict(pCtx, eType+1, pIter);
      if( res==SQLITE_CHANGESET_REPLACE ) rc = SQLITE_MISUSE;
    }
  }

  if( rc==SQLITE_OK ){
    switch( res ){
      case SQLITE_CHANGESET_REPLACE:
        if( pbReplace ) *pbReplace = 1;
        break;

      case SQLITE_CHANGESET_OMIT:
        break;

      case SQLITE_CHANGESET_ABORT:
        rc = SQLITE_ABORT;
        break;

      default:
        rc = SQLITE_MISUSE;
        break;
    }
  }

  return rc;
}

/*
** Attempt to apply the change that the iterator passed as the first argument
** currently points to to the database. If a conflict is encountered, invoke
** the conflict handler callback.
**
** If argument pbRetry is NULL, then ignore any CHANGESET_DATA conflict. If
** one is encountered, update or delete the row with the matching primary key
** instead. Or, if pbRetry is not NULL and a CHANGESET_DATA conflict occurs,
** invoke the conflict handler. If it returns CHANGESET_REPLACE, set *pbRetry
** to true before returning. In this case the caller will invoke this function
** again, this time with pbRetry set to NULL.
**
** If argument pbReplace is NULL and a CHANGESET_CONFLICT conflict is 
** encountered invoke the conflict handler with CHANGESET_CONSTRAINT instead.
** Or, if pbReplace is not NULL, invoke it with CHANGESET_CONFLICT. If such
** an invocation returns SQLITE_CHANGESET_REPLACE, set *pbReplace to true
** before retrying. In this case the caller attempts to remove the conflicting
** row before invoking this function again, this time with pbReplace set 
** to NULL.
**
** If any conflict handler returns SQLITE_CHANGESET_ABORT, this function
** returns SQLITE_ABORT. Otherwise, if no error occurs, SQLITE_OK is 
** returned.
*/
static int sessionApplyOneOp(
  sqlite3_changeset_iter *pIter,  /* Changeset iterator */
  SessionApplyCtx *p,             /* changeset_apply() context */
  int(*xConflict)(void *, int, sqlite3_changeset_iter *),
  void *pCtx,                     /* First argument for the conflict handler */
  int *pbReplace,                 /* OUT: True to remove PK row and retry */
  int *pbRetry                    /* OUT: True to retry. */
){
  const char *zDummy;
  int op;
  int nCol;
  int rc = SQLITE_OK;

  assert( p->pDelete && p->pUpdate && p->pInsert && p->pSelect );
  assert( p->azCol && p->abPK );
  assert( !pbReplace || *pbReplace==0 );

  sqlite3changeset_op(pIter, &zDummy, &nCol, &op);

  if( op==SQLITE_DELETE ){
    int i;

    /* Bind values to the DELETE statement. */
    for(i=0; rc==SQLITE_OK && i<nCol; i++){
      sqlite3_value *pVal;
      rc = sqlite3changeset_old(pIter, i, &pVal);
      if( rc==SQLITE_OK ){
        rc = sqlite3_bind_value(p->pDelete, i+1, pVal);
      }
    }
    if( rc==SQLITE_OK && sqlite3_bind_parameter_count(p->pDelete)>nCol ){
      rc = sqlite3_bind_int(p->pDelete, nCol+1, pbRetry==0);
    }
    if( rc!=SQLITE_OK ) return rc;

    sqlite3_step(p->pDelete);
    rc = sqlite3_reset(p->pDelete);
    if( rc==SQLITE_OK && sqlite3_changes(p->db)==0 ){
      rc = sessionConflictHandler(
          SQLITE_CHANGESET_DATA, p, pIter, xConflict, pCtx, pbRetry
      );
    }else if( rc==SQLITE_CONSTRAINT ){
      rc = sessionConflictHandler(
          SQLITE_CHANGESET_CONFLICT, p, pIter, xConflict, pCtx, 0
      );
    }

  }else if( op==SQLITE_UPDATE ){
    int i;

    /* Bind values to the UPDATE statement. */
    for(i=0; rc==SQLITE_OK && i<nCol; i++){
      sqlite3_value *pOld = 0;
      sqlite3_value *pNew = 0;
      rc = sqlite3changeset_old(pIter, i, &pOld);
      if( rc==SQLITE_OK ){
        rc = sqlite3changeset_new(pIter, i, &pNew);
      }
      if( rc==SQLITE_OK ){
        if( pOld ) sqlite3_bind_value(p->pUpdate, i*3+1, pOld);
        sqlite3_bind_int(p->pUpdate, i*3+2, !!pNew);
        if( pNew ) sqlite3_bind_value(p->pUpdate, i*3+3, pNew);
      }
    }
    if( rc==SQLITE_OK ) rc = sqlite3_bind_int(p->pUpdate, nCol*3+1, pbRetry==0);
    if( rc!=SQLITE_OK ) return rc;

    /* Attempt the UPDATE. In the case of a NOTFOUND or DATA conflict,
    ** the result will be SQLITE_OK with 0 rows modified. */
    sqlite3_step(p->pUpdate);
    rc = sqlite3_reset(p->pUpdate);

    if( rc==SQLITE_OK && sqlite3_changes(p->db)==0 ){
      /* A NOTFOUND or DATA error. Search the table to see if it contains
      ** a row with a matching primary key. If so, this is a DATA conflict.
      ** Otherwise, if there is no primary key match, it is a NOTFOUND. */

      rc = sessionConflictHandler(
          SQLITE_CHANGESET_DATA, p, pIter, xConflict, pCtx, pbRetry
      );

    }else if( rc==SQLITE_CONSTRAINT ){
      /* This may be a CONSTRAINT or CONFLICT error. It is a CONFLICT if
      ** the only problem is a duplicate PRIMARY KEY, or a CONSTRAINT 
      ** otherwise. */
      int bPKChange = 0;

      /* Check if the PK has been modified. */
      rc = SQLITE_OK;
      for(i=0; i<nCol && rc==SQLITE_OK; i++){
        if( p->abPK[i] ){
          sqlite3_value *pNew;
          rc = sqlite3changeset_new(pIter, i, &pNew);
          if( rc==SQLITE_OK && pNew ){
            bPKChange = 1;
            break;
          }
        }
      }

      rc = sessionConflictHandler(SQLITE_CHANGESET_CONFLICT, 
          p, pIter, xConflict, pCtx, (bPKChange ? pbReplace : 0)
      );
    }

  }else{
    int i;
    assert( op==SQLITE_INSERT );
    for(i=0; rc==SQLITE_OK && i<nCol; i++){
      sqlite3_value *pVal;
      rc = sqlite3changeset_new(pIter, i, &pVal);
      if( rc==SQLITE_OK ){
        rc = sqlite3_bind_value(p->pInsert, i+1, pVal);
      }
    }
    if( rc!=SQLITE_OK ) return rc;

    sqlite3_step(p->pInsert);
    rc = sqlite3_reset(p->pInsert);
    if( rc==SQLITE_CONSTRAINT && xConflict ){
      rc = sessionConflictHandler(
          SQLITE_CHANGESET_CONFLICT, p, pIter, xConflict, pCtx, pbReplace
      );
    }
  }

  return rc;
}

/*
** Apply the changeset passed via pChangeset/nChangeset to the main database
** attached to handle "db". Invoke the supplied conflict handler callback
** to resolve any conflicts encountered while applying the change.
*/
int sqlite3changeset_apply(
  sqlite3 *db,                    /* Apply change to "main" db of this handle */
  int nChangeset,                 /* Size of changeset in bytes */
  void *pChangeset,               /* Changeset blob */
  int(*xConflict)(
    void *pCtx,                   /* Copy of fifth arg to _apply() */
    int eConflict,                /* DATA, MISSING, CONFLICT, CONSTRAINT */
    sqlite3_changeset_iter *p     /* Handle describing change and conflict */
  ),
  void *pCtx                      /* First argument passed to xConflict */
){
  sqlite3_changeset_iter *pIter;  /* Iterator to skip through changeset */  
  int rc;                         /* Return code */
  const char *zTab = 0;           /* Name of current table */
  int nTab = 0;                   /* Result of strlen(zTab) */
  SessionApplyCtx sApply;         /* changeset_apply() context object */

  memset(&sApply, 0, sizeof(sApply));
  sqlite3changeset_start(&pIter, nChangeset, pChangeset);

  sqlite3_mutex_enter(sqlite3_db_mutex(db));
  rc = sqlite3_exec(db, "SAVEPOINT changeset_apply", 0, 0, 0);
  while( rc==SQLITE_OK && SQLITE_ROW==sqlite3changeset_next(pIter) ){
    int nCol;
    int op;
    int bReplace = 0;
    int bRetry = 0;
    const char *zNew;
    sqlite3changeset_op(pIter, &zNew, &nCol, &op);

    if( zTab==0 || sqlite3_strnicmp(zNew, zTab, nTab+1) ){
      sqlite3_free(sApply.azCol);
      sqlite3_finalize(sApply.pDelete);
      sqlite3_finalize(sApply.pUpdate); 
      sqlite3_finalize(sApply.pInsert);
      sqlite3_finalize(sApply.pSelect);
      memset(&sApply, 0, sizeof(sApply));
      sApply.db = db;
      sApply.nCol = nCol;

      rc = sessionTableInfo(
          db, "main", zNew, nCol, &zTab, &sApply.azCol, &sApply.abPK);

      if( rc!=SQLITE_OK 
       || (rc = sessionSelectRow(db, zTab, &sApply))
       || (rc = sessionUpdateRow(db, zTab, &sApply))
       || (rc = sessionDeleteRow(db, zTab, &sApply))
       || (rc = sessionInsertRow(db, zTab, &sApply))
      ){
        break;
      }

      nTab = strlen(zTab);
    }

    rc = sessionApplyOneOp(pIter, &sApply, xConflict, pCtx, &bReplace, &bRetry);

    if( rc==SQLITE_OK && bRetry ){
      rc = sessionApplyOneOp(pIter, &sApply, xConflict, pCtx, &bReplace, 0);
    }

    if( bReplace ){
      rc = sqlite3_exec(db, "SAVEPOINT replace_op", 0, 0, 0);
      if( rc==SQLITE_OK ){
        int i;
        for(i=0; i<sApply.nCol; i++){
          if( sApply.abPK[i] ){
            sqlite3_value *pVal;
            rc = sqlite3changeset_new(pIter, i, &pVal);
            if( rc==SQLITE_OK && pVal==0 ){
              rc = sqlite3changeset_old(pIter, i, &pVal);
            }
            if( rc==SQLITE_OK ){
              rc = sqlite3_bind_value(sApply.pDelete, i+1, pVal);
            }
          }
        }
        sqlite3_bind_int(sApply.pDelete, sApply.nCol+1, 1);
      }
      if( rc==SQLITE_OK ){
        sqlite3_step(sApply.pDelete);
        rc = sqlite3_reset(sApply.pDelete);
      }
      if( rc==SQLITE_OK ){
        rc = sessionApplyOneOp(pIter, &sApply, xConflict, pCtx, 0, 0);
      }
      if( rc==SQLITE_OK ){
        rc = sqlite3_exec(db, "RELEASE replace_op", 0, 0, 0);
      }
    }
  }

  if( rc==SQLITE_OK ){
    rc = sqlite3changeset_finalize(pIter);
  }else{
    sqlite3changeset_finalize(pIter);
  }

  if( rc==SQLITE_OK ){
    rc = sqlite3_exec(db, "RELEASE changeset_apply", 0, 0, 0);
  }else{
    sqlite3_exec(db, "ROLLBACK TO changeset_apply", 0, 0, 0);
    sqlite3_exec(db, "RELEASE changeset_apply", 0, 0, 0);
  }

  sqlite3_finalize(sApply.pInsert);
  sqlite3_finalize(sApply.pDelete);
  sqlite3_finalize(sApply.pUpdate);
  sqlite3_finalize(sApply.pSelect);
  sqlite3_free(sApply.azCol);
  sqlite3_mutex_leave(sqlite3_db_mutex(db));
  return rc;
}

#endif        /* #ifdef SQLITE_ENABLE_SESSION */
