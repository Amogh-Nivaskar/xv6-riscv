// In-memory dirty-block caches (data / indirect / inode / imap).
//
// A block is buffered here as it's written and only gets a real on-disk
// address once its cache is drained into a segment at flush time -- blocks
// can't be updated in place, so repeated writes to the same inode, indirect
// or data block must find and update the existing node here rather than
// appending a duplicate. See fs.h for the node layouts and the fixed flush
// ordering (data, then indirect, then inode, then imap) this depends on.
//
// Interface, per cache: <type>_lookup() (read-only, may return 0),
// <type>_get_or_create() (never returns 0), <type>_pop() (finds, unlinks
// and returns the node -- caller now owns it, e.g. free(x_cache_pop(...))
// for a plain delete). Each also has a static "_nolock" variant used
// internally by get_or_create to avoid a lock-release/re-acquire gap
// between checking for an existing node and inserting a new one.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "cache.h"

struct data_cache_node dataCacheHead;
struct indirect_cache_node indirectCacheHead;
struct inode_cache_node inodeCacheHead;
struct imap_cache_node imapCacheHead;
struct segsum_cache_node segsumCacheHead;

struct spinlock dataCacheLock;
struct spinlock indirectCacheLock;
struct spinlock inodeCacheLock;
struct spinlock imapCacheLock;
struct spinlock segsumCacheLock;

void cacheinit(void)
{
  initlock(&dataCacheLock, "dataCacheLock");
  initlock(&indirectCacheLock, "indirectCacheLock");
  initlock(&inodeCacheLock, "inodeCacheLock");
  initlock(&imapCacheLock, "imapCacheLock");
  initlock(&segsumCacheLock, "segsumCacheLock");

  initsleeplock(&dataCacheHead.lock, "dataCacheHead");
  initsleeplock(&indirectCacheHead.lock, "indirectCacheHead");
  initsleeplock(&inodeCacheHead.lock, "inodeCacheHead");
  initsleeplock(&imapCacheHead.lock, "imapCacheHead");
  initsleeplock(&segsumCacheHead.lock, "segsumCacheHead");
}

