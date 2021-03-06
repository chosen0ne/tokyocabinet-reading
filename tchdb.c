/*************************************************************************************************
 * The hash database API of Tokyo Cabinet
 *                                                               Copyright (C) 2006-2012 FAL Labs
 * This file is part of Tokyo Cabinet.
 * Tokyo Cabinet is free software; you can redistribute it and/or modify it under the terms of
 * the GNU Lesser General Public License as published by the Free Software Foundation; either
 * version 2.1 of the License or any later version.  Tokyo Cabinet is distributed in the hope
 * that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 * You should have received a copy of the GNU Lesser General Public License along with Tokyo
 * Cabinet; if not, write to the Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307 USA.
 *************************************************************************************************/


#include "tcutil.h"
#include "tchdb.h"
#include "tcbdb.h"
#include "myconf.h"

#define HDBFILEMODE    00644             // permission of created files
#define HDBIOBUFSIZ    8192              // size of an I/O buffer

#define HDBMAGICDATA   "ToKyO CaBiNeT"   // magic data for identification
#define HDBHEADSIZ     256               // size of the reagion of the header
#define HDBTYPEOFF     32                // offset of the region for the database type
#define HDBFLAGSOFF    33                // offset of the region for the additional flags
#define HDBAPOWOFF     34                // offset of the region for the alignment power
#define HDBFPOWOFF     35                // offset of the region for the free block pool power
#define HDBOPTSOFF     36                // offset of the region for the options
#define HDBBNUMOFF     40                // offset of the region for the bucket number
#define HDBRNUMOFF     48                // offset of the region for the record number
#define HDBFSIZOFF     56                // offset of the region for the file size
#define HDBFRECOFF     64                // offset of the region for the first record offset
#define HDBOPAQUEOFF   128               // offset of the region for the opaque field

#define HDBDEFBNUM     131071            // default bucket number
#define HDBDEFAPOW     4                 // default alignment power
#define HDBMAXAPOW     16                // maximum alignment power
#define HDBDEFFPOW     10                // default free block pool power
#define HDBMAXFPOW     20                // maximum free block pool power
#define HDBDEFXMSIZ    (64LL<<20)        // default size of the extra mapped memory
#define HDBXFSIZINC    32768             // increment of extra file size
#define HDBMINRUNIT    48                // minimum record reading unit
#define HDBMAXHSIZ     32                // maximum record header size
#define HDBFBPALWRAT   2                 // allowance ratio of the free block pool
#define HDBFBPBSIZ     64                // base region size of the free block pool
#define HDBFBPESIZ     4                 // size of each region of the free block pool
#define HDBFBPMGFREQ   4096              // frequency to merge the free block pool
#define HDBDRPUNIT     65536             // unit size of the delayed record pool
#define HDBDRPLAT      2048              // latitude size of the delayed record pool
#define HDBDFRSRAT     2                 // step ratio of auto defragmentation
#define HDBFBMAXSIZ    (INT32_MAX/4)     // maximum size of a free block pool
#define HDBCACHEOUT    128               // number of records in a process of cacheout
#define HDBWALSUFFIX   "wal"             // suffix of write ahead logging file

typedef struct {                         // type of structure for a record
  uint64_t off;                          // offset of the record
  uint32_t rsiz;                         // size of the whole record
  uint8_t magic;                         // magic number
  uint8_t hash;                          // second hash value
  uint64_t left;                         // offset of the left child record
  uint64_t right;                        // offset of the right child record
  uint32_t ksiz;                         // size of the key
  uint32_t vsiz;                         // size of the value
  // <MM>
  // 当前记录所在文件块中剩余空闲的部分，有两种情况
  //    - 用于对齐，应该小于4字节
  //    - 当剩余空闲不大于某值时，比如记录大小或者65536
  // </MM>
  uint16_t psiz;                         // size of the padding
  const char *kbuf;                      // pointer to the key
  const char *vbuf;                      // pointer to the value
  uint64_t boff;                         // offset of the body
  char *bbuf;                            // buffer of the body
} TCHREC;

typedef struct {                         // type of structure for a free block
  uint64_t off;                          // offset of the block
  uint32_t rsiz;                         // size of the block
} HDBFB;

enum {                                   // enumeration for magic data
  HDBMAGICREC = 0xc8,                    // for data block
  HDBMAGICFB = 0xb0                      // for free block
};

enum {                                   // enumeration for duplication behavior
  HDBPDOVER,                             // overwrite an existing value
  HDBPDKEEP,                             // keep the existing value
  HDBPDCAT,                              // concatenate values
  HDBPDADDINT,                           // add an integer
  HDBPDADDDBL,                           // add a real number
  HDBPDPROC                              // process by a callback function
};

typedef struct {                         // type of structure for a duplication callback
  TCPDPROC proc;                         // function pointer
  void *op;                              // opaque pointer
} HDBPDPROCOP;


/* private macros */
#define HDBLOCKMETHOD(TC_hdb, TC_wr)                            \
  ((TC_hdb)->mmtx ? tchdblockmethod((TC_hdb), (TC_wr)) : true)
#define HDBUNLOCKMETHOD(TC_hdb)                         \
  ((TC_hdb)->mmtx ? tchdbunlockmethod(TC_hdb) : true)
#define HDBLOCKRECORD(TC_hdb, TC_bidx, TC_wr)                           \
  ((TC_hdb)->mmtx ? tchdblockrecord((TC_hdb), (uint8_t)(TC_bidx), (TC_wr)) : true)
#define HDBUNLOCKRECORD(TC_hdb, TC_bidx)                                \
  ((TC_hdb)->mmtx ? tchdbunlockrecord((TC_hdb), (uint8_t)(TC_bidx)) : true)
#define HDBLOCKALLRECORDS(TC_hdb, TC_wr)                                \
  ((TC_hdb)->mmtx ? tchdblockallrecords((TC_hdb), (TC_wr)) : true)
#define HDBUNLOCKALLRECORDS(TC_hdb)                             \
  ((TC_hdb)->mmtx ? tchdbunlockallrecords(TC_hdb) : true)
#define HDBLOCKDB(TC_hdb)                       \
  ((TC_hdb)->mmtx ? tchdblockdb(TC_hdb) : true)
#define HDBUNLOCKDB(TC_hdb)                             \
  ((TC_hdb)->mmtx ? tchdbunlockdb(TC_hdb) : true)
#define HDBLOCKWAL(TC_hdb)                              \
  ((TC_hdb)->mmtx ? tchdblockwal(TC_hdb) : true)
#define HDBUNLOCKWAL(TC_hdb)                            \
  ((TC_hdb)->mmtx ? tchdbunlockwal(TC_hdb) : true)
#define HDBTHREADYIELD(TC_hdb)                          \
  do { if((TC_hdb)->mmtx) sched_yield(); } while(false)


/* private function prototypes */
static uint64_t tcgetprime(uint64_t num);
static bool tchdbseekwrite(TCHDB *hdb, off_t off, const void *buf, size_t size);
static bool tchdbseekread(TCHDB *hdb, off_t off, void *buf, size_t size);
static bool tchdbseekreadtry(TCHDB *hdb, off_t off, void *buf, size_t size);
static void tchdbdumpmeta(TCHDB *hdb, char *hbuf);
static void tchdbloadmeta(TCHDB *hdb, const char *hbuf);
static void tchdbclear(TCHDB *hdb);
static int32_t tchdbpadsize(TCHDB *hdb, uint64_t off);
static void tchdbsetflag(TCHDB *hdb, int flag, bool sign);
static uint64_t tchdbbidx(TCHDB *hdb, const char *kbuf, int ksiz, uint8_t *hp);
static off_t tchdbgetbucket(TCHDB *hdb, uint64_t bidx);
static void tchdbsetbucket(TCHDB *hdb, uint64_t bidx, uint64_t off);
static bool tchdbsavefbp(TCHDB *hdb);
static bool tchdbloadfbp(TCHDB *hdb);
static void tcfbpsortbyoff(HDBFB *fbpool, int fbpnum);
static void tcfbpsortbyrsiz(HDBFB *fbpool, int fbpnum);
static void tchdbfbpmerge(TCHDB *hdb);
static void tchdbfbpinsert(TCHDB *hdb, uint64_t off, uint32_t rsiz);
static bool tchdbfbpsearch(TCHDB *hdb, TCHREC *rec);
static bool tchdbfbpsplice(TCHDB *hdb, TCHREC *rec, uint32_t nsiz);
static void tchdbfbptrim(TCHDB *hdb, uint64_t base, uint64_t next, uint64_t off, uint32_t rsiz);
static bool tchdbwritefb(TCHDB *hdb, uint64_t off, uint32_t rsiz);
static bool tchdbwriterec(TCHDB *hdb, TCHREC *rec, uint64_t bidx, off_t entoff);
static bool tchdbreadrec(TCHDB *hdb, TCHREC *rec, char *rbuf);
static bool tchdbreadrecbody(TCHDB *hdb, TCHREC *rec);
static bool tchdbremoverec(TCHDB *hdb, TCHREC *rec, char *rbuf, uint64_t bidx, off_t entoff);
static bool tchdbshiftrec(TCHDB *hdb, TCHREC *rec, char *rbuf, off_t destoff);
static int tcreckeycmp(const char *abuf, int asiz, const char *bbuf, int bsiz);
static bool tchdbflushdrp(TCHDB *hdb);
static void tchdbcacheadjust(TCHDB *hdb);
static bool tchdbwalinit(TCHDB *hdb);
static bool tchdbwalwrite(TCHDB *hdb, uint64_t off, int64_t size);
static int tchdbwalrestore(TCHDB *hdb, const char *path);
static bool tchdbwalremove(TCHDB *hdb, const char *path);
static bool tchdbopenimpl(TCHDB *hdb, const char *path, int omode);
static bool tchdbcloseimpl(TCHDB *hdb);
static bool tchdbputimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash,
                         const char *vbuf, int vsiz, int dmode);
static void tchdbdrpappend(TCHDB *hdb, const char *kbuf, int ksiz, const char *vbuf, int vsiz,
                           uint8_t hash);
static bool tchdbputasyncimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx,
                              uint8_t hash, const char *vbuf, int vsiz);
static bool tchdboutimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash);
static char *tchdbgetimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash,
                          int *sp);
static int tchdbgetintobuf(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash,
                           char *vbuf, int max);
static char *tchdbgetnextimpl(TCHDB *hdb, const char *kbuf, int ksiz, int *sp,
                              const char **vbp, int *vsp);
static int tchdbvsizimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash);
static bool tchdbiterinitimpl(TCHDB *hdb);
static char *tchdbiternextimpl(TCHDB *hdb, int *sp);
static bool tchdbiternextintoxstr(TCHDB *hdb, TCXSTR *kxstr, TCXSTR *vxstr);
static bool tchdboptimizeimpl(TCHDB *hdb, int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts);
static bool tchdbvanishimpl(TCHDB *hdb);
static bool tchdbcopyimpl(TCHDB *hdb, const char *path);
static bool tchdbdefragimpl(TCHDB *hdb, int64_t step);
static bool tchdbiterjumpimpl(TCHDB *hdb, const char *kbuf, int ksiz);
static bool tchdbforeachimpl(TCHDB *hdb, TCITER iter, void *op);
static bool tchdblockmethod(TCHDB *hdb, bool wr);
static bool tchdbunlockmethod(TCHDB *hdb);
static bool tchdblockrecord(TCHDB *hdb, uint8_t bidx, bool wr);
static bool tchdbunlockrecord(TCHDB *hdb, uint8_t bidx);
static bool tchdblockallrecords(TCHDB *hdb, bool wr);
static bool tchdbunlockallrecords(TCHDB *hdb);
static bool tchdblockdb(TCHDB *hdb);
static bool tchdbunlockdb(TCHDB *hdb);
static bool tchdblockwal(TCHDB *hdb);
static bool tchdbunlockwal(TCHDB *hdb);


/* debugging function prototypes */
void tchdbprintmeta(TCHDB *hdb);
void tchdbprintrec(TCHDB *hdb, TCHREC *rec);



/*************************************************************************************************
 * API
 *************************************************************************************************/


/* Get the message string corresponding to an error code. */
const char *tchdberrmsg(int ecode){
  return tcerrmsg(ecode);
}


/* Create a hash database object. */
TCHDB *tchdbnew(void){
  TCHDB *hdb;
  TCMALLOC(hdb, sizeof(*hdb));
  tchdbclear(hdb);
  return hdb;
}


/* Delete a hash database object. */
void tchdbdel(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd >= 0) tchdbclose(hdb);
  if(hdb->mmtx){
    pthread_key_delete(*(pthread_key_t *)hdb->eckey);
    pthread_mutex_destroy(hdb->wmtx);
    pthread_mutex_destroy(hdb->dmtx);
    for(int i = UINT8_MAX; i >= 0; i--){
      pthread_rwlock_destroy((pthread_rwlock_t *)hdb->rmtxs + i);
    }
    pthread_rwlock_destroy(hdb->mmtx);
    TCFREE(hdb->eckey);
    TCFREE(hdb->wmtx);
    TCFREE(hdb->dmtx);
    TCFREE(hdb->rmtxs);
    TCFREE(hdb->mmtx);
  }
  TCFREE(hdb);
}


/* Get the last happened error code of a hash database object. */
int tchdbecode(TCHDB *hdb){
  assert(hdb);
  return hdb->mmtx ?
    (int)(intptr_t)pthread_getspecific(*(pthread_key_t *)hdb->eckey) : hdb->ecode;
}


