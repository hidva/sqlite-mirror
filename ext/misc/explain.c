/*
** 2018-09-16
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
** This file demonstrates an eponymous virtual table that returns the
** EXPLAIN output from an SQL statement.
**
** Usage example:
**
**     .load ./explain
**     SELECT p2 FROM explain('SELECT * FROM sqlite_master')
**      WHERE opcode='OpenRead';
*/
#if !defined(SQLITEINT_H)
#include "sqlite3ext.h"
#endif
SQLITE_EXTENSION_INIT1
#include <assert.h>
#include <string.h>

#ifndef SQLITE_OMIT_VIRTUALTABLE

/* explain_vtab is a subclass of sqlite3_vtab which will
** serve as the underlying representation of a explain virtual table
*/
typedef struct explain_vtab explain_vtab;
struct explain_vtab {
  sqlite3_vtab base;  /* Base class - must be first */
  sqlite3 *db;        /* Database connection for this explain vtab */
};

/* explain_cursor is a subclass of sqlite3_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result from an EXPLAIN operation.
*/
typedef struct explain_cursor explain_cursor;
struct explain_cursor {
  sqlite3_vtab_cursor base;  /* Base class - must be first */
  sqlite3 *db;               /* Database connection for this cursor */
  char *zSql;                /* Value for the EXPLN_COLUMN_SQL column */
  sqlite3_stmt *pExplain;    /* Statement being explained */
  int rc;                    /* Result of last sqlite3_step() on pExplain */
};

/*
** The explainConnect() method is invoked to create a new
** explain_vtab that describes the explain virtual table.
**
** Think of this routine as the constructor for explain_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the explain_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the sqlite3_declare_vtab() interface) what the
**        result set of queries against explain will look like.
*/
static int explainConnect(
  sqlite3 *db,
  void *pAux,
  int argc, const char *const*argv,
  sqlite3_vtab **ppVtab,
  char **pzErr
){
  explain_vtab *pNew;
  int rc;

/* Column numbers */
#define EXPLN_COLUMN_ADDR     0   /* Instruction address */
#define EXPLN_COLUMN_OPCODE   1   /* Opcode */
#define EXPLN_COLUMN_P1       2   /* Operand 1 */
#define EXPLN_COLUMN_P2       3   /* Operand 2 */
#define EXPLN_COLUMN_P3       4   /* Operand 3 */
#define EXPLN_COLUMN_P4       5   /* Operand 4 */
#define EXPLN_COLUMN_P5       6   /* Operand 5 */
#define EXPLN_COLUMN_COMMENT  7   /* Comment */
#define EXPLN_COLUMN_SQL      8   /* SQL that is being explained */


  rc = sqlite3_declare_vtab(db,
     "CREATE TABLE x(addr,opcode,p1,p2,p3,p4,p5,comment,sql HIDDEN)");
  if( rc==SQLITE_OK ){
    pNew = sqlite3_malloc( sizeof(*pNew) );
    *ppVtab = (sqlite3_vtab*)pNew;
    if( pNew==0 ) return SQLITE_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
    pNew->db = db;
  }
  return rc;
}

/*
** This method is the destructor for explain_cursor objects.
*/
static int explainDisconnect(sqlite3_vtab *pVtab){
  sqlite3_free(pVtab);
  return SQLITE_OK;
}

/*
** Constructor for a new explain_cursor object.
*/
static int explainOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor){
  explain_cursor *pCur;
  pCur = sqlite3_malloc( sizeof(*pCur) );
  if( pCur==0 ) return SQLITE_NOMEM;
  memset(pCur, 0, sizeof(*pCur));
  pCur->db = ((explain_vtab*)p)->db;
  *ppCursor = &pCur->base;
  return SQLITE_OK;
}

/*
** Destructor for a explain_cursor.
*/
static int explainClose(sqlite3_vtab_cursor *cur){
  explain_cursor *pCur = (explain_cursor*)cur;
  sqlite3_finalize(pCur->pExplain);
  sqlite3_free(pCur->zSql);
  sqlite3_free(pCur);
  return SQLITE_OK;
}


/*
** Advance a explain_cursor to its next row of output.
*/
static int explainNext(sqlite3_vtab_cursor *cur){
  explain_cursor *pCur = (explain_cursor*)cur;
  pCur->rc = sqlite3_step(pCur->pExplain);
  if( pCur->rc!=SQLITE_DONE && pCur->rc!=SQLITE_ROW ) return pCur->rc;
  return SQLITE_OK;
}

