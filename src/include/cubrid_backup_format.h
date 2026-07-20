#ifndef _CUBRID_BACKUP_FORMAT_H_
#define _CUBRID_BACKUP_FORMAT_H_

/*
 * Vendored snapshot of the CUBRID server backup-stream on-wire format, used ONLY
 * by the observational log-phase parser (see backup_core.c). Pinned to CUBRID
 * v11.3 (format stable through 11.5); each constant is annotated with its server
 * origin. These are SERVER INTERNALS, NOT an API contract: if the server backup
 * format changes, this file MUST be re-verified. The _Static_assert block turns
 * layout drift on the build host into a build failure; a mismatch that slips the
 * build still degrades safely at runtime (self-check -> parser disabled -> fallback).
 *
 * Layout is for x86-64 LP64 with PATH_MAX==4096.
 */

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

/* ── server scalar typedefs (storage_common.h) ── */
typedef int32_t CUB_INT32;
typedef int64_t CUB_INT64;
typedef int16_t CUB_INT16;
typedef CUB_INT32 CUB_PAGEID;    /* storage_common.h:73  INT32 */
typedef CUB_INT16 CUB_VOLID;     /* storage_common.h:70  INT16 */
typedef CUB_INT16 CUB_PGLENGTH;  /* storage_common.h:84  INT16 */

/* ── constants (file_io.{c,h}, storage_common.h, release_string.h) ── */
#define CUB_MAGIC_MAX_LENGTH   25            /* storage_common.h:402 */
#define CUB_REL_MAX_RELEASE    15            /* release_string.h:31  */
#define CUB_BK_MAGIC           "CUBRID/Backup_v2"   /* storage_common.h:408 (encodes v2) */
#define CUB_BK_HDR_VERSION     2             /* file_io.c:210 FILEIO_BACKUP_CURRENT_HEADER_VERSION */
#define CUB_BK_ZIP_LZ4         3             /* file_io.h:109 FILEIO_ZIP_LZ4_METHOD ordinal */
#define CUB_BK_ZIP_NONE        0             /* file_io.h:107 FILEIO_ZIP_NONE_METHOD (uncompressed: fixed-stride walk) */
#define CUB_BK_UNDEF_LEVEL     3             /* file_io.h:101 (previnfo[] size) */

/* on-wire page-id sentinels (file_io.c:273-277) */
#define CUB_BK_START_PAGE_ID       (-2)
#define CUB_BK_END_PAGE_ID         (-3)
#define CUB_BK_FILE_START_PAGE_ID  (-4)
#define CUB_BK_FILE_END_PAGE_ID    (-5)   /* LZ4: compressed, never a raw tag. NONE: raw tag on a bkpagesize+OVERHEAD unit. */
#define CUB_BK_VOL_CONT_PAGE_ID    (-6)

/* volids of streamed FILE_STARTs (log_volids.hpp) */
#define CUB_LOG_DBFIRST_VOLID   0      /* :38  first data volume                 */
#define CUB_LOG_ARCHIVE_VOLID   (-20)  /* :53  archive log = negative-volid floor */
#define CUB_LOG_DWB_VOLID       (-22)  /* :57  robustness floor for volid_valid   */
#define CUB_LOG_MAX_DBVOLID     32766  /* :34  VOLID_MAX-1                         */

/* max plausible bkpagesize = db_iopagesize(<=16K) * FILEIO_FULL_LEVEL_EXP(32)
 * (file_io.c:197,7225). Upper bound for the compressed buf_len self-check. */
#define CUB_BK_MAX_BKPAGESIZE   524288

/* PINNED on-wire sizes (probe-verified, x86-64 LP64, PATH_MAX 4096) */
#define CUB_BK_TAG_SIZE         4       /* sizeof(PAGEID) tag lead                 */
#define CUB_BK_HEADER_STRUCT    12464   /* sizeof(FILEIO_BACKUP_HEADER)            */
#define CUB_BK_HEADER_IO_SIZE   13312   /* GET_NEXT_1K_SIZE(struct): on-wire block */
#define CUB_BK_FILE_UNIT        4120    /* FILEIO_BACKUP_FILE_HEADER_PAGE_SIZE     */
#define CUB_BK_PAGE_OVERHEAD    12      /* FILEIO_BACKUP_PAGE_OVERHEAD (file_io.c:214) */
#define CUB_BK_PAGE_IOPAGE_OFF  8       /* offsetof(FILEIO_BACKUP_PAGE, iopage)    */
#define CUB_BK_WIRE_NBYTES_OFF  8       /* page prefix(8) + file_header nbytes(0)  */
#define CUB_BK_WIRE_VOLID_OFF   16      /* page prefix(8) + file_header volid(8)   */
#define CUB_BK_FS_PEEK          18      /* bytes to inspect: tag..volid(16..17)    */

/* ── vendored struct: global backup header (file_io.h:280-313) ──
 * Copied verbatim for the gate cast; _Static_assert pins its layout. */
