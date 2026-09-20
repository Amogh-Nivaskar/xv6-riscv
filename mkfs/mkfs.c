#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "kernel/param.h"

#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

// Disk layout:
// [ boot block | sb block | checkpoint region 1 | checkpoint region 2 | segments ]

int nmeta;    // Number of meta blocks (boot, sb, checkpoint region 1, checkpoint region 2)
int nblocks;  // Number of blocks available for segments

int fsfd;
struct lfs_superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;

// Head nodes for block caches.
// Head node is empty, always start lookup from head.next
struct data_cache_node dataCacheHead;
struct indirect_cache_node indirectCacheHead;
struct inode_cache_node inodeCacheHead;
struct imap_cache_node imapCacheHead;


// Forward declarations of functions
void wsect(uint, void*);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);

struct data_cache_node *data_cache_lookup(uint inum, uint fbn);
struct data_cache_node *data_cache_get_or_create(uint inum, uint fbn);
struct data_cache_node *data_cache_pop(uint inum, uint fbn);
struct indirect_cache_node *indirect_cache_lookup(uint inum);
struct indirect_cache_node *indirect_cache_get_or_create(uint inum);
struct indirect_cache_node *indirect_cache_pop(uint inum);
struct inode_cache_node *inode_cache_lookup(uint inum);
struct inode_cache_node *inode_cache_get_or_create(uint inum);
struct inode_cache_node *inode_cache_pop(uint inum);
struct imap_cache_node *imap_cache_lookup(uint idx);
struct imap_cache_node *imap_cache_get_or_create(uint idx);
struct imap_cache_node *imap_cache_pop(uint idx);

uint checksum(void *data, int nbytes);
uint segment_start_block(uint segnum);
uint next_free_segment(void);
int group_capacity(void);
void seg_flush_group(void);
uint seg_write_block(uint tag1, uint tag2, void *data);
void checkpoint_flush(uint checkpointstart, struct checkpoint *cp);



struct checkpoint cp;              // in-memory checkpoint state; written to disk once fully built
struct segsum_block cur_summary;
char group_data[GROUP_CAP][BSIZE];
int group_n;                      // blocks buffered in the current, unflushed group
int cp_counter;


// convert to riscv byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}

