// In-memory dirty-block cache node layouts, shared between the kernel
// (cache.c) and mkfs (mkfs.c) -- both build the same on-disk segment
// content this way, one host-side and one at kernel runtime.
//
// Requires fs.h to be included first (for BSIZE, NDIRECT, NINDIRECT,
// IMAP_ENTRIES_PER_BLK and struct dinode); this file has no include
// guard and no #include of its own, matching the rest of this codebase's
// convention of relying on careful, ordered includes rather than guards.
//
// A block is buffered here as it's written (or, in mkfs, as the initial
// image is assembled) and only gets a real on-disk address once its
// cache is drained into a segment at flush time -- blocks can't be
// updated in place, so repeated writes to the same inode, indirect or
// data block must find and update the existing node here rather than
// appending a duplicate. Each node's key mirrors exactly the identifying
// fields used for its block's segsum_entry tag, so building that tag at
// flush time is direct. Flush order must walk these bottom-up (data,
// then indirect, then inode, then imap) since each stage needs the
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

// Which imap block (checkpoint.imap_addr[] index / imap_cache key) covers this inode.
#define IMAP_BLK_IDX(inum) (((inum) - 1) / IMAP_ENTRIES_PER_BLK)

// Offset within that imap block's addrs[] array for this inode.
#define IMAP_OFFSET(inum) (((inum) - 1) % IMAP_ENTRIES_PER_BLK)