/* Set mutual exclusion control of a hash database object for threading. */
bool tchdbsetmutex(TCHDB *hdb){
  assert(hdb);
  if(!TCUSEPTHREAD) return true;
  if(hdb->mmtx || hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  pthread_mutexattr_t rma;
  pthread_mutexattr_init(&rma);
  TCMALLOC(hdb->mmtx, sizeof(pthread_rwlock_t));
  TCMALLOC(hdb->rmtxs, (UINT8_MAX + 1) * sizeof(pthread_rwlock_t));
  TCMALLOC(hdb->dmtx, sizeof(pthread_mutex_t));
  TCMALLOC(hdb->wmtx, sizeof(pthread_mutex_t));
  TCMALLOC(hdb->eckey, sizeof(pthread_key_t));
  bool err = false;
  if(pthread_mutexattr_settype(&rma, PTHREAD_MUTEX_RECURSIVE) != 0) err = true;
  if(pthread_rwlock_init(hdb->mmtx, NULL) != 0) err = true;
  for(int i = 0; i <= UINT8_MAX; i++){
    if(pthread_rwlock_init((pthread_rwlock_t *)hdb->rmtxs + i, NULL) != 0) err = true;
  }
  if(pthread_mutex_init(hdb->dmtx, &rma) != 0) err = true;
  if(pthread_mutex_init(hdb->wmtx, NULL) != 0) err = true;
  if(pthread_key_create(hdb->eckey, NULL) != 0) err = true;
  if(err){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    pthread_mutexattr_destroy(&rma);
    TCFREE(hdb->eckey);
    TCFREE(hdb->wmtx);
    TCFREE(hdb->dmtx);
    TCFREE(hdb->rmtxs);
    TCFREE(hdb->mmtx);
    hdb->eckey = NULL;
    hdb->wmtx = NULL;
    hdb->dmtx = NULL;
    hdb->rmtxs = NULL;
    hdb->mmtx = NULL;
    return false;
  }
  pthread_mutexattr_destroy(&rma);
  return true;
}


/* Set the tuning parameters of a hash database object. */
bool tchdbtune(TCHDB *hdb, int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts){
  assert(hdb);
  if(hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  hdb->bnum = (bnum > 0) ? tcgetprime(bnum) : HDBDEFBNUM;
  hdb->apow = (apow >= 0) ? tclmin(apow, HDBMAXAPOW) : HDBDEFAPOW;
  hdb->fpow = (fpow >= 0) ? tclmin(fpow, HDBMAXFPOW) : HDBDEFFPOW;
  hdb->opts = opts;
  if(!_tc_deflate) hdb->opts &= ~HDBTDEFLATE;
  if(!_tc_bzcompress) hdb->opts &= ~HDBTBZIP;
  return true;
}


/* Set the caching parameters of a hash database object. */
bool tchdbsetcache(TCHDB *hdb, int32_t rcnum){
  assert(hdb);
  if(hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  hdb->rcnum = (rcnum > 0) ? tclmin(tclmax(rcnum, HDBCACHEOUT * 2), INT_MAX / 4) : 0;
  return true;
}


/* Set the size of the extra mapped memory of a hash database object. */
bool tchdbsetxmsiz(TCHDB *hdb, int64_t xmsiz){
  assert(hdb);
  if(hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  hdb->xmsiz = (xmsiz > 0) ? tcpagealign(xmsiz) : 0;
  return true;
}


/* Set the unit step number of auto defragmentation of a hash database object. */
bool tchdbsetdfunit(TCHDB *hdb, int32_t dfunit){
  assert(hdb);
  if(hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  hdb->dfunit = (dfunit > 0) ? dfunit : 0;
  return true;
}


/* Open a database file and connect a hash database object. */
bool tchdbopen(TCHDB *hdb, const char *path, int omode){
  assert(hdb && path);
  // <MM>
  // 方法级加锁，写锁
  // </MM>
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  // <MM>
  // 要open的hdb对应的fd应是0，否则hdb已经被打开
  // </MM>
  if(hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  // <MM>
  // 拼接成全路径
  // </MM>
  char *rpath = tcrealpath(path);
  if(!rpath){
    int ecode = TCEOPEN;
    switch(errno){
      case EACCES: ecode = TCENOPERM; break;
      case ENOENT: ecode = TCENOFILE; break;
      case ENOTDIR: ecode = TCENOFILE; break;
    }
    tchdbsetecode(hdb, ecode, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  // <MM>
  // 将数据文件全路径放入map中，判断是否重复，用来解决什么问题？
  //
  // 避免多线程打开hdb。打开hdb的进程会持有一个map，用于存放打开的hdb路径，如果rpath在这个map中
  // 认为已经有线程打开这个hdb，返回错误
  //
  // 为什么不采用文件锁的方式，从而避免多进程同时打开？
  //
  // </MM>
  if(!tcpathlock(rpath)){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    TCFREE(rpath);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool rv = tchdbopenimpl(hdb, path, omode);
  if(rv){
    hdb->rpath = rpath;
  } else {
    tcpathunlock(rpath);
    TCFREE(rpath);
  }
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Close a database object. */
bool tchdbclose(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool rv = tchdbcloseimpl(hdb);
  tcpathunlock(hdb->rpath);
  TCFREE(hdb->rpath);
  hdb->rpath = NULL;
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Store a record into a hash database object. */
bool tchdbput(TCHDB *hdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  // <MM>
  // 加读锁
  // 大部分情况下是加读锁，比如put、get
  // 在整个DB open，close以及iterate时加的是写锁
  // </MM>
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  uint8_t hash;
  // <MM>
  // 取hash及bucket下标
  // </MM>
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  // <MM>
  // 这个判断可以提前
  // </MM>
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  // <MM>
  // 多个bucket共享一把锁。默认情况下，共有131071个bucket，256个mutex。
  // 通过bucket index的低8bit进行hash，取得对应的锁下标
  // 此处加写锁
  // </MM>
  if(!HDBLOCKRECORD(hdb, bidx, true)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->zmode){
    char *zbuf;
    if(hdb->opts & HDBTDEFLATE){
      zbuf = _tc_deflate(vbuf, vsiz, &vsiz, _TCZMRAW);
    } else if(hdb->opts & HDBTBZIP){
      zbuf = _tc_bzcompress(vbuf, vsiz, &vsiz);
    } else if(hdb->opts & HDBTTCBS){
      zbuf = tcbsencode(vbuf, vsiz, &vsiz);
    } else {
      zbuf = hdb->enc(vbuf, vsiz, &vsiz, hdb->encop);
    }
    if(!zbuf){
      tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
      HDBUNLOCKRECORD(hdb, bidx);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, zbuf, vsiz, HDBPDOVER);
    TCFREE(zbuf);
    HDBUNLOCKRECORD(hdb, bidx);
    HDBUNLOCKMETHOD(hdb);
    if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
       !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
    return rv;
  }
  bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, vbuf, vsiz, HDBPDOVER);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
     !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
  return rv;
}


/* Store a string record into a hash database object. */
bool tchdbput2(TCHDB *hdb, const char *kstr, const char *vstr){
  assert(hdb && kstr && vstr);
  return tchdbput(hdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Store a new record into a hash database object. */
bool tchdbputkeep(TCHDB *hdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(!HDBLOCKRECORD(hdb, bidx, true)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->zmode){
    char *zbuf;
    if(hdb->opts & HDBTDEFLATE){
      zbuf = _tc_deflate(vbuf, vsiz, &vsiz, _TCZMRAW);
    } else if(hdb->opts & HDBTBZIP){
      zbuf = _tc_bzcompress(vbuf, vsiz, &vsiz);
    } else if(hdb->opts & HDBTTCBS){
      zbuf = tcbsencode(vbuf, vsiz, &vsiz);
    } else {
      zbuf = hdb->enc(vbuf, vsiz, &vsiz, hdb->encop);
    }
    if(!zbuf){
      tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
      HDBUNLOCKRECORD(hdb, bidx);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, zbuf, vsiz, HDBPDKEEP);
    TCFREE(zbuf);
    HDBUNLOCKRECORD(hdb, bidx);
    HDBUNLOCKMETHOD(hdb);
    if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
       !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
    return rv;
  }
  bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, vbuf, vsiz, HDBPDKEEP);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
     !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
  return rv;
}


/* Store a new string record into a hash database object. */
bool tchdbputkeep2(TCHDB *hdb, const char *kstr, const char *vstr){
  return tchdbputkeep(hdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Concatenate a value at the end of the existing record in a hash database object. */
bool tchdbputcat(TCHDB *hdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(!HDBLOCKRECORD(hdb, bidx, true)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->zmode){
    char *zbuf;
    int osiz;
    char *obuf = tchdbgetimpl(hdb, kbuf, ksiz, bidx, hash, &osiz);
    if(obuf){
      TCREALLOC(obuf, obuf, osiz + vsiz + 1);
      memcpy(obuf + osiz, vbuf, vsiz);
      if(hdb->opts & HDBTDEFLATE){
        zbuf = _tc_deflate(obuf, osiz + vsiz, &vsiz, _TCZMRAW);
      } else if(hdb->opts & HDBTBZIP){
        zbuf = _tc_bzcompress(obuf, osiz + vsiz, &vsiz);
      } else if(hdb->opts & HDBTTCBS){
        zbuf = tcbsencode(obuf, osiz + vsiz, &vsiz);
      } else {
        zbuf = hdb->enc(obuf, osiz + vsiz, &vsiz, hdb->encop);
      }
      TCFREE(obuf);
    } else {
      if(hdb->opts & HDBTDEFLATE){
        zbuf = _tc_deflate(vbuf, vsiz, &vsiz, _TCZMRAW);
      } else if(hdb->opts & HDBTBZIP){
        zbuf = _tc_bzcompress(vbuf, vsiz, &vsiz);
      } else if(hdb->opts & HDBTTCBS){
        zbuf = tcbsencode(vbuf, vsiz, &vsiz);
      } else {
        zbuf = hdb->enc(vbuf, vsiz, &vsiz, hdb->encop);
      }
    }
    if(!zbuf){
      tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
      HDBUNLOCKRECORD(hdb, bidx);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, zbuf, vsiz, HDBPDOVER);
    TCFREE(zbuf);
    HDBUNLOCKRECORD(hdb, bidx);
    HDBUNLOCKMETHOD(hdb);
    if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
       !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
    return rv;
  }
  bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, vbuf, vsiz, HDBPDCAT);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
     !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
  return rv;
}


/* Concatenate a string value at the end of the existing record in a hash database object. */
bool tchdbputcat2(TCHDB *hdb, const char *kstr, const char *vstr){
  assert(hdb && kstr && vstr);
  return tchdbputcat(hdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Store a record into a hash database object in asynchronous fashion. */
bool tchdbputasync(TCHDB *hdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  // <MM>
  // 加method写锁
  // </MM>
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  // <MM>
  // 将async置为true，表示当前处于异步状态
  // 后续的同步操作，第一步都要进行flush
  // </MM>
  hdb->async = true;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->zmode){
    char *zbuf;
    if(hdb->opts & HDBTDEFLATE){
      zbuf = _tc_deflate(vbuf, vsiz, &vsiz, _TCZMRAW);
    } else if(hdb->opts & HDBTBZIP){
      zbuf = _tc_bzcompress(vbuf, vsiz, &vsiz);
    } else if(hdb->opts & HDBTTCBS){
      zbuf = tcbsencode(vbuf, vsiz, &vsiz);
    } else {
      zbuf = hdb->enc(vbuf, vsiz, &vsiz, hdb->encop);
    }
    if(!zbuf){
      tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    bool rv = tchdbputasyncimpl(hdb, kbuf, ksiz, bidx, hash, zbuf, vsiz);
    TCFREE(zbuf);
    HDBUNLOCKMETHOD(hdb);
    return rv;
  }
  bool rv = tchdbputasyncimpl(hdb, kbuf, ksiz, bidx, hash, vbuf, vsiz);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Store a string record into a hash database object in asynchronous fashion. */
bool tchdbputasync2(TCHDB *hdb, const char *kstr, const char *vstr){
  assert(hdb && kstr && vstr);
  return tchdbputasync(hdb, kstr, strlen(kstr), vstr, strlen(vstr));
}


/* Remove a record of a hash database object. */
bool tchdbout(TCHDB *hdb, const void *kbuf, int ksiz){
  assert(hdb && kbuf && ksiz >= 0);
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  // <MM>
  // 之前执行异步操作，首先将这些操作落盘
  // </MM>
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(!HDBLOCKRECORD(hdb, bidx, true)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool rv = tchdboutimpl(hdb, kbuf, ksiz, bidx, hash);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
     !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
  return rv;
}


/* Remove a string record of a hash database object. */
bool tchdbout2(TCHDB *hdb, const char *kstr){
  assert(hdb && kstr);
  return tchdbout(hdb, kstr, strlen(kstr));
}


/* Retrieve a record in a hash database object. */
void *tchdbget(TCHDB *hdb, const void *kbuf, int ksiz, int *sp){
  assert(hdb && kbuf && ksiz >= 0 && sp);
  if(!HDBLOCKMETHOD(hdb, false)) return NULL;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  // <MM>
  // 完成异步操作
  // </MM>
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  if(!HDBLOCKRECORD(hdb, bidx, false)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  char *rv = tchdbgetimpl(hdb, kbuf, ksiz, bidx, hash, sp);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Retrieve a string record in a hash database object. */
char *tchdbget2(TCHDB *hdb, const char *kstr){
  assert(hdb && kstr);
  int vsiz;
  return tchdbget(hdb, kstr, strlen(kstr), &vsiz);
}


/* Retrieve a record in a hash database object and write the value into a buffer. */
int tchdbget3(TCHDB *hdb, const void *kbuf, int ksiz, void *vbuf, int max){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && max >= 0);
  if(!HDBLOCKMETHOD(hdb, false)) return -1;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return -1;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return -1;
  }
  if(!HDBLOCKRECORD(hdb, bidx, false)){
    HDBUNLOCKMETHOD(hdb);
    return -1;
  }
  int rv = tchdbgetintobuf(hdb, kbuf, ksiz, bidx, hash, vbuf, max);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Get the size of the value of a record in a hash database object. */
int tchdbvsiz(TCHDB *hdb, const void *kbuf, int ksiz){
  assert(hdb && kbuf && ksiz >= 0);
  if(!HDBLOCKMETHOD(hdb, false)) return -1;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return -1;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return -1;
  }
  if(!HDBLOCKRECORD(hdb, bidx, false)){
    HDBUNLOCKMETHOD(hdb);
    return -1;
  }
  int rv = tchdbvsizimpl(hdb, kbuf, ksiz, bidx, hash);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Get the size of the value of a string record in a hash database object. */
int tchdbvsiz2(TCHDB *hdb, const char *kstr){
  assert(hdb && kstr);
  return tchdbvsiz(hdb, kstr, strlen(kstr));
}


/* Initialize the iterator of a hash database object. */
bool tchdbiterinit(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool rv = tchdbiterinitimpl(hdb);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Get the next key of the iterator of a hash database object. */
void *tchdbiternext(TCHDB *hdb, int *sp){
  assert(hdb && sp);
  if(!HDBLOCKMETHOD(hdb, true)) return NULL;
  if(hdb->fd < 0 || hdb->iter < 1){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  char *rv = tchdbiternextimpl(hdb, sp);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Get the next key string of the iterator of a hash database object. */
char *tchdbiternext2(TCHDB *hdb){
  assert(hdb);
  int vsiz;
  return tchdbiternext(hdb, &vsiz);
}


/* Get the next extensible objects of the iterator of a hash database object. */
bool tchdbiternext3(TCHDB *hdb, TCXSTR *kxstr, TCXSTR *vxstr){
  assert(hdb && kxstr && vxstr);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0 || hdb->iter < 1){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool rv = tchdbiternextintoxstr(hdb, kxstr, vxstr);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Get forward matching keys in a hash database object. */
TCLIST *tchdbfwmkeys(TCHDB *hdb, const void *pbuf, int psiz, int max){
  assert(hdb && pbuf && psiz >= 0);
  TCLIST* keys = tclistnew();
  if(!HDBLOCKMETHOD(hdb, true)) return keys;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return keys;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return keys;
  }
  if(max < 0) max = INT_MAX;
  uint64_t iter = hdb->iter;
  tchdbiterinitimpl(hdb);
  char *kbuf;
  int ksiz;
  while(TCLISTNUM(keys) < max && (kbuf = tchdbiternextimpl(hdb, &ksiz)) != NULL){
    if(ksiz >= psiz && !memcmp(kbuf, pbuf, psiz)){
      tclistpushmalloc(keys, kbuf, ksiz);
    } else {
      TCFREE(kbuf);
    }
  }
  hdb->iter = iter;
  HDBUNLOCKMETHOD(hdb);
  return keys;
}


/* Get forward matching string keys in a hash database object. */
TCLIST *tchdbfwmkeys2(TCHDB *hdb, const char *pstr, int max){
  assert(hdb && pstr);
  return tchdbfwmkeys(hdb, pstr, strlen(pstr), max);
}


/* Add an integer to a record in a hash database object. */
int tchdbaddint(TCHDB *hdb, const void *kbuf, int ksiz, int num){
  assert(hdb && kbuf && ksiz >= 0);
  if(!HDBLOCKMETHOD(hdb, false)) return INT_MIN;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return INT_MIN;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return INT_MIN;
  }
  if(!HDBLOCKRECORD(hdb, bidx, true)){
    HDBUNLOCKMETHOD(hdb);
    return INT_MIN;
  }
  if(hdb->zmode){
    char *zbuf;
    int osiz;
    char *obuf = tchdbgetimpl(hdb, kbuf, ksiz, bidx, hash, &osiz);
    if(obuf){
      if(osiz != sizeof(num)){
        tchdbsetecode(hdb, TCEKEEP, __FILE__, __LINE__, __func__);
        TCFREE(obuf);
        HDBUNLOCKRECORD(hdb, bidx);
        HDBUNLOCKMETHOD(hdb);
        return INT_MIN;
      }
      num += *(int *)obuf;
      TCFREE(obuf);
    }
    int zsiz;
    if(hdb->opts & HDBTDEFLATE){
      zbuf = _tc_deflate((char *)&num, sizeof(num), &zsiz, _TCZMRAW);
    } else if(hdb->opts & HDBTBZIP){
      zbuf = _tc_bzcompress((char *)&num, sizeof(num), &zsiz);
    } else if(hdb->opts & HDBTTCBS){
      zbuf = tcbsencode((char *)&num, sizeof(num), &zsiz);
    } else {
      zbuf = hdb->enc((char *)&num, sizeof(num), &zsiz, hdb->encop);
    }
    if(!zbuf){
      tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
      HDBUNLOCKRECORD(hdb, bidx);
      HDBUNLOCKMETHOD(hdb);
      return INT_MIN;
    }
    bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, zbuf, zsiz, HDBPDOVER);
    TCFREE(zbuf);
    HDBUNLOCKRECORD(hdb, bidx);
    HDBUNLOCKMETHOD(hdb);
    if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
       !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
    return rv ? num : INT_MIN;
  }
  bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, (char *)&num, sizeof(num), HDBPDADDINT);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
     !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
  return rv ? num : INT_MIN;
}


/* Add a real number to a record in a hash database object. */
double tchdbadddouble(TCHDB *hdb, const void *kbuf, int ksiz, double num){
  assert(hdb && kbuf && ksiz >= 0);
  if(!HDBLOCKMETHOD(hdb, false)) return nan("");
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return nan("");
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return nan("");
  }
  if(!HDBLOCKRECORD(hdb, bidx, true)){
    HDBUNLOCKMETHOD(hdb);
    return nan("");
  }
  if(hdb->zmode){
    char *zbuf;
    int osiz;
    char *obuf = tchdbgetimpl(hdb, kbuf, ksiz, bidx, hash, &osiz);
    if(obuf){
      if(osiz != sizeof(num)){
        tchdbsetecode(hdb, TCEKEEP, __FILE__, __LINE__, __func__);
        TCFREE(obuf);
        HDBUNLOCKRECORD(hdb, bidx);
        HDBUNLOCKMETHOD(hdb);
        return nan("");
      }
      num += *(double *)obuf;
      TCFREE(obuf);
    }
    int zsiz;
    if(hdb->opts & HDBTDEFLATE){
      zbuf = _tc_deflate((char *)&num, sizeof(num), &zsiz, _TCZMRAW);
    } else if(hdb->opts & HDBTBZIP){
      zbuf = _tc_bzcompress((char *)&num, sizeof(num), &zsiz);
    } else if(hdb->opts & HDBTTCBS){
      zbuf = tcbsencode((char *)&num, sizeof(num), &zsiz);
    } else {
      zbuf = hdb->enc((char *)&num, sizeof(num), &zsiz, hdb->encop);
    }
    if(!zbuf){
      tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
      HDBUNLOCKRECORD(hdb, bidx);
      HDBUNLOCKMETHOD(hdb);
      return nan("");
    }
    bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, zbuf, zsiz, HDBPDOVER);
    TCFREE(zbuf);
    HDBUNLOCKRECORD(hdb, bidx);
    HDBUNLOCKMETHOD(hdb);
    if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
       !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
    return rv ? num : nan("");
  }
  bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, (char *)&num, sizeof(num), HDBPDADDDBL);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
     !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
  return rv ? num : nan("");
}


/* Synchronize updated contents of a hash database object with the file and the device. */
bool tchdbsync(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER) || hdb->tran){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool rv = tchdbmemsync(hdb, true);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Optimize the file of a hash database object. */
bool tchdboptimize(TCHDB *hdb, int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER) || hdb->tran){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  HDBTHREADYIELD(hdb);
  bool rv = tchdboptimizeimpl(hdb, bnum, apow, fpow, opts);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Remove all records of a hash database object. */
bool tchdbvanish(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER) || hdb->tran){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  HDBTHREADYIELD(hdb);
  bool rv = tchdbvanishimpl(hdb);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Copy the database file of a hash database object. */
bool tchdbcopy(TCHDB *hdb, const char *path){
  assert(hdb && path);
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(!HDBLOCKALLRECORDS(hdb, false)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  HDBTHREADYIELD(hdb);
  bool rv = tchdbcopyimpl(hdb, path);
  HDBUNLOCKALLRECORDS(hdb);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Begin the transaction of a hash database object. */
bool tchdbtranbegin(TCHDB *hdb){
  assert(hdb);
  // <MM>
  // 尝试等待tran为false，即没有其他transaction进行
  // </MM>
  for(double wsec = 1.0 / sysconf(_SC_CLK_TCK); true; wsec *= 2){
    if(!HDBLOCKMETHOD(hdb, true)) return false;
    if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER) || hdb->fatal){
      tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    if(!hdb->tran) break;
    HDBUNLOCKMETHOD(hdb);
    if(wsec > 1.0) wsec = 1.0;
    tcsleep(wsec);
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  // <MM>
  // 开始transaction前，将内存中的改动落地
  // </MM>
  if(!tchdbmemsync(hdb, false)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  // <MW>
  // sync模式下，不是应该在每次写操作后进行sync?为什么这里需要sync
  // </MW>
  if((hdb->omode & HDBOTSYNC) && fsync(hdb->fd) == -1){
    tchdbsetecode(hdb, TCESYNC, __FILE__, __LINE__, __func__);
    return false;
  }
  if(hdb->walfd < 0){
    // <MM>
    // 创建wal文件
    // </MM>
    char *tpath = tcsprintf("%s%c%s", hdb->path, MYEXTCHR, HDBWALSUFFIX);
    int walfd = open(tpath, O_RDWR | O_CREAT | O_TRUNC, HDBFILEMODE);
    TCFREE(tpath);
    if(walfd < 0){
      int ecode = TCEOPEN;
      switch(errno){
        case EACCES: ecode = TCENOPERM; break;
        case ENOENT: ecode = TCENOFILE; break;
        case ENOTDIR: ecode = TCENOFILE; break;
      }
      tchdbsetecode(hdb, ecode, __FILE__, __LINE__, __func__);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    hdb->walfd = walfd;
  }
  // <MW>
  // 为什么要将HDBFOPEN置为false，此时已经打开了呀?
  //
  // 应该是为了禁止db操作，但是没有发现操作的地方校验
  // </MW>
  tchdbsetflag(hdb, HDBFOPEN, false);
  // <MM>
  // 每次开启transaction都进行init
  // </MM>
  if(!tchdbwalinit(hdb)){
    tchdbsetflag(hdb, HDBFOPEN, true);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  tchdbsetflag(hdb, HDBFOPEN, true);
  hdb->tran = true;
  HDBUNLOCKMETHOD(hdb);
  return true;
}


/* Commit the transaction of a hash database object. */
bool tchdbtrancommit(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER) || hdb->fatal || !hdb->tran){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool err = false;
  if(hdb->async && !tchdbflushdrp(hdb)) err = true;
  if(!tchdbmemsync(hdb, hdb->omode & HDBOTSYNC)) err = true;
  // <MM>
  // 事务提交，主要的操作是清空wal文件，即不需要进行回滚
  // </MM>
  if(!err && ftruncate(hdb->walfd, 0) == -1){
    tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
    err = true;
  }
  hdb->tran = false;
  HDBUNLOCKMETHOD(hdb);
  return !err;
}


/* Abort the transaction of a hash database object. */
bool tchdbtranabort(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER) || !hdb->tran){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool err = false;
  if(hdb->async && !tchdbflushdrp(hdb)) err = true;
  if(!tchdbmemsync(hdb, false)) err = true;
  if(!tchdbwalrestore(hdb, hdb->path)) err = true;
  char hbuf[HDBHEADSIZ];
  if(lseek(hdb->fd, 0, SEEK_SET) == -1){
    tchdbsetecode(hdb, TCESEEK, __FILE__, __LINE__, __func__);
    err = false;
  } else if(!tcread(hdb->fd, hbuf, HDBHEADSIZ)){
    tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
    err = false;
  } else {
    tchdbloadmeta(hdb, hbuf);
  }
  hdb->dfcur = hdb->frec;
  hdb->iter = 0;
  hdb->xfsiz = 0;
  hdb->fbpnum = 0;
  if(hdb->recc) tcmdbvanish(hdb->recc);
  hdb->tran = false;
  HDBUNLOCKMETHOD(hdb);
  return !err;
}


/* Get the file path of a hash database object. */
const char *tchdbpath(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, false)) return NULL;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  const char *rv = hdb->path;
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Get the number of records of a hash database object. */
uint64_t tchdbrnum(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, false)) return 0;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return 0;
  }
  uint64_t rv = hdb->rnum;
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Get the size of the database file of a hash database object. */
uint64_t tchdbfsiz(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, false)) return 0;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return 0;
  }
  uint64_t rv = hdb->fsiz;
  HDBUNLOCKMETHOD(hdb);
  return rv;
}



/*************************************************************************************************
 * features for experts
 *************************************************************************************************/


/* Set the error code of a hash database object. */
void tchdbsetecode(TCHDB *hdb, int ecode, const char *filename, int line, const char *func){
  assert(hdb && filename && line >= 1 && func);
  int myerrno = errno;
  if(!hdb->fatal){
    if(hdb->mmtx){
      pthread_setspecific(*(pthread_key_t *)hdb->eckey, (void *)(intptr_t)ecode);
    } else {
      hdb->ecode = ecode;
    }
  }
  if(ecode != TCESUCCESS && ecode != TCEINVALID && ecode != TCEKEEP && ecode != TCENOREC){
    hdb->fatal = true;
    if(hdb->fd >= 0 && (hdb->omode & HDBOWRITER)) tchdbsetflag(hdb, HDBFFATAL, true);
  }
  if(hdb->dbgfd >= 0 && (hdb->dbgfd != UINT16_MAX || hdb->fatal)){
    int dbgfd = (hdb->dbgfd == UINT16_MAX) ? 1 : hdb->dbgfd;
    char obuf[HDBIOBUFSIZ];
    int osiz = sprintf(obuf, "ERROR:%s:%d:%s:%s:%d:%s:%d:%s\n", filename, line, func,
                       hdb->path ? hdb->path : "-", ecode, tchdberrmsg(ecode),
                       myerrno, strerror(myerrno));
    tcwrite(dbgfd, obuf, osiz);
  }
}


/* Set the type of a hash database object. */
void tchdbsettype(TCHDB *hdb, uint8_t type){
  assert(hdb);
  if(hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return;
  }
  hdb->type = type;
}


/* Set the file descriptor for debugging output. */
void tchdbsetdbgfd(TCHDB *hdb, int fd){
  assert(hdb && fd >= 0);
  hdb->dbgfd = fd;
}


/* Get the file descriptor for debugging output. */
int tchdbdbgfd(TCHDB *hdb){
  assert(hdb);
  return hdb->dbgfd;
}


/* Check whether mutual exclusion control is set to a hash database object. */
bool tchdbhasmutex(TCHDB *hdb){
  assert(hdb);
  return hdb->mmtx != NULL;
}


/* Synchronize updating contents on memory of a hash database object. */
bool tchdbmemsync(TCHDB *hdb, bool phys){
  assert(hdb);
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return false;
  }
  bool err = false;
  char hbuf[HDBHEADSIZ];
  // <MW>
  // db header部分不是即时更新的么？此处为什么要重新序列化
  // </MW>
  tchdbdumpmeta(hdb, hbuf);
  memcpy(hdb->map, hbuf, HDBOPAQUEOFF);
  if(phys){
    size_t xmsiz = (hdb->xmsiz > hdb->msiz) ? hdb->xmsiz : hdb->msiz;
    // <MM>
    // 更新mmap部分的db文件
    // </MM>
    if(msync(hdb->map, xmsiz, MS_SYNC) == -1){
      tchdbsetecode(hdb, TCEMMAP, __FILE__, __LINE__, __func__);
      err = true;
    }
    // <MM>
    // 将page cache刷新到磁盘
    // </MM>
    if(fsync(hdb->fd) == -1){
      tchdbsetecode(hdb, TCESYNC, __FILE__, __LINE__, __func__);
      err = true;
    }
  }
  return !err;
}


/* Get the number of elements of the bucket array of a hash database object. */
uint64_t tchdbbnum(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->bnum;
}


/* Get the record alignment a hash database object. */
uint32_t tchdbalign(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->align;
}


/* Get the maximum number of the free block pool of a a hash database object. */
uint32_t tchdbfbpmax(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->fbpmax;
}


/* Get the size of the extra mapped memory of a hash database object. */
uint64_t tchdbxmsiz(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->xmsiz;
}


/* Get the inode number of the database file of a hash database object. */
uint64_t tchdbinode(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->inode;
}


/* Get the modification time of the database file of a hash database object. */
time_t tchdbmtime(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->mtime;
}


/* Get the connection mode of a hash database object. */
int tchdbomode(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->omode;
}


/* Get the database type of a hash database object. */
uint8_t tchdbtype(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->type;
}


/* Get the additional flags of a hash database object. */
uint8_t tchdbflags(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->flags;
}


/* Get the options of a hash database object. */
uint8_t tchdbopts(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  return hdb->opts;
}


/* Get the pointer to the opaque field of a hash database object. */
char *tchdbopaque(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return NULL;
  }
  return hdb->map + HDBOPAQUEOFF;
}


/* Get the number of used elements of the bucket array of a hash database object. */
uint64_t tchdbbnumused(TCHDB *hdb){
  assert(hdb);
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    return 0;
  }
  uint64_t unum = 0;
  if(hdb->ba64){
    uint64_t *buckets = hdb->ba64;
    for(int i = 0; i < hdb->bnum; i++){
      if(buckets[i]) unum++;
    }
  } else {
    uint32_t *buckets = hdb->ba32;
    for(int i = 0; i < hdb->bnum; i++){
      if(buckets[i]) unum++;
    }
  }
  return unum;
}


/* Set the custom codec functions of a hash database object. */
bool tchdbsetcodecfunc(TCHDB *hdb, TCCODEC enc, void *encop, TCCODEC dec, void *decop){
  assert(hdb && enc && dec);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd >= 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  hdb->enc = enc;
  hdb->encop = encop;
  hdb->dec = dec;
  hdb->decop = decop;
  HDBUNLOCKMETHOD(hdb);
  return true;
}


/* Get the unit step number of auto defragmentation of a hash database object. */
uint32_t tchdbdfunit(TCHDB *hdb){
  assert(hdb);
  return hdb->dfunit;
}


/* Perform dynamic defragmentation of a hash database object. */
bool tchdbdefrag(TCHDB *hdb, int64_t step){
  assert(hdb);
  if(step > 0){
    if(!HDBLOCKMETHOD(hdb, true)) return false;
    if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
      tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    if(hdb->async && !tchdbflushdrp(hdb)){
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    bool rv = tchdbdefragimpl(hdb, step);
    HDBUNLOCKMETHOD(hdb);
    return rv;
  }
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool err = false;
  if(HDBLOCKALLRECORDS(hdb, true)){
    hdb->dfcur = hdb->frec;
    HDBUNLOCKALLRECORDS(hdb);
  } else {
    err = true;
  }
  bool stop = false;
  while(!err && !stop){
    if(HDBLOCKALLRECORDS(hdb, true)){
      uint64_t cur = hdb->dfcur;
      if(!tchdbdefragimpl(hdb, UINT8_MAX)) err = true;
      if(hdb->dfcur <= cur) stop = true;
      HDBUNLOCKALLRECORDS(hdb);
      HDBTHREADYIELD(hdb);
    } else {
      err = true;
    }
  }
  HDBUNLOCKMETHOD(hdb);
  return !err;
}


/* Clear the cache of a hash tree database object. */
bool tchdbcacheclear(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  HDBTHREADYIELD(hdb);
  if(hdb->recc) tcmdbvanish(hdb->recc);
  HDBUNLOCKMETHOD(hdb);
  return true;
}


/* Store a record into a hash database object with a duplication handler. */
bool tchdbputproc(TCHDB *hdb, const void *kbuf, int ksiz, const void *vbuf, int vsiz,
                  TCPDPROC proc, void *op){
  assert(hdb && kbuf && ksiz >= 0 && proc);
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(!HDBLOCKRECORD(hdb, bidx, true)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->zmode){
    char *zbuf;
    int osiz;
    char *obuf = tchdbgetimpl(hdb, kbuf, ksiz, bidx, hash, &osiz);
    if(obuf){
      int nsiz;
      char *nbuf = proc(obuf, osiz, &nsiz, op);
      if(nbuf == (void *)-1){
        bool rv = tchdboutimpl(hdb, kbuf, ksiz, bidx, hash);
        TCFREE(obuf);
        HDBUNLOCKRECORD(hdb, bidx);
        HDBUNLOCKMETHOD(hdb);
        return rv;
      } else if(nbuf){
        if(hdb->opts & HDBTDEFLATE){
          zbuf = _tc_deflate(nbuf, nsiz, &vsiz, _TCZMRAW);
        } else if(hdb->opts & HDBTBZIP){
          zbuf = _tc_bzcompress(nbuf, nsiz, &vsiz);
        } else if(hdb->opts & HDBTTCBS){
          zbuf = tcbsencode(nbuf, nsiz, &vsiz);
        } else {
          zbuf = hdb->enc(nbuf, nsiz, &vsiz, hdb->encop);
        }
        TCFREE(nbuf);
      } else {
        zbuf = NULL;
      }
      TCFREE(obuf);
    } else if(vbuf){
      if(hdb->opts & HDBTDEFLATE){
        zbuf = _tc_deflate(vbuf, vsiz, &vsiz, _TCZMRAW);
      } else if(hdb->opts & HDBTBZIP){
        zbuf = _tc_bzcompress(vbuf, vsiz, &vsiz);
      } else if(hdb->opts & HDBTTCBS){
        zbuf = tcbsencode(vbuf, vsiz, &vsiz);
      } else {
        zbuf = hdb->enc(vbuf, vsiz, &vsiz, hdb->encop);
      }
    } else {
      tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
      HDBUNLOCKRECORD(hdb, bidx);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    if(!zbuf){
      tchdbsetecode(hdb, TCEKEEP, __FILE__, __LINE__, __func__);
      HDBUNLOCKRECORD(hdb, bidx);
      HDBUNLOCKMETHOD(hdb);
      return false;
    }
    bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, zbuf, vsiz, HDBPDOVER);
    TCFREE(zbuf);
    HDBUNLOCKRECORD(hdb, bidx);
    HDBUNLOCKMETHOD(hdb);
    if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
       !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
    return rv;
  }
  HDBPDPROCOP procop;
  procop.proc = proc;
  procop.op = op;
  HDBPDPROCOP *procptr = &procop;
  tcgeneric_t stack[(TCNUMBUFSIZ*2)/sizeof(tcgeneric_t)+1];
  char *rbuf;
  if(ksiz <= sizeof(stack) - sizeof(procptr)){
    rbuf = (char *)stack;
  } else {
    TCMALLOC(rbuf, ksiz + sizeof(procptr));
  }
  char *wp = rbuf;
  memcpy(wp, &procptr, sizeof(procptr));
  wp += sizeof(procptr);
  memcpy(wp, kbuf, ksiz);
  kbuf = rbuf + sizeof(procptr);
  bool rv = tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, vbuf, vsiz, HDBPDPROC);
  if(rbuf != (char *)stack) TCFREE(rbuf);
  HDBUNLOCKRECORD(hdb, bidx);
  HDBUNLOCKMETHOD(hdb);
  if(hdb->dfunit > 0 && hdb->dfcnt > hdb->dfunit &&
     !tchdbdefrag(hdb, hdb->dfunit * HDBDFRSRAT + 1)) rv = false;
  return rv;
}


/* Get the custom codec functions of a hash database object. */
void tchdbcodecfunc(TCHDB *hdb, TCCODEC *ep, void **eop, TCCODEC *dp, void **dop){
  assert(hdb && ep && eop && dp && dop);
  *ep = hdb->enc;
  *eop = hdb->encop;
  *dp = hdb->dec;
  *dop = hdb->decop;
}


/* Retrieve the next record of a record in a hash database object. */
void *tchdbgetnext(TCHDB *hdb, const void *kbuf, int ksiz, int *sp){
  assert(hdb && sp);
  if(!HDBLOCKMETHOD(hdb, true)) return NULL;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  char *rv = tchdbgetnextimpl(hdb, kbuf, ksiz, sp, NULL, NULL);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Retrieve the next record of a string record in a hash database object. */
char *tchdbgetnext2(TCHDB *hdb, const char *kstr){
  assert(hdb);
  int vsiz;
  return tchdbgetnext(hdb, kstr, strlen(kstr), &vsiz);
}


/* Retrieve the key and the value of the next record of a record in a hash database object. */
char *tchdbgetnext3(TCHDB *hdb, const char *kbuf, int ksiz, int *sp, const char **vbp, int *vsp){
  assert(hdb && sp && vbp && vsp);
  if(!HDBLOCKMETHOD(hdb, true)) return NULL;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return NULL;
  }
  char *rv = tchdbgetnextimpl(hdb, kbuf, ksiz, sp, vbp, vsp);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Move the iterator to the record corresponding a key of a hash database object. */
bool tchdbiterinit2(TCHDB *hdb, const void *kbuf, int ksiz){
  assert(hdb && kbuf && ksiz >= 0);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  bool rv = tchdbiterjumpimpl(hdb, kbuf, ksiz);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Move the iterator to the record corresponding a key string of a hash database object. */
bool tchdbiterinit3(TCHDB *hdb, const char *kstr){
  assert(hdb && kstr);
  return tchdbiterinit2(hdb, kstr, strlen(kstr));
}


/* Process each record atomically of a hash database object. */
bool tchdbforeach(TCHDB *hdb, TCITER iter, void *op){
  assert(hdb && iter);
  if(!HDBLOCKMETHOD(hdb, false)) return false;
  if(hdb->fd < 0){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(hdb->async && !tchdbflushdrp(hdb)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  if(!HDBLOCKALLRECORDS(hdb, false)){
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  HDBTHREADYIELD(hdb);
  bool rv = tchdbforeachimpl(hdb, iter, op);
  HDBUNLOCKALLRECORDS(hdb);
  HDBUNLOCKMETHOD(hdb);
  return rv;
}


/* Void the transaction of a hash database object. */
bool tchdbtranvoid(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKMETHOD(hdb, true)) return false;
  if(hdb->fd < 0 || !(hdb->omode & HDBOWRITER) || hdb->fatal || !hdb->tran){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    HDBUNLOCKMETHOD(hdb);
    return false;
  }
  hdb->tran = false;
  HDBUNLOCKMETHOD(hdb);
  return true;
}



/*************************************************************************************************
 * private features
 *************************************************************************************************/


/* Get a natural prime number not less than a floor number.
   `num' specified the floor number.
   The return value is a prime number not less than the floor number. */
static uint64_t tcgetprime(uint64_t num){
  uint64_t primes[] = {
    1, 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 43, 47, 53, 59, 61, 71, 79, 83,
    89, 103, 109, 113, 127, 139, 157, 173, 191, 199, 223, 239, 251, 283, 317, 349,
    383, 409, 443, 479, 509, 571, 631, 701, 761, 829, 887, 953, 1021, 1151, 1279,
    1399, 1531, 1663, 1789, 1913, 2039, 2297, 2557, 2803, 3067, 3323, 3583, 3833,
    4093, 4603, 5119, 5623, 6143, 6653, 7159, 7673, 8191, 9209, 10223, 11261,
    12281, 13309, 14327, 15359, 16381, 18427, 20479, 22511, 24571, 26597, 28669,
    30713, 32749, 36857, 40949, 45053, 49139, 53239, 57331, 61417, 65521, 73727,
    81919, 90107, 98299, 106487, 114679, 122869, 131071, 147451, 163819, 180221,
    196597, 212987, 229373, 245759, 262139, 294911, 327673, 360439, 393209, 425977,
    458747, 491503, 524287, 589811, 655357, 720887, 786431, 851957, 917503, 982981,
    1048573, 1179641, 1310719, 1441771, 1572853, 1703903, 1835003, 1966079,
    2097143, 2359267, 2621431, 2883577, 3145721, 3407857, 3670013, 3932153,
    4194301, 4718579, 5242877, 5767129, 6291449, 6815741, 7340009, 7864301,
    8388593, 9437179, 10485751, 11534329, 12582893, 13631477, 14680063, 15728611,
    16777213, 18874367, 20971507, 23068667, 25165813, 27262931, 29360087, 31457269,
    33554393, 37748717, 41943023, 46137319, 50331599, 54525917, 58720253, 62914549,
    67108859, 75497467, 83886053, 92274671, 100663291, 109051903, 117440509,
    125829103, 134217689, 150994939, 167772107, 184549373, 201326557, 218103799,
    234881011, 251658227, 268435399, 301989881, 335544301, 369098707, 402653171,
    436207613, 469762043, 503316469, 536870909, 603979769, 671088637, 738197503,
    805306357, 872415211, 939524087, 1006632947, 1073741789, 1207959503,
    1342177237, 1476394991, 1610612711, 1744830457, 1879048183, 2013265907,
    2576980349, 3092376431, 3710851741, 4718021527, 6133428047, 7973456459,
    10365493393, 13475141413, 17517683831, 22772988923, 29604885677, 38486351381,
    50032256819, 65041933867, 84554514043, 109920868241, 153889215497, 0
  };
  int i;
  for(i = 0; primes[i] > 0; i++){
    if(num <= primes[i]) return primes[i];
  }
  return primes[i-1];
}


/* Seek and write data into a file.
   `hdb' specifies the hash database object.
   `off' specifies the offset of the region to seek.
   `buf' specifies the buffer to store into.
   `size' specifies the size of the buffer.
   The return value is true if successful, else, it is false. */
static bool tchdbseekwrite(TCHDB *hdb, off_t off, const void *buf, size_t size){
  assert(hdb && off >= 0 && buf && size >= 0);
  // <MM>
  // 事务中，记录wal日志，以便后续回滚时进行撤销
  // </MM>
  if(hdb->tran && !tchdbwalwrite(hdb, off, size)) return false;
  off_t end = off + size;
  if(end <= hdb->xmsiz){
    if(end >= hdb->fsiz && end >= hdb->xfsiz){
      uint64_t xfsiz = end + HDBXFSIZINC;
      if(ftruncate(hdb->fd, xfsiz) == -1){
        tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
        return false;
      }
      hdb->xfsiz = xfsiz;
    }
    memcpy(hdb->map + off, buf, size);
    return true;
  }
  if(!TCUBCACHE && off < hdb->xmsiz){
    if(end >= hdb->fsiz && end >= hdb->xfsiz){
      uint64_t xfsiz = end + HDBXFSIZINC;
      if(ftruncate(hdb->fd, xfsiz) == -1){
        tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
        return false;
      }
      hdb->xfsiz = xfsiz;
    }
    int head = hdb->xmsiz - off;
    memcpy(hdb->map + off, buf, head);
    off += head;
    buf = (char *)buf + head;
    size -= head;
  }
  while(true){
    int wb = pwrite(hdb->fd, buf, size, off);
    if(wb >= size){
      return true;
    } else if(wb > 0){
      buf = (char *)buf + wb;
      size -= wb;
      off += wb;
    } else if(wb == -1){
      if(errno != EINTR){
        tchdbsetecode(hdb, TCEWRITE, __FILE__, __LINE__, __func__);
        return false;
      }
    } else {
      if(size > 0){
        tchdbsetecode(hdb, TCEWRITE, __FILE__, __LINE__, __func__);
        return false;
      }
    }
  }
  return true;
}


/* Seek and read data from a file.
   `hdb' specifies the hash database object.
   `off' specifies the offset of the region to seek.
   `buf' specifies the buffer to store into.
   `size' specifies the size of the buffer.
   The return value is true if successful, else, it is false. */
static bool tchdbseekread(TCHDB *hdb, off_t off, void *buf, size_t size){
  assert(hdb && off >= 0 && buf && size >= 0);
  if(off + size <= hdb->xmsiz){
    // <MM>
    // 待读的文件块命中mmap区域，直接copy
    // </MM>
    memcpy(buf, hdb->map + off, size);
    return true;
  }
  if(!TCUBCACHE && off < hdb->xmsiz){
    int head = hdb->xmsiz - off;
    memcpy(buf, hdb->map + off, head);
    off += head;
    buf = (char *)buf + head;
    size -= head;
  }
  while(true){
    int rb = pread(hdb->fd, buf, size, off);
    if(rb >= size){
      break;
    } else if(rb > 0){
      buf = (char *)buf + rb;
      size -= rb;
      off += rb;
    } else if(rb == -1){
      if(errno != EINTR){
        tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
        return false;
      }
    } else {
      if(size > 0){
        tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
        return false;
      }
    }
  }
  return true;
}


/* Try to seek and read data from a file.
   `hdb' specifies the hash database object.
   `off' specifies the offset of the region to seek.
   `buf' specifies the buffer to store into.
   `size' specifies the size of the buffer.
   The return value is true if successful, else, it is false. */
static bool tchdbseekreadtry(TCHDB *hdb, off_t off, void *buf, size_t size){
  assert(hdb && off >= 0 && buf && size >= 0);
  off_t end = off + size;
  if(end > hdb->fsiz) return false;
  if(end <= hdb->xmsiz){
    memcpy(buf, hdb->map + off, size);
    return true;
  }
  if(!TCUBCACHE && off < hdb->xmsiz){
    int head = hdb->xmsiz - off;
    memcpy(buf, hdb->map + off, head);
    off += head;
    buf = (char *)buf + head;
    size -= head;
  }
  // <MM>
  // 原子读取偏移量处的数据
  // </MM>
  int rb = pread(hdb->fd, buf, size, off);
  if(rb == size) return true;
  if(rb == -1) tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
  return false;
}


/* Serialize meta data into a buffer.
   `hdb' specifies the hash database object.
   `hbuf' specifies the buffer. */
static void tchdbdumpmeta(TCHDB *hdb, char *hbuf){
  memset(hbuf, 0, HDBHEADSIZ);
  sprintf(hbuf, "%s\n%s:%d\n", HDBMAGICDATA, _TC_FORMATVER, _TC_LIBVER);
  memcpy(hbuf + HDBTYPEOFF, &(hdb->type), sizeof(hdb->type));
  memcpy(hbuf + HDBFLAGSOFF, &(hdb->flags), sizeof(hdb->flags));
  memcpy(hbuf + HDBAPOWOFF, &(hdb->apow), sizeof(hdb->apow));
  memcpy(hbuf + HDBFPOWOFF, &(hdb->fpow), sizeof(hdb->fpow));
  memcpy(hbuf + HDBOPTSOFF, &(hdb->opts), sizeof(hdb->opts));
  uint64_t llnum;
  llnum = hdb->bnum;
  llnum = TCHTOILL(llnum);
  memcpy(hbuf + HDBBNUMOFF, &llnum, sizeof(llnum));
  llnum = hdb->rnum;
  llnum = TCHTOILL(llnum);
  memcpy(hbuf + HDBRNUMOFF, &llnum, sizeof(llnum));
  llnum = hdb->fsiz;
  llnum = TCHTOILL(llnum);
  memcpy(hbuf + HDBFSIZOFF, &llnum, sizeof(llnum));
  llnum = hdb->frec;
  llnum = TCHTOILL(llnum);
  memcpy(hbuf + HDBFRECOFF, &llnum, sizeof(llnum));
}


/* Deserialize meta data from a buffer.
   `hdb' specifies the hash database object.
   `hbuf' specifies the buffer. */
static void tchdbloadmeta(TCHDB *hdb, const char *hbuf){
  memcpy(&(hdb->type), hbuf + HDBTYPEOFF, sizeof(hdb->type));
  memcpy(&(hdb->flags), hbuf + HDBFLAGSOFF, sizeof(hdb->flags));
  memcpy(&(hdb->apow), hbuf + HDBAPOWOFF, sizeof(hdb->apow));
  memcpy(&(hdb->fpow), hbuf + HDBFPOWOFF, sizeof(hdb->fpow));
  memcpy(&(hdb->opts), hbuf + HDBOPTSOFF, sizeof(hdb->opts));
  uint64_t llnum;
  memcpy(&llnum, hbuf + HDBBNUMOFF, sizeof(llnum));
  hdb->bnum = TCITOHLL(llnum);
  memcpy(&llnum, hbuf + HDBRNUMOFF, sizeof(llnum));
  hdb->rnum = TCITOHLL(llnum);
  memcpy(&llnum, hbuf + HDBFSIZOFF, sizeof(llnum));
  hdb->fsiz = TCITOHLL(llnum);
  memcpy(&llnum, hbuf + HDBFRECOFF, sizeof(llnum));
  hdb->frec = TCITOHLL(llnum);
}


/* Clear all members.
   `hdb' specifies the hash database object. */
static void tchdbclear(TCHDB *hdb){
  assert(hdb);
  hdb->mmtx = NULL;
  hdb->rmtxs = NULL;
  hdb->dmtx = NULL;
  hdb->wmtx = NULL;
  hdb->eckey = NULL;
  hdb->rpath = NULL;
  hdb->type = TCDBTHASH;
  hdb->flags = 0;
  hdb->bnum = HDBDEFBNUM;
  hdb->apow = HDBDEFAPOW;
  hdb->fpow = HDBDEFFPOW;
  hdb->opts = 0;
  hdb->path = NULL;
  hdb->fd = -1;
  hdb->omode = 0;
  hdb->rnum = 0;
  hdb->fsiz = 0;
  hdb->frec = 0;
  hdb->dfcur = 0;
  hdb->iter = 0;
  hdb->map = NULL;
  hdb->msiz = 0;
  hdb->xmsiz = HDBDEFXMSIZ;
  hdb->xfsiz = 0;
  hdb->ba32 = NULL;
  hdb->ba64 = NULL;
  hdb->align = 0;
  hdb->runit = 0;
  hdb->zmode = false;
  hdb->fbpmax = 0;
  hdb->fbpool = NULL;
  hdb->fbpnum = 0;
  hdb->fbpmis = 0;
  hdb->async = false;
  hdb->drpool = NULL;
  hdb->drpdef = NULL;
  hdb->drpoff = 0;
  hdb->recc = NULL;
  hdb->rcnum = 0;
  hdb->enc = NULL;
  hdb->encop = NULL;
  hdb->dec = NULL;
  hdb->decop = NULL;
  hdb->ecode = TCESUCCESS;
  hdb->fatal = false;
  hdb->inode = 0;
  hdb->mtime = 0;
  hdb->dfunit = 0;
  hdb->dfcnt = 0;
  hdb->tran = false;
  hdb->walfd = -1;
  hdb->walend = 0;
  hdb->dbgfd = -1;
  hdb->cnt_writerec = -1;
  hdb->cnt_reuserec = -1;
  hdb->cnt_moverec = -1;
  hdb->cnt_readrec = -1;
  hdb->cnt_searchfbp = -1;
  hdb->cnt_insertfbp = -1;
  hdb->cnt_splicefbp = -1;
  hdb->cnt_dividefbp = -1;
  hdb->cnt_mergefbp = -1;
  hdb->cnt_reducefbp = -1;
  hdb->cnt_appenddrp = -1;
  hdb->cnt_deferdrp = -1;
  hdb->cnt_flushdrp = -1;
  hdb->cnt_adjrecc = -1;
  hdb->cnt_defrag = -1;
  hdb->cnt_shiftrec = -1;
  hdb->cnt_trunc = -1;
  TCDODEBUG(hdb->cnt_writerec = 0);
  TCDODEBUG(hdb->cnt_reuserec = 0);
  TCDODEBUG(hdb->cnt_moverec = 0);
  TCDODEBUG(hdb->cnt_readrec = 0);
  TCDODEBUG(hdb->cnt_searchfbp = 0);
  TCDODEBUG(hdb->cnt_insertfbp = 0);
  TCDODEBUG(hdb->cnt_splicefbp = 0);
  TCDODEBUG(hdb->cnt_dividefbp = 0);
  TCDODEBUG(hdb->cnt_mergefbp = 0);
  TCDODEBUG(hdb->cnt_reducefbp = 0);
  TCDODEBUG(hdb->cnt_appenddrp = 0);
  TCDODEBUG(hdb->cnt_deferdrp = 0);
  TCDODEBUG(hdb->cnt_flushdrp = 0);
  TCDODEBUG(hdb->cnt_adjrecc = 0);
  TCDODEBUG(hdb->cnt_defrag = 0);
  TCDODEBUG(hdb->cnt_shiftrec = 0);
  TCDODEBUG(hdb->cnt_trunc = 0);
}


/* Get the padding size to record alignment.
   `hdb' specifies the hash database object.
   `off' specifies the current offset.
   The return value is the padding size. */
static int32_t tchdbpadsize(TCHDB *hdb, uint64_t off){
  assert(hdb);
  int32_t diff = off & (hdb->align - 1);
  return (diff > 0) ? hdb->align - diff : 0;
}


/* Set the open flag.
   `hdb' specifies the hash database object.
   `flag' specifies the flag value.
   `sign' specifies the sign. */
static void tchdbsetflag(TCHDB *hdb, int flag, bool sign){
  assert(hdb);
  char *fp = (char *)hdb->map + HDBFLAGSOFF;
  if(sign){
    *fp |= (uint8_t)flag;
  } else {
    *fp &= ~(uint8_t)flag;
  }
  hdb->flags = *fp;
}


/* Get the bucket index of a record.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `hp' specifies the pointer to the variable into which the second hash value is assigned.
   The return value is the bucket index. */
static uint64_t tchdbbidx(TCHDB *hdb, const char *kbuf, int ksiz, uint8_t *hp){
  assert(hdb && kbuf && ksiz >= 0 && hp);
  uint64_t idx = 19780211;
  uint32_t hash = 751;
  const char *rp = kbuf + ksiz;
  // <MW>
  // idx并不是通过hash计算得到的，why？
  //    - idx正序
  //    - hash倒序
  // </MW>
  while(ksiz--){
    idx = idx * 37 + *(uint8_t *)kbuf++;
    // <MM>
    // 不仅可以取得签名的效果，而且保留顺序信息
    // 后续磁盘文件上的hash查找时，可以先通过hash的大小比较，进行二叉树的查找
    // 减少磁盘读写
    //
    // 并没有保留顺序信息:
    // 'sjkdfja': 20573446376046
    // 'cjfsjke': 20576510083273
    //
    // </MM>
    hash = (hash * 31) ^ *(uint8_t *)--rp;
  }
  // <MM>
  // 最终的hash值会截断为1字节，取最后8位
  // </MM>
  *hp = hash;
  return idx % hdb->bnum;
}


/* Get the offset of the record of a bucket element.
   `hdb' specifies the hash database object.
   `bidx' specifies the index of the bucket.
   The return value is the offset of the record. */
static off_t tchdbgetbucket(TCHDB *hdb, uint64_t bidx){
  assert(hdb && bidx >= 0);
  if(hdb->ba64){
    uint64_t llnum = hdb->ba64[bidx];
    // <MW>
    // 为什么要左移4bit
    // </MW>
    return TCITOHLL(llnum) << hdb->apow;
  }
  uint32_t lnum = hdb->ba32[bidx];
  return (off_t)TCITOHL(lnum) << hdb->apow;
}


/* Get the offset of the record of a bucket element.
   `hdb' specifies the hash database object.
   `bidx' specifies the index of the record.
   `off' specifies the offset of the record. */
static void tchdbsetbucket(TCHDB *hdb, uint64_t bidx, uint64_t off){
  assert(hdb && bidx >= 0);
  if(hdb->ba64){
    uint64_t llnum = off >> hdb->apow;
    if(hdb->tran) tchdbwalwrite(hdb, HDBHEADSIZ + bidx * sizeof(llnum), sizeof(llnum));
    hdb->ba64[bidx] = TCHTOILL(llnum);
  } else {
    uint32_t lnum = off >> hdb->apow;
    if(hdb->tran) tchdbwalwrite(hdb, HDBHEADSIZ + bidx * sizeof(lnum), sizeof(lnum));
    hdb->ba32[bidx] = TCHTOIL(lnum);
  }
}


/* Load the free block pool from the file.
   The return value is true if successful, else, it is false. */
static bool tchdbsavefbp(TCHDB *hdb){
  assert(hdb);
  if(hdb->fbpnum > hdb->fbpmax){
    tchdbfbpmerge(hdb);
  } else if(hdb->fbpnum > 1){
    tcfbpsortbyoff(hdb->fbpool, hdb->fbpnum);
  }
  int bsiz = hdb->frec - hdb->msiz;
  char *buf;
  TCMALLOC(buf, bsiz);
  char *wp = buf;
  HDBFB *cur = hdb->fbpool;
  HDBFB *end = cur + hdb->fbpnum;
  uint64_t base = 0;
  bsiz -= sizeof(HDBFB) + sizeof(uint8_t) + sizeof(uint8_t);
  // <MM>
  // free block存入磁盘时，按照block的offset从小到大排序
  // 在写入磁盘时，为了压缩空间，将offset写成基于前驱block的
  // offset的增量，由于offset采用可变整数编码可以减少字节大小
  // </MM>
  while(cur < end && bsiz > 0){
    uint64_t noff = cur->off >> hdb->apow;
    int step;
    uint64_t llnum = noff - base;
    TCSETVNUMBUF64(step, wp, llnum);
    wp += step;
    bsiz -= step;
    uint32_t lnum = cur->rsiz >> hdb->apow;
    TCSETVNUMBUF(step, wp, lnum);
    wp += step;
    bsiz -= step;
    base = noff;
    cur++;
  }
  *(wp++) = '\0';
  *(wp++) = '\0';
  if(!tchdbseekwrite(hdb, hdb->msiz, buf, wp - buf)){
    TCFREE(buf);
    return false;
  }
  TCFREE(buf);
  return true;
}


/* Save the free block pool into the file.
   The return value is true if successful, else, it is false. */
static bool tchdbloadfbp(TCHDB *hdb){
  // <MM>
  // hdb->msiz = HEADERSIZE + Bucket Array
  // hdb->frec = 第一条记录的offset
  //
  // bsiz = file block pool数组的大小
  // </MM>
  int bsiz = hdb->frec - hdb->msiz;
  char *buf;
  TCMALLOC(buf, bsiz);
  // <MM>
  // 命中mmap时，不需要copy buffer，不过本身free block pool
  // 占用内存应该不是很大
  // </MM>
  if(!tchdbseekread(hdb, hdb->msiz, buf, bsiz)){
    TCFREE(buf);
    return false;
  }
  const char *rp = buf;
  // <MM>
  // 从数据文件中加载free block pool，并填充到内存数据结构hdb->fbpool
  // </MM>
  HDBFB *cur = hdb->fbpool;
  HDBFB *end = cur + hdb->fbpmax * HDBFBPALWRAT;
  uint64_t base = 0;
  // <MM>
  // 初始情况下，文件中的free block pool被清零
  // </MM>
  while(cur < end && *rp != '\0'){
    int step;
    uint64_t llnum;
    TCREADVNUMBUF64(rp, llnum, step);
    // <MW>
    // 为什么需要基于base从0累加
    //
    // fbp dump时时按照free block的offset从小到大排序，从而通过变长整数编码减少存储占用量？
    // </MW>
    base += llnum << hdb->apow;
    cur->off = base;
    rp += step;
    uint32_t lnum;
    TCREADVNUMBUF(rp, lnum, step);
    cur->rsiz = lnum << hdb->apow;
    rp += step;
    cur++;
  }
  hdb->fbpnum = cur - (HDBFB *)hdb->fbpool;
  TCFREE(buf);
  tcfbpsortbyrsiz(hdb->fbpool, hdb->fbpnum);
  return true;
}


/* Sort the free block pool by offset.
   `fbpool' specifies the free block pool.
   `fbpnum' specifies the number of blocks. */
static void tcfbpsortbyoff(HDBFB *fbpool, int fbpnum){
  assert(fbpool && fbpnum >= 0);
  fbpnum--;
  int bottom = fbpnum / 2 + 1;
  int top = fbpnum;
  while(bottom > 0){
    bottom--;
    int mybot = bottom;
    int i = mybot * 2;
    while(i <= top){
      if(i < top && fbpool[i+1].off > fbpool[i].off) i++;
      if(fbpool[mybot].off >= fbpool[i].off) break;
      HDBFB swap = fbpool[mybot];
      fbpool[mybot] = fbpool[i];
      fbpool[i] = swap;
      mybot = i;
      i = mybot * 2;
    }
  }
  while(top > 0){
    HDBFB swap = fbpool[0];
    fbpool[0] = fbpool[top];
    fbpool[top] = swap;
    top--;
    int mybot = bottom;
    int i = mybot * 2;
    while(i <= top){
      if(i < top && fbpool[i+1].off > fbpool[i].off) i++;
      if(fbpool[mybot].off >= fbpool[i].off) break;
      swap = fbpool[mybot];
      fbpool[mybot] = fbpool[i];
      fbpool[i] = swap;
      mybot = i;
      i = mybot * 2;
    }
  }
}


/* Sort the free block pool by record size.
   `fbpool' specifies the free block pool.
   `fbpnum' specifies the number of blocks. */
static void tcfbpsortbyrsiz(HDBFB *fbpool, int fbpnum){
  assert(fbpool && fbpnum >= 0);
  fbpnum--;
  int bottom = fbpnum / 2 + 1;
  int top = fbpnum;
  while(bottom > 0){
    bottom--;
    int mybot = bottom;
    int i = mybot * 2;
    while(i <= top){
      if(i < top && fbpool[i+1].rsiz > fbpool[i].rsiz) i++;
      if(fbpool[mybot].rsiz >= fbpool[i].rsiz) break;
      HDBFB swap = fbpool[mybot];
      fbpool[mybot] = fbpool[i];
      fbpool[i] = swap;
      mybot = i;
      i = mybot * 2;
    }
  }
  while(top > 0){
    HDBFB swap = fbpool[0];
    fbpool[0] = fbpool[top];
    fbpool[top] = swap;
    top--;
    int mybot = bottom;
    int i = mybot * 2;
    while(i <= top){
      if(i < top && fbpool[i+1].rsiz > fbpool[i].rsiz) i++;
      if(fbpool[mybot].rsiz >= fbpool[i].rsiz) break;
      swap = fbpool[mybot];
      fbpool[mybot] = fbpool[i];
      fbpool[i] = swap;
      mybot = i;
      i = mybot * 2;
    }
  }
}


/* Merge successive records in the free block pool.
   `hdb' specifies the hash database object. */
static void tchdbfbpmerge(TCHDB *hdb){
  assert(hdb);
  TCDODEBUG(hdb->cnt_mergefbp++);
  tcfbpsortbyoff(hdb->fbpool, hdb->fbpnum);
  HDBFB *wp = hdb->fbpool;
  HDBFB *cur = wp;
  HDBFB *end = wp + hdb->fbpnum - 1;
  while(cur < end){
    if(cur->off > 0){
      HDBFB *next = cur + 1;
      // <MM>
      // free block间连续
      // </MM>
      if(cur->off + cur->rsiz == next->off && cur->rsiz + next->rsiz <= HDBFBMAXSIZ){
        if(hdb->dfcur == next->off) hdb->dfcur += next->rsiz;
        if(hdb->iter == next->off) hdb->iter += next->rsiz;
        cur->rsiz += next->rsiz;
        next->off = 0;
      }
      *(wp++) = *cur;
    }
    cur++;
  }
  if(end->off > 0) *(wp++) = *end;
  hdb->fbpnum = wp - (HDBFB *)hdb->fbpool;
  hdb->fbpmis = hdb->fbpnum * -1;
}


/* Insert a block into the free block pool.
   `hdb' specifies the hash database object.
   `off' specifies the offset of the block.
   `rsiz' specifies the size of the block. */
static void tchdbfbpinsert(TCHDB *hdb, uint64_t off, uint32_t rsiz){
  assert(hdb && off > 0 && rsiz > 0);
  TCDODEBUG(hdb->cnt_insertfbp++);
  hdb->dfcnt++;
  if(hdb->fpow < 1) return;
  HDBFB *pv = hdb->fbpool;
  if(hdb->fbpnum >= hdb->fbpmax * HDBFBPALWRAT){
    tchdbfbpmerge(hdb);
    tcfbpsortbyrsiz(hdb->fbpool, hdb->fbpnum);
    int diff = hdb->fbpnum - hdb->fbpmax;
    if(diff > 0){
      TCDODEBUG(hdb->cnt_reducefbp++);
      memmove(pv, pv + diff, (hdb->fbpnum - diff) * sizeof(*pv));
      hdb->fbpnum -= diff;
    }
    hdb->fbpmis = 0;
  }
  int num = hdb->fbpnum;
  int left = 0;
  int right = num;
  int i = (left + right) / 2;
  int cand = -1;
  while(right >= left && i < num){
    int rv = (int)rsiz - (int)pv[i].rsiz;
    if(rv == 0){
      cand = i;
      break;
    } else if(rv <= 0){
      cand = i;
      right = i - 1;
    } else {
      left = i + 1;
    }
    i = (left + right) / 2;
  }
  if(cand >= 0){
    pv += cand;
    memmove(pv + 1, pv, sizeof(*pv) * (num - cand));
  } else {
    pv += num;
  }
  pv->off = off;
  pv->rsiz = rsiz;
  hdb->fbpnum++;
}


/* Search the free block pool for the minimum region.
   `hdb' specifies the hash database object.
   `rec' specifies the record object to be stored.
   The return value is true if successful, else, it is false. */
static bool tchdbfbpsearch(TCHDB *hdb, TCHREC *rec){
  assert(hdb && rec);
  TCDODEBUG(hdb->cnt_searchfbp++);
  // <MM>
  // free block pool为空
  // </MM>
  if(hdb->fbpnum < 1){
    rec->off = hdb->fsiz;
    rec->rsiz = 0;
    return true;
  }
  uint32_t rsiz = rec->rsiz;
  HDBFB *pv = hdb->fbpool;
  int num = hdb->fbpnum;
  int left = 0;
  int right = num;
  int i = (left + right) / 2;
  int cand = -1;
  // <MM>
  // free block pool按照剩余空间从小到大排序
  // 此处二分查找第一个最适合该大小的block
  // </MM>
  while(right >= left && i < num){
    int rv = (int)rsiz - (int)pv[i].rsiz;
    if(rv == 0){
      cand = i;
      break;
    } else if(rv <= 0){
      cand = i;
      right = i - 1;
    } else {
      left = i + 1;
    }
    i = (left + right) / 2;
  }
  if(cand >= 0){
    // <MM>
    // 存在合适的free block pool
    // </MM>
    pv += cand;
    // <MM>
    // 在free block大小大于记录大小的2倍时，会将该block分裂
    // </MM>
    if(pv->rsiz > rsiz * 2){
      uint32_t psiz = tchdbpadsize(hdb, pv->off + rsiz);
      uint64_t noff = pv->off + rsiz + psiz;
      if(pv->rsiz >= (noff - pv->off) * 2){
        TCDODEBUG(hdb->cnt_dividefbp++);
        rec->off = pv->off;
        rec->rsiz = noff - pv->off;
        pv->off = noff;
        pv->rsiz -= rec->rsiz;
        return tchdbwritefb(hdb, pv->off, pv->rsiz);
      }
    }
    rec->off = pv->off;
    rec->rsiz = pv->rsiz;
    // <MM>
    // 更新free block pool
    // 大数组的copy
    // </MM>
    memmove(pv, pv + 1, sizeof(*pv) * (num - cand - 1));
    hdb->fbpnum--;
    return true;
  }
  // <MM>
  // cand < 0意味着不存在合适大小的free block pool
  // </MM>
  rec->off = hdb->fsiz;
  rec->rsiz = 0;
  hdb->fbpmis++;
  if(hdb->fbpmis >= HDBFBPMGFREQ){
    // <MM>
    // 当查找free block pool miss超过4096次时，会尝试将free block pool
    // 中相邻的block合并
    // </MM>
    tchdbfbpmerge(hdb);
    tcfbpsortbyrsiz(hdb->fbpool, hdb->fbpnum);
  }
  return true;
}


/* Splice the trailing free block
   `hdb' specifies the hash database object.
   `rec' specifies the record object to be stored.
   `nsiz' specifies the needed size.
   The return value is whether splicing succeeded or not. */
static bool tchdbfbpsplice(TCHDB *hdb, TCHREC *rec, uint32_t nsiz){
  assert(hdb && rec && nsiz > 0);
  if(hdb->mmtx){
    if(hdb->fbpnum < 1) return false;
    // <MM>
    // 有free block的情况，找到record随后临近的文件块
    // </MM>
    uint64_t off = rec->off + rec->rsiz;
    uint32_t rsiz = rec->rsiz;
    uint8_t magic;
    // <MM>
    // 必须保证此record临近的文件块必须是free block
    // </MM>
    if(tchdbseekreadtry(hdb, off, &magic, sizeof(magic)) && magic != HDBMAGICFB) return false;
    HDBFB *pv = hdb->fbpool;
    HDBFB *ep = pv + hdb->fbpnum;
    while(pv < ep){
      // <MM>
      // 遍历free block pool找到off对应的文件块
      // fbp按照剩余大小排序，此处按地址查找，只能遍历
      // </MM>
      if(pv->off == off && rsiz + pv->rsiz >= nsiz){
        // <MM>
        // 找到合适的文件块，使用它，并从fbp中删除
        // </MM>
        if(hdb->dfcur == pv->off) hdb->dfcur += pv->rsiz;
        if(hdb->iter == pv->off) hdb->iter += pv->rsiz;
        rec->rsiz += pv->rsiz;
        memmove(pv, pv + 1, sizeof(*pv) * ((ep - pv) - 1));
        hdb->fbpnum--;
        return true;
      }
      pv++;
    }
    return false;
  }
  // <MM>
  // 下列情况下
  //    - free block pool为空
  //    - record随后临近的文件块无法合并成单独的文件块
  // </MM>
  uint64_t off = rec->off + rec->rsiz;
  TCHREC nrec;
  char nbuf[HDBIOBUFSIZ];
  // <MM>
  // 在此record和下一个reocrd间有多个free block的情况
  // 会将这些block合并使用
  // </MM>
  while(off < hdb->fsiz){
    // <MM>
    // 找到下一个record
    // </MM>
    nrec.off = off;
    if(!tchdbreadrec(hdb, &nrec, nbuf)) return false;
    if(nrec.magic != HDBMAGICFB) break;
    if(hdb->dfcur == off) hdb->dfcur += nrec.rsiz;
    if(hdb->iter == off) hdb->iter += nrec.rsiz;
    off += nrec.rsiz;
  }
  uint32_t jsiz = off - rec->off;
  if(jsiz < nsiz) return false;
  rec->rsiz = jsiz;
  uint64_t base = rec->off;
  HDBFB *wp = hdb->fbpool;
  HDBFB *cur = wp;
  HDBFB *end = wp + hdb->fbpnum;
  // <MM>
  // 将待合并使用的多个free block从fbp中删除
  // </MM>
  while(cur < end){
    if(cur->off < base || cur->off > off) *(wp++) = *cur;
    cur++;
  }
  hdb->fbpnum = wp - (HDBFB *)hdb->fbpool;
  // <MM>
  // 分配的block(padding后)是记录大小的2倍以上时，
  // 会将空想的部分分裂，重新生成free block插入fbp
  //
  // 此时，分配给该record的文件块大小正好和记录大小合适
  // 而如果文件块小于2倍时，就会有最大接近一倍的空余空间
  // 为什么这么处理？
  // </MM>
  if(jsiz > nsiz * 2){
    uint32_t psiz = tchdbpadsize(hdb, rec->off + nsiz);
    uint64_t noff = rec->off + nsiz + psiz;
    if(jsiz >= (noff - rec->off) * 2){
      TCDODEBUG(hdb->cnt_dividefbp++);
      nsiz = off - noff;
      if(!tchdbwritefb(hdb, noff, nsiz)) return false;
      rec->rsiz = noff - rec->off;
      tchdbfbpinsert(hdb, noff, nsiz);
    }
  }
  return true;
}


/* Remove blocks of a region from the free block pool.
   `hdb' specifies the hash database object.
   `base' specifies the base offset of the region.
   `next' specifies the offset of the next region.
   `off' specifies the offset of the block.
   `rsiz' specifies the size of the block. */
static void tchdbfbptrim(TCHDB *hdb, uint64_t base, uint64_t next, uint64_t off, uint32_t rsiz){
  assert(hdb && base > 0 && next > 0);
  if(hdb->fpow < 1) return;
  if(hdb->fbpnum < 1){
    if(off > 0){
      HDBFB *fbpool = hdb->fbpool;
      fbpool->off = off;
      fbpool->rsiz = rsiz;
      hdb->fbpnum = 1;
    }
    return;
  }
  HDBFB *wp = hdb->fbpool;
  HDBFB *cur = wp;
  HDBFB *end = wp + hdb->fbpnum;
  if(hdb->fbpnum >= hdb->fbpmax * HDBFBPALWRAT) cur++;
  while(cur < end){
    if(cur->rsiz >= rsiz && off > 0){
      TCDODEBUG(hdb->cnt_insertfbp++);
      wp->off = off;
      wp->rsiz = rsiz;
      wp++;
      off = 0;
    } else if(cur->off < base || cur->off >= next){
      *(wp++) = *cur;
    }
    cur++;
  }
  if(off > 0){
    TCDODEBUG(hdb->cnt_insertfbp++);
    wp->off = off;
    wp->rsiz = rsiz;
    wp++;
    off = 0;
  }
  hdb->fbpnum = wp - (HDBFB *)hdb->fbpool;
}


/* Write a free block into the file.
   `hdb' specifies the hash database object.
   `off' specifies the offset of the block.
   `rsiz' specifies the size of the block.
   The return value is true if successful, else, it is false. */
static bool tchdbwritefb(TCHDB *hdb, uint64_t off, uint32_t rsiz){
  assert(hdb && off > 0 && rsiz > 0);
  char rbuf[HDBMAXHSIZ];
  char *wp = rbuf;
  *(uint8_t *)(wp++) = HDBMAGICFB;
  uint32_t lnum = TCHTOIL(rsiz);
  memcpy(wp, &lnum, sizeof(lnum));
  wp += sizeof(lnum);
  if(!tchdbseekwrite(hdb, off, rbuf, wp - rbuf)) return false;
  return true;
}


/* Write a record into the file.
   `hdb' specifies the hash database object.
   `rec' specifies the record object.
   `bidx' specifies the index of the bucket.
   `entoff' specifies the offset of the tree entry.
   The return value is true if successful, else, it is false. */
static bool tchdbwriterec(TCHDB *hdb, TCHREC *rec, uint64_t bidx, off_t entoff){
  // <MM>
  // 对于记录部分重用没有优化，比如key覆盖时header和key不变，只有value变化，
  // 在写磁盘时，应该只更新value部分
  //
  // 实际上，为了统一化处理，放弃了这部分优化。同时，由于header加key的大小应该
  // 不会很大，所以io量比较小，优化的提升也就比较小
  // </MM>
  assert(hdb && rec);
  TCDODEBUG(hdb->cnt_writerec++);
  char stack[HDBIOBUFSIZ];
  int bsiz = (rec->rsiz > 0) ? rec->rsiz : HDBMAXHSIZ + rec->ksiz + rec->vsiz + hdb->align;
  char *rbuf;
  // <MM>
  // 通过栈上分配内存，以避免动态在堆上内存分配的开销
  // </MM>
  if(bsiz <= HDBIOBUFSIZ){
    rbuf = stack;
  } else {
    TCMALLOC(rbuf, bsiz);
  }
  char *wp = rbuf;
  // <MM>
  // 文件块的header，标识此块是一条记录
  // </MM>
  *(uint8_t *)(wp++) = HDBMAGICREC;
  *(uint8_t *)(wp++) = rec->hash;
  // <MM>
  // 序列化left、right
  // </MM>
  if(hdb->ba64){
    uint64_t llnum;
    llnum = rec->left >> hdb->apow;
    llnum = TCHTOILL(llnum);
    memcpy(wp, &llnum, sizeof(llnum));
    wp += sizeof(llnum);
    llnum = rec->right >> hdb->apow;
    llnum = TCHTOILL(llnum);
    memcpy(wp, &llnum, sizeof(llnum));
    wp += sizeof(llnum);
  } else {
    uint32_t lnum;
    lnum = rec->left >> hdb->apow;
    lnum = TCHTOIL(lnum);
    memcpy(wp, &lnum, sizeof(lnum));
    wp += sizeof(lnum);
    lnum = rec->right >> hdb->apow;
    lnum = TCHTOIL(lnum);
    memcpy(wp, &lnum, sizeof(lnum));
    wp += sizeof(lnum);
  }
  uint16_t snum;
  char *pwp = wp;
  // <MM>
  // padding size，即文件块中record没有使用的部分
  // </MM>
  wp += sizeof(snum);
  int step;
  TCSETVNUMBUF(step, wp, rec->ksiz);
  wp += step;
  TCSETVNUMBUF(step, wp, rec->vsiz);
  wp += step;
  // <MM>
  // header size
  // </MM>
  int32_t hsiz = wp - rbuf;
  int32_t rsiz = hsiz + rec->ksiz + rec->vsiz;
  // <MM>
  // finc: 数据文件需新增的大小
  // </MM>
  int32_t finc = 0;
  if(rec->rsiz < 1){
    // <MW>
    // 什么情况下，走这个分支？
    //
    // free block?
    // </MW>
    uint16_t psiz = tchdbpadsize(hdb, hdb->fsiz + rsiz);
    rec->rsiz = rsiz + psiz;
    rec->psiz = psiz;
    finc = rec->rsiz;
  } else if(rsiz > rec->rsiz){
    // <MM>
    // 当需要对现有key进行追加时，走此分支，会导致记录大小变大
    // overwrite时，记录的value比现有记录大
    // </MM>
    if(rbuf != stack) TCFREE(rbuf);
    if(!HDBLOCKDB(hdb)) return false;
    // <MM>
    // 首先，尝试将当前record与后续的free block进行拼接
    // 可以拼接1个或多个free block
    // 如果拼接成功，会更新rec->rsiz为最小文件块的大小
    // </MM>
    if(tchdbfbpsplice(hdb, rec, rsiz)){
      TCDODEBUG(hdb->cnt_splicefbp++);
      // <MM>
      // 拼接成功，文件块可以容纳该记录，重新调用进行记录的写入
      // </MM>
      bool rv = tchdbwriterec(hdb, rec, bidx, entoff);
      HDBUNLOCKDB(hdb);
      return rv;
    }
    TCDODEBUG(hdb->cnt_moverec++);
    // <MM>
    // 文件块拼接失败，会进行record的移动
    //  - 将记录文件块更新为free block
    //  - 将该free block插入fbp
    // </MM>
    if(!tchdbwritefb(hdb, rec->off, rec->rsiz)){
      HDBUNLOCKDB(hdb);
      return false;
    }
    tchdbfbpinsert(hdb, rec->off, rec->rsiz);
    // <MM>
    // 按照记录大小查找fbp
    // </MM>
    rec->rsiz = rsiz;
    if(!tchdbfbpsearch(hdb, rec)){
      HDBUNLOCKDB(hdb);
      return false;
    }
    // <MM>
    // 空间分配成功，重新递归调用方法写入记录
    // </MM>
    bool rv = tchdbwriterec(hdb, rec, bidx, entoff);
    HDBUNLOCKDB(hdb);
    return rv;
  } else {
    // <MM>
    // record当前文件块可以容纳整条记录的情况
    // </MM>
    TCDODEBUG(hdb->cnt_reuserec++);
    uint32_t psiz = rec->rsiz - rsiz;
    if(psiz > UINT16_MAX){
      // <MM>
      // 当前文件块剩余大小大于65536时，需要进行文件块拆分
      // </MM>
      TCDODEBUG(hdb->cnt_dividefbp++);
      psiz = tchdbpadsize(hdb, rec->off + rsiz);
      uint64_t noff = rec->off + rsiz + psiz;
      uint32_t nsiz = rec->rsiz - rsiz - psiz;
      rec->rsiz = noff - rec->off;
      rec->psiz = psiz;
      if(!tchdbwritefb(hdb, noff, nsiz)){
        if(rbuf != stack) TCFREE(rbuf);
        return false;
      }
      if(!HDBLOCKDB(hdb)){
        if(rbuf != stack) TCFREE(rbuf);
        return false;
      }
      tchdbfbpinsert(hdb, noff, nsiz);
      HDBUNLOCKDB(hdb);
    }
    rec->psiz = psiz;
  }
  snum = rec->psiz;
  snum = TCHTOIS(snum);
  memcpy(pwp, &snum, sizeof(snum));
  rsiz = rec->rsiz;
  rsiz -= hsiz;
  memcpy(wp, rec->kbuf, rec->ksiz);
  wp += rec->ksiz;
  rsiz -= rec->ksiz;
  memcpy(wp, rec->vbuf, rec->vsiz);
  wp += rec->vsiz;
  rsiz -= rec->vsiz;
  // <MM>
  // 将剩余部分清零
  // </MM>
  memset(wp, 0, rsiz);
  if(!tchdbseekwrite(hdb, rec->off, rbuf, rec->rsiz)){
    if(rbuf != stack) TCFREE(rbuf);
    return false;
  }
  if(finc != 0){
    hdb->fsiz += finc;
    uint64_t llnum = hdb->fsiz;
    llnum = TCHTOILL(llnum);
    memcpy(hdb->map + HDBFSIZOFF, &llnum, sizeof(llnum));
  }
  if(rbuf != stack) TCFREE(rbuf);
  // <MM>
  // entry offset是新增记录的前驱记录对应的left或right指针偏移量，需要更新为记录的偏移量
  // 即新增记录parent node的left或right的指针偏移量
  // </MM>
  if(entoff > 0){
    if(hdb->ba64){
      // <MW>
      // 为什么需要将offset右移4bit
      //
      // 记录的offset都是按照16Byte对齐的，在读取时会进行左移4bit
      // </MW>
      uint64_t llnum = rec->off >> hdb->apow;
      llnum = TCHTOILL(llnum);
      if(!tchdbseekwrite(hdb, entoff, &llnum, sizeof(uint64_t))) return false;
    } else {
      uint32_t lnum = rec->off >> hdb->apow;
      lnum = TCHTOIL(lnum);
      if(!tchdbseekwrite(hdb, entoff, &lnum, sizeof(uint32_t))) return false;
    }
  } else {
    // <MM>
    // 新建的bucket，需要设置bucket array
    // </MM>
    tchdbsetbucket(hdb, bidx, rec->off);
  }
  return true;
}


/* Read a record from the file.
   `hdb' specifies the hash database object.
   `rec' specifies the record object.
   `rbuf' specifies the buffer for reading.
   The return value is true if successful, else, it is false. */
static bool tchdbreadrec(TCHDB *hdb, TCHREC *rec, char *rbuf){
  assert(hdb && rec && rbuf);
  TCDODEBUG(hdb->cnt_readrec++);
  int rsiz = hdb->runit;
  if(!tchdbseekreadtry(hdb, rec->off, rbuf, rsiz)){
    if(!HDBLOCKDB(hdb)) return false;
    rsiz = hdb->fsiz - rec->off;
    if(rsiz > hdb->runit){
      rsiz = hdb->runit;
    } else if(rsiz < (int)(sizeof(uint8_t) + sizeof(uint32_t))){
      tchdbsetecode(hdb, TCERHEAD, __FILE__, __LINE__, __func__);
      HDBUNLOCKDB(hdb);
      return false;
    }
    if(!tchdbseekread(hdb, rec->off, rbuf, rsiz)){
      HDBUNLOCKDB(hdb);
      return false;
    }
    HDBUNLOCKDB(hdb);
  }
  // <MM>
  // 完成record header读取
  // </MM>
  const char *rp = rbuf;
  rec->magic = *(uint8_t *)(rp++);
  if(rec->magic == HDBMAGICFB){
    // <MW>
    // 什么情况下是free block?
    //
    // free block也是一个记录，在tchdbwritefb会进行free block
    // 的写入
    // </MW>
    uint32_t lnum;
    memcpy(&lnum, rp, sizeof(lnum));
    rec->rsiz = TCITOHL(lnum);
    return true;
  } else if(rec->magic != HDBMAGICREC){
    tchdbsetecode(hdb, TCERHEAD, __FILE__, __LINE__, __func__);
    return false;
  }
  rec->hash = *(uint8_t *)(rp++);
  if(hdb->ba64){
    uint64_t llnum;
    memcpy(&llnum, rp, sizeof(llnum));
    rec->left = TCITOHLL(llnum) << hdb->apow;
    rp += sizeof(llnum);
    memcpy(&llnum, rp, sizeof(llnum));
    rec->right = TCITOHLL(llnum) << hdb->apow;
    rp += sizeof(llnum);
  } else {
    uint32_t lnum;
    memcpy(&lnum, rp, sizeof(lnum));
    rec->left = (uint64_t)TCITOHL(lnum) << hdb->apow;
    rp += sizeof(lnum);
    memcpy(&lnum, rp, sizeof(lnum));
    rec->right = (uint64_t)TCITOHL(lnum) << hdb->apow;
    rp += sizeof(lnum);
  }
  uint16_t snum;
  memcpy(&snum, rp, sizeof(snum));
  rec->psiz = TCITOHS(snum);
  rp += sizeof(snum);
  uint32_t lnum;
  int step;
  TCREADVNUMBUF(rp, lnum, step);
  rec->ksiz = lnum;
  rp += step;
  TCREADVNUMBUF(rp, lnum, step);
  rec->vsiz = lnum;
  rp += step;
  int32_t hsiz = rp - rbuf;
  rec->rsiz = hsiz + rec->ksiz + rec->vsiz + rec->psiz;
  rec->kbuf = NULL;
  rec->vbuf = NULL;
  rec->boff = rec->off + hsiz;
  rec->bbuf = NULL;
  rsiz -= hsiz;
  // <MM>
  // 优化：针对record整体大小小于48B。
  //    如果已读取buf的剩余大小大于key size或value size，则顺便更新record的kbuf和vbuf
  // </MM>
  if(rsiz >= rec->ksiz){
    rec->kbuf = rp;
    rsiz -= rec->ksiz;
    rp += rec->ksiz;
    if(rsiz >= rec->vsiz) rec->vbuf = rp;
  }
  return true;
}


/* Read the body of a record from the file.
   `hdb' specifies the hash database object.
   `rec' specifies the record object.
   The return value is true if successful, else, it is false. */
static bool tchdbreadrecbody(TCHDB *hdb, TCHREC *rec){
  assert(hdb && rec);
  int32_t bsiz = rec->ksiz + rec->vsiz;
  TCMALLOC(rec->bbuf, bsiz + 1);
  if(!tchdbseekread(hdb, rec->boff, rec->bbuf, bsiz)) return false;
  rec->kbuf = rec->bbuf;
  rec->vbuf = rec->bbuf + rec->ksiz;
  return true;
}


/* Remove a record from the file.
   `hdb' specifies the hash database object.
   `rec' specifies the record object.
   `rbuf' specifies the buffer for reading.
   `bidx' specifies the index of the bucket.
   `entoff' specifies the offset of the tree entry.
   The return value is true if successful, else, it is false. */
static bool tchdbremoverec(TCHDB *hdb, TCHREC *rec, char *rbuf, uint64_t bidx, off_t entoff){
  assert(hdb && rec);
  // <MM>
  // 将record对应的文件块转换为free block
  // </MM>
  if(!tchdbwritefb(hdb, rec->off, rec->rsiz)) return false;
  if(!HDBLOCKDB(hdb)) return false;
  // <MM>
  // 插入fbp
  // </MM>
  tchdbfbpinsert(hdb, rec->off, rec->rsiz);
  HDBUNLOCKDB(hdb);
  uint64_t child;
  if(rec->left > 0 && rec->right < 1){
    child = rec->left;
  } else if(rec->left < 1 && rec->right > 0){
    child = rec->right;
  } else if(rec->left < 1){
    child = 0;
  } else {
    child = rec->left;
    uint64_t right = rec->right;
    rec->right = child;
    while(rec->right > 0){
      rec->off = rec->right;
      if(!tchdbreadrec(hdb, rec, rbuf)) return false;
    }
    if(hdb->ba64){
      off_t toff = rec->off + (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint64_t));
      uint64_t llnum = right >> hdb->apow;
      llnum = TCHTOILL(llnum);
      if(!tchdbseekwrite(hdb, toff, &llnum, sizeof(uint64_t))) return false;
    } else {
      off_t toff = rec->off + (sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t));
      uint32_t lnum = right >> hdb->apow;
      lnum = TCHTOIL(lnum);
      if(!tchdbseekwrite(hdb, toff, &lnum, sizeof(uint32_t))) return false;
    }
  }
  if(entoff > 0){
    if(hdb->ba64){
      uint64_t llnum = child >> hdb->apow;
      llnum = TCHTOILL(llnum);
      if(!tchdbseekwrite(hdb, entoff, &llnum, sizeof(uint64_t))) return false;
    } else {
      uint32_t lnum = child >> hdb->apow;
      lnum = TCHTOIL(lnum);
      if(!tchdbseekwrite(hdb, entoff, &lnum, sizeof(uint32_t))) return false;
    }
  } else {
    tchdbsetbucket(hdb, bidx, child);
  }
  if(!HDBLOCKDB(hdb)) return false;
  hdb->rnum--;
  uint64_t llnum = hdb->rnum;
  llnum = TCHTOILL(llnum);
  memcpy(hdb->map + HDBRNUMOFF, &llnum, sizeof(llnum));
  HDBUNLOCKDB(hdb);
  return true;
}


/* Remove a record from the file.
   `hdb' specifies the hash database object.
   `rec' specifies the record object.
   `rbuf' specifies the buffer for reading.
   `destoff' specifies the offset of the destination.
   The return value is true if successful, else, it is false. */
static bool tchdbshiftrec(TCHDB *hdb, TCHREC *rec, char *rbuf, off_t destoff){
  assert(hdb && rec && rbuf && destoff >= 0);
  TCDODEBUG(hdb->cnt_shiftrec++);
  if(!rec->vbuf && !tchdbreadrecbody(hdb, rec)) return false;
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, rec->kbuf, rec->ksiz, &hash);
  off_t off = tchdbgetbucket(hdb, bidx);
  if(rec->off == off){
    bool err = false;
    rec->off = destoff;
    if(!tchdbwriterec(hdb, rec, bidx, 0)) err = true;
    TCFREE(rec->bbuf);
    rec->kbuf = NULL;
    rec->vbuf = NULL;
    rec->bbuf = NULL;
    return !err;
  }
  TCHREC trec;
  char tbuf[HDBIOBUFSIZ];
  char *bbuf = rec->bbuf;
  const char *kbuf = rec->kbuf;
  int ksiz = rec->ksiz;
  const char *vbuf = rec->vbuf;
  int vsiz = rec->vsiz;
  rec->kbuf = NULL;
  rec->vbuf = NULL;
  rec->bbuf = NULL;
  off_t entoff = 0;
  while(off > 0){
    trec.off = off;
    if(!tchdbreadrec(hdb, &trec, tbuf)){
      TCFREE(bbuf);
      return false;
    }
    if(hash > trec.hash){
      off = trec.left;
      entoff = trec.off + (sizeof(uint8_t) + sizeof(uint8_t));
    } else if(hash < trec.hash){
      off = trec.right;
      entoff = trec.off + (sizeof(uint8_t) + sizeof(uint8_t)) +
        (hdb->ba64 ? sizeof(uint64_t) : sizeof(uint32_t));
    } else {
      if(!trec.kbuf && !tchdbreadrecbody(hdb, &trec)){
        TCFREE(bbuf);
        return false;
      }
      int kcmp = tcreckeycmp(kbuf, ksiz, trec.kbuf, trec.ksiz);
      if(kcmp > 0){
        off = trec.left;
        TCFREE(trec.bbuf);
        trec.kbuf = NULL;
        trec.bbuf = NULL;
        entoff = trec.off + (sizeof(uint8_t) + sizeof(uint8_t));
      } else if(kcmp < 0){
        off = trec.right;
        TCFREE(trec.bbuf);
        trec.kbuf = NULL;
        trec.bbuf = NULL;
        entoff = trec.off + (sizeof(uint8_t) + sizeof(uint8_t)) +
          (hdb->ba64 ? sizeof(uint64_t) : sizeof(uint32_t));
      } else {
        TCFREE(trec.bbuf);
        trec.bbuf = NULL;
        bool err = false;
        rec->off = destoff;
        rec->kbuf = kbuf;
        rec->ksiz = ksiz;
        rec->vbuf = vbuf;
        rec->vsiz = vsiz;
        if(!tchdbwriterec(hdb, rec, bidx, entoff)) err = true;
        TCFREE(bbuf);
        return !err;
      }
    }
  }
  TCFREE(bbuf);
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return false;
}


/* Compare keys of two records.
   `abuf' specifies the pointer to the region of the former.
   `asiz' specifies the size of the region.
   `bbuf' specifies the pointer to the region of the latter.
   `bsiz' specifies the size of the region.
   The return value is 0 if two equals, positive if the formar is big, else, negative. */
static int tcreckeycmp(const char *abuf, int asiz, const char *bbuf, int bsiz){
  assert(abuf && asiz >= 0 && bbuf && bsiz >= 0);
  if(asiz > bsiz) return 1;
  if(asiz < bsiz) return -1;
  return memcmp(abuf, bbuf, asiz);
}


/* Flush the delayed record pool.
   `hdb' specifies the hash database object.
   The return value is true if successful, else, it is false. */
static bool tchdbflushdrp(TCHDB *hdb){
  assert(hdb);
  if(!HDBLOCKDB(hdb)) return false;
  if(!hdb->drpool){
    HDBUNLOCKDB(hdb);
    return true;
  }
  TCDODEBUG(hdb->cnt_flushdrp++);
  // <MM>
  // hdb->drpool中存储的是已经按照正确格式序列化后的结果
  // 所以此处直接拷贝
  // hdb->drpoff是初次执行async put时的file size。这里直接将drpool中的内容拷贝至对应的文件块
  // </MM>
  if(!tchdbseekwrite(hdb, hdb->drpoff, TCXSTRPTR(hdb->drpool), TCXSTRSIZE(hdb->drpool))){
    HDBUNLOCKDB(hdb);
    return false;
  }
  const char *rp = TCXSTRPTR(hdb->drpdef);
  int size = TCXSTRSIZE(hdb->drpdef);
  // <MM>
  // hdb->drpdef是字节数组，按照|key size|val size|key|val|格式
  // 将每条记录序列化，此处迭代每条记录并最终写入磁盘
  // </MM>
  while(size > 0){
    int ksiz, vsiz;
    memcpy(&ksiz, rp, sizeof(int));
    rp += sizeof(int);
    memcpy(&vsiz, rp, sizeof(int));
    rp += sizeof(int);
    const char *kbuf = rp;
    rp += ksiz;
    const char *vbuf = rp;
    rp += vsiz;
    uint8_t hash;
    uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
    if(!tchdbputimpl(hdb, kbuf, ksiz, bidx, hash, vbuf, vsiz, HDBPDOVER)){
      tcxstrdel(hdb->drpdef);
      tcxstrdel(hdb->drpool);
      hdb->drpool = NULL;
      hdb->drpdef = NULL;
      hdb->drpoff = 0;
      HDBUNLOCKDB(hdb);
      return false;
    }
    size -= sizeof(int) * 2 + ksiz + vsiz;
  }
  tcxstrdel(hdb->drpdef);
  tcxstrdel(hdb->drpool);
  // <MM>
  // 每次flush之后会重置delayed record pool
  // </MM>
  hdb->drpool = NULL;
  hdb->drpdef = NULL;
  hdb->drpoff = 0;
  uint64_t llnum;
  llnum = hdb->rnum;
  llnum = TCHTOILL(llnum);
  memcpy(hdb->map + HDBRNUMOFF, &llnum, sizeof(llnum));
  llnum = hdb->fsiz;
  llnum = TCHTOILL(llnum);
  memcpy(hdb->map + HDBFSIZOFF, &llnum, sizeof(llnum));
  HDBUNLOCKDB(hdb);
  return true;
}


/* Adjust the caches for leaves and nodes.
   `hdb' specifies the hash tree database object. */
static void tchdbcacheadjust(TCHDB *hdb){
  assert(hdb);
  TCDODEBUG(hdb->cnt_adjrecc++);
  tcmdbcutfront(hdb->recc, HDBCACHEOUT);
}


/* Initialize the write ahead logging file.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbwalinit(TCHDB *hdb){
  assert(hdb);
  if(lseek(hdb->walfd, 0, SEEK_SET) == -1){
    tchdbsetecode(hdb, TCESEEK, __FILE__, __LINE__, __func__);
    return false;
  }
  if(ftruncate(hdb->walfd, 0) == -1){
    tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
    return false;
  }
  uint64_t llnum = hdb->fsiz;
  llnum = TCHTOILL(llnum);
  if(!tcwrite(hdb->walfd, &llnum, sizeof(llnum))){
    tchdbsetecode(hdb, TCEWRITE, __FILE__, __LINE__, __func__);
    return false;
  }
  hdb->walend = hdb->fsiz;
  if(!tchdbwalwrite(hdb, 0, HDBHEADSIZ)) return false;
  return true;
}


// <MM>
// 将db数据文件的一个文件块写入wal日志，用于transaction回滚时，恢复数据文件
// </MM>
/* Write an event into the write ahead logging file.
   `hdb' specifies the hash database object.
   `off' specifies the offset of the region to be updated.
   `size' specifies the size of the region.
   If successful, the return value is true, else, it is false. */
static bool tchdbwalwrite(TCHDB *hdb, uint64_t off, int64_t size){
  // <MM>
  // wal日志格式：|-- offset(8B) --|-- size(8B) --|-- content(size Bytes) --|
  //  offset和size: 指定了db文件的一个文件块
  //  content: 文件块的内容
  // </MM>
  assert(hdb && off >= 0 && size >= 0);
  if(off + size > hdb->walend) size = hdb->walend - off;
  if(size < 1) return true;
  char stack[HDBIOBUFSIZ];
  char *buf;
  if(size + sizeof(off) + sizeof(size) <= HDBIOBUFSIZ){
    buf = stack;
  } else {
    TCMALLOC(buf, size + sizeof(off) + sizeof(size));
  }
  char *wp = buf;
  int64_t llnum = TCHTOILL(off);
  memcpy(wp, &llnum, sizeof(llnum));
  wp += sizeof(llnum);
  uint32_t lnum = TCHTOIL(size);
  memcpy(wp, &lnum, sizeof(lnum));
  wp += sizeof(lnum);
  // <MW>
  // 为什么要再读一次文件?
  //
  // 虽然刚写磁盘，很大概率命中page cache
  //
  // --> 想错了，wal中存的是记录添加前的内容，以便回滚，删除该记录时可以恢复
  //
  // </MW>
  if(!tchdbseekread(hdb, off, wp, size)){
    if(buf != stack) TCFREE(buf);
    return false;
  }
  wp += size;
  if(!HDBLOCKWAL(hdb)) return false;
  if(!tcwrite(hdb->walfd, buf, wp - buf)){
    tchdbsetecode(hdb, TCEWRITE, __FILE__, __LINE__, __func__);
    if(buf != stack) TCFREE(buf);
    HDBUNLOCKWAL(hdb);
    return false;
  }
  if(buf != stack) TCFREE(buf);
  if((hdb->omode & HDBOTSYNC) && fsync(hdb->walfd) == -1){
    tchdbsetecode(hdb, TCESYNC, __FILE__, __LINE__, __func__);
    HDBUNLOCKWAL(hdb);
    return false;
  }
  HDBUNLOCKWAL(hdb);
  return true;
}


/* Restore the database from the write ahead logging file.
   `hdb' specifies the hash database object.
   `path' specifies the path of the database file.
   If successful, the return value is true, else, it is false. */
static int tchdbwalrestore(TCHDB *hdb, const char *path){
  assert(hdb && path);
  // <MM>
  // wal的文件路径
  //
  // 作为hdb的一个属性岂不是更好
  // </MM>
  char *tpath = tcsprintf("%s%c%s", path, MYEXTCHR, HDBWALSUFFIX);
  // <MM>
  // walfd不是已经存在，并打开了么?
  //
  // 打开hdb文件时，也会恢复wal，那时wal文件还没有打开
  // </MM>
  int walfd = open(tpath, O_RDONLY, HDBFILEMODE);
  TCFREE(tpath);
  // <MM>
  // wal文件打开失败
  // </MM>
  if(walfd < 0) return false;
  bool err = false;
  uint64_t walsiz = 0;
  struct stat sbuf;
  if(fstat(walfd, &sbuf) == 0){
    walsiz = sbuf.st_size;
  } else {
    tchdbsetecode(hdb, TCESTAT, __FILE__, __LINE__, __func__);
    err = true;
  }
  if(walsiz >= sizeof(walsiz) + HDBHEADSIZ){
    int dbfd = hdb->fd;
    int tfd = -1;
    if(!(hdb->omode & HDBOWRITER)){
      // <MM>
      // 非写方式打开
      // </MM>
      tfd = open(path, O_WRONLY, HDBFILEMODE);
      if(tfd >= 0){
        dbfd = tfd;
      } else {
        int ecode = TCEOPEN;
        switch(errno){
          case EACCES: ecode = TCENOPERM; break;
          case ENOENT: ecode = TCENOFILE; break;
          case ENOTDIR: ecode = TCENOFILE; break;
        }
        tchdbsetecode(hdb, ecode, __FILE__, __LINE__, __func__);
        err = true;
      }
    }
    // <MM>
    // WAL文件格式：
    // | DB数据文件的大小 | log1 | log2 | ... | logn|
    //
    // log的内容为新增记录在文件中的文件块的位置及内容
    // </MM>
    uint64_t fsiz = 0;
    if(tcread(walfd, &fsiz, sizeof(fsiz))){
      fsiz = TCITOHLL(fsiz);
    } else {
      tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
      err = true;
    }
    TCLIST *list = tclistnew();
    uint64_t waloff = sizeof(fsiz);
    char stack[HDBIOBUFSIZ];
    // 遍历wal将记录组织成list
    while(waloff < walsiz){
      // 每个记录的offset不是连续的，所以每个记录有独立的offset?
      uint64_t off;
      uint32_t size;
      if(!tcread(walfd, stack, sizeof(off) + sizeof(size))){
        tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
        err = true;
        break;
      }
      memcpy(&off, stack, sizeof(off));
      off = TCITOHLL(off);
      memcpy(&size, stack + sizeof(off), sizeof(size));
      size = TCITOHL(size);
      char *buf;
      if(sizeof(off) + size <= HDBIOBUFSIZ){
        buf = stack;
      } else {
        TCMALLOC(buf, sizeof(off) + size);
      }
      *(uint64_t *)buf = off;
      if(!tcread(walfd, buf + sizeof(off), size)){
        tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
        err = true;
        if(buf != stack) TCFREE(buf);
        break;
      }
      TCLISTPUSH(list, buf, sizeof(off) + size);
      if(buf != stack) TCFREE(buf);
      waloff += sizeof(off) + sizeof(size) + size;
    }
    size_t xmsiz = 0;
    if(hdb->fd >= 0 && hdb->map) xmsiz = (hdb->xmsiz > hdb->msiz) ? hdb->xmsiz : hdb->msiz;
    // <MM>
    // 遍历list，对每条记录写入DB文件
    // list中每条WAL日志记录格式：
    //      | 记录在DB文件中的offset | 具体数据记录内容 |
    // </MM>
    for(int i = TCLISTNUM(list) - 1; i >= 0; i--){
      // <MW>
      // 为什么采用WAL文件倒序进行插入
      //
      // 事务恢复时，应该按照FILO顺序恢复单个操作，即倒序将操作的undo log恢复
      // </MW>
      const char *rec;
      int size;
      TCLISTVAL(rec, list, i, size);
      uint64_t off = *(uint64_t *)rec;
      rec += sizeof(off);
      size -= sizeof(off);
      if(lseek(dbfd, off, SEEK_SET) == -1){
        tchdbsetecode(hdb, TCESEEK, __FILE__, __LINE__, __func__);
        err = true;
        break;
      }
      if(!tcwrite(dbfd, rec, size)){
        tchdbsetecode(hdb, TCEWRITE, __FILE__, __LINE__, __func__);
        err = true;
        break;
      }
      if(!TCUBCACHE && off < xmsiz){
        size = (size <= xmsiz - off) ? size : xmsiz - off;
        memcpy(hdb->map + off, rec, size);
      }
    }
    tclistdel(list);
    if(ftruncate(dbfd, fsiz) == -1){
      tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
      err = true;
    }
    if((hdb->omode & HDBOTSYNC) && fsync(dbfd) == -1){
      tchdbsetecode(hdb, TCESYNC, __FILE__, __LINE__, __func__);
      err = true;
    }
    if(tfd >= 0 && close(tfd) == -1){
      tchdbsetecode(hdb, TCECLOSE, __FILE__, __LINE__, __func__);
      err = true;
    }
  } else {
    err = true;
  }
  if(close(walfd) == -1){
    tchdbsetecode(hdb, TCECLOSE, __FILE__, __LINE__, __func__);
    err = true;
  }
  return !err;
}


/* Remove the write ahead logging file.
   `hdb' specifies the hash database object.
   `path' specifies the path of the database file.
   If successful, the return value is true, else, it is false. */
static bool tchdbwalremove(TCHDB *hdb, const char *path){
  assert(hdb && path);
  char *tpath = tcsprintf("%s%c%s", path, MYEXTCHR, HDBWALSUFFIX);
  bool err = false;
  if(unlink(tpath) == -1 && errno != ENOENT){
    tchdbsetecode(hdb, TCEUNLINK, __FILE__, __LINE__, __func__);
    err = true;
  }
  TCFREE(tpath);
  return !err;
}


/* Open a database file and connect a hash database object.
   `hdb' specifies the hash database object.
   `path' specifies the path of the database file.
   `omode' specifies the connection mode.
   If successful, the return value is true, else, it is false. */
static bool tchdbopenimpl(TCHDB *hdb, const char *path, int omode){
  assert(hdb && path);
  int mode = O_RDONLY;
  if(omode & HDBOWRITER){
    mode = O_RDWR;
    if(omode & HDBOCREAT) mode |= O_CREAT;
  }
  // <MM>
  // 打开数据文件
  // </MM>
  int fd = open(path, mode, HDBFILEMODE);
  if(fd < 0){
    int ecode = TCEOPEN;
    switch(errno){
      case EACCES: ecode = TCENOPERM; break;
      case ENOENT: ecode = TCENOFILE; break;
      case ENOTDIR: ecode = TCENOFILE; break;
    }
    tchdbsetecode(hdb, ecode, __FILE__, __LINE__, __func__);
    return false;
  }
  if(!(omode & HDBONOLCK)){
    // <MM>
    // 文件加锁
    // </MM>
    if(!tclock(fd, omode & HDBOWRITER, omode & HDBOLCKNB)){
      tchdbsetecode(hdb, TCELOCK, __FILE__, __LINE__, __func__);
      close(fd);
      return false;
    }
  }
  if((omode & HDBOWRITER) && (omode & HDBOTRUNC)){
    if(ftruncate(fd, 0) == -1){
      tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
      close(fd);
      return false;
    }
    // <MM>
    // 截断数据文件，同时清空wal
    // </MM>
    if(!tchdbwalremove(hdb, path)){
      close(fd);
      return false;
    }
  }
  // <MM>
  // 确保是普通文件，用于数据存储
  // </MM>
  struct stat sbuf;
  if(fstat(fd, &sbuf) == -1 || !S_ISREG(sbuf.st_mode)){
    tchdbsetecode(hdb, TCESTAT, __FILE__, __LINE__, __func__);
    close(fd);
    return false;
  }
  char hbuf[HDBHEADSIZ];
  if((omode & HDBOWRITER) && sbuf.st_size < 1){
    // <MM>
    // 空的数据文件，初始化header
    // </MM>
    hdb->flags = 0;
    hdb->rnum = 0;
    // <MM>
    // fpow默认为10，则fbpmax = 1 << 10 = 1024
    // 设置free block pool的最大值
    // </MM>
    uint32_t fbpmax = 1 << hdb->fpow;
    // <MM>
    // 初始情况下，free block pool占用内存大小
    // free block pool中每个free block元数据通过变长编码offset和size
    // </MM>
    uint32_t fbpsiz = HDBFBPBSIZ + fbpmax * HDBFBPESIZ;
    // <MM>
    // bucket element的大小
    // </MM>
    int besiz = (hdb->opts & HDBTLARGE) ? sizeof(int64_t) : sizeof(int32_t);
    // <MM>
    // apow默认是4，则align = 1 << 4 = 16
    // 即record按照16Byte对齐
    // </MM>
    hdb->align = 1 << hdb->apow;
    // <MM>
    // 计算数据文件大小，目前是文件的头部
    // 包括：Header + bucket array + free block pool
    // </MM>
    hdb->fsiz = HDBHEADSIZ + besiz * hdb->bnum + fbpsiz;
    // <MM>
    // 按照hdb->align（默认是16Bytes）将文件对齐，即文件大小是16n
    // </MM>
    hdb->fsiz += tchdbpadsize(hdb, hdb->fsiz);
    // <MM>
    // 接下来初始化第一条记录的offset
    // </MM>
    hdb->frec = hdb->fsiz;
    // <MM>
    // Hash DB的内存数据结构表示序列化到字节数组中，然后写入文件的header
    // 元数据包括：magic number + 数据库类型 + flags + apow + fpow + opts
    //      + bucket array number + record number + file size + offset of first record
    // </MM>
    tchdbdumpmeta(hdb, hbuf);
    bool err = false;
    // <MM>
    // 将header写入数据文件
    // </MM>
    if(!tcwrite(fd, hbuf, HDBHEADSIZ)) err = true;
    char pbuf[HDBIOBUFSIZ];
    memset(pbuf, 0, HDBIOBUFSIZ);
    uint64_t psiz = hdb->fsiz - HDBHEADSIZ;
    // <MM>
    // 将整个数据文件清零
    // 如果文件很大，这一步会很耗时?
    //      -> 数据文件初始化时才会进行清零操作，本身文件不大
    //         小于MB级
    // </MM>
    while(psiz > 0){
      if(psiz > HDBIOBUFSIZ){
        if(!tcwrite(fd, pbuf, HDBIOBUFSIZ)) err = true;
        psiz -= HDBIOBUFSIZ;
      } else {
        if(!tcwrite(fd, pbuf, psiz)) err = true;
        psiz = 0;
      }
    }
    if(err){
      tchdbsetecode(hdb, TCEWRITE, __FILE__, __LINE__, __func__);
      close(fd);
      return false;
    }
    sbuf.st_size = hdb->fsiz;
  }
  if(lseek(fd, 0, SEEK_SET) == -1){
    tchdbsetecode(hdb, TCESEEK, __FILE__, __LINE__, __func__);
    close(fd);
    return false;
  }
  // <MM>
  // 读取header二进制数据
  // </MM>
  if(!tcread(fd, hbuf, HDBHEADSIZ)){
    tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
    close(fd);
    return false;
  }
  int type = hdb->type;
  // <MM>
  // 将header中元数据加载到内存数据结构，读取options
  // </MM>
  tchdbloadmeta(hdb, hbuf);
  // <MM>
  // replay WAL，写到DB文件
  // </MM>
  if((hdb->flags & HDBFOPEN) && tchdbwalrestore(hdb, path)){
    if(lseek(fd, 0, SEEK_SET) == -1){
      tchdbsetecode(hdb, TCESEEK, __FILE__, __LINE__, __func__);
      close(fd);
      return false;
    }
    if(!tcread(fd, hbuf, HDBHEADSIZ)){
      tchdbsetecode(hdb, TCEREAD, __FILE__, __LINE__, __func__);
      close(fd);
      return false;
    }
    // <MW>
    // 为什么需要在恢复WAL文件后，再次加载元数据？
    // 貌似在WAL文件恢复时，并没有修改元数据
    //
    // 加载WAL会改变数据文件的大小
    // </MW>
    tchdbloadmeta(hdb, hbuf);
    if(!tchdbwalremove(hdb, path)){
      close(fd);
      return false;
    }
  }
  int besiz = (hdb->opts & HDBTLARGE) ? sizeof(int64_t) : sizeof(int32_t);
  // <MM>
  // 默认情况下，只会将header和bucket array放入mmap
  // </MM>
  size_t msiz = HDBHEADSIZ + hdb->bnum * besiz;
  if(!(omode & HDBONOLCK)){
    // <MM>
    // 对元数据进行校验
    // </MM>
    if(memcmp(hbuf, HDBMAGICDATA, strlen(HDBMAGICDATA)) || hdb->type != type ||
       hdb->frec < msiz + HDBFBPBSIZ || hdb->frec > hdb->fsiz || sbuf.st_size < hdb->fsiz){
      tchdbsetecode(hdb, TCEMETA, __FILE__, __LINE__, __func__);
      close(fd);
      return false;
    }
  }
  // <MM>
  // 校验压缩，保证配置的压缩实现函数必须存在
  // </MM>
  if(((hdb->opts & HDBTDEFLATE) && !_tc_deflate) ||
     ((hdb->opts & HDBTBZIP) && !_tc_bzcompress) || ((hdb->opts & HDBTEXCODEC) && !hdb->enc)){
    tchdbsetecode(hdb, TCEINVALID, __FILE__, __LINE__, __func__);
    close(fd);
    return false;
  }
  // <MM>
  // 默认情况下，hdb->xmsiz=64<<20 = 2 << 26 = 64MB
  //
  // hdb->xmsiz和hdb->msiz的关系
  // miz：header + bucket array的内存大小
  // xmiz：固定为64MB
  //
  // mmap时取max(miz, xmiz)进行
  // </MM>
  size_t xmsiz = (hdb->xmsiz > msiz) ? hdb->xmsiz : msiz;
  // <MM>
  // 在只读模式下，mmap最大为数据文件大小
  // 此时，数据完全走page cache
  // </MM>
  if(!(omode & HDBOWRITER) && xmsiz > hdb->fsiz) xmsiz = hdb->fsiz;
  // <MM>
  // 进行mmap，map的范围是[0, xmsiz)
  // </MM>
  void *map = mmap(0, xmsiz, PROT_READ | ((omode & HDBOWRITER) ? PROT_WRITE : 0),
                   MAP_SHARED, fd, 0);
  if(map == MAP_FAILED){
    tchdbsetecode(hdb, TCEMMAP, __FILE__, __LINE__, __func__);
    close(fd);
    return false;
  }
  // <MM>
  // fpow默认值为10，fbpmax默认为1024
  // </MM>
  hdb->fbpmax = 1 << hdb->fpow;
  if(omode & HDBOWRITER){
    // <MM>
    // 只有在写时，才需free block pool
    // 分配free block pool需要的内存
    //
    // 只读时不会修改文件，所有不会存在free block的操作
    // </MM>
    TCMALLOC(hdb->fbpool, hdb->fbpmax * HDBFBPALWRAT * sizeof(HDBFB));
  } else {
    hdb->fbpool = NULL;
  }
  hdb->fbpnum = 0;
  hdb->fbpmis = 0;
  hdb->async = false;
  hdb->drpool = NULL;
  hdb->drpdef = NULL;
  hdb->drpoff = 0;
  hdb->recc = (hdb->rcnum > 0) ? tcmdbnew2(hdb->rcnum * 2 + 1) : NULL;
  hdb->path = tcstrdup(path);
  hdb->fd = fd;
  hdb->omode = omode;
  hdb->dfcur = hdb->frec;
  hdb->iter = 0;
  hdb->map = map;
  hdb->msiz = msiz;
  hdb->xfsiz = 0;
  // <MM>
  // 初始化bucket array，在mmap下
  //
  // 在数据文件中，bucket array跟随在header之后
  // </MM>
  if(hdb->opts & HDBTLARGE){
    hdb->ba32 = NULL;
    hdb->ba64 = (uint64_t *)((char *)map + HDBHEADSIZ);
  } else {
    hdb->ba32 = (uint32_t *)((char *)map + HDBHEADSIZ);
    hdb->ba64 = NULL;
  }
  hdb->align = 1 << hdb->apow;
  // <MM>
  // 默认apow = 4，则align = 16
  // runit默认应该等于48
  // </MM>
  hdb->runit = tclmin(tclmax(hdb->align, HDBMINRUNIT), HDBIOBUFSIZ);
  hdb->zmode = (hdb->opts & HDBTDEFLATE) || (hdb->opts & HDBTBZIP) ||
    (hdb->opts & HDBTTCBS) || (hdb->opts & HDBTEXCODEC);
  hdb->ecode = TCESUCCESS;
  hdb->fatal = false;
  hdb->inode = (uint64_t)sbuf.st_ino;
  hdb->mtime = sbuf.st_mtime;
  hdb->dfcnt = 0;
  hdb->tran = false;
  hdb->walfd = -1;
  hdb->walend = 0;
  // <MM>
  // 写模式下，加载free block pool
  // </MM>
  if(hdb->omode & HDBOWRITER){
    bool err = false;
    // <MM>
    // 新创建的hdb，free block pool为空
    // </MM>
    if(!(hdb->flags & HDBFOPEN) && !tchdbloadfbp(hdb)) err = true;
    memset(hbuf, 0, 2);
    // <MW>
    // 为什么要在bucket array后写2个0?
    // </MW>
    if(!tchdbseekwrite(hdb, hdb->msiz, hbuf, 2)) err = true;
    if(err){
      TCFREE(hdb->path);
      TCFREE(hdb->fbpool);
      munmap(hdb->map, xmsiz);
      close(fd);
      hdb->fd = -1;
      return false;
    }
    // <MM>
    // 将hdb设置为打开状态
    // </MM>
    tchdbsetflag(hdb, HDBFOPEN, true);
  }
  return true;
}


/* Close a hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbcloseimpl(TCHDB *hdb){
  assert(hdb);
  bool err = false;
  if(hdb->recc){
    tcmdbdel(hdb->recc);
    hdb->recc = NULL;
  }
  if(hdb->omode & HDBOWRITER){
    if(!tchdbflushdrp(hdb)) err = true;
    if(hdb->tran) hdb->fbpnum = 0;
    if(!tchdbsavefbp(hdb)) err = true;
    TCFREE(hdb->fbpool);
    tchdbsetflag(hdb, HDBFOPEN, false);
  }
  if((hdb->omode & HDBOWRITER) && !tchdbmemsync(hdb, false)) err = true;
  size_t xmsiz = (hdb->xmsiz > hdb->msiz) ? hdb->xmsiz : hdb->msiz;
  if(!(hdb->omode & HDBOWRITER) && xmsiz > hdb->fsiz) xmsiz = hdb->fsiz;
  if(munmap(hdb->map, xmsiz) == -1){
    tchdbsetecode(hdb, TCEMMAP, __FILE__, __LINE__, __func__);
    err = true;
  }
  hdb->map = NULL;
  if((hdb->omode & HDBOWRITER) && ftruncate(hdb->fd, hdb->fsiz) == -1){
    tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
    err = true;
  }
  if(hdb->tran){
    if(!tchdbwalrestore(hdb, hdb->path)) err = true;
    hdb->tran = false;
  }
  if(hdb->walfd >= 0){
    if(close(hdb->walfd) == -1){
      tchdbsetecode(hdb, TCECLOSE, __FILE__, __LINE__, __func__);
      err = true;
    }
    if(!hdb->fatal && !tchdbwalremove(hdb, hdb->path)) err = true;
  }
  if(close(hdb->fd) == -1){
    tchdbsetecode(hdb, TCECLOSE, __FILE__, __LINE__, __func__);
    err = true;
  }
  TCFREE(hdb->path);
  hdb->path = NULL;
  hdb->fd = -1;
  return !err;
}


/* Store a record.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `bidx' specifies the index of the bucket array.
   `hash' specifies the hash value for the collision tree.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   `dmode' specifies behavior when the key overlaps.
   If successful, the return value is true, else, it is false. */
static bool tchdbputimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash,
                         const char *vbuf, int vsiz, int dmode){
  assert(hdb && kbuf && ksiz >= 0);
  // <MM>
  // 从内存cache删除该记录
  //
  // 为什么不直接put，进行覆盖
  // </MM>
  if(hdb->recc) tcmdbout(hdb->recc, kbuf, ksiz);
  // <MW>
  // bucket array何时初始化的？
  //
  // 在第一次创建DB时，会将所有的bucket清零，即当前所有bucket为空
  //
  // 第一次创建后，off应该等于0，即该bucket为空
  // </MW>
  off_t off = tchdbgetbucket(hdb, bidx);
  // <MM>
  // entry offset
  //
  // 指向要加入的记录的前一个记录的left或right，用于对其进行更新
  //
  // 在bucket插入节点时，用于指向已有节点的指针的偏移量
  // 会将插入节点的offset赋值给entoff
  // </MM>
  off_t entoff = 0;
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  // <MM>
  // bucket指向第一个元素在磁盘的偏移量，该bucket在磁盘上存在
  //
  // bucket被组织为二叉搜索树，根据hash值进行组织
  // 相对于链表解决冲突，二叉树搜索的复杂度会低一些
  // </MM>
  while(off > 0){
    rec.off = off;
    // <MM>
    // 只读取record的header，不包括key，value
    // 此时的rsiz是由各种属性字段计算得到
    //
    // 在record大小小于48B时，可能包括key、value
    //
    // </MM>
    if(!tchdbreadrec(hdb, &rec, rbuf)) return false;
    // <MW>
    // hash值也可以比较？
    //
    // hash值主要用于对key比较的简化，以免产生大量key的读取
    //
    // => 将同一个bucket中的节点组织成二叉搜索树，按照hash+key进行排序比较（从left->right，从大到小）
    // => 所以这里先查找hash，hash相等后再查找key
    // => 并不是hash值反应了key的顺序
    //
    // </MW>
    if(hash > rec.hash){
      // <MM>
      // off为rec的left节点的偏移量
      // </MM>
      off = rec.left;
      // <MM>
      // entry offset为rec.left的偏移量
      //
      // 两个uint8_t分别针对的是record序列化后的magic num和hash
      // </MM>
      entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t));
    } else if(hash < rec.hash){
      off = rec.right;
      entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t)) +
        (hdb->ba64 ? sizeof(uint64_t) : sizeof(uint32_t));
    } else {
      // <MM>
      // kuf为null时，尝试读取record body
      // </MM>
      if(!rec.kbuf && !tchdbreadrecbody(hdb, &rec)) return false;
      int kcmp = tcreckeycmp(kbuf, ksiz, rec.kbuf, rec.ksiz);
      // <MM>
      // key的内容不相等时，继续查找左右子树
      // </MM>
      if(kcmp > 0){
        off = rec.left;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
        entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t));
      } else if(kcmp < 0){
        off = rec.right;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
        entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t)) +
          (hdb->ba64 ? sizeof(uint64_t) : sizeof(uint32_t));
      } else {
        // <MM>
        // 找到对应的kv entry
        // </MM>
        bool rv;
        int nvsiz;
        char *nvbuf;
        HDBPDPROCOP *procptr;
        switch(dmode){
          case HDBPDKEEP:
            tchdbsetecode(hdb, TCEKEEP, __FILE__, __LINE__, __func__);
            TCFREE(rec.bbuf);
            return false;
          case HDBPDCAT:
            if(vsiz < 1){
              TCFREE(rec.bbuf);
              return true;
            }
            // <MM>
            // vbuf为null，尝试读取record body
            // </MM>
            if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)){
              TCFREE(rec.bbuf);
              return false;
            }
            nvsiz = rec.vsiz + vsiz;
            // <MW>
            // 这里为什么不确认一下，磁盘上record剩余空间是否可以存放下待拼接的
            // value，避免record移动?
            // </MW>
            if(rec.bbuf){
              TCREALLOC(rec.bbuf, rec.bbuf, rec.ksiz + nvsiz);
              memcpy(rec.bbuf + rec.ksiz + rec.vsiz, vbuf, vsiz);
              rec.kbuf = rec.bbuf;
              rec.vbuf = rec.kbuf + rec.ksiz;
              rec.vsiz = nvsiz;
            } else {
              TCMALLOC(rec.bbuf, nvsiz + 1);
              memcpy(rec.bbuf, rec.vbuf, rec.vsiz);
              memcpy(rec.bbuf + rec.vsiz, vbuf, vsiz);
              rec.vbuf = rec.bbuf;
              rec.vsiz = nvsiz;
            }
            rv = tchdbwriterec(hdb, &rec, bidx, entoff);
            TCFREE(rec.bbuf);
            return rv;
          case HDBPDADDINT:
            if(rec.vsiz != sizeof(int)){
              tchdbsetecode(hdb, TCEKEEP, __FILE__, __LINE__, __func__);
              TCFREE(rec.bbuf);
              return false;
            }
            if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)){
              TCFREE(rec.bbuf);
              return false;
            }
            int lnum;
            memcpy(&lnum, rec.vbuf, sizeof(lnum));
            if(*(int *)vbuf == 0){
              TCFREE(rec.bbuf);
              *(int *)vbuf = lnum;
              return true;
            }
            lnum += *(int *)vbuf;
            rec.vbuf = (char *)&lnum;
            // <MM>
            // 持久化，没有处理大小端的问题
            // </MM>
            *(int *)vbuf = lnum;
            rv = tchdbwriterec(hdb, &rec, bidx, entoff);
            TCFREE(rec.bbuf);
            return rv;
          case HDBPDADDDBL:
            if(rec.vsiz != sizeof(double)){
              tchdbsetecode(hdb, TCEKEEP, __FILE__, __LINE__, __func__);
              TCFREE(rec.bbuf);
              return false;
            }
            if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)){
              TCFREE(rec.bbuf);
              return false;
            }
            double dnum;
            memcpy(&dnum, rec.vbuf, sizeof(dnum));
            if(*(double *)vbuf == 0.0){
              TCFREE(rec.bbuf);
              *(double *)vbuf = dnum;
              return true;
            }
            dnum += *(double *)vbuf;
            rec.vbuf = (char *)&dnum;
            *(double *)vbuf = dnum;
            rv = tchdbwriterec(hdb, &rec, bidx, entoff);
            TCFREE(rec.bbuf);
            return rv;
          case HDBPDPROC:
            if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)){
              TCFREE(rec.bbuf);
              return false;
            }
            procptr = *(HDBPDPROCOP **)((char *)kbuf - sizeof(procptr));
            nvbuf = procptr->proc(rec.vbuf, rec.vsiz, &nvsiz, procptr->op);
            TCFREE(rec.bbuf);
            if(nvbuf == (void *)-1){
              return tchdbremoverec(hdb, &rec, rbuf, bidx, entoff);
            } else if(nvbuf){
              rec.kbuf = kbuf;
              rec.ksiz = ksiz;
              rec.vbuf = nvbuf;
              rec.vsiz = nvsiz;
              rv = tchdbwriterec(hdb, &rec, bidx, entoff);
              TCFREE(nvbuf);
              return rv;
            }
            tchdbsetecode(hdb, TCEKEEP, __FILE__, __LINE__, __func__);
            return false;
          default:
            break;
        }
        TCFREE(rec.bbuf);
        rec.ksiz = ksiz;
        rec.vsiz = vsiz;
        rec.kbuf = kbuf;
        rec.vbuf = vbuf;
        return tchdbwriterec(hdb, &rec, bidx, entoff);
      }
    }
  }
  // <MM>
  // 新增节点的情况，即在二叉树中没有找到key对应的节点
  // </MM>
  if(!vbuf){
    tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
    return false;
  }
  // <MM>
  // 加DB锁
  //
  // 可以在向后推迟下，在search fbp前
  //    - 不能推迟，fbp也要互斥访问
  // </MM>
  if(!HDBLOCKDB(hdb)) return false;
  // <MM>
  // 计算该条记录的占用的磁盘空间
  // </MM>
  rec.rsiz = hdb->ba64 ? sizeof(uint8_t) * 2 + sizeof(uint64_t) * 2 + sizeof(uint16_t) :
    sizeof(uint8_t) * 2 + sizeof(uint32_t) * 2 + sizeof(uint16_t);
  // <MM>
  // ksiz和vsiz的处理代码重复，可以提取为函数
  // </MM>
  if(ksiz < (1U << 7)){
    rec.rsiz += 1;
  } else if(ksiz < (1U << 14)){
    rec.rsiz += 2;
  } else if(ksiz < (1U << 21)){
    rec.rsiz += 3;
  } else if(ksiz < (1U << 28)){
    rec.rsiz += 4;
  } else {
    rec.rsiz += 5;
  }
  if(vsiz < (1U << 7)){
    rec.rsiz += 1;
  } else if(vsiz < (1U << 14)){
    rec.rsiz += 2;
  } else if(vsiz < (1U << 21)){
    rec.rsiz += 3;
  } else if(vsiz < (1U << 28)){
    rec.rsiz += 4;
  } else {
    rec.rsiz += 5;
  }
  // <MM>
  // 根据记录大小rec.rsiz查找合适的free block pool
  // </MM>
  if(!tchdbfbpsearch(hdb, &rec)){
    HDBUNLOCKDB(hdb);
    return false;
  }
  rec.hash = hash;
  rec.left = 0;
  rec.right = 0;
  rec.ksiz = ksiz;
  rec.vsiz = vsiz;
  rec.psiz = 0;
  rec.kbuf = kbuf;
  rec.vbuf = vbuf;
  if(!tchdbwriterec(hdb, &rec, bidx, entoff)){
    HDBUNLOCKDB(hdb);
    return false;
  }
  // <MM>
  // 更新记录数
  // </MM>
  hdb->rnum++;
  // <MM>
  // 更新header中记录数
  // </MM>
  uint64_t llnum = hdb->rnum;
  llnum = TCHTOILL(llnum);
  memcpy(hdb->map + HDBRNUMOFF, &llnum, sizeof(llnum));
  HDBUNLOCKDB(hdb);
  return true;
}