typedef struct { CUB_INT64 pageid:48; CUB_INT64 offset:16; } CUB_LOG_LSA;          /* 8B */
typedef struct { CUB_INT64 at_time; CUB_LOG_LSA lsa; } CUB_BK_RECORD_INFO;         /* 16B */

typedef struct cub_bkup_header
{
  CUB_PAGEID iopageid;
  char       magic[CUB_MAGIC_MAX_LENGTH];
  float      db_compatibility;
  int        bk_hdr_version;
  CUB_INT64  db_creation;
  CUB_INT64  start_time;
  CUB_INT64  end_time;
  char       db_release[CUB_REL_MAX_RELEASE];
  char       db_fullname[PATH_MAX];
  CUB_PGLENGTH db_iopagesize;
  int        level;                  /* FILEIO_BACKUP_LEVEL enum */
  CUB_LOG_LSA start_lsa;
  CUB_LOG_LSA chkpt_lsa;
  int        unit_num;
  int        bkup_iosize;
  CUB_BK_RECORD_INFO previnfo[CUB_BK_UNDEF_LEVEL];
  char       db_prec_bkvolname[PATH_MAX];
  char       db_next_bkvolname[PATH_MAX];
  int        bkpagesize;
  int        zip_method;             /* FILEIO_ZIP_METHOD enum */
  int        zip_level;              /* FILEIO_ZIP_LEVEL enum  */
  int        skip_activelog;
} CUB_BKUP_HEADER;

/* file (FILE_START) header (file_io.c:351-358) — read via wire offsets, not cast */
typedef struct cub_bkup_file_header
{
  CUB_INT64 nbytes;
  CUB_VOLID volid;
  short     dummy1;
  int       dummy2;
  char      vlabel[PATH_MAX];
} CUB_BKUP_FILE_HEADER;

/* ── compile-time layout pins (drift => build failure) ── */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert (PATH_MAX == 4096, "vendored layout pinned to PATH_MAX==4096; re-verify");
_Static_assert (sizeof (CUB_BKUP_HEADER) == CUB_BK_HEADER_STRUCT, "backup header size drift");
_Static_assert (offsetof (CUB_BKUP_HEADER, iopageid) == 0, "iopageid offset drift");
_Static_assert (offsetof (CUB_BKUP_HEADER, magic) == 4, "magic offset drift");
_Static_assert (offsetof (CUB_BKUP_HEADER, bk_hdr_version) == 36, "bk_hdr_version offset drift");
_Static_assert (offsetof (CUB_BKUP_HEADER, bkpagesize) == 12448, "bkpagesize offset drift");
_Static_assert (offsetof (CUB_BKUP_HEADER, zip_method) == 12452, "zip_method offset drift");
_Static_assert (offsetof (CUB_BKUP_FILE_HEADER, nbytes) == 0, "fh nbytes offset drift");
_Static_assert (offsetof (CUB_BKUP_FILE_HEADER, volid) == 8, "fh volid offset drift");
_Static_assert (sizeof (CUB_BKUP_FILE_HEADER) + CUB_BK_PAGE_IOPAGE_OFF == CUB_BK_FILE_UNIT,
                "FILE_START unit size drift");
_Static_assert (CUB_BK_WIRE_NBYTES_OFF == CUB_BK_PAGE_IOPAGE_OFF + 0, "wire nbytes off");
_Static_assert (CUB_BK_WIRE_VOLID_OFF == CUB_BK_PAGE_IOPAGE_OFF + 8, "wire volid off");
#endif

/* ── parser state (drain-thread-private; embedded in the handle) ── */
typedef enum
{
  PS_GATE,       /* accumulating the global header, then gate                */
  PS_SKIP,       /* discarding N bytes (compressed payload / unit tail / pad) */
  PS_TAG,        /* accumulating the 4-byte frame tag                         */
  PS_FS_HDR,     /* accumulating a FILE_START header up to volid              */
  PS_DONE,       /* log phase latched: the tail is all log, stop parsing      */
  PS_DISABLED    /* gate/self-check failed: fallback policy in force          */
} CUB_PSTATE;

typedef struct bk_parser
{
  CUB_PSTATE state;
  size_t     need_bytes;      /* bytes still to accumulate for the current field */
  size_t     got_bytes;       /* bytes accumulated into header_accum[]/global_header so far */
  long long  skip_bytes;      /* bytes still to discard                          */
  int        saw_data_volume; /* a FILE_START with volid>=0 has passed           */
  int        backup_page_size;/* from the gated header; buf_len self-check bound */
  int        compressed;      /* gated zip_method: 1=LZ4 (self-delimiting), 0=NONE (fixed stride) */
  char      *global_header;   /* malloc(CUB_BK_HEADER_STRUCT); accumulates header */
  unsigned char header_accum[24]; /* >= CUB_BK_FS_PEEK(18)                       */
} BK_PARSER;

#endif /* _CUBRID_BACKUP_FORMAT_H_ */
