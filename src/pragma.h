/* DO NOT EDIT!
** This file is automatically generated by the script at
** ../tool/mkpragmatab.tcl.  To update the set of pragmas, edit
** that script and rerun it.
*/

/* The various pragma types */
#define PragTyp_HEADER_VALUE                   0
#define PragTyp_AUTO_VACUUM                    1
#define PragTyp_FLAG                           2
#define PragTyp_BUSY_TIMEOUT                   3
#define PragTyp_CACHE_SIZE                     4
#define PragTyp_CACHE_SPILL                    5
#define PragTyp_CASE_SENSITIVE_LIKE            6
#define PragTyp_COLLATION_LIST                 7
#define PragTyp_COMPILE_OPTIONS                8
#define PragTyp_DATA_STORE_DIRECTORY           9
#define PragTyp_DATABASE_LIST                 10
#define PragTyp_DEFAULT_CACHE_SIZE            11
#define PragTyp_ENCODING                      12
#define PragTyp_FOREIGN_KEY_CHECK             13
#define PragTyp_FOREIGN_KEY_LIST              14
#define PragTyp_INCREMENTAL_VACUUM            15
#define PragTyp_INDEX_INFO                    16
#define PragTyp_INDEX_LIST                    17
#define PragTyp_INTEGRITY_CHECK               18
#define PragTyp_JOURNAL_MODE                  19
#define PragTyp_JOURNAL_SIZE_LIMIT            20
#define PragTyp_LOCK_PROXY_FILE               21
#define PragTyp_LOCKING_MODE                  22
#define PragTyp_PAGE_COUNT                    23
#define PragTyp_MMAP_SIZE                     24
#define PragTyp_PAGE_SIZE                     25
#define PragTyp_SECURE_DELETE                 26
#define PragTyp_SHRINK_MEMORY                 27
#define PragTyp_SOFT_HEAP_LIMIT               28
#define PragTyp_SYNCHRONOUS                   29
#define PragTyp_TABLE_INFO                    30
#define PragTyp_TEMP_STORE                    31
#define PragTyp_TEMP_STORE_DIRECTORY          32
#define PragTyp_THREADS                       33
#define PragTyp_WAL_AUTOCHECKPOINT            34
#define PragTyp_WAL_CHECKPOINT                35
#define PragTyp_ACTIVATE_EXTENSIONS           36
#define PragTyp_HEXKEY                        37
#define PragTyp_KEY                           38
#define PragTyp_REKEY                         39
#define PragTyp_LOCK_STATUS                   40
#define PragTyp_PARSER_TRACE                  41
#define PragTyp_STATS                         42

/* Property flags associated with various pragma. */
#define PragFlg_NeedSchema 0x01 /* Force schema load before running */
#define PragFlg_NoColumns  0x02 /* OP_ResultRow called with zero columns */
#define PragFlg_NoColumns1 0x04 /* zero columns if RHS argument is present */
#define PragFlg_ReadOnly   0x08 /* Read-only HEADER_VALUE */
#define PragFlg_Result0    0x10 /* Acts as query when no argument */
#define PragFlg_Result1    0x20 /* Acts as query when has one argument */
#define PragFlg_SchemaOpt  0x40 /* Schema restricts name search if present */
#define PragFlg_SchemaReq  0x80 /* Schema required - "main" is default */

/* Names of columns for pragmas that return multi-column result
** or that return single-column results where the name of the
** result column is different from the name of the pragma
*/
static const char *const pragCName[] = {
  /*   0 */ "cache_size",  /* Used by: default_cache_size */
  /*   1 */ "cid",         /* Used by: table_info */
  /*   2 */ "name",       
  /*   3 */ "type",       
  /*   4 */ "notnull",    
  /*   5 */ "dflt_value", 
  /*   6 */ "pk",         
  /*   7 */ "table",       /* Used by: stats */
  /*   8 */ "index",      
  /*   9 */ "width",      
  /*  10 */ "height",     
  /*  11 */ "flags",      
  /*  12 */ "seqno",       /* Used by: index_info */
  /*  13 */ "cid",        
  /*  14 */ "name",       
  /*  15 */ "seqno",       /* Used by: index_xinfo */
  /*  16 */ "cid",        
  /*  17 */ "name",       
  /*  18 */ "desc",       
  /*  19 */ "coll",       
  /*  20 */ "key",        
  /*  21 */ "seq",         /* Used by: index_list */
  /*  22 */ "name",       
  /*  23 */ "unique",     
  /*  24 */ "origin",     
  /*  25 */ "partial",    
  /*  26 */ "seq",         /* Used by: database_list */
  /*  27 */ "name",       
  /*  28 */ "file",       
  /*  29 */ "seq",         /* Used by: collation_list */
  /*  30 */ "name",       
  /*  31 */ "id",          /* Used by: foreign_key_list */
  /*  32 */ "seq",        
  /*  33 */ "table",      
  /*  34 */ "from",       
  /*  35 */ "to",         
  /*  36 */ "on_update",  
  /*  37 */ "on_delete",  
  /*  38 */ "match",      
  /*  39 */ "table",       /* Used by: foreign_key_check */
  /*  40 */ "rowid",      
  /*  41 */ "parent",     
  /*  42 */ "fkid",       
  /*  43 */ "busy",        /* Used by: wal_checkpoint */
  /*  44 */ "log",        
  /*  45 */ "checkpointed",
  /*  46 */ "timeout",     /* Used by: busy_timeout */
  /*  47 */ "database",    /* Used by: lock_status */
  /*  48 */ "status",     
};