/* Append a record to the delayed record pool.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   `hash' specifies the second hash value. */
static void tchdbdrpappend(TCHDB *hdb, const char *kbuf, int ksiz, const char *vbuf, int vsiz,
                           uint8_t hash){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  TCDODEBUG(hdb->cnt_appenddrp++);
  char rbuf[HDBIOBUFSIZ];
  char *wp = rbuf;
  *(uint8_t *)(wp++) = HDBMAGICREC;
  *(uint8_t *)(wp++) = hash;
  if(hdb->ba64){
    memset(wp, 0, sizeof(uint64_t) * 2);
    wp += sizeof(uint64_t) * 2;
  } else {
    memset(wp, 0, sizeof(uint32_t) * 2);
    wp += sizeof(uint32_t) * 2;
  }
  uint16_t snum;
  char *pwp = wp;
  wp += sizeof(snum);
  int step;
  TCSETVNUMBUF(step, wp, ksiz);
  wp += step;
  TCSETVNUMBUF(step, wp, vsiz);
  wp += step;
  int32_t hsiz = wp - rbuf;
  int32_t rsiz = hsiz + ksiz + vsiz;
  uint16_t psiz = tchdbpadsize(hdb, hdb->fsiz + rsiz);
  hdb->fsiz += rsiz + psiz;
  snum = TCHTOIS(psiz);
  memcpy(pwp, &snum, sizeof(snum));
  TCXSTR *drpool = hdb->drpool;
  TCXSTRCAT(drpool, rbuf, hsiz);
  TCXSTRCAT(drpool, kbuf, ksiz);
  TCXSTRCAT(drpool, vbuf, vsiz);
  if(psiz > 0){
    char pbuf[psiz];
    memset(pbuf, 0, psiz);
    TCXSTRCAT(drpool, pbuf, psiz);
  }
}


