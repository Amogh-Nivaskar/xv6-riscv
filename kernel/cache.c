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
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "cache.h"

struct data_cache_node dataCacheHead;
struct indirect_cache_node indirectCacheHead;
struct inode_cache_node inodeCacheHead;
struct imap_cache_node imapCacheHead;

struct spinlock dataCacheLock;
struct spinlock indirectCacheLock;
struct spinlock inodeCacheLock;
struct spinlock imapCacheLock;

void cacheinit(void)
{
  initlock(&dataCacheLock, "dataCacheLock");
  initlock(&indirectCacheLock, "indirectCacheLock");
  initlock(&inodeCacheLock, "inodeCacheLock");
  initlock(&imapCacheLock, "imapCacheLock");
}

// caller must already hold dataCacheLock
static struct data_cache_node* data_cache_lookup_nolock(uint inum, uint fbn)
{
  struct data_cache_node *nxt = dataCacheHead.next;
  while(nxt){
    if(nxt->inum == inum && nxt->fbn == fbn)
      return nxt;
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

  n->next = dataCacheHead.next;
  dataCacheHead.next = n;
  release(&dataCacheLock);

  return n;
}

struct data_cache_node* data_cache_pop(uint inum, uint fbn)
{
  acquire(&dataCacheLock);
  struct data_cache_node *prev = &dataCacheHead;
  struct data_cache_node *cur = dataCacheHead.next;

  while(cur){
    if(cur->inum == inum && cur->fbn == fbn){
      prev->next = cur->next;
      release(&dataCacheLock);
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&dataCacheLock);
  return 0;
}

// caller must already hold indirectCacheLock
static struct indirect_cache_node* indirect_cache_lookup_nolock(uint inum)
{
  struct indirect_cache_node *nxt = indirectCacheHead.next;
  while(nxt){
    if(nxt->inum == inum)
      return nxt;
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

  n->next = indirectCacheHead.next;
  indirectCacheHead.next = n;
  release(&indirectCacheLock);

  return n;
}

struct indirect_cache_node* indirect_cache_pop(uint inum)
{
  acquire(&indirectCacheLock);
  struct indirect_cache_node *prev = &indirectCacheHead;
  struct indirect_cache_node *cur = indirectCacheHead.next;

  while(cur){
    if(cur->inum == inum){
      prev->next = cur->next;
      release(&indirectCacheLock);
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&indirectCacheLock);
  return 0;
}

// caller must already hold inodeCacheLock
static struct inode_cache_node* inode_cache_lookup_nolock(uint inum)
{
  struct inode_cache_node *nxt = inodeCacheHead.next;
  while(nxt){
    if(nxt->inum == inum)
      return nxt;
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

  n->next = inodeCacheHead.next;
  inodeCacheHead.next = n;
  release(&inodeCacheLock);

  return n;
}

struct inode_cache_node* inode_cache_pop(uint inum)
{
  acquire(&inodeCacheLock);
  struct inode_cache_node *prev = &inodeCacheHead;
  struct inode_cache_node *cur = inodeCacheHead.next;

  while(cur){
    if(cur->inum == inum){
      prev->next = cur->next;
      release(&inodeCacheLock);
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&inodeCacheLock);
  return 0;
}

// caller must already hold imapCacheLock
static struct imap_cache_node* imap_cache_lookup_nolock(uint idx)
{
  struct imap_cache_node *nxt = imapCacheHead.next;
  while(nxt){
    if(nxt->idx == idx)
      return nxt;
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

  n->next = imapCacheHead.next;
  imapCacheHead.next = n;
  release(&imapCacheLock);

  return n;
}

struct imap_cache_node* imap_cache_pop(uint idx)
{
  acquire(&imapCacheLock);
  struct imap_cache_node *prev = &imapCacheHead;
  struct imap_cache_node *cur = imapCacheHead.next;

  while(cur){
    if(cur->idx == idx){
      prev->next = cur->next;
      release(&imapCacheLock);
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  release(&imapCacheLock);
  return 0;
}