/*
** Return values of columns for the row at which the explain_cursor
** is currently pointing.
*/
static int explainColumn(
  sqlite3_vtab_cursor *cur,   /* The cursor */
  sqlite3_context *ctx,       /* First argument to sqlite3_result_...() */
  int i                       /* Which column to return */
){
  explain_cursor *pCur = (explain_cursor*)cur;
  if( i==EXPLN_COLUMN_SQL ){
    sqlite3_result_text(ctx, pCur->zSql, -1, SQLITE_TRANSIENT);
  }else{
    sqlite3_result_value(ctx, sqlite3_column_value(pCur->pExplain, i));
  }
  return SQLITE_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int explainRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
  explain_cursor *pCur = (explain_cursor*)cur;
  *pRowid = sqlite3_column_int64(pCur->pExplain, 0);
  return SQLITE_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int explainEof(sqlite3_vtab_cursor *cur){
  explain_cursor *pCur = (explain_cursor*)cur;
  return pCur->rc!=SQLITE_ROW;
}

/*
** This method is called to "rewind" the explain_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to explainColumn() or explainRowid() or 
** explainEof().
**
** The argv[0] is the SQL statement that is to be explained.
*/
static int explainFilter(
  sqlite3_vtab_cursor *pVtabCursor, 
  int idxNum, const char *idxStr,
  int argc, sqlite3_value **argv
){
  explain_cursor *pCur = (explain_cursor *)pVtabCursor;
  char *zSql = 0;
  int rc;
  sqlite3_finalize(pCur->pExplain);
  pCur->pExplain = 0;
  if( sqlite3_value_type(argv[0])!=SQLITE_TEXT ){
    pCur->rc = SQLITE_DONE;
    return SQLITE_OK;
  }
  sqlite3_free(pCur->zSql);
  pCur->zSql = sqlite3_mprintf("%s", sqlite3_value_text(argv[0]));
  if( pCur->zSql ){
    zSql = sqlite3_mprintf("EXPLAIN %s", pCur->zSql);
  }
  if( zSql==0 ){
    rc = SQLITE_NOMEM;
  }else{
    rc = sqlite3_prepare_v2(pCur->db, zSql, -1, &pCur->pExplain, 0);
    sqlite3_free(zSql);
  }
  if( rc ){
    sqlite3_finalize(pCur->pExplain);
    pCur->pExplain = 0;
    sqlite3_free(pCur->zSql);
    pCur->zSql = 0;
  }else{
    pCur->rc = sqlite3_step(pCur->pExplain);
    rc = (pCur->rc==SQLITE_DONE || pCur->rc==SQLITE_ROW) ? SQLITE_OK : pCur->rc;
  }
  return rc;
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the explain virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int explainBestIndex(
  sqlite3_vtab *tab,
  sqlite3_index_info *pIdxInfo
){
  int i;

  pIdxInfo->estimatedCost = (double)1000000;
  pIdxInfo->estimatedRows = 500;
  for(i=0; i<pIdxInfo->nConstraint; i++){
    struct sqlite3_index_constraint *p = &pIdxInfo->aConstraint[i];
    if( p->usable
     && p->iColumn==EXPLN_COLUMN_SQL
     && p->op==SQLITE_INDEX_CONSTRAINT_EQ
    ){
      pIdxInfo->estimatedCost = 10.0;
      pIdxInfo->idxNum = 1;
      pIdxInfo->aConstraintUsage[i].argvIndex = 1;
      pIdxInfo->aConstraintUsage[i].omit = 1;
      break;
    }
  }
  return SQLITE_OK;
}

/*
** This following structure defines all the methods for the 
** explain virtual table.
*/
static sqlite3_module explainModule = {
  0,                         /* iVersion */
  0,                         /* xCreate */
  explainConnect,            /* xConnect */
  explainBestIndex,          /* xBestIndex */
  explainDisconnect,         /* xDisconnect */
  0,                         /* xDestroy */
  explainOpen,               /* xOpen - open a cursor */
  explainClose,              /* xClose - close a cursor */
  explainFilter,             /* xFilter - configure scan constraints */
  explainNext,               /* xNext - advance a cursor */
  explainEof,                /* xEof - check for end of scan */
  explainColumn,             /* xColumn - read data */
  explainRowid,              /* xRowid - read data */
  0,                         /* xUpdate */
  0,                         /* xBegin */
  0,                         /* xSync */
  0,                         /* xCommit */
  0,                         /* xRollback */
  0,                         /* xFindMethod */
  0,                         /* xRename */
  0,                         /* xSavepoint */
  0,                         /* xRelease */
  0,                         /* xRollbackTo */
};

#endif /* SQLITE_OMIT_VIRTUALTABLE */

int sqlite3ExplainVtabInit(sqlite3 *db){
  int rc = SQLITE_OK;
#ifndef SQLITE_OMIT_VIRTUALTABLE
  rc = sqlite3_create_module(db, "explain", &explainModule, 0);
#endif
  return rc;
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_explain_init(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  SQLITE_EXTENSION_INIT2(pApi);
#ifndef SQLITE_OMIT_VIRTUALTABLE
  rc = sqlite3ExplainVtabInit(db);
#endif
  return rc;
}