/* Store a record in asynchronus fashion.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `bidx' specifies the index of the bucket array.
   `hash' specifies the hash value for the collision tree.
   `vbuf' specifies the pointer to the region of the value.
   `vsiz' specifies the size of the region of the value.
   If successful, the return value is true, else, it is false. */
static bool tchdbputasyncimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx,
                              uint8_t hash, const char *vbuf, int vsiz){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && vsiz >= 0);
  if(hdb->recc) tcmdbout(hdb->recc, kbuf, ksiz);
  if(!hdb->drpool){
    hdb->drpool = tcxstrnew3(HDBDRPUNIT + HDBDRPLAT);
    hdb->drpdef = tcxstrnew3(HDBDRPUNIT);
    hdb->drpoff = hdb->fsiz;
  }
  off_t off = tchdbgetbucket(hdb, bidx);
  off_t entoff = 0;
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  // <MM>
  // 查找bucket的二叉树，看是否需要新增节点
  // </MM>
  while(off > 0){
    // <MM>
    // 待读取的记录落在数据文件中最后的大小为runit文件块（48Byte）内
    // </MM>
    // <MW>
    // 这么做肯定是优化io，但是如何实现优化的?
    // 说明记录小于hdb->runit（默认是48Byte），这种情况为什么直接延迟插入？
    // </MW>
    if(off >= hdb->drpoff - hdb->runit){
      TCDODEBUG(hdb->cnt_deferdrp++);
      TCXSTR *drpdef = hdb->drpdef;
      TCXSTRCAT(drpdef, &ksiz, sizeof(ksiz));
      TCXSTRCAT(drpdef, &vsiz, sizeof(vsiz));
      TCXSTRCAT(drpdef, kbuf, ksiz);
      TCXSTRCAT(drpdef, vbuf, vsiz);
      // <MM>
      // 当defered的record总大小大于HDBDRPUNIT时会进行一次刷盘
      // </MM>
      if(TCXSTRSIZE(hdb->drpdef) > HDBDRPUNIT && !tchdbflushdrp(hdb)) return false;
      return true;
    }
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return false;
    if(hash > rec.hash){
      off = rec.left;
      entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t));
    } else if(hash < rec.hash){
      off = rec.right;
      entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t)) +
        (hdb->ba64 ? sizeof(uint64_t) : sizeof(uint32_t));
    } else {
      // <MW>
      // 为什么不进行key的相等判断?
      //
      // 仅仅hash值相等，而key不等，这种情况也需要新增节点。所以判断key相等，会有更多节点进行
      // delayed record pool中，从而提升async操作性能
      // </MW>
      TCDODEBUG(hdb->cnt_deferdrp++);
      TCXSTR *drpdef = hdb->drpdef;
      TCXSTRCAT(drpdef, &ksiz, sizeof(ksiz));
      TCXSTRCAT(drpdef, &vsiz, sizeof(vsiz));
      TCXSTRCAT(drpdef, kbuf, ksiz);
      TCXSTRCAT(drpdef, vbuf, vsiz);
      if(TCXSTRSIZE(hdb->drpdef) > HDBDRPUNIT && !tchdbflushdrp(hdb)) return false;
      return true;
    }
  }
  // <MM>
  // 没有在bucket二叉树中找到包含key的节点，需要新增节点
  // </MM>
  if(entoff > 0){
    // <MM>
    // entoff > 0意味着在bucket链表中有前驱节点
    // 此处将前驱节点的指针更新为hdb->fsiz
    //
    // 针对delayed record pool被flush之前，新增多条record的情况。如果hbd->fsiz
    // 不随着record的新增，同时变化的话，在查找时，record不能被正确的找到。
    // 为解决这个问题，后续在tchdbdrpappend中，将record序列化后，会根据序列化后
    // 大小更新hdb->fsiz
    // </MM>
    if(hdb->ba64){
      uint64_t llnum = hdb->fsiz >> hdb->apow;
      llnum = TCHTOILL(llnum);
      if(!tchdbseekwrite(hdb, entoff, &llnum, sizeof(uint64_t))) return false;
    } else {
      uint32_t lnum = hdb->fsiz >> hdb->apow;
      lnum = TCHTOIL(lnum);
      if(!tchdbseekwrite(hdb, entoff, &lnum, sizeof(uint32_t))) return false;
    }
  } else {
    tchdbsetbucket(hdb, bidx, hdb->fsiz);
  }
  // <MM>
  // 此处会按照record在磁盘上的序列化格式进行格式化
  // 并追加到hdb->drpool
  // </MM>
  tchdbdrpappend(hdb, kbuf, ksiz, vbuf, vsiz, hash);
  hdb->rnum++;
  if(TCXSTRSIZE(hdb->drpool) > HDBDRPUNIT && !tchdbflushdrp(hdb)) return false;
  return true;
}