/* Definitions of all built-in pragmas */
typedef struct PragmaName {
  const char *const zName; /* Name of pragma */
  u8 ePragTyp;             /* PragTyp_XXX value */
  u8 mPragFlg;             /* Zero or more PragFlg_XXX values */
  u8 iPragCName;           /* Start of column names in pragCName[] */
  u8 nPragCName;           /* Num of col names. 0 means use pragma name */
  u32 iArg;                /* Extra argument */
} PragmaName;
static const PragmaName aPragmaName[] = {
#if defined(SQLITE_HAS_CODEC) || defined(SQLITE_ENABLE_CEROD)
 {/* zName:     */ "activate_extensions",
  /* ePragTyp:  */ PragTyp_ACTIVATE_EXTENSIONS,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS)
 {/* zName:     */ "application_id",
  /* ePragTyp:  */ PragTyp_HEADER_VALUE,
  /* ePragFlg:  */ PragFlg_NoColumns1|PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ BTREE_APPLICATION_ID },
#endif
#if !defined(SQLITE_OMIT_AUTOVACUUM)
 {/* zName:     */ "auto_vacuum",
  /* ePragTyp:  */ PragTyp_AUTO_VACUUM,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
#if !defined(SQLITE_OMIT_AUTOMATIC_INDEX)
 {/* zName:     */ "automatic_index",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_AutoIndex },
#endif
#endif
 {/* zName:     */ "busy_timeout",
  /* ePragTyp:  */ PragTyp_BUSY_TIMEOUT,
  /* ePragFlg:  */ PragFlg_Result0,
  /* ColNames:  */ 46, 1,
  /* iArg:      */ 0 },
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS)
 {/* zName:     */ "cache_size",
  /* ePragTyp:  */ PragTyp_CACHE_SIZE,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "cache_spill",
  /* ePragTyp:  */ PragTyp_CACHE_SPILL,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_SchemaReq|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
 {/* zName:     */ "case_sensitive_like",
  /* ePragTyp:  */ PragTyp_CASE_SENSITIVE_LIKE,
  /* ePragFlg:  */ PragFlg_NoColumns,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "cell_size_check",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_CellSizeCk },
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "checkpoint_fullfsync",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_CkptFullFSync },
#endif
#if !defined(SQLITE_OMIT_SCHEMA_PRAGMAS)
 {/* zName:     */ "collation_list",
  /* ePragTyp:  */ PragTyp_COLLATION_LIST,
  /* ePragFlg:  */ PragFlg_Result0,
  /* ColNames:  */ 29, 2,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_COMPILEOPTION_DIAGS)
 {/* zName:     */ "compile_options",
  /* ePragTyp:  */ PragTyp_COMPILE_OPTIONS,
  /* ePragFlg:  */ PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "count_changes",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_CountRows },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS) && SQLITE_OS_WIN
 {/* zName:     */ "data_store_directory",
  /* ePragTyp:  */ PragTyp_DATA_STORE_DIRECTORY,
  /* ePragFlg:  */ PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS)
 {/* zName:     */ "data_version",
  /* ePragTyp:  */ PragTyp_HEADER_VALUE,
  /* ePragFlg:  */ PragFlg_ReadOnly|PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ BTREE_DATA_VERSION },