int
main(int argc, char *argv[])
{
  int i, cc, fd;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];


  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);

  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0)
    die(argv[1]);

  // 1 fs block = 1 disk sector
  // [ boot(1) | super(1) | checkpoint region 1(NCHECKPOINTBLOCKS) | checkpoint region 2(NCHECKPOINTBLOCKS) ]
  nmeta = 2 + 2*NCHECKPOINTBLOCKS;
  nblocks = FSSIZE - nmeta;
  assert(nblocks / SSIZE == SEG_NUM);

  sb.magic = FSMAGIC;
  sb.size = xint(FSSIZE);
  sb.checkpoint1start = xint(2);
  sb.checkpoint2start = xint(2 + NCHECKPOINTBLOCKS);
  sb.checkpointsize = xint(NCHECKPOINTBLOCKS);
  sb.ninodes = xint(NINODES);
  sb.segsize = xint(SSIZE);


  printf("nmeta %d (boot, super, checkpoint region blocks %u x2) segment blocks %d total %d\n",
         nmeta, NCHECKPOINTBLOCKS, nblocks, FSSIZE);

  bzero(&cp, sizeof(cp));
  memset(cp.seg_freemap, 0xff, sizeof(cp.seg_freemap));  // all segments start free

  cp.global_seq = 0;
  cp.segment_num = next_free_segment();
  cp.global_seq++;
  cp.fill_offset = 0;
  group_n = 0;

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes);

  memset(buf, 0, sizeof(buf));
  memmove(buf, &sb, sizeof(sb));
  wsect(1, buf);

  rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, ".");
  iappend(rootino, &de, sizeof(de));

  bzero(&de, sizeof(de));
  de.inum = xshort(rootino);
  strcpy(de.name, "..");
  iappend(rootino, &de, sizeof(de));

  for(i = 2; i < argc; i++){
    // get rid of "user/"
    char *shortname;
    if(strncmp(argv[i], "user/", 5) == 0)
      shortname = argv[i] + 5;
    else
      shortname = argv[i];
    
    assert(index(shortname, '/') == 0);

    if((fd = open(argv[i], 0)) < 0)
      die(argv[i]);

    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(shortname[0] == '_')
      shortname += 1;

    assert(strlen(shortname) <= DIRSIZ);
    
    inum = ialloc(T_FILE);

    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    strncpy(de.name, shortname, DIRSIZ);
    iappend(rootino, &de, sizeof(de));

    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);

    struct inode_cache_node *inode_node = inode_cache_pop(inum);
    struct indirect_cache_node *indirect_node = indirect_cache_pop(inum);
    struct imap_cache_node *imap_node = imap_cache_lookup(IMAP_BLK_IDX(inum));

    for (int fbn = 0; fbn < MAXFILE; fbn++){
      struct data_cache_node* data_node = data_cache_pop(inum, fbn);

      if (data_node == NULL){
        break;
      }

      uint data_diskaddr = seg_write_block(inum, fbn+1, data_node->data);
      free(data_node);

      if (fbn < NDIRECT){
        inode_node->din.addrs[fbn] = xint(data_diskaddr);
      }else{
        indirect_node->addrs[fbn - NDIRECT] = xint(data_diskaddr);
      }
    }

    if (indirect_node != NULL){
      uint indirect_diskaddr = seg_write_block(inum, 0xFFFFFFFF, indirect_node->addrs);
      free(indirect_node);

      inode_node->din.addrs[NDIRECT] = xint(indirect_diskaddr); 
    }

    uint inode_diskaddr = seg_write_block(inum, 0, &inode_node->din);
    free(inode_node);

    imap_node->addrs[IMAP_OFFSET(inum)] = xint(inode_diskaddr);

    close(fd);
  }

  // fix size of root inode dir
  struct inode_cache_node *root_inode_node = inode_cache_pop(rootino);
  off = xint(root_inode_node->din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  root_inode_node->din.size = xint(off);

  
  struct imap_cache_node *imap_node = imap_cache_lookup(IMAP_BLK_IDX(rootino));
  uint root_inode_diskaddr = seg_write_block(rootino, 0, &root_inode_node->din);
  imap_node->addrs[IMAP_OFFSET(rootino)] = xint(root_inode_diskaddr);
  free(root_inode_node);

  for (int idx = 0; idx < IMAP_BLK_NUM; idx++){
    struct imap_cache_node *imap_node = imap_cache_pop(idx);

    if (imap_node == NULL){
      break;
    }

    uint imap_diskaddr = seg_write_block(0, idx+1, imap_node->addrs);
    free(imap_node);

    cp.imap_addr[idx] = xint(imap_diskaddr);
  }

  seg_flush_group();  // flush whatever's left in the in-progress group

  cp.timestamp = xint(++cp_counter);

  checkpoint_flush(xint(sb.checkpoint1start), &cp);
  checkpoint_flush(xint(sb.checkpoint2start), &cp);

  exit(0);
}

void
wsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(write(fsfd, buf, BSIZE) != BSIZE)
    die("write");
}

void
rsect(uint sec, void *buf)
{
  if(lseek(fsfd, sec * BSIZE, 0) != sec * BSIZE)
    die("lseek");
  if(read(fsfd, buf, BSIZE) != BSIZE)
    die("read");
}

// simple rotate-xor checksum; not cryptographic, only meant to catch a
// torn/incomplete write during crash recovery, not adversarial corruption.
uint
checksum(void *data, int nbytes)
{
  uint *w = (uint*) data;
  uint sum = 0;
  int nwords = nbytes / sizeof(uint);
  for(int i = 0; i < nwords; i++)
    sum = (sum << 1 | sum >> 31) ^ w[i];
  return sum;
}

uint
segment_start_block(uint segnum)
{
  return nmeta + segnum * SSIZE;
}

uint
next_free_segment(void)
{
  for(uint s = 0; s < SEG_NUM; s++){
    if(cp.seg_freemap[s/8] & (1 << (s%8))){
      cp.seg_freemap[s/8] &= ~(1 << (s%8));  // claim it: no longer free
      return s;
    }
  }
  die("next_free_segment: no free segments");
  return 0;  // unreachable
}

