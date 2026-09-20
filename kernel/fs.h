// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size
#define SSIZE 512  // Segment size in blocks

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

struct lfs_superblock  {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint checkpoint1start;      // Start of Checkpoint 1 region
  uint checkpoint2start;      // Start of Checkpoint 2 region
  uint checkpointsize;        // Size of a Checkpoint region
  uint ninodes;      // Number of inodes
  uint segsize;      // Segment size
};

#define FSMAGIC 0x10203040

#define NINODES 64768

// One entry per physical block in a segment, held in a segsum_block.
// (0, 0)              -> this block is itself a segment summary block
// (0, idx+1)          -> imap block, idx into checkpoint.imap_addr[]
// (inum, 0)           -> inode block for inode inum
// (inum, fbn+1)       -> file data block, fbn is the 0-indexed file block number
struct segsum_entry {
  uint tag1;
  uint tag2;
};

// A segment summary block describes NDATA_PER_SEGSUM consecutive physical
// blocks: its own logical position (0) plus the NDATA_PER_SEGSUM-1 real
// (imap/inode/data) blocks that immediately follow it on disk. Logical
// position 0 never needs a real tag pair -- it's always "this position is
// a segment summary block" by structural convention, never read from its
// bytes -- so those bytes are reused for this block's own checksums and
// seq number instead of being wasted on a literal (0, 0).
// seq is assigned once, from a monotonically increasing global counter,
// the moment a segment is claimed from the free list, and is stamped
// identically into every group's summary block written into that segment
// for its whole lifetime. Crash recovery reconstructs write order by
// scanning all segments and comparing seq values, rather than trusting a
// forward pointer or a possibly-stale persisted free list.
// self_checksum is kept physically LAST in the struct (rather than in
// logical position 0, which is physically first) so that, under the
// assumed sequential/in-order write behavior, it still covers every other
// byte of this block -- including seq and data_checksum -- and can detect
// a torn write of the block itself, which a checksum placed early in the
// block could not.
#define NDATA_PER_SEGSUM 127                     // logical positions covered (0 = self)
#define BLOCKS_PER_SEG SSIZE

struct segsum_block {
  struct segsum_entry entries[NDATA_PER_SEGSUM - 1];  // logical positions 1..126
  uint data_checksum;   // checksum over the blocks this group describes
  uint seq;             // this segment's write-order sequence number
  uint self_checksum;   // checksum over entries[]+data_checksum+seq, must be last
};

#define IMAP_ENTRIES_PER_BLK (BSIZE / sizeof(uint))          // 256
#define IMAP_BLK_NUM ((NINODES + IMAP_ENTRIES_PER_BLK - 1) / IMAP_ENTRIES_PER_BLK)  // 253
#define SEG_NUM 100

struct sut_entry {
  uint live_count;
  uint last_mod_time;
};

#define GROUP_CAP (NDATA_PER_SEGSUM - 1)  // real (non-summary) blocks per group

// bit i set means segment i is free (available to be handed out by the
// allocator). Separate from sut[]: sut[] records live-block accounting
// used to decide which segments are worth cleaning, while seg_freemap is
// the pool the allocator actually draws from -- a segment only rejoins it
// once the cleaner (or mkfs, at image-build time) has fully vacated it.
#define SEG_FREEMAP_BYTES ((SEG_NUM + 7) / 8)  // 13 bytes for 100 segments

// checkpoint.timestamp must remain the last field: recovery trusts a
// checkpoint region only if its timestamp shows the write completed.
struct checkpoint {
  uint segment_num;
  uint fill_offset;
  uint imap_addr[IMAP_BLK_NUM];
  struct sut_entry sut[SEG_NUM];
  uchar seg_freemap[SEG_FREEMAP_BYTES];
  uint global_seq;
  uint timestamp;
};

#define NCHECKPOINTBLOCKS 2  // size of a single checkpoint region, in blocks

// kept small for now so testing exercises the indirect-block path; bump to
// 252 later (makes addrs[] fill the rest of the block exactly: 12 fixed
// bytes + 253*4 = 1024 = BSIZE, so sizeof(struct dinode) == BSIZE and
// IPB == 1) once indirect access is confirmed working.
#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint))
#define MAXFILE (NDIRECT + NINDIRECT)

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEVICE only)
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT+1];   // Data block addresses
};

// In-memory dirty-block caches. A block is buffered here as it's written
// (or, in mkfs, as the initial image is assembled) and only gets a real
// on-disk address once its cache is drained into a segment at flush time --
// blocks can't be updated in place, so repeated writes to the same inode,
// indirect or data block must find and update the existing node here
// rather than appending a duplicate. Each node's key mirrors exactly the
// identifying fields used for its block's segsum_entry tag, so building
// that tag at flush time is direct. Flush order must walk these bottom-up
// (data, then indirect, then inode, then imap) since each stage needs the
// previous stage's blocks to already have their final addresses.

struct data_cache_node {
  uint inum;                 // which file this data block belongs to
  uint fbn;                  // 0-indexed file block number
  char data[BSIZE];
  struct data_cache_node *next;
};

struct indirect_cache_node {
  uint inum;                 // which file this indirect block belongs to
  uint addrs[NINDIRECT];     // data block addresses for fbn >= NDIRECT
  struct indirect_cache_node *next;
};

struct inode_cache_node {
  uint inum;
  struct dinode din;
  struct inode_cache_node *next;
};

struct imap_cache_node {
  uint idx;                           // index into checkpoint.imap_addr[]
  uint addrs[IMAP_ENTRIES_PER_BLK];   // inode block addresses for this range
  struct imap_cache_node *next;
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

// Which imap block (checkpoint.imap_addr[] index / imap_cache key) covers this inode.
#define IMAP_BLK_IDX(inum) (((inum) - 1) / IMAP_ENTRIES_PER_BLK)

// Offset within that imap block's addrs[] array for this inode.
#define IMAP_OFFSET(inum) (((inum) - 1) % IMAP_ENTRIES_PER_BLK)

// The name field may have DIRSIZ characters and not end in a NUL
// character.
struct dirent {
  ushort inum;
  char name[DIRSIZ] __attribute__((nonstring));
};