#endif
#if !defined(SQLITE_OMIT_SCHEMA_PRAGMAS)
 {/* zName:     */ "database_list",
  /* ePragTyp:  */ PragTyp_DATABASE_LIST,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0,
  /* ColNames:  */ 26, 3,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS) && !defined(SQLITE_OMIT_DEPRECATED)
 {/* zName:     */ "default_cache_size",
  /* ePragTyp:  */ PragTyp_DEFAULT_CACHE_SIZE,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 1,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
#if !defined(SQLITE_OMIT_FOREIGN_KEY) && !defined(SQLITE_OMIT_TRIGGER)
 {/* zName:     */ "defer_foreign_keys",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_DeferFKs },
#endif
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "empty_result_callbacks",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_NullCallback },
#endif
#if !defined(SQLITE_OMIT_UTF16)
 {/* zName:     */ "encoding",
  /* ePragTyp:  */ PragTyp_ENCODING,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FOREIGN_KEY) && !defined(SQLITE_OMIT_TRIGGER)
 {/* zName:     */ "foreign_key_check",
  /* ePragTyp:  */ PragTyp_FOREIGN_KEY_CHECK,
  /* ePragFlg:  */ PragFlg_NeedSchema,
  /* ColNames:  */ 39, 4,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FOREIGN_KEY)
 {/* zName:     */ "foreign_key_list",
  /* ePragTyp:  */ PragTyp_FOREIGN_KEY_LIST,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result1|PragFlg_SchemaOpt,
  /* ColNames:  */ 31, 8,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
#if !defined(SQLITE_OMIT_FOREIGN_KEY) && !defined(SQLITE_OMIT_TRIGGER)
 {/* zName:     */ "foreign_keys",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_ForeignKeys },
#endif
#endif
#if !defined(SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS)
 {/* zName:     */ "freelist_count",
  /* ePragTyp:  */ PragTyp_HEADER_VALUE,
  /* ePragFlg:  */ PragFlg_ReadOnly|PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ BTREE_FREE_PAGE_COUNT },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "full_column_names",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_FullColNames },
 {/* zName:     */ "fullfsync",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_FullFSync },
