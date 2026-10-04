// In-memory dirty-block caches (data / indirect / inode / imap / segsum).
//
// A block is buffered here as it's written and only gets a real on-disk
// address once its cache is drained into a segment at flush time -- blocks
// can't be updated in place, so repeated writes to the same inode, indirect
// or data block must find and update the existing node here rather than
// appending a duplicate. See fs.h for the node layouts and the fixed flush
// ordering (data, then indirect, then inode, then imap) this depends on.
//
// All five caches' state (head sentinel, lock, node count) is collected
// into one struct cache below, rather than five sets of loose globals --
// matches bio.c's bcache (lock + head bundled together), just extended to
// cover all five types and the count fields each now needs for eviction.
// This struct is cache.c-internal: nothing outside this file reaches into
// it directly, same as before the refactor -- callers still only go
// through the functions below.
//
// Interface, per cache: <type>_lookup() (read-only, may return 0),
// <type>_get_or_create() (never returns 0), <type>_pop() (finds, unlinks
// and returns the node -- caller now owns it, e.g. free(x_cache_pop(...))
// for a plain delete), <type>_cache_count() (current node count, for
// eviction). Each also has a static "_nolock" variant used internally by
// get_or_create to avoid a lock-release/re-acquire gap between checking
// for an existing node and inserting a new one.

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "cache.h"

struct cache {
  struct {
    struct data_cache_node head;
    struct spinlock lock;
    uint count;
  } data;

  struct {
    struct indirect_cache_node head;
    struct spinlock lock;
    uint count;
  } indirect;

  struct {
    struct inode_cache_node head;
    struct spinlock lock;
    uint count;
  } inode;

  struct {
    struct imap_cache_node head;
    struct spinlock lock;
    uint count;
  } imap;

  struct {
    struct segsum_cache_node head;
    struct spinlock lock;
    uint count;
  } segsum;

  // combined dirty-node count across data/indirect/inode/imap (not segsum --
  // those aren't part of the flush-threshold decision). Touched from four
  // different type-lock contexts on the increment side and none on the
  // decrement side, so it gets its own lock rather than borrowing one of
  // the type locks.
  uint dirtyCount;
  struct spinlock dirtyCountLock;
};

struct cache cache;

void cacheinit(void)
{
  initlock(&cache.data.lock, "dataCacheLock");
  initlock(&cache.indirect.lock, "indirectCacheLock");
  initlock(&cache.inode.lock, "inodeCacheLock");
  initlock(&cache.imap.lock, "imapCacheLock");
  initlock(&cache.segsum.lock, "segsumCacheLock");
  initlock(&cache.dirtyCountLock, "dirtyCountLock");

  initsleeplock(&cache.data.head.lock, "dataCacheHead");
  initsleeplock(&cache.indirect.head.lock, "indirectCacheHead");
  initsleeplock(&cache.inode.head.lock, "inodeCacheHead");
  initsleeplock(&cache.imap.head.lock, "imapCacheHead");
  initsleeplock(&cache.segsum.head.lock, "segsumCacheHead");
}

// combined dirty-node count across data/indirect/inode/imap -- what
// maybe_flush_dirty()'s threshold check compares against.
uint cache_dirty_count(void)
{
  acquire(&cache.dirtyCountLock);
  uint n = cache.dirtyCount;
  release(&cache.dirtyCountLock);
  return n;
}

