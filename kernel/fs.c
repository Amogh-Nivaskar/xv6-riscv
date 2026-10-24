// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "cache.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct lfs_superblock sb;
struct checkpoint cp;
struct segsum_block sumblk;
struct segbuf seg;

char group_data[GROUP_CAP][BSIZE];
int group_n;                      // blocks buffered in the current, unflushed group

int cp_counter;
int cp_last_region;  // 0 = checkpoint1 is current, 1 = checkpoint2 is current
struct spinlock cpLock;
struct sleeplock flushLock;

uint lastFlushTicks;
uint lastCheckpointTicks;

int nmeta;
int nblocks;
uint freeinode;

void checkpointinit(int dev, struct lfs_superblock *sb, struct checkpoint *cp);
static struct inode* iget(uint dev, uint inum);
struct inode_cache_node* inode_get(uint dev, uint inum);
struct indirect_cache_node *indirect_get(uint dev, uint inum);
struct imap_cache_node* imap_get(uint dev, uint idx);
struct data_cache_node *data_get(uint dev, uint inum, uint fbn);

// Read the super block.
static void
readsb(int dev, struct lfs_superblock* sb)
{
  struct buf* bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void fsinit(int dev)
{
  readsb(dev, &sb);
  if (sb.magic != FSMAGIC)
    panic("invalid file system");

  checkpointinit(dev, &sb, &cp);
  cacheinit();
  initsleeplock(&seg.lock, "segLock");
  initsleeplock(&flushLock, "flushLock");

  nmeta = 1 + 1 + 2 * sb.checkpointsize;
  nblocks = FSSIZE - nmeta;
}

void checkpointinit(int dev, struct lfs_superblock *sb, struct checkpoint *cp){
  struct buf* b;
  struct checkpoint *cp1 = kalloc();
  if (cp1 == 0){
    panic("initCheckpoint: kalloc");
  }
  struct checkpoint *cp2 = kalloc();
  if (cp2 == 0){
    panic("initCheckpoint: kalloc");
  }

  char *p = (char*) cp1;
  for (int i=0; i < sb->checkpointsize; i++){
    b = bread(dev, sb->checkpoint1start + i);
    memmove(p + (i*BSIZE), b->data, BSIZE);
    brelse(b);
  }

  p = (char*) cp2;
  for (int i=0; i < sb->checkpointsize; i++){
    b = bread(dev, sb->checkpoint2start + i);
    memmove(p + (i*BSIZE), b->data, BSIZE);
    brelse(b);
  }

  if (cp1->timestamp > cp2->timestamp){
    memmove(cp, cp1, sizeof(struct checkpoint));
    cp_last_region = 0;  // checkpoint1 is current; next flush targets checkpoint2
  }else{
    memmove(cp, cp2, sizeof(struct checkpoint));
    cp_last_region = 1;  // checkpoint2 is current; next flush targets checkpoint1
  }

  freeinode = cp->next_inode;
  cp_counter = (int) cp->timestamp;

  kfree(cp1);
  kfree(cp2);
  initlock(&cpLock, "cpLock");
}

// Persists the in-memory checkpoint to whichever of CR0/CR1 was NOT
// most recently written, stamped with a new timestamp -- so if a crash
// happens mid-write, the other region still holds the last fully
// written, consistent checkpoint. 
void checkpoint_flush(void)
{
  struct checkpoint *snapshot = kalloc();
  if (snapshot == 0){
    return;  // best-effort; skip this persist rather than block on OOM
  }

  acquire(&cpLock);
  cp.next_inode = freeinode;
  cp.timestamp = (uint) (++cp_counter);
  memmove(snapshot, &cp, sizeof(cp));
  release(&cpLock);

  uint target = (cp_last_region == 0) ? sb.checkpoint2start : sb.checkpoint1start;
  cp_last_region = 1 - cp_last_region;

  char *p = (char*) snapshot;
  for (int i = 0; i < NCHECKPOINTBLOCKS; i++){
    struct buf *b = bread(ROOTDEV, target + i);
    memmove(b->data, p + (i*BSIZE), BSIZE);
    bwrite(b);
    brelse(b);
  }

  kfree(snapshot);
}


// SEGMENT WRITER

uint checksum(void *data, int nbytes)
{
  uint *w = (uint*) data;
  uint sum = 0;
  int nwords = nbytes / sizeof(uint);
  for(int i = 0; i < nwords; i++)
    sum = (sum << 1 | sum >> 31) ^ w[i];
  return sum;
}

uint segment_start_block(uint segnum)
{
  return nmeta + segnum * SSIZE;
}

uint segment_of_block(uint blockAddr)
{
  return (blockAddr - nmeta) / SSIZE;
}

// Records that a block's owner now points somewhere new: the segment it
// used to live in (if any) loses one live block, the segment it was just
// written into gains one. oldAddr of 0 (never allocated) or 0xFFFFFFFF
// (still awaiting its first-ever flush) means there's no real previous
// segment to charge -- this is the block's first real placement.
void sut_update(uint oldAddr, uint newSegNum)
{
  acquire(&cpLock);
  if (oldAddr != 0 && oldAddr != 0xFFFFFFFF){
    cp.sut[segment_of_block(oldAddr)].live_count--;
  }
  cp.sut[newSegNum].live_count++;
  cp.sut[newSegNum].last_mod_time = cp.global_seq;
  release(&cpLock);
}

void free_segment(uint segNo){
  cp.seg_freemap[segNo/8] |= (1 << (segNo%8));
}

void claim_segment(uint segNo){
  cp.seg_freemap[segNo/8] &= ~(1 << (segNo%8));
}

uint free_segment_count(void)
{
  uint n = 0;
  acquire(&cpLock);
  for(uint s = 0; s < SEG_NUM; s++){
    if(cp.seg_freemap[s/8] & (1 << (s%8))){
      n++;
    }
  }
  release(&cpLock);
  return n;
}

// Picks the claimed, non-active segment with the best cost/benefit score
// to reclaim: score = age*(1-u)/(1+u), rewritten with u = live/SSIZE to
// avoid floating point: age*(SSIZE-live)/(SSIZE+live). Higher is better
// -- old segments that are mostly dead are the cheapest to clean and
// free the most space. Returns -1 if nothing is worth cleaning (every
// claimed segment is either fully live or there simply are none).
int select_segment_to_clean(void)
{
  acquire(&cpLock);
  uint activeSeg = cp.segment_num;
  uint curTime = cp.global_seq;
  int best = -1;
  uint bestScore = 0;

  for(uint s = 0; s < SEG_NUM; s++){
    if(cp.seg_freemap[s/8] & (1 << (s%8))){
      continue;  // free, nothing to clean
    }
    if(s == activeSeg){
      continue;  // still being filled, not a cleaning candidate
    }

    uint live = cp.sut[s].live_count;
    if(live >= SSIZE){
      continue;  // fully live, nothing to gain
    }

    uint age = curTime - cp.sut[s].last_mod_time;
    uint score = age * (SSIZE - live) / (SSIZE + live);

    if(best == -1 || score > bestScore){
      best = (int) s;
      bestScore = score;
    }
  }

  release(&cpLock);
  return best;
}

uint next_free_segment(void)
{
  for(uint s = 0; s < SEG_NUM; s++){
    if(cp.seg_freemap[s/8] & (1 << (s%8))){
      claim_segment(s);  // claim it: no longer free
      return s;
    }
  }
  panic("next_free_segment: no free segments");
  return 0;  // unreachable
}

// Scans a to be cleaned segment's groups and marks every block that's still
// live (its owner still points at this exact position) dirty, so that
// the next flush picks it up and rewrites it elsewhere.
// Blocks that are already dead are left alone as they are orphaned
// with nothing pointing to them and the disk content is about to be
// reclaimed as free space after the segment is cleaned and can be reused.
void clean_segment(uint segnum)
{
  uint base = segment_start_block(segnum);
  uint pos = 0;

  uint foundLive = 0;

  while (pos < SSIZE){
    struct buf *b = bread(ROOTDEV, base + pos);
    struct segsum_block *ssb = (struct segsum_block*) b->data;

    if (checksum(ssb, sizeof(*ssb) - sizeof(uint)) != ssb->self_checksum){
      brelse(b);
      break;  // not a real group -- the segment's unused trailing slot
    }

    int count = 0;
    while (count < NDATA_PER_SEGSUM - 1 &&
           !(ssb->entries[count].tag1 == 0 && ssb->entries[count].tag2 == 0)){
      count++;
    }

    for (int i = 0; i < count; i++){
      uint tag1 = ssb->entries[i].tag1;
      uint tag2 = ssb->entries[i].tag2;
      uint blockPos = base + pos + 1 + i;

      if (tag1 == 0){
        // imap block, idx = tag2-1
        uint idx = tag2 - 1;
        acquire(&cpLock);
        uint cur = cp.imap_addr[idx];
        release(&cpLock);
        if (cur == blockPos){
          struct imap_cache_node *n = imap_get(ROOTDEV, idx);
          if (n != 0){
            imap_node_mark_dirty(n);
            imap_node_release(n);
            foundLive++;
          }
        }
      }else if (tag2 == 0){
        // inode block, inum = tag1
        uint inum = tag1;
        struct imap_cache_node *imapblk = imap_get(ROOTDEV, IMAP_BLK_IDX(inum));
        if (imapblk != 0){
          uint cur = imapblk->addrs[IMAP_OFFSET(inum)];
          imap_node_release(imapblk);
          if (cur == blockPos){
            struct inode_cache_node *n = inode_get(ROOTDEV, inum);
            if (n != 0){
              inode_node_mark_dirty(n);
              inode_node_release(n);
              foundLive++;
            }
          }
        }
      }else if (tag2 == 0xFFFFFFFF){
        // indirect block, inum = tag1
        uint inum = tag1;
        struct inode_cache_node *inodeblk = inode_get(ROOTDEV, inum);
        if (inodeblk != 0){
          uint cur = inodeblk->din.addrs[NDIRECT];
          inode_node_release(inodeblk);
          if (cur == blockPos){
            struct indirect_cache_node *n = indirect_get(ROOTDEV, inum);
            if (n != 0){
              indirect_node_mark_dirty(n);
              indirect_node_release(n);
              foundLive++;
            }
          }
        }
      }else{
        // data block, inum = tag1, fbn = tag2-1
        uint inum = tag1;
        uint fbn = tag2 - 1;
        uint cur = 0;
        int haveCur = 0;

        if (fbn < NDIRECT){
          struct inode_cache_node *inodeblk = inode_get(ROOTDEV, inum);
          if (inodeblk != 0){
            cur = inodeblk->din.addrs[fbn];
            inode_node_release(inodeblk);
            haveCur = 1;
          }
        }else{
          struct indirect_cache_node *indblk = indirect_get(ROOTDEV, inum);
          if (indblk != 0){
            cur = indblk->addrs[fbn - NDIRECT];
            indirect_node_release(indblk);
            haveCur = 1;
          }
        }

        if (haveCur && cur == blockPos){
          struct data_cache_node *n = data_get(ROOTDEV, inum, fbn);
          if (n != 0){
            data_node_mark_dirty(n);
            data_node_release(n);
            foundLive++;
          }
        }
      }
    }

    brelse(b);
    pos += 1 + count;
  }

  acquire(&cpLock);
  if (cp.sut[segnum].live_count != foundLive){
    printf("gc: segment %d live_count was %d, correcting to %d (exhaustive scan)\n",
           segnum, cp.sut[segnum].live_count, foundLive);
    cp.sut[segnum].live_count = foundLive;
  }
  release(&cpLock);
}

// how many more real (non-summary) blocks the in-progress group could
// take before it would run past the end of the current segment.
int group_capacity(int offset)
{
  int remaining = SSIZE - offset;  // includes room for the group's own summary block
  if(remaining <= 1)
    return 0;
  int cap = remaining - 1;
  if(cap > GROUP_CAP)
    cap = GROUP_CAP;
  return cap;
}

void move_group_to_segbuf(int segOffset, int blocks){
  
  acquiresleep(&seg.lock);

  int startOffset = segOffset;
  int endOffset = startOffset + blocks;
  int pageOffset = segOffset % 4;
  int pageBase = segOffset - pageOffset;
  uint64 pageAddr;

  if (seg.addrs[pageBase] == 0){
    pageAddr = (uint64) kalloc();
  }else{
    pageAddr = seg.addrs[pageBase];
  }

  uint64 targetAddr = pageAddr + (pageOffset * BSIZE);
  memmove((void*) targetAddr, &sumblk, BSIZE);

  seg.addrs[pageBase + pageOffset] = targetAddr;

  segOffset += 1;

  while (segOffset < endOffset){
    pageOffset = segOffset % 4;
    pageBase = segOffset - pageOffset;

    if (seg.addrs[pageBase] == 0){
      pageAddr = (uint64) kalloc();
    }else{
      pageAddr = seg.addrs[pageBase];
    }

    targetAddr = pageAddr + (pageOffset * BSIZE);
    memmove((void*) targetAddr, group_data[segOffset-startOffset-1], BSIZE);

    seg.addrs[pageBase + pageOffset] = targetAddr;

    segOffset += 1;
  }

  releasesleep(&seg.lock);

}

// Gathers up to a group's worth of dirty blocks (data, then indirect,
// then inode, then imap -- the fixed flush order) at segment offset
// `offset`, migrates them to the segbuf, and returns how many real
// blocks were gathered (0 if nothing was dirty).
int build_group(int offset)
{
  for (int i=0; i < NDATA_PER_SEGSUM - 1; i++){
    sumblk.entries[i].tag1 = 0;
    sumblk.entries[i].tag2 = 0;
  }

  acquire(&cpLock);
  uint segNum = cp.segment_num;
  release(&cpLock);
  uint groupBase = segment_start_block(segNum) + offset + 1;

  int idx = 0; // index in the group list

  while (idx < group_capacity(offset)){
    struct data_cache_node *data_node = data_cache_lookup_dirty();

    if (data_node == 0){
      break;
    }

    memmove(group_data[idx], data_node->data, BSIZE);

    sumblk.entries[idx].tag1 = data_node->inum;
    sumblk.entries[idx].tag2 = data_node->fbn+1;

    uint realAddr = groupBase + idx;
    uint dnum = data_node->inum;
    uint dfbn = data_node->fbn;

    data_node_mark_clean(data_node);
    data_node_release(data_node);

    if (dfbn < NDIRECT){
      struct inode_cache_node *owner = inode_get(ROOTDEV, dnum);
      if (owner == 0){
        panic("build_group: data owner inode not found");
      }
      uint oldAddr = owner->din.addrs[dfbn];
      owner->din.addrs[dfbn] = realAddr;
      sut_update(oldAddr, segNum);
      inode_node_mark_dirty(owner);
      inode_node_release(owner);
    }else{
      struct indirect_cache_node *owner = indirect_get(ROOTDEV, dnum);
      if (owner == 0){
        panic("build_group: data owner indirect not found");
      }
      uint oldAddr = owner->addrs[dfbn - NDIRECT];
      owner->addrs[dfbn - NDIRECT] = realAddr;
      sut_update(oldAddr, segNum);
      indirect_node_mark_dirty(owner);
      indirect_node_release(owner);
    }

    idx += 1;
  }


  while (idx < group_capacity(offset)){
    struct indirect_cache_node *indirect_node = indirect_cache_lookup_dirty();

    if (indirect_node == 0){
      break;
    }

    memmove(group_data[idx], indirect_node->addrs, BSIZE);

    sumblk.entries[idx].tag1 = indirect_node->inum;
    sumblk.entries[idx].tag2 = 0xFFFFFFFF;

    uint realAddr = groupBase + idx;
    uint inum = indirect_node->inum;

    indirect_node_mark_clean(indirect_node);
    indirect_node_release(indirect_node);

    struct inode_cache_node *owner = inode_get(ROOTDEV, inum);
    if (owner == 0){
      panic("build_group: indirect owner inode not found");
    }
    uint oldAddr = owner->din.addrs[NDIRECT];
    owner->din.addrs[NDIRECT] = realAddr;
    sut_update(oldAddr, segNum);
    inode_node_mark_dirty(owner);
    inode_node_release(owner);

    idx += 1;
  }

  while (idx < group_capacity(offset)){
    struct inode_cache_node *inode_node = inode_cache_lookup_dirty();

    if (inode_node == 0){
      break;
    }

    memset(group_data[idx], 0, BSIZE);
    memmove(group_data[idx], &inode_node->din, sizeof(struct dinode));

    sumblk.entries[idx].tag1 = inode_node->inum;
    sumblk.entries[idx].tag2 = 0;

    uint realAddr = groupBase + idx;
    uint inum = inode_node->inum;

    inode_node_mark_clean(inode_node);
    inode_node_release(inode_node);

    struct imap_cache_node *owner = imap_get(ROOTDEV, IMAP_BLK_IDX(inum));
    if (owner == 0){
      panic("build_group: inode owner imap block not found");
    }
    uint oldAddr = owner->addrs[IMAP_OFFSET(inum)];
    owner->addrs[IMAP_OFFSET(inum)] = realAddr;
    sut_update(oldAddr, segNum);
    imap_node_mark_dirty(owner);
    imap_node_release(owner);

    idx += 1;
  }


  while (idx < group_capacity(offset)){
    struct imap_cache_node *imap_node = imap_cache_lookup_dirty();

    if (imap_node == 0){
      break;
    }

    memmove(group_data[idx], imap_node->addrs, BSIZE);

    sumblk.entries[idx].tag1 = 0;
    sumblk.entries[idx].tag2 = imap_node->idx+1;

    uint realAddr = groupBase + idx;
    uint imapIdx = imap_node->idx;

    imap_node_mark_clean(imap_node);
    imap_node_release(imap_node);

    acquire(&cpLock);
    uint oldAddr = cp.imap_addr[imapIdx];
    cp.imap_addr[imapIdx] = realAddr;
    release(&cpLock);
    sut_update(oldAddr, segNum);

    idx += 1;
  }

  // No dirty blocks were found at all
  if (idx == 0){
    return 0;
  }

  acquire(&cpLock);
  sumblk.seq = cp.global_seq;
  release(&cpLock);
  sumblk.data_checksum = checksum(group_data, idx * BSIZE);
  sumblk.self_checksum = checksum(&sumblk, sizeof(sumblk) - sizeof(uint));

  // constructs segment summary block and
  // migrates group to the `segbuf`
  move_group_to_segbuf(offset, idx+1);

  return idx;
}




uint get_cacheSize(){
  return segsum_cache_count() + data_cache_count() + indirect_cache_count() + inode_cache_count() + imap_cache_count();
}

void lru_cache_eviction(){

  if (get_cacheSize() <= CACHE_EVICT_THRESHOLD){
    return;
  }

  int evictedThisTurn = -1;

  while (get_cacheSize() > CACHE_EVICT_THRESHOLD && (evictedThisTurn == -1 || evictedThisTurn > 0)){
    
    evictedThisTurn = 0;
    if (segsum_cache_count() > 0){
      evictedThisTurn = segsum_cache_evict_all();
    }

    if (get_cacheSize() <= CACHE_EVICT_TARGET){
      return;
    }

    int evictDataBlocks = data_cache_count()/2;
    for (int i=0; i < evictDataBlocks; i++){
      int x = data_cache_evict_lru();
      if (x == 0){
        break;
      }
      evictedThisTurn += x;
    }

    if (get_cacheSize() <= CACHE_EVICT_TARGET){
      return;
    }

    int evictIndirectBlocks = indirect_cache_count()/2;
    for (int i=0; i < evictIndirectBlocks; i++){
      int x = indirect_cache_evict_lru();
      if (x == 0){
        break;
      }
      evictedThisTurn += x;
    }

    if (get_cacheSize() <= CACHE_EVICT_TARGET){
      return;
    }

    int evictInodeBlocks = inode_cache_count()/2;
    for (int i=0; i < evictInodeBlocks; i++){
      int x = inode_cache_evict_lru();
      if (x == 0){
        break;
      }
      evictedThisTurn += x;
    }

    if (get_cacheSize() <= CACHE_EVICT_TARGET){
      return;
    }

    int evictImapBlocks = imap_cache_count()/2;
    for (int i=0; i < evictImapBlocks; i++){
      int x = imap_cache_evict_lru();
      if (x == 0){
        break;
      }
      evictedThisTurn += x;
    }

    if (get_cacheSize() <= CACHE_EVICT_TARGET){
      return;
    }

  }
  
}

// Drains every currently dirty block to disk, claiming new segments as
// needed. Safe to call with nothing dirty (no-op). Caller holds
// flushLock.
static void drain_dirty(void)
{
  acquire(&cpLock);
  int offset = cp.fill_offset;
  release(&cpLock);

  while (cache_dirty_count() > 0){

    if (group_capacity(offset) == 0){
      int s = next_free_segment();
      acquire(&cpLock);
      cp.fill_offset = 0;
      cp.segment_num = s;
      cp.global_seq += 1;
      release(&cpLock);
      offset = 0;

      acquiresleep(&seg.lock);
      // every 4 consecutive positions share one kalloc'd page as
      // seg.addrs[pageBase] holds that page's raw address.
      // Free each page exactly once before dropping the pointers
      for (int i=0; i<SSIZE; i+=4){
        if (seg.addrs[i] != 0){
          kfree((void*)seg.addrs[i]);
        }
      }
      for (int i=0; i<SSIZE; i++){
        seg.addrs[i] = 0;
      }
      releasesleep(&seg.lock);
    }

    int startOffset = offset;

    while (offset < SSIZE){
      int idx = build_group(offset);

      // No dirty blocks were found at all, break early
      if (idx == 0){
        break;
      }

      // '+1' for segment summary block
      offset += idx + 1;
    }

    virtio_disk_seg_w(&seg, offset - startOffset, startOffset,
                       segment_start_block(cp.segment_num) + startOffset);
  }

  acquire(&cpLock);
  cp.fill_offset = offset;
  release(&cpLock);
}

void maybe_flush_dirty()
{
  uint currTicks;

  acquire(&tickslock);
  currTicks = ticks;
  release(&tickslock);

  if (cache_dirty_count() < SSIZE - 5 && currTicks - lastFlushTicks < NFLUSHTICKS){
    return;
  }

  lastFlushTicks = currTicks;

  acquiresleep(&flushLock);

  // low on free segments: clean segments into a batch sized to fit the
  // current segment's remaining room, then drain once per batch --
  // one contiguous write filling up the active segment, rather than a
  // small write per segment cleaned. 
  int gcRounds = 0;
  while (free_segment_count() < GC_MIN_FREE_SEGMENTS && gcRounds < GC_SEGMENTS_PER_ROUND){
    acquire(&cpLock);
    int fillOffset = cp.fill_offset;
    release(&cpLock);
    uint remainingCapacity = (uint) group_capacity(fillOffset);

    int batchSegnums[GC_SEGMENTS_PER_ROUND];
    int batchCount = 0;
    uint accumulatedLive = 0;

    while (gcRounds < GC_SEGMENTS_PER_ROUND){
      int segnum = select_segment_to_clean();
      if (segnum < 0){
        break;  // nothing left worth cleaning
      }

      int alreadyInBatch = 0;
      for (int k = 0; k < batchCount; k++){
        if (batchSegnums[k] == segnum){
          alreadyInBatch = 1;
          break;
        }
      }
      if (alreadyInBatch){
        break;  // nothing new left to add to this batch
      }

      acquire(&cpLock);
      uint candidateLive = cp.sut[segnum].live_count;
      release(&cpLock);

      if (batchCount > 0 && accumulatedLive + candidateLive > remainingCapacity){
        break;  // doesn't fit alongside what this batch already committed to
      }

      printf("gc: cleaning segment %d (live=%d)\n", segnum, candidateLive);
      clean_segment((uint) segnum);
      batchSegnums[batchCount++] = segnum;
      accumulatedLive += candidateLive;
      gcRounds++;

      if (accumulatedLive >= remainingCapacity){
        break;  // current segment's remaining room is spoken for
      }
    }

    if (batchCount == 0){
      break;  // nothing left worth cleaning at all
    }

    drain_dirty();

    for (int k = 0; k < batchCount; k++){
      acquire(&cpLock);
      uint remaining = cp.sut[batchSegnums[k]].live_count;
      release(&cpLock);
      if (remaining == 0){
        printf("gc: segment %d fully reclaimed\n", batchSegnums[k]);
        free_segment((uint) batchSegnums[k]);
      }else{
        printf("gc: segment %d still has %d live blocks, retrying later\n",
               batchSegnums[k], remaining);
      }
    }
  }

  // drain whatever's left: the ordinary, non-GC-triggered case, or
  // any leftover ambient dirty data after the loop above.
  drain_dirty();

  // checkpoint flush is cheap (NCHECKPOINTBLOCKS writes) but still is a
  // non-sequential write to a fixed location -- so we do it after much
  // longer intervals, not on every flush.
  acquire(&tickslock);
  uint nowTicks = ticks;
  release(&tickslock);
  if (nowTicks - lastCheckpointTicks >= CHECKPOINT_INTERVAL_TICKS){
    lastCheckpointTicks = nowTicks;
    checkpoint_flush();
  }

  releasesleep(&flushLock);

  lru_cache_eviction();
}




uint
inode_alloc(ushort type)
{
  uint inum = freeinode++;

  if (inum > NINODES){
    panic("inode_alloc: inum out of bounds");
  }

  struct inode_cache_node *inode_node = inode_cache_get_or_create(inum);
  if (inode_node == 0){
    return 0;
  }

  inode_node->din.type = type;
  inode_node->din.nlink = 1;
  inode_node->din.size = 0;

  struct imap_cache_node *imap_node = imap_get(ROOTDEV, IMAP_BLK_IDX(inum));
  if (imap_node == 0){
    imap_node = imap_cache_get_or_create(IMAP_BLK_IDX(inum));
    if (imap_node == 0){
      inode_node_release(inode_node);
      return 0;
    }
  }
  imap_node->addrs[IMAP_OFFSET(inum)] = 0xFFFFFFFF;
  imap_node_mark_dirty(imap_node);
  imap_node_release(imap_node);

  inode_node_release(inode_node);

  return inum;
}

// Allocate an inode on device dev.
// Mark it as allocated by giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.
struct inode* ialloc(uint dev, short type)
{
  uint inum = inode_alloc(type);
  if (inum == 0){
    return 0;
  }
  return iget(dev, inum);
}


struct imap_cache_node* imap_get(uint dev, uint idx){
  struct imap_cache_node *imap_node = imap_cache_lookup(idx);

  acquire(&cpLock);
  uint imap_blockno = cp.imap_addr[idx];
  release(&cpLock);

  if (imap_node == 0 && imap_blockno != 0){
    struct buf *b = bread(dev, imap_blockno);
    imap_node = imap_cache_add(idx, (uint*)b->data);
    brelse(b);
  }
  return imap_node;
}

struct inode_cache_node* inode_get(uint dev, uint inum)
{

  struct inode_cache_node *inode_node = inode_cache_lookup(inum);

  if (inode_node == 0){
    struct imap_cache_node *imap_node = imap_get(dev, IMAP_BLK_IDX(inum));
    if (imap_node == 0){
      return 0;
    }
      
    uint inode_blockno = imap_node->addrs[IMAP_OFFSET(inum)];
    imap_node_release(imap_node);

    if (inode_blockno == 0){
      return 0;
    }

    struct buf *b = bread(dev, inode_blockno);
    inode_node = inode_cache_add(inum, (struct dinode*)b->data);
    brelse(b);
  }

  return inode_node;
}

struct indirect_cache_node *indirect_get(uint dev, uint inum){

  struct indirect_cache_node *indirect_node = indirect_cache_lookup(inum);

  if (indirect_node == 0){
    struct inode_cache_node *inode_node = inode_get(dev, inum);

    if (inode_node == 0){
      return 0;
    }

    uint indirect_blockno = inode_node->din.addrs[NDIRECT];
    inode_node_release(inode_node);

    if (indirect_blockno == 0){
      return 0;
    }

    struct buf *b = bread(dev, indirect_blockno);
    indirect_node = indirect_cache_add(inum, (uint*)b->data);
    brelse(b);
  }

  return indirect_node;
}

struct data_cache_node *data_get(uint dev, uint inum, uint fbn){
  struct data_cache_node *data_node = data_cache_lookup(inum, fbn);
  uint data_blockno;

  if (data_node == 0){
    if (fbn < NDIRECT){
      struct inode_cache_node *inode_node = inode_get(dev, inum);

      if (inode_node == 0){
        return 0;
      }

      data_blockno = inode_node->din.addrs[fbn];
      inode_node_release(inode_node);
    }
    else{
      struct indirect_cache_node *indirect_node = indirect_get(dev, inum);

      if (indirect_node == 0){
        return 0;
      }

      data_blockno = indirect_node->addrs[fbn-NDIRECT];
      indirect_node_release(indirect_node);
    }

    if (data_blockno == 0){
      return 0;
    }

    struct buf *b = bread(dev, data_blockno);
    data_node = data_cache_add(inum, fbn, (char*)b->data);
    brelse(b);
  }

  return data_node;
}





#define min(a, b) ((a) < (b) ? (a) : (b))

void
inode_append(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;

  struct inode_cache_node *inode_node = inode_cache_get_or_create(inum);
  off = inode_node->din.size;
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;

    if (fbn >= MAXFILE){
      panic("inode_append: fbn out of bounds");
    }

    if(fbn < NDIRECT){
      if(inode_node->din.addrs[fbn] == 0){
        inode_node->din.addrs[fbn] = 0xFFFFFFFF;
      }
    } else {
      if(inode_node->din.addrs[NDIRECT] == 0){
        inode_node->din.addrs[NDIRECT] = 0xFFFFFFFF;
      }

      struct indirect_cache_node *indirect_node = indirect_cache_get_or_create(inum);
      indirect_node->addrs[fbn - NDIRECT] = 0xFFFFFFFF;
    }

    n1 = min(n, (fbn + 1) * BSIZE - off);

    struct data_cache_node *data_node = data_cache_get_or_create(inum, fbn);
    memmove(data_node->data + off - (fbn * BSIZE), p, n1);

    n -= n1;
    off += n1;
    p += n1;
  }
  inode_node->din.size = off;
}









// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct
{
  struct spinlock lock;
  struct inode inode[NINODE];
} itable;

void iinit()
{
  int i = 0;

  initlock(&itable.lock, "itable");
  for (i = 0; i < NINODE; i++)
  {
    initsleeplock(&itable.inode[i].lock, "inode");
  }
}

static struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.


// Copy a modified in-memory inode to the LFS inode cache, marking it
// dirty so the next flush persists it. Must be called after every
// change to an ip->xxx field that lives on disk.
// Caller must hold ip->lock.
void iupdate(struct inode* ip)
{
  struct inode_cache_node* inode_node = inode_get(ip->dev, ip->inum);
  if (inode_node == 0){
    panic("iupdate: inode not found");
  }

  inode_node->din.type = ip->type;
  inode_node->din.major = ip->major;
  inode_node->din.minor = ip->minor;
  inode_node->din.nlink = ip->nlink;
  inode_node->din.size = ip->size;
  memmove(inode_node->din.addrs, ip->addrs, sizeof(ip->addrs));
  inode_node_mark_dirty(inode_node);

  inode_node_release(inode_node);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode* ip, * empty;

  acquire(&itable.lock);

  // Is the inode already in the table?
  empty = 0;
  for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++)
  {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum)
    {
      ip->ref++;
      release(&itable.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&itable.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
  idup(struct inode* ip)
{
  acquire(&itable.lock);
  ip->ref++;
  release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void ilock(struct inode* ip)
{
  struct dinode* dip;

  if (ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if (ip->valid == 0)
  {
    struct inode_cache_node* inode_node = inode_get(ip->dev, ip->inum);
    if (inode_node == 0){
      panic("ilock: inode not found");
    }
    dip = &inode_node->din;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    inode_node_release(inode_node);
    ip->valid = 1;
    if (ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void iunlock(struct inode* ip)
{
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void iput(struct inode* ip)
{
  acquire(&itable.lock);

  if (ip->ref == 1 && ip->valid && ip->nlink == 0)
  {
    // inode has no links and no other references: truncate and free.

    // ip->ref == 1 means no other process can have ip locked,
    // so this acquiresleep() won't block (or deadlock).
    acquiresleep(&ip->lock);

    release(&itable.lock);

    itrunc(ip);
    ip->type = 0;
    struct inode_cache_node* inode_node = inode_get(ip->dev, ip->inum);
    inode_node->din.type = 0;
    inode_node_mark_dirty(inode_node);
    inode_node_release(inode_node);
    
    releasesleep(&ip->lock);

    acquire(&itable.lock);
  }

  ip->ref--;
  release(&itable.lock);
}

// Common idiom: unlock, then put.
void iunlockput(struct inode* ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Truncate inode (discard contents).
// Caller must hold ip->lock.
// Credits a freed block's old segment, if it had a real previous
// location, and drops any still-cached copy -- mirrors sut_update()'s
// "old side" plus a pop, since this content isn't moving to a new
// segment, it's just gone.
static void itrunc_release_data(uint oldAddr, uint inum, uint fbn)
{
  if (oldAddr != 0 && oldAddr != 0xFFFFFFFF){
    acquire(&cpLock);
    cp.sut[segment_of_block(oldAddr)].live_count--;
    release(&cpLock);
  }
  struct data_cache_node *dn = data_cache_pop(inum, fbn);
  if (dn != 0){
    kfree(dn);
  }
}

void itrunc(struct inode* ip)
{
  struct inode_cache_node* inode_node = inode_get(ip->dev, ip->inum);

  if (inode_node == 0){
    panic("itrunc: inode not found");
  }

  for (int i=0; i < NDIRECT; i++){
    itrunc_release_data(inode_node->din.addrs[i], ip->inum, i);
    inode_node->din.addrs[i] = 0;
  }

  uint oldIndirectAddr = inode_node->din.addrs[NDIRECT];
  if (oldIndirectAddr != 0 && oldIndirectAddr != 0xFFFFFFFF){
    acquire(&cpLock);
    cp.sut[segment_of_block(oldIndirectAddr)].live_count--;
    release(&cpLock);
  }

  struct indirect_cache_node *indirect_node = indirect_cache_pop(ip->inum);
  if (indirect_node != 0){
    for (int j=0; j < NINDIRECT; j++){
      itrunc_release_data(indirect_node->addrs[j], ip->inum, NDIRECT + j);
    }
    kfree(indirect_node);
  }
  inode_node->din.addrs[NDIRECT] = 0;

  inode_node->din.size = 0;
  inode_node_mark_dirty(inode_node);

  inode_node_release(inode_node);
  ip->size = 0;
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode* ip, struct stat* st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int readi(struct inode* ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;

  if (off > ip->size || off + n < off)
    return 0;
  if (off + n > ip->size)
    n = ip->size - off;

  for (tot = 0; tot < n; tot += m, off += m, dst += m)
  {
    int fbn = off/BSIZE;
    struct data_cache_node* data_node = data_get(ip->dev, ip->inum, fbn);
    if (data_node == 0)
      break;
    
    m = min(n - tot, BSIZE - off % BSIZE);
    if (either_copyout(user_dst, dst, data_node->data + (off % BSIZE), m) == -1)
    {
      data_node_release(data_node);
      tot = -1;
      break;
    }
    data_node_release(data_node);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int writei(struct inode* ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > MAXFILE * BSIZE)
    return -1;

  
  for (tot = 0; tot < n; tot += m, off += m, src += m)
  {
    int fbn = off/BSIZE;
    int data_node_created = 0;
    int indirect_node_created = 0;

    struct data_cache_node* data_node = data_get(ip->dev, ip->inum, fbn);

    if (data_node == 0){
      data_node = data_cache_get_or_create(ip->inum, fbn);
      if (data_node == 0){
        break;
      }

      data_node_created = 1;
      if (fbn < NDIRECT){
        struct inode_cache_node* inode_node = inode_get(ip->dev, ip->inum);
        if (inode_node == 0){
          data_node_release(data_node);
          struct data_cache_node* orphan = data_cache_pop(ip->inum, fbn);
          kfree(orphan);
          break;
        }
        inode_node->din.addrs[fbn] = 0xFFFFFFFF;
        inode_node_release(inode_node);
      }else{
        struct indirect_cache_node* indirect_node = indirect_get(ip->dev, ip->inum);
        if (indirect_node == 0){
          indirect_node = indirect_cache_get_or_create(ip->inum);
          if (indirect_node == 0){
            data_node_release(data_node);
            struct data_cache_node* orphan = data_cache_pop(ip->inum, fbn);
            kfree(orphan);
            break;
          }
          indirect_node_created = 1;

          struct inode_cache_node* inode_node2 = inode_get(ip->dev, ip->inum);
          if (inode_node2 == 0){
            indirect_node_release(indirect_node);
            struct indirect_cache_node* indirect_orphan = indirect_cache_pop(ip->inum);
            kfree(indirect_orphan);
            data_node_release(data_node);
            struct data_cache_node* orphan = data_cache_pop(ip->inum, fbn);
            kfree(orphan);
            break;
          }
          inode_node2->din.addrs[NDIRECT] = 0xFFFFFFFF;
          inode_node_release(inode_node2);
        }
        indirect_node->addrs[fbn-NDIRECT] = 0xFFFFFFFF;
        indirect_node_release(indirect_node);
      }

    }

    m = min(n - tot, BSIZE - off % BSIZE);
    if (either_copyin(data_node->data + (off % BSIZE), user_src, src, m) == -1)
    {
      data_node_release(data_node);

      if (data_node_created){
        if (fbn < NDIRECT){
          struct inode_cache_node* inode_node = inode_get(ip->dev, ip->inum);
          inode_node->din.addrs[fbn] = 0;
          inode_node_release(inode_node);
        }else{
          struct indirect_cache_node* indirect_node = indirect_get(ip->dev, ip->inum);
          indirect_node->addrs[fbn-NDIRECT] = 0;
          indirect_node_release(indirect_node);
          
          if (indirect_node_created){
            struct inode_cache_node* inode_node2 = inode_get(ip->dev, ip->inum);
            inode_node2->din.addrs[NDIRECT] = 0;
            inode_node_release(inode_node2);
            struct indirect_cache_node* indirect_orphan = indirect_cache_pop(ip->inum);
            kfree(indirect_orphan);
          }
        }

        struct data_cache_node* data_orphan = data_cache_pop(ip->inum, fbn);
        kfree(data_orphan);
      }
      break;
    }
    data_node_mark_dirty(data_node);
    data_node_release(data_node);
  }

  
  if (off > ip->size){
    struct inode_cache_node* inode_node = inode_get(ip->dev, ip->inum);
    if (inode_node != 0){
      ip->size = off;
      inode_node->din.size = off;
      inode_node_mark_dirty(inode_node);
      inode_node_release(inode_node);
    }
  }

  maybe_flush_dirty();

  return tot;
}

// Directories

int namecmp(const char* s, const char* t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
  dirlookup(struct inode* dp, char* name, uint* poff)
{
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de))
  {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0)
    {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
int dirlink(struct inode* dp, char* name, uint inum)
{
  int off;
  struct dirent de;
  struct inode* ip;

  // Check that name is not present.
  if ((ip = dirlookup(dp, name, 0)) != 0)
  {
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for (off = 0; off < dp->size; off += sizeof(de))
  {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char* path, char* name)
{
  char* s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else
  {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char* path, int nameiparent, char* name)
{
  struct inode* ip, * next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myfamily()->cwd);

  while ((path = skipelem(path, name)) != 0)
  {
    ilock(ip);
    if (ip->type != T_DIR)
    {
      iunlockput(ip);
      return 0;
    }
    if (nameiparent && *path == '\0')
    {
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if ((next = dirlookup(ip, name, 0)) == 0)
    {
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if (nameiparent)
  {
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
  namei(char* path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
  nameiparent(char* path, char* name)
{
  return namex(path, 1, name);
}