// how many more real (non-summary) blocks the in-progress group could
// take before it would run past the end of the current segment.
int
group_capacity(void)
{
  int remaining = BLOCKS_PER_SEG - cp.fill_offset;  // includes room for the group's own summary block
  if(remaining <= 1)
    return 0;
  int cap = remaining - 1;
  if(cap > GROUP_CAP)
    cap = GROUP_CAP;
  return cap;
}

// write the in-progress group's summary block followed by its data
// blocks to disk, in one contiguous run, and reset group state.
void
seg_flush_group(void)
{
  if(group_n == 0)
    return;

  for(int i = group_n; i < NDATA_PER_SEGSUM - 1; i++){
    cur_summary.entries[i].tag1 = xint(0);
    cur_summary.entries[i].tag2 = xint(0);
  }

  cur_summary.data_checksum = xint(checksum(group_data, group_n * BSIZE));
  cur_summary.self_checksum = xint(checksum(&cur_summary, sizeof(cur_summary) - sizeof(uint)));

  uint summary_addr = segment_start_block(cp.segment_num) + cp.fill_offset;
  wsect(summary_addr, &cur_summary);
  for(int i = 0; i < group_n; i++)
    wsect(summary_addr + 1 + i, group_data[i]);

  cp.fill_offset += 1 + group_n;
  cp.sut[cp.segment_num].live_count++;
  cp.sut[cp.segment_num].last_mod_time = cp.global_seq; 
  group_n = 0;
}

// append one logical block (imap/inode/data) to the segment being built,
// flushing the current group and/or rolling to a new segment as needed.
// returns the physical block address the caller should record (e.g. in
// an inode's addrs[] or in checkpoint.imap_addr[]) for this block.
uint
seg_write_block(uint tag1, uint tag2, void *data)
{
  if(group_n > 0 && group_n == group_capacity())
    seg_flush_group();

  if(group_capacity() == 0){
    cp.segment_num = next_free_segment();
    cp.global_seq++;
    cp.fill_offset = 0;
  }

  if(group_n == 0)
    cur_summary.seq = xint(cp.global_seq);

  cur_summary.entries[group_n].tag1 = xint(tag1);
  cur_summary.entries[group_n].tag2 = xint(tag2);
  memmove(group_data[group_n], data, BSIZE);

  uint addr = segment_start_block(cp.segment_num) + cp.fill_offset + 1 + group_n;
  group_n++;
  cp.sut[cp.segment_num].live_count++;
  cp.sut[cp.segment_num].last_mod_time = cp.global_seq; 
  return addr;
}

void checkpoint_flush(uint checkpointstart, struct checkpoint *cp){
  char *p = (char*)cp;

  for (int b=0; b < NCHECKPOINTBLOCKS; b++){
    wsect(checkpointstart + b, p + (b * BSIZE));
  }
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  assert(inum <= NINODES);

  struct inode_cache_node *inode_node = inode_cache_get_or_create(inum);

  inode_node->din.type = xshort(type);
  inode_node->din.nlink = xshort(1);
  inode_node->din.size = xint(0);

  struct imap_cache_node *imap_node = imap_cache_get_or_create(IMAP_BLK_IDX(inum));
  imap_node->addrs[IMAP_OFFSET(inum)] = xint(0xFFFFFFFF);
  
  return inum;
}



#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;

  struct inode_cache_node *inode_node = inode_cache_get_or_create(inum);
  off = xint(inode_node->din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(inode_node->din.addrs[fbn]) == 0){
        inode_node->din.addrs[fbn] = xint(0xFFFFFFFF);
      }
    } else {
      if(xint(inode_node->din.addrs[NDIRECT]) == 0){
        inode_node->din.addrs[NDIRECT] = xint(0xFFFFFFFF);
      }

      struct indirect_cache_node *indirect_node = indirect_cache_get_or_create(inum);
      indirect_node->addrs[fbn - NDIRECT] = xint(0xFFFFFFFF);
    }

    n1 = min(n, (fbn + 1) * BSIZE - off);

    struct data_cache_node *data_node = data_cache_get_or_create(inum, fbn);
    memmove(data_node->data + off - (fbn * BSIZE), p, n1);

    n -= n1;
    off += n1;
    p += n1;
  }
  inode_node->din.size = xint(off);
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}


struct data_cache_node* data_cache_lookup(uint inum, uint fbn)
{
  struct data_cache_node *nxt = dataCacheHead.next;