// caller must already hold cache.data.lock
static struct data_cache_node* data_cache_lookup_nolock(uint inum, uint fbn)
{
  struct data_cache_node *nxt = cache.data.head.next;
  while(nxt){
    if(nxt->inum == inum && nxt->fbn == fbn){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      nxt->lastAccessTicks = ticks;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct data_cache_node* data_cache_lookup(uint inum, uint fbn)
{
  acquire(&cache.data.lock);
  struct data_cache_node *n = data_cache_lookup_nolock(inum, fbn);
  release(&cache.data.lock);
  return n;
}

struct data_cache_node* data_cache_get_or_create(uint inum, uint fbn)
{
  acquire(&cache.data.lock);
  struct data_cache_node *n = data_cache_lookup_nolock(inum, fbn);
  if(n){
    release(&cache.data.lock);
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
  n->lastAccessTicks = ticks;

  n->next = cache.data.head.next;
  cache.data.head.next = n;
  cache.data.count++;
  release(&cache.data.lock);

  acquire(&cache.dirtyCountLock);
  cache.dirtyCount++;
  release(&cache.dirtyCountLock);

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
  acquire(&cache.data.lock);
  struct data_cache_node *n = data_cache_lookup_nolock(inum, fbn);
  if(n){
    release(&cache.data.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("data_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->fbn = fbn;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  n->lastAccessTicks = ticks;
  memmove(n->data, data, BSIZE);

  n->next = cache.data.head.next;
  cache.data.head.next = n;
  cache.data.count++;
  release(&cache.data.lock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct data_cache_node* data_cache_pop(uint inum, uint fbn)
{
  acquire(&cache.data.lock);
  struct data_cache_node *prev = &cache.data.head;
  struct data_cache_node *cur = cache.data.head.next;

  while(cur){
    if(cur->inum == inum && cur->fbn == fbn){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      cache.data.count--;
      release(&cache.data.lock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&cache.data.lock);
  return 0;
}

void data_node_release(struct data_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("data_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// clears isdirty and decrements the combined dirty count together, so the
// two can't drift out of sync the way dirtyBlocksCounter's standalone
// decrement in fs.c did. Caller must hold node's sleeplock (as returned
// by data_cache_lookup_dirty()) and still release it afterward.
void data_node_mark_clean(struct data_cache_node *node){
  node->isdirty = 0;
  acquire(&cache.dirtyCountLock);
  cache.dirtyCount--;
  release(&cache.dirtyCountLock);
}

uint data_cache_count(void)
{
  acquire(&cache.data.lock);
  uint n = cache.data.count;
  release(&cache.data.lock);
  return n;
}

// finds the first dirty node, if any, and hands it back checked out --
// unlike _pop(), does NOT unlink it. Caller (the flush path) is expected
// to clear isdirty and call data_node_release() once it's been written
// to a segment, leaving the node in the cache as a clean entry.
struct data_cache_node* data_cache_lookup_dirty(void)
{
  acquire(&cache.data.lock);
  struct data_cache_node *nxt = cache.data.head.next;
  while(nxt){
    if(nxt->isdirty){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      release(&cache.data.lock);
      return nxt;
    }
    nxt = nxt->next;
  }
  release(&cache.data.lock);
  return 0;
}

// evicts the single least-recently-used eligible (clean, not checked
// out) node, if any. One node per call, not a batch -- finding the
// oldest eligible node is an O(n) scan, so evicting several under one
// lock acquisition would make the critical section both longer and
// O(n*k); the caller loops this instead. Returns how many nodes were
// evicted -- 1 if one was found, 0 if nothing eligible was found --
// matching segsum_cache_evict_all()'s "count evicted" return shape.
uint data_cache_evict_lru(void)
{
  acquire(&cache.data.lock);

  struct data_cache_node *prev = &cache.data.head;
  struct data_cache_node *cur = cache.data.head.next;
  struct data_cache_node *victimPrev = 0;
  struct data_cache_node *victim = 0;

  while(cur){
    if(!cur->isdirty && cur->refcnt == 0 &&
       (victim == 0 || cur->lastAccessTicks < victim->lastAccessTicks)){
      victim = cur;
      victimPrev = prev;
    }
    prev = cur;
    cur = cur->next;
  }

  if(victim == 0){
    release(&cache.data.lock);
    return 0;
  }

  // already confirmed refcnt == 0 under this same lock, so no one else
  // could have checked it out in the meantime -- acquiresleep here
  // matches _pop()'s pattern rather than because this should ever block.
  acquiresleep(&victim->lock);
  victimPrev->next = victim->next;
  cache.data.count--;
  release(&cache.data.lock);

  kfree(victim);
  return 1;
}

// caller must already hold cache.indirect.lock
static struct indirect_cache_node* indirect_cache_lookup_nolock(uint inum)
{
  struct indirect_cache_node *nxt = cache.indirect.head.next;
  while(nxt){
    if(nxt->inum == inum){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      nxt->lastAccessTicks = ticks;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct indirect_cache_node* indirect_cache_lookup(uint inum)
{
  acquire(&cache.indirect.lock);
  struct indirect_cache_node *n = indirect_cache_lookup_nolock(inum);
  release(&cache.indirect.lock);
  return n;
}

struct indirect_cache_node* indirect_cache_get_or_create(uint inum)
{
  acquire(&cache.indirect.lock);
  struct indirect_cache_node *n = indirect_cache_lookup_nolock(inum);
  if(n){
    release(&cache.indirect.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("indirect_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->isdirty = 1;   // brand new, never-persisted content
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "indirect_cache_node");

  n->next = cache.indirect.head.next;
  cache.indirect.head.next = n;
  cache.indirect.count++;
  release(&cache.indirect.lock);

  acquire(&cache.dirtyCountLock);
  cache.dirtyCount++;
  release(&cache.dirtyCountLock);

  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct indirect_cache_node* indirect_cache_add(uint inum, uint *addrs)
{
  acquire(&cache.indirect.lock);
  struct indirect_cache_node *n = indirect_cache_lookup_nolock(inum);
  if(n){
    release(&cache.indirect.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("indirect_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "indirect_cache_node");

  memmove(n->addrs, addrs, sizeof(n->addrs));

  n->next = cache.indirect.head.next;
  cache.indirect.head.next = n;
  cache.indirect.count++;
  release(&cache.indirect.lock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct indirect_cache_node* indirect_cache_pop(uint inum)
{
  acquire(&cache.indirect.lock);
  struct indirect_cache_node *prev = &cache.indirect.head;
  struct indirect_cache_node *cur = cache.indirect.head.next;

  while(cur){
    if(cur->inum == inum){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      cache.indirect.count--;
      release(&cache.indirect.lock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&cache.indirect.lock);
  return 0;
}

void indirect_node_release(struct indirect_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("indirect_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// see data_node_mark_clean -- same reasoning.
void indirect_node_mark_clean(struct indirect_cache_node *node){
  node->isdirty = 0;
  acquire(&cache.dirtyCountLock);
  cache.dirtyCount--;
  release(&cache.dirtyCountLock);
}

uint indirect_cache_count(void)
{
  acquire(&cache.indirect.lock);
  uint n = cache.indirect.count;
  release(&cache.indirect.lock);
  return n;
}

// see data_cache_lookup_dirty -- same reasoning (found node stays in the
// cache, caller clears isdirty and releases once it's been flushed).
struct indirect_cache_node* indirect_cache_lookup_dirty(void)
{
  acquire(&cache.indirect.lock);
  struct indirect_cache_node *nxt = cache.indirect.head.next;
  while(nxt){
    if(nxt->isdirty){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      release(&cache.indirect.lock);
      return nxt;
    }
    nxt = nxt->next;
  }
  release(&cache.indirect.lock);
  return 0;
}

// see data_cache_evict_lru -- same reasoning.
uint indirect_cache_evict_lru(void)
{
  acquire(&cache.indirect.lock);

  struct indirect_cache_node *prev = &cache.indirect.head;
  struct indirect_cache_node *cur = cache.indirect.head.next;
  struct indirect_cache_node *victimPrev = 0;
  struct indirect_cache_node *victim = 0;

  while(cur){
    if(!cur->isdirty && cur->refcnt == 0 &&
       (victim == 0 || cur->lastAccessTicks < victim->lastAccessTicks)){
      victim = cur;
      victimPrev = prev;
    }
    prev = cur;
    cur = cur->next;
  }

  if(victim == 0){
    release(&cache.indirect.lock);
    return 0;
  }

  acquiresleep(&victim->lock);
  victimPrev->next = victim->next;
  cache.indirect.count--;
  release(&cache.indirect.lock);

  kfree(victim);
  return 1;
}

// caller must already hold cache.inode.lock
static struct inode_cache_node* inode_cache_lookup_nolock(uint inum)
{
  struct inode_cache_node *nxt = cache.inode.head.next;
  while(nxt){
    if(nxt->inum == inum){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      nxt->lastAccessTicks = ticks;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct inode_cache_node* inode_cache_lookup(uint inum)
{
  acquire(&cache.inode.lock);
  struct inode_cache_node *n = inode_cache_lookup_nolock(inum);
  release(&cache.inode.lock);
  return n;
}

struct inode_cache_node* inode_cache_get_or_create(uint inum)
{
  acquire(&cache.inode.lock);
  struct inode_cache_node *n = inode_cache_lookup_nolock(inum);
  if(n){
    release(&cache.inode.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("inode_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->isdirty = 1;   // brand new, never-persisted content
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "inode_cache_node");

  n->next = cache.inode.head.next;
  cache.inode.head.next = n;
  cache.inode.count++;
  release(&cache.inode.lock);

  acquire(&cache.dirtyCountLock);
  cache.dirtyCount++;
  release(&cache.dirtyCountLock);

  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct inode_cache_node* inode_cache_add(uint inum, struct dinode *din)
{
  acquire(&cache.inode.lock);
  struct inode_cache_node *n = inode_cache_lookup_nolock(inum);
  if(n){
    release(&cache.inode.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("inode_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->inum = inum;
  n->din = *din;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "inode_cache_node");

  n->next = cache.inode.head.next;
  cache.inode.head.next = n;
  cache.inode.count++;
  release(&cache.inode.lock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct inode_cache_node* inode_cache_pop(uint inum)
{
  acquire(&cache.inode.lock);
  struct inode_cache_node *prev = &cache.inode.head;
  struct inode_cache_node *cur = cache.inode.head.next;

  while(cur){
    if(cur->inum == inum){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      cache.inode.count--;
      release(&cache.inode.lock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&cache.inode.lock);
  return 0;
}

void inode_node_release(struct inode_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("inode_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// see data_node_mark_clean -- same reasoning.
void inode_node_mark_clean(struct inode_cache_node *node){
  node->isdirty = 0;
  acquire(&cache.dirtyCountLock);
  cache.dirtyCount--;
  release(&cache.dirtyCountLock);
}

uint inode_cache_count(void)
{
  acquire(&cache.inode.lock);
  uint n = cache.inode.count;
  release(&cache.inode.lock);
  return n;
}

// see data_cache_lookup_dirty -- same reasoning (found node stays in the
// cache, caller clears isdirty and releases once it's been flushed).
struct inode_cache_node* inode_cache_lookup_dirty(void)
{
  acquire(&cache.inode.lock);
  struct inode_cache_node *nxt = cache.inode.head.next;
  while(nxt){
    if(nxt->isdirty){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      release(&cache.inode.lock);
      return nxt;
    }
    nxt = nxt->next;
  }
  release(&cache.inode.lock);
  return 0;
}

// see data_cache_evict_lru -- same reasoning.
uint inode_cache_evict_lru(void)
{
  acquire(&cache.inode.lock);

  struct inode_cache_node *prev = &cache.inode.head;
  struct inode_cache_node *cur = cache.inode.head.next;
  struct inode_cache_node *victimPrev = 0;
  struct inode_cache_node *victim = 0;

  while(cur){
    if(!cur->isdirty && cur->refcnt == 0 &&
       (victim == 0 || cur->lastAccessTicks < victim->lastAccessTicks)){
      victim = cur;
      victimPrev = prev;
    }
    prev = cur;
    cur = cur->next;
  }

  if(victim == 0){
    release(&cache.inode.lock);
    return 0;
  }

  acquiresleep(&victim->lock);
  victimPrev->next = victim->next;
  cache.inode.count--;
  release(&cache.inode.lock);

  kfree(victim);
  return 1;
}

// caller must already hold cache.imap.lock
static struct imap_cache_node* imap_cache_lookup_nolock(uint idx)
{
  struct imap_cache_node *nxt = cache.imap.head.next;
  while(nxt){
    if(nxt->idx == idx){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      nxt->lastAccessTicks = ticks;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct imap_cache_node* imap_cache_lookup(uint idx)
{
  acquire(&cache.imap.lock);
  struct imap_cache_node *n = imap_cache_lookup_nolock(idx);
  release(&cache.imap.lock);
  return n;
}

struct imap_cache_node* imap_cache_get_or_create(uint idx)
{
  acquire(&cache.imap.lock);
  struct imap_cache_node *n = imap_cache_lookup_nolock(idx);
  if(n){
    release(&cache.imap.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("imap_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->idx = idx;
  n->isdirty = 1;   // brand new, never-persisted content
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "imap_cache_node");

  n->next = cache.imap.head.next;
  cache.imap.head.next = n;
  cache.imap.count++;
  release(&cache.imap.lock);

  acquire(&cache.dirtyCountLock);
  cache.dirtyCount++;
  release(&cache.dirtyCountLock);

  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct imap_cache_node* imap_cache_add(uint idx, uint *addrs)
{
  acquire(&cache.imap.lock);
  struct imap_cache_node *n = imap_cache_lookup_nolock(idx);
  if(n){
    release(&cache.imap.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("imap_cache_add: out of memory");

  memset(n, 0, sizeof(*n));
  n->idx = idx;
  n->isdirty = 0;   // just read from disk, identical to the on-disk copy
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "imap_cache_node");
  memmove(n->addrs, addrs, sizeof(n->addrs));

  n->next = cache.imap.head.next;
  cache.imap.head.next = n;
  cache.imap.count++;
  release(&cache.imap.lock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct imap_cache_node* imap_cache_pop(uint idx)
{
  acquire(&cache.imap.lock);
  struct imap_cache_node *prev = &cache.imap.head;
  struct imap_cache_node *cur = cache.imap.head.next;

  while(cur){
    if(cur->idx == idx){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      cache.imap.count--;
      release(&cache.imap.lock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&cache.imap.lock);
  return 0;
}

void imap_node_release(struct imap_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("imap_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

// see data_node_mark_clean -- same reasoning.
void imap_node_mark_clean(struct imap_cache_node *node){
  node->isdirty = 0;
  acquire(&cache.dirtyCountLock);
  cache.dirtyCount--;
  release(&cache.dirtyCountLock);
}

uint imap_cache_count(void)
{
  acquire(&cache.imap.lock);
  uint n = cache.imap.count;
  release(&cache.imap.lock);
  return n;
}

// see data_cache_lookup_dirty -- same reasoning (found node stays in the
// cache, caller clears isdirty and releases once it's been flushed).
struct imap_cache_node* imap_cache_lookup_dirty(void)
{
  acquire(&cache.imap.lock);
  struct imap_cache_node *nxt = cache.imap.head.next;
  while(nxt){
    if(nxt->isdirty){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      release(&cache.imap.lock);
      return nxt;
    }
    nxt = nxt->next;
  }
  release(&cache.imap.lock);
  return 0;
}

// see data_cache_evict_lru -- same reasoning.
uint imap_cache_evict_lru(void)
{
  acquire(&cache.imap.lock);

  struct imap_cache_node *prev = &cache.imap.head;
  struct imap_cache_node *cur = cache.imap.head.next;
  struct imap_cache_node *victimPrev = 0;
  struct imap_cache_node *victim = 0;

  while(cur){
    if(!cur->isdirty && cur->refcnt == 0 &&
       (victim == 0 || cur->lastAccessTicks < victim->lastAccessTicks)){
      victim = cur;
      victimPrev = prev;
    }
    prev = cur;
    cur = cur->next;
  }

  if(victim == 0){
    release(&cache.imap.lock);
    return 0;
  }

  acquiresleep(&victim->lock);
  victimPrev->next = victim->next;
  cache.imap.count--;
  release(&cache.imap.lock);

  kfree(victim);
  return 1;
}

// caller must already hold cache.segsum.lock
static struct segsum_cache_node* segsum_cache_lookup_nolock(uint segnum, uint groupidx)
{
  struct segsum_cache_node *nxt = cache.segsum.head.next;
  while(nxt){
    if(nxt->segnum == segnum && nxt->groupidx == groupidx){
      acquiresleep(&nxt->lock);
      nxt->refcnt++;
      nxt->lastAccessTicks = ticks;
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct segsum_cache_node* segsum_cache_lookup(uint segnum, uint groupidx)
{
  acquire(&cache.segsum.lock);
  struct segsum_cache_node *n = segsum_cache_lookup_nolock(segnum, groupidx);
  release(&cache.segsum.lock);
  return n;
}

struct segsum_cache_node* segsum_cache_get_or_create(uint segnum, uint groupidx)
{
  acquire(&cache.segsum.lock);
  struct segsum_cache_node *n = segsum_cache_lookup_nolock(segnum, groupidx);
  if(n){
    release(&cache.segsum.lock);
    return n;
  }

  n = kalloc();
  if(n == 0)
    panic("segsum_cache_get_or_create: out of memory");

  memset(n, 0, sizeof(*n));
  n->segnum = segnum;
  n->groupidx = groupidx;
  n->isdirty = 1;   // brand new, never-persisted content
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "segsum_cache_node");

  n->next = cache.segsum.head.next;
  cache.segsum.head.next = n;
  cache.segsum.count++;
  release(&cache.segsum.lock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

// see data_cache_add -- same reasoning (populate from disk, re-check
// under the lock in case another CPU raced ahead of us).
struct segsum_cache_node* segsum_cache_add(uint segnum, uint groupidx, struct segsum_block *ssb)
{
  acquire(&cache.segsum.lock);
  struct segsum_cache_node *n = segsum_cache_lookup_nolock(segnum, groupidx);
  if(n){
    release(&cache.segsum.lock);
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
  n->lastAccessTicks = ticks;
  initsleeplock(&n->lock, "segsum_cache_node");

  n->next = cache.segsum.head.next;
  cache.segsum.head.next = n;
  cache.segsum.count++;
  release(&cache.segsum.lock);
  acquiresleep(&n->lock);
  n->refcnt++;
  return n;
}

struct segsum_cache_node* segsum_cache_pop(uint segnum, uint groupidx)
{
  acquire(&cache.segsum.lock);
  struct segsum_cache_node *prev = &cache.segsum.head;
  struct segsum_cache_node *cur = cache.segsum.head.next;

  while(cur){
    if(cur->segnum == segnum && cur->groupidx == groupidx){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      cache.segsum.count--;
      release(&cache.segsum.lock);
      cur->refcnt++;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&cache.segsum.lock);
  return 0;
}

void segsum_node_release(struct segsum_cache_node *node){
  if(!holdingsleep(&node->lock))
    panic("segsum_node_release");

  node->refcnt--;
  releasesleep(&node->lock);
}

uint segsum_cache_count(void)
{
  acquire(&cache.segsum.lock);
  uint n = cache.segsum.count;
  release(&cache.segsum.lock);
  return n;
}

// evicts every eligible (clean, not checked out) segsum node in one pass
// and returns how many were actually evicted -- not every node present is
// necessarily eligible (some may be dirty or currently checked out), so
// this can be less than segsum_cache_count() read beforehand. Unlike the
// other four types' _evict_lru(), order doesn't matter here -- segsum
// isn't part of the quota-based round-robin (nothing calls this
// repeatedly tracking how many are left to remove), so there's no reason
// to hunt for the single oldest node when every eligible one is getting
// removed anyway. A straight single-pass walk is also cheaper: no need
// to restart the scan looking for a new minimum after each removal.
uint segsum_cache_evict_all(void)
{
  acquire(&cache.segsum.lock);

  struct segsum_cache_node *prev = &cache.segsum.head;
  struct segsum_cache_node *cur = cache.segsum.head.next;
  uint evicted = 0;

  while(cur){
    if(!cur->isdirty && cur->refcnt == 0){
      acquiresleep(&cur->lock);
      prev->next = cur->next;
      cache.segsum.count--;
      kfree(cur);
      evicted++;
      cur = prev->next;
      continue;
    }
    prev = cur;
    cur = cur->next;
  }

  release(&cache.segsum.lock);
  return evicted;
}