#endif
#if defined(SQLITE_HAS_CODEC)
 {/* zName:     */ "hexkey",
  /* ePragTyp:  */ PragTyp_HEXKEY,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "hexrekey",
  /* ePragTyp:  */ PragTyp_HEXKEY,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
#if !defined(SQLITE_OMIT_CHECK)
 {/* zName:     */ "ignore_check_constraints",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_IgnoreChecks },
#endif
#endif
#if !defined(SQLITE_OMIT_AUTOVACUUM)
 {/* zName:     */ "incremental_vacuum",
  /* ePragTyp:  */ PragTyp_INCREMENTAL_VACUUM,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_NoColumns,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_SCHEMA_PRAGMAS)
 {/* zName:     */ "index_info",
  /* ePragTyp:  */ PragTyp_INDEX_INFO,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result1|PragFlg_SchemaOpt,
  /* ColNames:  */ 12, 3,
  /* iArg:      */ 0 },
 {/* zName:     */ "index_list",
  /* ePragTyp:  */ PragTyp_INDEX_LIST,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result1|PragFlg_SchemaOpt,
  /* ColNames:  */ 21, 5,
  /* iArg:      */ 0 },
 {/* zName:     */ "index_xinfo",
  /* ePragTyp:  */ PragTyp_INDEX_INFO,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result1|PragFlg_SchemaOpt,
  /* ColNames:  */ 15, 6,
  /* iArg:      */ 1 },
#endif
#if !defined(SQLITE_OMIT_INTEGRITY_CHECK)
 {/* zName:     */ "integrity_check",
  /* ePragTyp:  */ PragTyp_INTEGRITY_CHECK,
  /* ePragFlg:  */ PragFlg_NeedSchema,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS)
 {/* zName:     */ "journal_mode",
  /* ePragTyp:  */ PragTyp_JOURNAL_MODE,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "journal_size_limit",
  /* ePragTyp:  */ PragTyp_JOURNAL_SIZE_LIMIT,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_SchemaReq,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if defined(SQLITE_HAS_CODEC)
 {/* zName:     */ "key",
  /* ePragTyp:  */ PragTyp_KEY,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "legacy_file_format",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_LegacyFileFmt },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS) && SQLITE_ENABLE_LOCKING_STYLE
 {/* zName:     */ "lock_proxy_file",
  /* ePragTyp:  */ PragTyp_LOCK_PROXY_FILE,
  /* ePragFlg:  */ PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if defined(SQLITE_DEBUG) || defined(SQLITE_TEST)
 {/* zName:     */ "lock_status",
  /* ePragTyp:  */ PragTyp_LOCK_STATUS,
  /* ePragFlg:  */ PragFlg_Result0,
  /* ColNames:  */ 47, 2,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS)
 {/* zName:     */ "locking_mode",
  /* ePragTyp:  */ PragTyp_LOCKING_MODE,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_SchemaReq,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "max_page_count",
  /* ePragTyp:  */ PragTyp_PAGE_COUNT,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "mmap_size",
  /* ePragTyp:  */ PragTyp_MMAP_SIZE,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "page_count",
  /* ePragTyp:  */ PragTyp_PAGE_COUNT,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "page_size",
  /* ePragTyp:  */ PragTyp_PAGE_SIZE,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_SchemaReq|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if defined(SQLITE_DEBUG) && !defined(SQLITE_OMIT_PARSER_TRACE)
 {/* zName:     */ "parser_trace",
  /* ePragTyp:  */ PragTyp_PARSER_TRACE,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "query_only",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_QueryOnly },
#endif
#if !defined(SQLITE_OMIT_INTEGRITY_CHECK)
 {/* zName:     */ "quick_check",
  /* ePragTyp:  */ PragTyp_INTEGRITY_CHECK,
  /* ePragFlg:  */ PragFlg_NeedSchema,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "read_uncommitted",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_ReadUncommitted },
 {/* zName:     */ "recursive_triggers",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_RecTriggers },
#endif
#if defined(SQLITE_HAS_CODEC)
 {/* zName:     */ "rekey",
  /* ePragTyp:  */ PragTyp_REKEY,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "reverse_unordered_selects",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_ReverseOrder },
#endif
#if !defined(SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS)
 {/* zName:     */ "schema_version",
  /* ePragTyp:  */ PragTyp_HEADER_VALUE,
  /* ePragFlg:  */ PragFlg_NoColumns1|PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ BTREE_SCHEMA_VERSION },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS)
 {/* zName:     */ "secure_delete",
  /* ePragTyp:  */ PragTyp_SECURE_DELETE,
  /* ePragFlg:  */ PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "short_column_names",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_ShortColNames },
#endif
 {/* zName:     */ "shrink_memory",
  /* ePragTyp:  */ PragTyp_SHRINK_MEMORY,
  /* ePragFlg:  */ PragFlg_NoColumns,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "soft_heap_limit",
  /* ePragTyp:  */ PragTyp_SOFT_HEAP_LIMIT,
  /* ePragFlg:  */ PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
#if defined(SQLITE_DEBUG)
 {/* zName:     */ "sql_trace",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_SqlTrace },
#endif
#endif
#if !defined(SQLITE_OMIT_SCHEMA_PRAGMAS) && defined(SQLITE_DEBUG)
 {/* zName:     */ "stats",
  /* ePragTyp:  */ PragTyp_STATS,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq,
  /* ColNames:  */ 7, 5,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS)
 {/* zName:     */ "synchronous",
  /* ePragTyp:  */ PragTyp_SYNCHRONOUS,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result0|PragFlg_SchemaReq|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_SCHEMA_PRAGMAS)
 {/* zName:     */ "table_info",
  /* ePragTyp:  */ PragTyp_TABLE_INFO,
  /* ePragFlg:  */ PragFlg_NeedSchema|PragFlg_Result1|PragFlg_SchemaOpt,
  /* ColNames:  */ 1, 6,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_PAGER_PRAGMAS)
 {/* zName:     */ "temp_store",
  /* ePragTyp:  */ PragTyp_TEMP_STORE,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "temp_store_directory",
  /* ePragTyp:  */ PragTyp_TEMP_STORE_DIRECTORY,
  /* ePragFlg:  */ PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#endif
 {/* zName:     */ "threads",
  /* ePragTyp:  */ PragTyp_THREADS,
  /* ePragFlg:  */ PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
#if !defined(SQLITE_OMIT_SCHEMA_VERSION_PRAGMAS)
 {/* zName:     */ "user_version",
  /* ePragTyp:  */ PragTyp_HEADER_VALUE,
  /* ePragFlg:  */ PragFlg_NoColumns1|PragFlg_Result0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ BTREE_USER_VERSION },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
#if defined(SQLITE_DEBUG)
 {/* zName:     */ "vdbe_addoptrace",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_VdbeAddopTrace },
 {/* zName:     */ "vdbe_debug",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_SqlTrace|SQLITE_VdbeListing|SQLITE_VdbeTrace },
 {/* zName:     */ "vdbe_eqp",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_VdbeEQP },
 {/* zName:     */ "vdbe_listing",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_VdbeListing },
 {/* zName:     */ "vdbe_trace",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_VdbeTrace },
#endif
#endif
#if !defined(SQLITE_OMIT_WAL)
 {/* zName:     */ "wal_autocheckpoint",
  /* ePragTyp:  */ PragTyp_WAL_AUTOCHECKPOINT,
  /* ePragFlg:  */ 0,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ 0 },
 {/* zName:     */ "wal_checkpoint",
  /* ePragTyp:  */ PragTyp_WAL_CHECKPOINT,
  /* ePragFlg:  */ PragFlg_NeedSchema,
  /* ColNames:  */ 43, 3,
  /* iArg:      */ 0 },
#endif
#if !defined(SQLITE_OMIT_FLAG_PRAGMAS)
 {/* zName:     */ "writable_schema",
  /* ePragTyp:  */ PragTyp_FLAG,
  /* ePragFlg:  */ PragFlg_Result0|PragFlg_NoColumns1,
  /* ColNames:  */ 0, 0,
  /* iArg:      */ SQLITE_WriteSchema|SQLITE_RecoveryMode },
#endif
};
/* Number of pragmas: 59 on by default, 73 total. */