// caller must already hold dataCacheLock
static struct data_cache_node* data_cache_lookup_nolock(uint inum, uint fbn)
{
  struct data_cache_node *nxt = dataCacheHead.next;
  while(nxt){
    if(nxt->inum == inum && nxt->fbn == fbn){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct data_cache_node* data_cache_lookup(uint inum, uint fbn)
{
  acquire(&dataCacheLock);
  struct data_cache_node *n = data_cache_lookup_nolock(inum, fbn);
  release(&dataCacheLock);
  return n;
}

struct data_cache_node* data_cache_get_or_create(uint inum, uint fbn)
{
  acquire(&dataCacheLock);
  struct data_cache_node *n = data_cache_lookup_nolock(inum, fbn);
  if(n){
    release(&dataCacheLock);
    return n;
  }

  // not found: allocate, zero, link in, hand back
  n = kalloc();
  if(n == 0)
    panic("data_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->fbn = fbn;
  n->isdirty = 1;   // brand new, never-persisted content

  n->next = dataCacheHead.next;
  dataCacheHead.next = n;
  release(&dataCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// populates a new node from a block just read off disk (fs.c's job --
// this file stays disk-unaware). Re-checks for an existing node under
// the lock before inserting: another CPU could have raced ahead and
// added this same key while we were off doing the (slow, sleeping) disk
// read, in which case that node is authoritative and our just-read
// content is discarded in favor of it.
struct data_cache_node* data_cache_add(uint inum, uint fbn, char *data)
{
  acquire(&dataCacheLock);
  struct data_cache_node *n = data_cache_lookup_nolock(inum, fbn);
  if(n){
    release(&dataCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("data_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->fbn = fbn;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  memmove(n->data, data, BSIZE);

  n->next = dataCacheHead.next;
  dataCacheHead.next = n;
  release(&dataCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct data_cache_node* data_cache_pop(uint inum, uint fbn)
{
  acquire(&dataCacheLock);
  struct data_cache_node *prev = &dataCacheHead;
  struct data_cache_node *cur = dataCacheHead.next;

  while(cur){
    if(cur->inum == inum && cur->fbn == fbn){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      release(&dataCacheLock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&dataCacheLock);
  return 0;
}

void data_node_release(struct data_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("data_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// caller must already hold indirectCacheLock
static struct indirect_cache_node* indirect_cache_lookup_nolock(uint inum)
{
  struct indirect_cache_node *nxt = indirectCacheHead.next;
  while(nxt){
    if(nxt->inum == inum){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct indirect_cache_node* indirect_cache_lookup(uint inum)
{
  acquire(&indirectCacheLock);
  struct indirect_cache_node *n = indirect_cache_lookup_nolock(inum);
  release(&indirectCacheLock);
  return n;
}

struct indirect_cache_node* indirect_cache_get_or_create(uint inum)
{
  acquire(&indirectCacheLock);
  struct indirect_cache_node *n = indirect_cache_lookup_nolock(inum);
  if(n){
    release(&indirectCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("indirect_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->isdirty = 1;   // brand new, never-persisted content
  initsleeplock(&n->lock, "indirect_cache_node");

  n->next = indirectCacheHead.next;
  indirectCacheHead.next = n;
  release(&indirectCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct indirect_cache_node* indirect_cache_add(uint inum, uint *addrs)
{
  acquire(&indirectCacheLock);
  struct indirect_cache_node *n = indirect_cache_lookup_nolock(inum);
  if(n){
    release(&indirectCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("indirect_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  initsleeplock(&n->lock, "indirect_cache_node");

  memmove(n->addrs, addrs, sizeof(n->addrs));

  n->next = indirectCacheHead.next;
  indirectCacheHead.next = n;
  release(&indirectCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct indirect_cache_node* indirect_cache_pop(uint inum)
{
  acquire(&indirectCacheLock);
  struct indirect_cache_node *prev = &indirectCacheHead;
  struct indirect_cache_node *cur = indirectCacheHead.next;

  while(cur){
    if(cur->inum == inum){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      release(&indirectCacheLock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&indirectCacheLock);
  return 0;
}

void indirect_node_release(struct indirect_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("indirect_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// caller must already hold inodeCacheLock
static struct inode_cache_node* inode_cache_lookup_nolock(uint inum)
{
  struct inode_cache_node *nxt = inodeCacheHead.next;
  while(nxt){
    if(nxt->inum == inum){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct inode_cache_node* inode_cache_lookup(uint inum)
{
  acquire(&inodeCacheLock);
  struct inode_cache_node *n = inode_cache_lookup_nolock(inum);
  release(&inodeCacheLock);
  return n;
}

struct inode_cache_node* inode_cache_get_or_create(uint inum)
{
  acquire(&inodeCacheLock);
  struct inode_cache_node *n = inode_cache_lookup_nolock(inum);
  if(n){
    release(&inodeCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("inode_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->isdirty = 1;   // brand new, never-persisted content
  initsleeplock(&n->lock, "inode_cache_node");

  n->next = inodeCacheHead.next;
  inodeCacheHead.next = n;
  release(&inodeCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct inode_cache_node* inode_cache_add(uint inum, struct dinode *din)
{
  acquire(&inodeCacheLock);
  struct inode_cache_node *n = inode_cache_lookup_nolock(inum);
  if(n){
    release(&inodeCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("inode_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->din = *din;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  initsleeplock(&n->lock, "inode_cache_node");

  n->next = inodeCacheHead.next;
  inodeCacheHead.next = n;
  release(&inodeCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct inode_cache_node* inode_cache_pop(uint inum)
{
  acquire(&inodeCacheLock);
  struct inode_cache_node *prev = &inodeCacheHead;
  struct inode_cache_node *cur = inodeCacheHead.next;

  while(cur){
    if(cur->inum == inum){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      release(&inodeCacheLock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&inodeCacheLock);
  return 0;
}

void inode_node_release(struct inode_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("inode_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// caller must already hold imapCacheLock
static struct imap_cache_node* imap_cache_lookup_nolock(uint idx)
{
  struct imap_cache_node *nxt = imapCacheHead.next;
  while(nxt){
    if(nxt->idx == idx){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct imap_cache_node* imap_cache_lookup(uint idx)
{
  acquire(&imapCacheLock);
  struct imap_cache_node *n = imap_cache_lookup_nolock(idx);
  release(&imapCacheLock);
  return n;
}

struct imap_cache_node* imap_cache_get_or_create(uint idx)
{
  acquire(&imapCacheLock);
  struct imap_cache_node *n = imap_cache_lookup_nolock(idx);
  if(n){
    release(&imapCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("imap_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->idx = idx;
  n->isdirty = 1;   // brand new, never-persisted content
  initsleeplock(&n->lock, "imap_cache_node");

  n->next = imapCacheHead.next;
  imapCacheHead.next = n;
  release(&imapCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct imap_cache_node* imap_cache_add(uint idx, uint *addrs)
{
  acquire(&imapCacheLock);
  struct imap_cache_node *n = imap_cache_lookup_nolock(idx);
  if(n){
    release(&imapCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("imap_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->idx = idx;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  initsleeplock(&n->lock, "imap_cache_node");
  memmove(n->addrs, addrs, sizeof(n->addrs));

  n->next = imapCacheHead.next;
  imapCacheHead.next = n;
  release(&imapCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct imap_cache_node* imap_cache_pop(uint idx)
{
  acquire(&imapCacheLock);
  struct imap_cache_node *prev = &imapCacheHead;
  struct imap_cache_node *cur = imapCacheHead.next;

  while(cur){
    if(cur->idx == idx){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      release(&imapCacheLock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&imapCacheLock);
  return 0;
}

void imap_node_release(struct imap_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("imap_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// caller must already hold segsumCacheLock
static struct segsum_cache_node* segsum_cache_lookup_nolock(uint segnum, uint groupidx)
{
  struct segsum_cache_node *nxt = segsumCacheHead.next;
  while(nxt){
    if(nxt->segnum == segnum && nxt->groupidx == groupidx){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct segsum_cache_node* segsum_cache_lookup(uint segnum, uint groupidx)
{
  acquire(&segsumCacheLock);
  struct segsum_cache_node *n = segsum_cache_lookup_nolock(segnum, groupidx);
  release(&segsumCacheLock);
  return n;
}

struct segsum_cache_node* segsum_cache_get_or_create(uint segnum, uint groupidx)
{
  acquire(&segsumCacheLock);
  struct segsum_cache_node *n = segsum_cache_lookup_nolock(segnum, groupidx);
  if(n){
    release(&segsumCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("segsum_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->segnum = segnum;
  n->groupidx = groupidx;
  n->isdirty = 1;   // brand new, never-persisted content
  initsleeplock(&n->lock, "segsum_cache_node");

  n->next = segsumCacheHead.next;
  segsumCacheHead.next = n;
  release(&segsumCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct segsum_cache_node* segsum_cache_add(uint segnum, uint groupidx, struct segsum_block *ssb)
{
  acquire(&segsumCacheLock);
  struct segsum_cache_node *n = segsum_cache_lookup_nolock(segnum, groupidx);
  if(n){
    release(&segsumCacheLock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("segsum_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->segnum = segnum;
  n->groupidx = groupidx;
  n->ssb = *ssb;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  initsleeplock(&n->lock, "segsum_cache_node");

  n->next = segsumCacheHead.next;
  segsumCacheHead.next = n;
  release(&segsumCacheLock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct segsum_cache_node* segsum_cache_pop(uint segnum, uint groupidx)
{
  acquire(&segsumCacheLock);
  struct segsum_cache_node *prev = &segsumCacheHead;
  struct segsum_cache_node *cur = segsumCacheHead.next;

  while(cur){
    if(cur->segnum == segnum && cur->groupidx == groupidx){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      release(&segsumCacheLock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&segsumCacheLock);
  return 0;
}

void segsum_node_release(struct segsum_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("segsum_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}