/* Remove a record of a hash database object.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `bidx' specifies the index of the bucket array.
   `hash' specifies the hash value for the collision tree.
   If successful, the return value is true, else, it is false. */
static bool tchdboutimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash){
  assert(hdb && kbuf && ksiz >= 0);
  if(hdb->recc) tcmdbout(hdb->recc, kbuf, ksiz);
  off_t off = tchdbgetbucket(hdb, bidx);
  off_t entoff = 0;
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(off > 0){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return false;
    if(hash > rec.hash){
      off = rec.left;
      entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t));
    } else if(hash < rec.hash){
      off = rec.right;
      entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t)) +
        (hdb->ba64 ? sizeof(uint64_t) : sizeof(uint32_t));
    } else {
      if(!rec.kbuf && !tchdbreadrecbody(hdb, &rec)) return false;
      int kcmp = tcreckeycmp(kbuf, ksiz, rec.kbuf, rec.ksiz);
      if(kcmp > 0){
        off = rec.left;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
        entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t));
      } else if(kcmp < 0){
        off = rec.right;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
        entoff = rec.off + (sizeof(uint8_t) + sizeof(uint8_t)) +
          (hdb->ba64 ? sizeof(uint64_t) : sizeof(uint32_t));
      } else {
        TCFREE(rec.bbuf);
        rec.bbuf = NULL;
        return tchdbremoverec(hdb, &rec, rbuf, bidx, entoff);
      }
    }
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return false;
}