  while(nxt){
    if(nxt->inum == inum && nxt->fbn == fbn)
      return nxt;
    nxt = nxt->next;
  }
  return 0;
}

struct data_cache_node* data_cache_get_or_create(uint inum, uint fbn)
{
  struct data_cache_node *n = data_cache_lookup(inum, fbn);
  if(n)
    return n;

  // not found: allocate, zero, link in, hand back
  n = malloc(sizeof(*n));
  if(n == 0)
    die("data_cache_get_or_create: out of memory");
  bzero(n, sizeof(*n));
  n->inum = inum;
  n->fbn = fbn;
  n->next = dataCacheHead.next;
  dataCacheHead.next = n;
  return n;
}

// finds, unlinks and returns the node (caller now owns it -- free it, or
// let it leak into seg_write_block's copy and free it right after), or
// returns 0 if no matching node exists. plain deletion is just
// free(data_cache_pop(inum, fbn)).
struct data_cache_node* data_cache_pop(uint inum, uint fbn)
{
  struct data_cache_node *prev = &dataCacheHead;
  struct data_cache_node *cur = dataCacheHead.next;

  while(cur){
    if(cur->inum == inum && cur->fbn == fbn){
      prev->next = cur->next;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  return 0;
}


struct indirect_cache_node* indirect_cache_lookup(uint inum){
  struct indirect_cache_node *nxt = indirectCacheHead.next;

  while (nxt){
    if (nxt->inum == inum){
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct indirect_cache_node* indirect_cache_get_or_create(uint inum){
  struct indirect_cache_node *n = indirect_cache_lookup(inum);
  if (n)
    return n;

  n = malloc(sizeof(*n));
  if (n == 0){
    die("indirect_cache_get_or_create: out of memory");
  }

  bzero(n, sizeof(*n));
  n->inum = inum;
  n->next = indirectCacheHead.next;
  indirectCacheHead.next = n;

  return n;
}

struct indirect_cache_node* indirect_cache_pop(uint inum){
  struct indirect_cache_node *prev = &indirectCacheHead;
  struct indirect_cache_node *cur = indirectCacheHead.next;

  while(cur){
    if(cur->inum == inum){
      prev->next = cur->next;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  return 0;
}

struct inode_cache_node* inode_cache_lookup(uint inum){
  struct inode_cache_node *nxt = inodeCacheHead.next;

  while (nxt){
    if (nxt->inum == inum){
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct inode_cache_node* inode_cache_get_or_create(uint inum){
  struct inode_cache_node *n = inode_cache_lookup(inum);
  if (n)
    return n;

  n = malloc(sizeof(*n));
  if (n == 0){
    die("inode_cache_get_or_create: out of memory");
  }

  bzero(n, sizeof(*n));
  n->inum = inum;
  n->next = inodeCacheHead.next;
  inodeCacheHead.next = n;

  return n;
}

struct inode_cache_node* inode_cache_pop(uint inum){
  struct inode_cache_node *prev = &inodeCacheHead;
  struct inode_cache_node *cur = inodeCacheHead.next;

  while(cur){
    if(cur->inum == inum){
      prev->next = cur->next;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  return 0;
}

struct imap_cache_node* imap_cache_lookup(uint idx){
  struct imap_cache_node *nxt = imapCacheHead.next;

  while (nxt){
    if (nxt->idx == idx){
      return nxt;
    }
    nxt = nxt->next;
  }
  return 0;
}

struct imap_cache_node* imap_cache_get_or_create(uint idx){
  struct imap_cache_node *n = imap_cache_lookup(idx);
  if (n)
    return n;

  n = malloc(sizeof(*n));
  if (n == 0){
    die("imap_cache_get_or_create: out of memory");
  }

  bzero(n, sizeof(*n));
  n->idx = idx;
  n->next = imapCacheHead.next;
  imapCacheHead.next = n;

  return n;
}

struct imap_cache_node* imap_cache_pop(uint idx){
  struct imap_cache_node *prev = &imapCacheHead;
  struct imap_cache_node *cur = imapCacheHead.next;

  while(cur){
    if(cur->idx == idx){
      prev->next = cur->next;
      return cur;
    }
    prev = cur;
    cur = cur->next;
  }
  return 0;
}




