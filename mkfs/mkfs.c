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

#define NCHECKPOINTBLOCKS 2  // size of a single checkpoint region, in blocks
int nmeta;    // Number of meta blocks (boot, sb, checkpoint region 1, checkpoint region 2)
int nblocks;  // Number of blocks available for segments

int fsfd;
struct lfs_superblock sb;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;


void balloc(int);
void wsect(uint, void*);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);
void die(const char *);

// ---- segment writer ----
#define GROUP_CAP (NDATA_PER_SEGSUM - 1)  // real (non-summary) blocks per group

uint cur_segment_num;
uint cur_segment_seq;              // seq assigned to cur_segment_num when it was claimed
uint cur_fill_offset;             // next free relative block index in cur_segment_num
uint global_seq;                   // next seq to hand out when a segment is claimed
struct checkpoint cp;              // in-memory checkpoint state; written to disk once fully built

struct segsum_block cur_summary;
char group_data[GROUP_CAP][BSIZE];
int group_n;                      // blocks buffered in the current, unflushed group

uint checksum(void *data, int nbytes);
uint segment_start_block(uint segnum);
uint next_free_segment(void);
int group_capacity(void);
void seg_flush_group(void);
uint seg_write_block(uint tag1, uint tag2, void *data);

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
  struct dinode din;


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

  global_seq = 0;
  cur_segment_num = next_free_segment();
  cur_segment_seq = global_seq++;
  cur_fill_offset = 0;
  group_n = 0;

  freeblock = nmeta;     // the first free block that we can allocate

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

    close(fd);
  }

  // fix size of root inode dir
  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);

  balloc(freeblock);

  seg_flush_group();  // flush whatever's left in the in-progress group

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
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;

  bn = IBLOCK(inum, sb);
  rsect(bn, buf);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
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
  int remaining = BLOCKS_PER_SEG - cur_fill_offset;  // includes room for the group's own summary block
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

  uint summary_addr = segment_start_block(cur_segment_num) + cur_fill_offset;
  wsect(summary_addr, &cur_summary);
  for(int i = 0; i < group_n; i++)
    wsect(summary_addr + 1 + i, group_data[i]);

  cur_fill_offset += 1 + group_n;
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
    cur_segment_num = next_free_segment();
    cur_segment_seq = global_seq++;
    cur_fill_offset = 0;
  }

  if(group_n == 0)
    cur_summary.seq = xint(cur_segment_seq);

  cur_summary.entries[group_n].tag1 = xint(tag1);
  cur_summary.entries[group_n].tag2 = xint(tag2);
  memmove(group_data[group_n], data, BSIZE);

  uint addr = segment_start_block(cur_segment_num) + cur_fill_offset + 1 + group_n;
  group_n++;
  return addr;
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BPB);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }
  printf("balloc: write bitmap block at sector %d\n", sb.bmapstart);
  wsect(sb.bmapstart, buf);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}

void
die(const char *s)
{
  perror(s);
  exit(1);
}