/* Retrieve a record in a hash database object.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `bidx' specifies the index of the bucket array.
   `hash' specifies the hash value for the collision tree.
   `sp' specifies the pointer to the variable into which the size of the region of the return
   value is assigned.
   If successful, the return value is the pointer to the region of the value of the corresponding
   record. */
static char *tchdbgetimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash,
                          int *sp){
  assert(hdb && kbuf && ksiz >= 0 && sp);
  if(hdb->recc){
    int tvsiz;
    char *tvbuf = tcmdbget(hdb->recc, kbuf, ksiz, &tvsiz);
    if(tvbuf){
      if(*tvbuf == '*'){
        tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
        TCFREE(tvbuf);
        return NULL;
      }
      *sp = tvsiz - 1;
      memmove(tvbuf, tvbuf + 1, tvsiz);
      return tvbuf;
    }
  }
  off_t off = tchdbgetbucket(hdb, bidx);
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(off > 0){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return NULL;
    if(hash > rec.hash){
      off = rec.left;
    } else if(hash < rec.hash){
      off = rec.right;
    } else {
      if(!rec.kbuf && !tchdbreadrecbody(hdb, &rec)) return NULL;
      int kcmp = tcreckeycmp(kbuf, ksiz, rec.kbuf, rec.ksiz);
      if(kcmp > 0){
        off = rec.left;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else if(kcmp < 0){
        off = rec.right;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else {
        if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)) return NULL;
        if(hdb->zmode){
          int zsiz;
          char *zbuf;
          if(hdb->opts & HDBTDEFLATE){
            zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
          } else if(hdb->opts & HDBTBZIP){
            zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
          } else if(hdb->opts & HDBTTCBS){
            zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
          } else {
            zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
          }
          TCFREE(rec.bbuf);
          if(!zbuf){
            tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
            return NULL;
          }
          if(hdb->recc){
            if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
            tcmdbput4(hdb->recc, kbuf, ksiz, "=", 1, zbuf, zsiz);
          }
          *sp = zsiz;
          return zbuf;
        }
        if(hdb->recc){
          if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
          tcmdbput4(hdb->recc, kbuf, ksiz, "=", 1, rec.vbuf, rec.vsiz);
        }
        if(rec.bbuf){
          memmove(rec.bbuf, rec.vbuf, rec.vsiz);
          rec.bbuf[rec.vsiz] = '\0';
          *sp = rec.vsiz;
          return rec.bbuf;
        }
        *sp = rec.vsiz;
        char *rv;
        TCMEMDUP(rv, rec.vbuf, rec.vsiz);
        return rv;
      }
    }
  }
  if(hdb->recc){
    if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
    tcmdbput(hdb->recc, kbuf, ksiz, "*", 1);
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return NULL;
}


/* Retrieve a record in a hash database object and write the value into a buffer.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `bidx' specifies the index of the bucket array.
   `hash' specifies the hash value for the collision tree.
   `vbuf' specifies the pointer to the buffer into which the value of the corresponding record is
   written.
   `max' specifies the size of the buffer.
   If successful, the return value is the size of the written data, else, it is -1. */
static int tchdbgetintobuf(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash,
                           char *vbuf, int max){
  assert(hdb && kbuf && ksiz >= 0 && vbuf && max >= 0);
  if(hdb->recc){
    int tvsiz;
    char *tvbuf = tcmdbget(hdb->recc, kbuf, ksiz, &tvsiz);
    if(tvbuf){
      if(*tvbuf == '*'){
        tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
        TCFREE(tvbuf);
        return -1;
      }
      tvsiz = tclmin(tvsiz - 1, max);
      memcpy(vbuf, tvbuf + 1, tvsiz);
      TCFREE(tvbuf);
      return tvsiz;
    }
  }
  off_t off = tchdbgetbucket(hdb, bidx);
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(off > 0){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return -1;
    if(hash > rec.hash){
      off = rec.left;
    } else if(hash < rec.hash){
      off = rec.right;
    } else {
      if(!rec.kbuf && !tchdbreadrecbody(hdb, &rec)) return -1;
      int kcmp = tcreckeycmp(kbuf, ksiz, rec.kbuf, rec.ksiz);
      if(kcmp > 0){
        off = rec.left;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else if(kcmp < 0){
        off = rec.right;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else {
        if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)) return -1;
        if(hdb->zmode){
          int zsiz;
          char *zbuf;
          if(hdb->opts & HDBTDEFLATE){
            zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
          } else if(hdb->opts & HDBTBZIP){
            zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
          } else if(hdb->opts & HDBTTCBS){
            zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
          } else {
            zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
          }
          TCFREE(rec.bbuf);
          if(!zbuf){
            tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
            return -1;
          }
          if(hdb->recc){
            if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
            tcmdbput4(hdb->recc, kbuf, ksiz, "=", 1, zbuf, zsiz);
          }
          zsiz = tclmin(zsiz, max);
          memcpy(vbuf, zbuf, zsiz);
          TCFREE(zbuf);
          return zsiz;
        }
        if(hdb->recc){
          if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
          tcmdbput4(hdb->recc, kbuf, ksiz, "=", 1, rec.vbuf, rec.vsiz);
        }
        int vsiz = tclmin(rec.vsiz, max);
        memcpy(vbuf, rec.vbuf, vsiz);
        TCFREE(rec.bbuf);
        return vsiz;
      }
    }
  }
  if(hdb->recc){
    if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
    tcmdbput(hdb->recc, kbuf, ksiz, "*", 1);
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return -1;
}


/* Retrieve the next record of a record in a hash database object.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `sp' specifies the pointer to the variable into which the size of the region of the return
   value is assigned.
   `vbp' specifies the pointer to the variable into which the pointer to the value is assigned.
   `vsp' specifies the pointer to the variable into which the size of the value is assigned.
   If successful, the return value is the pointer to the region of the value of the next
   record. */
static char *tchdbgetnextimpl(TCHDB *hdb, const char *kbuf, int ksiz, int *sp,
                              const char **vbp, int *vsp){
  assert(hdb && sp);
  if(!kbuf){
    uint64_t iter = hdb->frec;
    TCHREC rec;
    char rbuf[HDBIOBUFSIZ];
    while(iter < hdb->fsiz){
      rec.off = iter;
      if(!tchdbreadrec(hdb, &rec, rbuf)) return NULL;
      iter += rec.rsiz;
      if(rec.magic == HDBMAGICREC){
        if(vbp){
          if(hdb->zmode){
            if(!tchdbreadrecbody(hdb, &rec)) return NULL;
            int zsiz;
            char *zbuf;
            if(hdb->opts & HDBTDEFLATE){
              zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
            } else if(hdb->opts & HDBTBZIP){
              zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
            } else if(hdb->opts & HDBTTCBS){
              zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
            } else {
              zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
            }
            if(!zbuf){
              tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
              TCFREE(rec.bbuf);
              return NULL;
            }
            char *rv;
            TCMALLOC(rv, rec.ksiz + zsiz + 1);
            memcpy(rv, rec.kbuf, rec.ksiz);
            memcpy(rv + rec.ksiz, zbuf, zsiz);
            *sp = rec.ksiz;
            *vbp = rv + rec.ksiz;
            *vsp = zsiz;
            TCFREE(zbuf);
            TCFREE(rec.bbuf);
            return rv;
          }
          if(rec.vbuf){
            char *rv;
            TCMALLOC(rv, rec.ksiz + rec.vsiz + 1);
            memcpy(rv, rec.kbuf, rec.ksiz);
            memcpy(rv + rec.ksiz, rec.vbuf, rec.vsiz);
            *sp = rec.ksiz;
            *vbp = rv + rec.ksiz;
            *vsp = rec.vsiz;
            return rv;
          }
          if(!tchdbreadrecbody(hdb, &rec)) return NULL;
          *sp = rec.ksiz;
          *vbp = rec.vbuf;
          *vsp = rec.vsiz;
          return rec.bbuf;
        }
        if(rec.kbuf){
          *sp = rec.ksiz;
          char *rv;
          TCMEMDUP(rv, rec.kbuf, rec.ksiz);
          return rv;
        }
        if(!tchdbreadrecbody(hdb, &rec)) return NULL;
        rec.bbuf[rec.ksiz] = '\0';
        *sp = rec.ksiz;
        return rec.bbuf;
      }
    }
    tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
    return NULL;
  }
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  off_t off = tchdbgetbucket(hdb, bidx);
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(off > 0){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return NULL;
    if(hash > rec.hash){
      off = rec.left;
    } else if(hash < rec.hash){
      off = rec.right;
    } else {
      if(!rec.kbuf && !tchdbreadrecbody(hdb, &rec)) return NULL;
      int kcmp = tcreckeycmp(kbuf, ksiz, rec.kbuf, rec.ksiz);
      if(kcmp > 0){
        off = rec.left;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else if(kcmp < 0){
        off = rec.right;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else {
        uint64_t iter = rec.off + rec.rsiz;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
        while(iter < hdb->fsiz){
          rec.off = iter;
          if(!tchdbreadrec(hdb, &rec, rbuf)) return NULL;
          iter += rec.rsiz;
          if(rec.magic == HDBMAGICREC){
            if(vbp){
              if(hdb->zmode){
                if(!tchdbreadrecbody(hdb, &rec)) return NULL;
                int zsiz;
                char *zbuf;
                if(hdb->opts & HDBTDEFLATE){
                  zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
                } else if(hdb->opts & HDBTBZIP){
                  zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
                } else if(hdb->opts & HDBTTCBS){
                  zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
                } else {
                  zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
                }
                if(!zbuf){
                  tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
                  TCFREE(rec.bbuf);
                  return NULL;
                }
                char *rv;
                TCMALLOC(rv, rec.ksiz + zsiz + 1);
                memcpy(rv, rec.kbuf, rec.ksiz);
                memcpy(rv + rec.ksiz, zbuf, zsiz);
                *sp = rec.ksiz;
                *vbp = rv + rec.ksiz;
                *vsp = zsiz;
                TCFREE(zbuf);
                TCFREE(rec.bbuf);
                return rv;
              }
              if(rec.vbuf){
                char *rv;
                TCMALLOC(rv, rec.ksiz + rec.vsiz + 1);
                memcpy(rv, rec.kbuf, rec.ksiz);
                memcpy(rv + rec.ksiz, rec.vbuf, rec.vsiz);
                *sp = rec.ksiz;
                *vbp = rv + rec.ksiz;
                *vsp = rec.vsiz;
                return rv;
              }
              if(!tchdbreadrecbody(hdb, &rec)) return NULL;
              *sp = rec.ksiz;
              *vbp = rec.vbuf;
              *vsp = rec.vsiz;
              return rec.bbuf;
            }
            if(rec.kbuf){
              *sp = rec.ksiz;
              char *rv;
              TCMEMDUP(rv, rec.kbuf, rec.ksiz);
              return rv;
            }
            if(!tchdbreadrecbody(hdb, &rec)) return NULL;
            rec.bbuf[rec.ksiz] = '\0';
            *sp = rec.ksiz;
            return rec.bbuf;
          }
        }
        tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
        return NULL;
      }
    }
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return NULL;
}


/* Get the size of the value of a record in a hash database object.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   `bidx' specifies the index of the bucket array.
   `hash' specifies the hash value for the collision tree.
   If successful, the return value is the size of the value of the corresponding record, else,
   it is -1. */
static int tchdbvsizimpl(TCHDB *hdb, const char *kbuf, int ksiz, uint64_t bidx, uint8_t hash){
  assert(hdb && kbuf && ksiz >= 0);
  if(hdb->recc){
    int tvsiz;
    char *tvbuf = tcmdbget(hdb->recc, kbuf, ksiz, &tvsiz);
    if(tvbuf){
      if(*tvbuf == '*'){
        tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
        TCFREE(tvbuf);
        return -1;
      }
      TCFREE(tvbuf);
      return tvsiz - 1;
    }
  }
  off_t off = tchdbgetbucket(hdb, bidx);
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(off > 0){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return -1;
    if(hash > rec.hash){
      off = rec.left;
    } else if(hash < rec.hash){
      off = rec.right;
    } else {
      if(!rec.kbuf && !tchdbreadrecbody(hdb, &rec)) return -1;
      int kcmp = tcreckeycmp(kbuf, ksiz, rec.kbuf, rec.ksiz);
      if(kcmp > 0){
        off = rec.left;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else if(kcmp < 0){
        off = rec.right;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else {
        if(hdb->zmode){
          if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)) return -1;
          int zsiz;
          char *zbuf;
          if(hdb->opts & HDBTDEFLATE){
            zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
          } else if(hdb->opts & HDBTBZIP){
            zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
          } else if(hdb->opts & HDBTTCBS){
            zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
          } else {
            zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
          }
          TCFREE(rec.bbuf);
          if(!zbuf){
            tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
            return -1;
          }
          if(hdb->recc){
            if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
            tcmdbput4(hdb->recc, kbuf, ksiz, "=", 1, zbuf, zsiz);
          }
          TCFREE(zbuf);
          return zsiz;
        }
        if(hdb->recc && rec.vbuf){
          if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
          tcmdbput4(hdb->recc, kbuf, ksiz, "=", 1, rec.vbuf, rec.vsiz);
        }
        TCFREE(rec.bbuf);
        return rec.vsiz;
      }
    }
  }
  if(hdb->recc){
    if(tcmdbrnum(hdb->recc) >= hdb->rcnum) tchdbcacheadjust(hdb);
    tcmdbput(hdb->recc, kbuf, ksiz, "*", 1);
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return -1;
}


/* Initialize the iterator of a hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbiterinitimpl(TCHDB *hdb){
  assert(hdb);
  hdb->iter = hdb->frec;
  return true;
}


/* Get the next key of the iterator of a hash database object.
   `hdb' specifies the hash database object.
   `sp' specifies the pointer to the variable into which the size of the region of the return
   value is assigned.
   If successful, the return value is the pointer to the region of the next key, else, it is
   `NULL'. */
static char *tchdbiternextimpl(TCHDB *hdb, int *sp){
  assert(hdb && sp);
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(hdb->iter < hdb->fsiz){
    rec.off = hdb->iter;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return NULL;
    hdb->iter += rec.rsiz;
    if(rec.magic == HDBMAGICREC){
      if(rec.kbuf){
        *sp = rec.ksiz;
        char *rv;
        TCMEMDUP(rv, rec.kbuf, rec.ksiz);
        return rv;
      }
      if(!tchdbreadrecbody(hdb, &rec)) return NULL;
      rec.bbuf[rec.ksiz] = '\0';
      *sp = rec.ksiz;
      return rec.bbuf;
    }
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return NULL;
}


/* Get the next extensible objects of the iterator of a hash database object. */
static bool tchdbiternextintoxstr(TCHDB *hdb, TCXSTR *kxstr, TCXSTR *vxstr){
  assert(hdb && kxstr && vxstr);
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(hdb->iter < hdb->fsiz){
    rec.off = hdb->iter;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return false;
    hdb->iter += rec.rsiz;
    if(rec.magic == HDBMAGICREC){
      if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)) return false;
      tcxstrclear(kxstr);
      TCXSTRCAT(kxstr, rec.kbuf, rec.ksiz);
      tcxstrclear(vxstr);
      if(hdb->zmode){
        int zsiz;
        char *zbuf;
        if(hdb->opts & HDBTDEFLATE){
          zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
        } else if(hdb->opts & HDBTBZIP){
          zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
        } else if(hdb->opts & HDBTTCBS){
          zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
        } else {
          zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
        }
        if(!zbuf){
          tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
          TCFREE(rec.bbuf);
          return false;
        }
        TCXSTRCAT(vxstr, zbuf, zsiz);
        TCFREE(zbuf);
      } else {
        TCXSTRCAT(vxstr, rec.vbuf, rec.vsiz);
      }
      TCFREE(rec.bbuf);
      return true;
    }
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return false;
}


/* Optimize the file of a hash database object.
   `hdb' specifies the hash database object.
   `bnum' specifies the number of elements of the bucket array.
   `apow' specifies the size of record alignment by power of 2.
   `fpow' specifies the maximum number of elements of the free block pool by power of 2.
   `opts' specifies options by bitwise-or.
   If successful, the return value is true, else, it is false. */
static bool tchdboptimizeimpl(TCHDB *hdb, int64_t bnum, int8_t apow, int8_t fpow, uint8_t opts){
  assert(hdb);
  char *tpath = tcsprintf("%s%ctmp%c%llu", hdb->path, MYEXTCHR, MYEXTCHR, hdb->inode);
  TCHDB *thdb = tchdbnew();
  thdb->dbgfd = hdb->dbgfd;
  thdb->enc = hdb->enc;
  thdb->encop = hdb->encop;
  thdb->dec = hdb->dec;
  thdb->decop = hdb->decop;
  if(bnum < 1){
    bnum = hdb->rnum * 2 + 1;
    if(bnum < HDBDEFBNUM) bnum = HDBDEFBNUM;
  }
  if(apow < 0) apow = hdb->apow;
  if(fpow < 0) fpow = hdb->fpow;
  if(opts == UINT8_MAX) opts = hdb->opts;
  tchdbtune(thdb, bnum, apow, fpow, opts);
  if(!tchdbopen(thdb, tpath, HDBOWRITER | HDBOCREAT | HDBOTRUNC)){
    tchdbsetecode(hdb, thdb->ecode, __FILE__, __LINE__, __func__);
    tchdbdel(thdb);
    TCFREE(tpath);
    return false;
  }
  memcpy(tchdbopaque(thdb), tchdbopaque(hdb), HDBHEADSIZ - HDBOPAQUEOFF);
  bool err = false;
  uint64_t off = hdb->frec;
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(off < hdb->fsiz){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)){
      err = true;
      break;
    }
    off += rec.rsiz;
    if(rec.magic == HDBMAGICREC){
      if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)){
        TCFREE(rec.bbuf);
        err = true;
      } else {
        if(hdb->zmode){
          int zsiz;
          char *zbuf;
          if(hdb->opts & HDBTDEFLATE){
            zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
          } else if(hdb->opts & HDBTBZIP){
            zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
          } else if(hdb->opts & HDBTTCBS){
            zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
          } else {
            zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
          }
          if(zbuf){
            if(!tchdbput(thdb, rec.kbuf, rec.ksiz, zbuf, zsiz)){
              tchdbsetecode(hdb, thdb->ecode, __FILE__, __LINE__, __func__);
              err = true;
            }
            TCFREE(zbuf);
          } else {
            tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
            err = true;
          }
        } else {
          if(!tchdbput(thdb, rec.kbuf, rec.ksiz, rec.vbuf, rec.vsiz)){
            tchdbsetecode(hdb, thdb->ecode, __FILE__, __LINE__, __func__);
            err = true;
          }
        }
      }
      TCFREE(rec.bbuf);
    }
  }
  if(!tchdbclose(thdb)){
    tchdbsetecode(hdb, thdb->ecode, __FILE__, __LINE__, __func__);
    err = true;
  }
  bool esc = false;
  if(err && (hdb->omode & HDBONOLCK) && !thdb->fatal){
    err = false;
    esc = true;
  }
  tchdbdel(thdb);
  if(err){
    TCFREE(tpath);
    return false;
  }
  if(esc){
    char *bpath = tcsprintf("%s%cbroken", tpath, MYEXTCHR);
    if(rename(hdb->path, bpath) == -1){
      tchdbsetecode(hdb, TCEUNLINK, __FILE__, __LINE__, __func__);
      err = true;
    }
    TCFREE(bpath);
  } else {
    if(unlink(hdb->path) == -1){
      tchdbsetecode(hdb, TCEUNLINK, __FILE__, __LINE__, __func__);
      err = true;
    }
  }
  if(rename(tpath, hdb->path) == -1){
    tchdbsetecode(hdb, TCERENAME, __FILE__, __LINE__, __func__);
    err = true;
  }
  TCFREE(tpath);
  if(err) return false;
  tpath = tcstrdup(hdb->path);
  int omode = (hdb->omode & ~HDBOCREAT) & ~HDBOTRUNC;
  if(!tchdbcloseimpl(hdb)){
    TCFREE(tpath);
    return false;
  }
  bool rv = tchdbopenimpl(hdb, tpath, omode);
  TCFREE(tpath);
  return rv;
}


/* Remove all records of a hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbvanishimpl(TCHDB *hdb){
  assert(hdb);
  char *path = tcstrdup(hdb->path);
  int omode = hdb->omode;
  bool err = false;
  if(!tchdbcloseimpl(hdb)) err = true;
  if(!tchdbopenimpl(hdb, path, HDBOTRUNC | omode)){
    tcpathunlock(hdb->rpath);
    TCFREE(hdb->rpath);
    err = true;
  }
  TCFREE(path);
  return !err;
}


/* Copy the database file of a hash database object.
   `hdb' specifies the hash database object.
   `path' specifies the path of the destination file.
   If successful, the return value is true, else, it is false. */
static bool tchdbcopyimpl(TCHDB *hdb, const char *path){
  assert(hdb && path);
  bool err = false;
  if(hdb->omode & HDBOWRITER){
    if(!tchdbsavefbp(hdb)) err = true;
    if(!tchdbmemsync(hdb, false)) err = true;
    tchdbsetflag(hdb, HDBFOPEN, false);
  }
  if(*path == '@'){
    char tsbuf[TCNUMBUFSIZ];
    sprintf(tsbuf, "%llu", (unsigned long long)(tctime() * 1000000));
    const char *args[3];
    args[0] = path + 1;
    args[1] = hdb->path;
    args[2] = tsbuf;
    if(tcsystem(args, sizeof(args) / sizeof(*args)) != 0) err = true;
  } else {
    if(!tccopyfile(hdb->path, path)){
      tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
      err = true;
    }
  }
  if(hdb->omode & HDBOWRITER) tchdbsetflag(hdb, HDBFOPEN, true);
  return !err;
}


/* Perform dynamic defragmentation of a hash database object.
   `hdb' specifies the hash database object connected.
   `step' specifie the number of steps.
   If successful, the return value is true, else, it is false. */
static bool tchdbdefragimpl(TCHDB *hdb, int64_t step){
  assert(hdb && step >= 0);
  TCDODEBUG(hdb->cnt_defrag++);
  hdb->dfcnt = 0;
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  // <MM>
  // 查找free block
  // </MM>
  while(true){
    if(hdb->dfcur >= hdb->fsiz){
      hdb->dfcur = hdb->frec;
      return true;
    }
    if(step-- < 1) return true;
    rec.off = hdb->dfcur;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return false;
    if(rec.magic == HDBMAGICFB) break;
    hdb->dfcur += rec.rsiz;
  }
  uint32_t align = hdb->align;
  // <MM>
  // 指向第一个free block
  // </MM>
  uint64_t base = hdb->dfcur;
  uint64_t dest = base;
  uint64_t cur = base;
  if(hdb->iter == cur) hdb->iter += rec.rsiz;
  cur += rec.rsiz;
  uint64_t fbsiz = cur - dest;
  step++;
  while(step > 0 && cur < hdb->fsiz){
    rec.off = cur;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return false;
    uint32_t rsiz = rec.rsiz;
    if(rec.magic == HDBMAGICREC){
      if(rec.psiz >= align){
        int diff = rec.psiz - rec.psiz % align;
        rec.psiz -= diff;
        rec.rsiz -= diff;
        fbsiz += diff;
      }
      if(!tchdbshiftrec(hdb, &rec, rbuf, dest)) return false;
      if(hdb->iter == cur) hdb->iter = dest;
      dest += rec.rsiz;
      step--;
    } else {
      if(hdb->iter == cur) hdb->iter += rec.rsiz;
      fbsiz += rec.rsiz;
    }
    cur += rsiz;
  }
  if(cur < hdb->fsiz){
    if(fbsiz > HDBFBMAXSIZ){
      tchdbfbptrim(hdb, base, cur, 0, 0);
      uint64_t off = dest;
      uint64_t size = fbsiz;
      while(size > 0){
        uint32_t rsiz = (size > HDBFBMAXSIZ) ? HDBFBMAXSIZ : size;
        if(size - rsiz < HDBMINRUNIT) rsiz = size;
        tchdbfbpinsert(hdb, off, rsiz);
        if(!tchdbwritefb(hdb, off, rsiz)) return false;
        off += rsiz;
        size -= rsiz;
      }
    } else {
      tchdbfbptrim(hdb, base, cur, dest, fbsiz);
      if(!tchdbwritefb(hdb, dest, fbsiz)) return false;
    }
    hdb->dfcur = cur - fbsiz;
  } else {
    TCDODEBUG(hdb->cnt_trunc++);
    if(hdb->tran && !tchdbwalwrite(hdb, dest, fbsiz)) return false;
    tchdbfbptrim(hdb, base, cur, 0, 0);
    hdb->dfcur = hdb->frec;
    hdb->fsiz = dest;
    uint64_t llnum = hdb->fsiz;
    llnum = TCHTOILL(llnum);
    memcpy(hdb->map + HDBFSIZOFF, &llnum, sizeof(llnum));
    if(hdb->iter >= hdb->fsiz) hdb->iter = UINT64_MAX;
    if(!hdb->tran){
      if(ftruncate(hdb->fd, hdb->fsiz) == -1){
        tchdbsetecode(hdb, TCETRUNC, __FILE__, __LINE__, __func__);
        return false;
      }
      hdb->xfsiz = 0;
    }
  }
  return true;
}


/* Move the iterator to the record corresponding a key of a hash database object.
   `hdb' specifies the hash database object.
   `kbuf' specifies the pointer to the region of the key.
   `ksiz' specifies the size of the region of the key.
   If successful, the return value is true, else, it is false. */
static bool tchdbiterjumpimpl(TCHDB *hdb, const char *kbuf, int ksiz){
  assert(hdb && kbuf && ksiz);
  uint8_t hash;
  uint64_t bidx = tchdbbidx(hdb, kbuf, ksiz, &hash);
  off_t off = tchdbgetbucket(hdb, bidx);
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  while(off > 0){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)) return false;
    if(hash > rec.hash){
      off = rec.left;
    } else if(hash < rec.hash){
      off = rec.right;
    } else {
      if(!rec.kbuf && !tchdbreadrecbody(hdb, &rec)) return false;
      int kcmp = tcreckeycmp(kbuf, ksiz, rec.kbuf, rec.ksiz);
      if(kcmp > 0){
        off = rec.left;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else if(kcmp < 0){
        off = rec.right;
        TCFREE(rec.bbuf);
        rec.kbuf = NULL;
        rec.bbuf = NULL;
      } else {
        hdb->iter = off;
        return true;
      }
    }
  }
  tchdbsetecode(hdb, TCENOREC, __FILE__, __LINE__, __func__);
  return false;
}


/* Process each record atomically of a hash database object.
   `hdb' specifies the hash database object.
   `func' specifies the pointer to the iterator function called for each record.
   `op' specifies an arbitrary pointer to be given as a parameter of the iterator function.
   If successful, the return value is true, else, it is false. */
static bool tchdbforeachimpl(TCHDB *hdb, TCITER iter, void *op){
  assert(hdb && iter);
  bool err = false;
  uint64_t off = hdb->frec;
  TCHREC rec;
  char rbuf[HDBIOBUFSIZ];
  bool cont = true;
  while(cont && off < hdb->fsiz){
    rec.off = off;
    if(!tchdbreadrec(hdb, &rec, rbuf)){
      err = true;
      break;
    }
    off += rec.rsiz;
    if(rec.magic == HDBMAGICREC){
      if(!rec.vbuf && !tchdbreadrecbody(hdb, &rec)){
        TCFREE(rec.bbuf);
        err = true;
      } else {
        if(hdb->zmode){
          int zsiz;
          char *zbuf;
          if(hdb->opts & HDBTDEFLATE){
            zbuf = _tc_inflate(rec.vbuf, rec.vsiz, &zsiz, _TCZMRAW);
          } else if(hdb->opts & HDBTBZIP){
            zbuf = _tc_bzdecompress(rec.vbuf, rec.vsiz, &zsiz);
          } else if(hdb->opts & HDBTTCBS){
            zbuf = tcbsdecode(rec.vbuf, rec.vsiz, &zsiz);
          } else {
            zbuf = hdb->dec(rec.vbuf, rec.vsiz, &zsiz, hdb->decop);
          }
          if(zbuf){
            cont = iter(rec.kbuf, rec.ksiz, zbuf, zsiz, op);
            TCFREE(zbuf);
          } else {
            tchdbsetecode(hdb, TCEMISC, __FILE__, __LINE__, __func__);
            err = true;
          }
        } else {
          cont = iter(rec.kbuf, rec.ksiz, rec.vbuf, rec.vsiz, op);
        }
      }
      TCFREE(rec.bbuf);
    }
  }
  return !err;
}


/* Lock a method of the hash database object.
   `hdb' specifies the hash database object.
   `wr' specifies whether the lock is writer or not.
   If successful, the return value is true, else, it is false. */
static bool tchdblockmethod(TCHDB *hdb, bool wr){
  assert(hdb);
  if(wr ? pthread_rwlock_wrlock(hdb->mmtx) != 0 : pthread_rwlock_rdlock(hdb->mmtx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Unlock a method of the hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbunlockmethod(TCHDB *hdb){
  assert(hdb);
  if(pthread_rwlock_unlock(hdb->mmtx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Lock a record of the hash database object.
   `hdb' specifies the hash database object.
   `bidx' specifies the bucket index of the record.
   `wr' specifies whether the lock is writer or not.
   If successful, the return value is true, else, it is false. */
static bool tchdblockrecord(TCHDB *hdb, uint8_t bidx, bool wr){
  assert(hdb);
  if(wr ? pthread_rwlock_wrlock((pthread_rwlock_t *)hdb->rmtxs + bidx) != 0 :
     pthread_rwlock_rdlock((pthread_rwlock_t *)hdb->rmtxs + bidx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Unlock a record of the hash database object.
   `hdb' specifies the hash database object.
   `bidx' specifies the bucket index of the record.
   If successful, the return value is true, else, it is false. */
static bool tchdbunlockrecord(TCHDB *hdb, uint8_t bidx){
  assert(hdb);
  if(pthread_rwlock_unlock((pthread_rwlock_t *)hdb->rmtxs + bidx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Lock all records of the hash database object.
   `hdb' specifies the hash database object.
   `wr' specifies whether the lock is writer or not.
   If successful, the return value is true, else, it is false. */
static bool tchdblockallrecords(TCHDB *hdb, bool wr){
  assert(hdb);
  for(int i = 0; i <= UINT8_MAX; i++){
    if(wr ? pthread_rwlock_wrlock((pthread_rwlock_t *)hdb->rmtxs + i) != 0 :
       pthread_rwlock_rdlock((pthread_rwlock_t *)hdb->rmtxs + i) != 0){
      tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
      while(--i >= 0){
        pthread_rwlock_unlock((pthread_rwlock_t *)hdb->rmtxs + i);
      }
      return false;
    }
  }
  TCTESTYIELD();
  return true;
}


/* Unlock all records of the hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbunlockallrecords(TCHDB *hdb){
  assert(hdb);
  bool err = false;
  for(int i = UINT8_MAX; i >= 0; i--){
    if(pthread_rwlock_unlock((pthread_rwlock_t *)hdb->rmtxs + i)) err = true;
  }
  TCTESTYIELD();
  if(err){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  return true;
}


/* Lock the whole database of the hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdblockdb(TCHDB *hdb){
  assert(hdb);
  if(pthread_mutex_lock(hdb->dmtx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Unlock the whole database of the hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbunlockdb(TCHDB *hdb){
  assert(hdb);
  if(pthread_mutex_unlock(hdb->dmtx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Lock the write ahead logging file of the hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdblockwal(TCHDB *hdb){
  assert(hdb);
  if(pthread_mutex_lock(hdb->wmtx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}


/* Unlock the write ahead logging file of the hash database object.
   `hdb' specifies the hash database object.
   If successful, the return value is true, else, it is false. */
static bool tchdbunlockwal(TCHDB *hdb){
  assert(hdb);
  if(pthread_mutex_unlock(hdb->wmtx) != 0){
    tchdbsetecode(hdb, TCETHREAD, __FILE__, __LINE__, __func__);
    return false;
  }
  TCTESTYIELD();
  return true;
}



/*************************************************************************************************
 * debugging functions
 *************************************************************************************************/


/* Print meta data of the header into the debugging output.
   `hdb' specifies the hash database object. */
void tchdbprintmeta(TCHDB *hdb){
  assert(hdb);
  if(hdb->dbgfd < 0) return;
  int dbgfd = (hdb->dbgfd == UINT16_MAX) ? 1 : hdb->dbgfd;
  char buf[HDBIOBUFSIZ];
  char *wp = buf;
  wp += sprintf(wp, "META:");
  wp += sprintf(wp, " mmtx=%p", (void *)hdb->mmtx);
  wp += sprintf(wp, " rmtxs=%p", (void *)hdb->rmtxs);
  wp += sprintf(wp, " dmtx=%p", (void *)hdb->dmtx);
  wp += sprintf(wp, " wmtx=%p", (void *)hdb->wmtx);
  wp += sprintf(wp, " eckey=%p", (void *)hdb->eckey);
  wp += sprintf(wp, " rpath=%s", hdb->rpath ? hdb->rpath : "-");
  wp += sprintf(wp, " type=%02X", hdb->type);
  wp += sprintf(wp, " flags=%02X", hdb->flags);
  wp += sprintf(wp, " bnum=%llu", (unsigned long long)hdb->bnum);
  wp += sprintf(wp, " apow=%u", hdb->apow);
  wp += sprintf(wp, " fpow=%u", hdb->fpow);
  wp += sprintf(wp, " opts=%u", hdb->opts);
  wp += sprintf(wp, " path=%s", hdb->path ? hdb->path : "-");
  wp += sprintf(wp, " fd=%d", hdb->fd);
  wp += sprintf(wp, " omode=%u", hdb->omode);
  wp += sprintf(wp, " rnum=%llu", (unsigned long long)hdb->rnum);
  wp += sprintf(wp, " fsiz=%llu", (unsigned long long)hdb->fsiz);
  wp += sprintf(wp, " frec=%llu", (unsigned long long)hdb->frec);
  wp += sprintf(wp, " dfcur=%llu", (unsigned long long)hdb->dfcur);
  wp += sprintf(wp, " iter=%llu", (unsigned long long)hdb->iter);
  wp += sprintf(wp, " map=%p", (void *)hdb->map);
  wp += sprintf(wp, " msiz=%llu", (unsigned long long)hdb->msiz);
  wp += sprintf(wp, " ba32=%p", (void *)hdb->ba32);
  wp += sprintf(wp, " ba64=%p", (void *)hdb->ba64);
  wp += sprintf(wp, " align=%u", hdb->align);
  wp += sprintf(wp, " runit=%u", hdb->runit);
  wp += sprintf(wp, " zmode=%u", hdb->zmode);
  wp += sprintf(wp, " fbpmax=%d", hdb->fbpmax);
  wp += sprintf(wp, " fbpool=%p", (void *)hdb->fbpool);
  wp += sprintf(wp, " fbpnum=%d", hdb->fbpnum);
  wp += sprintf(wp, " fbpmis=%d", hdb->fbpmis);
  wp += sprintf(wp, " drpool=%p", (void *)hdb->drpool);
  wp += sprintf(wp, " drpdef=%p", (void *)hdb->drpdef);
  wp += sprintf(wp, " drpoff=%llu", (unsigned long long)hdb->drpoff);
  wp += sprintf(wp, " recc=%p", (void *)hdb->recc);
  wp += sprintf(wp, " rcnum=%u", hdb->rcnum);
  wp += sprintf(wp, " ecode=%d", hdb->ecode);
  wp += sprintf(wp, " fatal=%u", hdb->fatal);
  wp += sprintf(wp, " inode=%llu", (unsigned long long)(uint64_t)hdb->inode);
  wp += sprintf(wp, " mtime=%llu", (unsigned long long)(uint64_t)hdb->mtime);
  wp += sprintf(wp, " dfunit=%u", hdb->dfunit);
  wp += sprintf(wp, " dfcnt=%u", hdb->dfcnt);
  wp += sprintf(wp, " tran=%d", hdb->tran);
  wp += sprintf(wp, " walfd=%d", hdb->walfd);
  wp += sprintf(wp, " walend=%llu", (unsigned long long)hdb->walend);
  wp += sprintf(wp, " dbgfd=%d", hdb->dbgfd);
  wp += sprintf(wp, " cnt_writerec=%lld", (long long)hdb->cnt_writerec);
  wp += sprintf(wp, " cnt_reuserec=%lld", (long long)hdb->cnt_reuserec);
  wp += sprintf(wp, " cnt_moverec=%lld", (long long)hdb->cnt_moverec);
  wp += sprintf(wp, " cnt_readrec=%lld", (long long)hdb->cnt_readrec);
  wp += sprintf(wp, " cnt_searchfbp=%lld", (long long)hdb->cnt_searchfbp);
  wp += sprintf(wp, " cnt_insertfbp=%lld", (long long)hdb->cnt_insertfbp);
  wp += sprintf(wp, " cnt_splicefbp=%lld", (long long)hdb->cnt_splicefbp);
  wp += sprintf(wp, " cnt_dividefbp=%lld", (long long)hdb->cnt_dividefbp);
  wp += sprintf(wp, " cnt_mergefbp=%lld", (long long)hdb->cnt_mergefbp);
  wp += sprintf(wp, " cnt_reducefbp=%lld", (long long)hdb->cnt_reducefbp);
  wp += sprintf(wp, " cnt_appenddrp=%lld", (long long)hdb->cnt_appenddrp);
  wp += sprintf(wp, " cnt_deferdrp=%lld", (long long)hdb->cnt_deferdrp);
  wp += sprintf(wp, " cnt_flushdrp=%lld", (long long)hdb->cnt_flushdrp);
  wp += sprintf(wp, " cnt_adjrecc=%lld", (long long)hdb->cnt_adjrecc);
  wp += sprintf(wp, " cnt_defrag=%lld", (long long)hdb->cnt_defrag);
  wp += sprintf(wp, " cnt_shiftrec=%lld", (long long)hdb->cnt_shiftrec);
  wp += sprintf(wp, " cnt_trunc=%lld", (long long)hdb->cnt_trunc);
  *(wp++) = '\n';
  tcwrite(dbgfd, buf, wp - buf);
}


/* Print a record information into the debugging output.
   `hdb' specifies the hash database object.
   `rec' specifies the record. */
void tchdbprintrec(TCHDB *hdb, TCHREC *rec){
  assert(hdb && rec);
  if(hdb->dbgfd < 0) return;
  int dbgfd = (hdb->dbgfd == UINT16_MAX) ? 1 : hdb->dbgfd;
  char buf[HDBIOBUFSIZ];
  char *wp = buf;
  wp += sprintf(wp, "REC:");
  wp += sprintf(wp, " off=%llu", (unsigned long long)rec->off);
  wp += sprintf(wp, " rsiz=%u", rec->rsiz);
  wp += sprintf(wp, " magic=%02X", rec->magic);
  wp += sprintf(wp, " hash=%02X", rec->hash);
  wp += sprintf(wp, " left=%llu", (unsigned long long)rec->left);
  wp += sprintf(wp, " right=%llu", (unsigned long long)rec->right);
  wp += sprintf(wp, " ksiz=%u", rec->ksiz);
  wp += sprintf(wp, " vsiz=%u", rec->vsiz);
  wp += sprintf(wp, " psiz=%u", rec->psiz);
  wp += sprintf(wp, " kbuf=%p", (void *)rec->kbuf);
  wp += sprintf(wp, " vbuf=%p", (void *)rec->vbuf);
  wp += sprintf(wp, " boff=%llu", (unsigned long long)rec->boff);
  wp += sprintf(wp, " bbuf=%p", (void *)rec->bbuf);
  *(wp++) = '\n';
  tcwrite(dbgfd, buf, wp - buf);
}



// END OF FILE
