# **p-LFS Design Document**

We are designing a parallel LFS in our modified xv6 kernel. Our modified xv6 kernel already has kernel threads implemented. p-LFS will aim to take advantage of that by having a log per core, so that threads can write to the file system concurrently.

*We will first design vanilla LFS here, and then modify it to p-LFS*

## Coverage
1. On-disk Layout
2. Write Path
3. Read Path
4. Crash Recovery
5. Garbage Collection

## On-disk Layout


Block size = 1024 bytes = 1 KB

Segment size = 512 KB = 512 blocks

Reserved Blocks = 6 

Total num of segments = 100

File System Size (FSSIZE) = No. of segments x Segment size + Reserved blocks
                          = 512 x 100 + 6 = 51,206 blocks


struct lfs_superblock  {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint checkpoint1start;      // Start of Checkpoint 1 region
  uint checkpoint2start;      // Start of Checkpoint 2 region
  uint checkpointsize;        // Size of a Checkpoint region
  uint ninodes;      // Number of inodes
  uint segsize;      // Segment size
};

All the other info can be derived.

### Checkpoint region
The checkpoint region will contain - 
 - Last update timestamp
 - Segment number of the last updated segment
 - Segment Fill offset
 - imap blocks address array
 - Segment usage table i.e. for each segment it records num of live blocks and most recent modified time of any block in segment

The layout of the checkpoint region is as follows:
 - Last update timestamp - 4 bytes
 - Segment number of the last updated segment - 4 bytes
 - Segment fill offset block index - 4 bytes
 - For imap block address array - 
    1 block = 1024 bytes
    block address = 4 bytes

    Hence, no. of inodes per imap block = 1024 / 4 = 256

    Space left in checkpoint block = 1024 - 4 - 4 - 4 = 1012

    Total no. of inodes = 1012 / 4 x 256 = 64,768
    
 - For SUT - 
    No. of live blocks - 4 bytes
    Most recent modified time for any block in segment - 4 bytes
    Total space per segment = 4 + 4 = 8 bytes

    For 100 segments, Total space = 100 x 8 = 800 bytes.

Hence, a Checkpoint region is within 2 blocks (with some space to spare)

Hence Reserved Blocks = boot block + super blocks + checkpoint region 1 + checkpoint region 2 
                      = 1 + 1 + 2 + 2 = 6 blocks


## Find an inode block from an inode number

We will work with 1 inode per block.

No. of inodes per imap block = 256

Hence, the imap block addresses in checkpoint regions will be like this - 

   Inode Num Range     Imap block address
  ---------------------------------------
   1 - 256              Addr 1
   257 - 512            Addr 2
   513 - 768            Addr 3
       .                   .
       .                   .
       .                   .  


So, given an inode number (i), we can find the index to get its imap block address like this -
   imap_index = (i-1) // 256

Given an index (idx), the range of inode nums covered in the imap at its block address can be found like this - 
   [idx * 256 + 1, (idx + 1) * 256]

In our design, since 1 inode takes up one block, we don't need any offset to find it. Just the block address is sufficient. Hence the imap is just an array of 4 byte inode block addresses.

Once we have the imap block containing the inode block address, we find the inode block address by offsetting in imap block by - 
   (i-1) % 256


Thus we have found the block address of an inode, for a given inode number.


## Segments structure

Now that we have seen how to read a file data block, we can design the write path, but before that lets see how a segment is designed in the first place as that info is important in the writing of data in a segment.

A segment has 512 blocks. The first block will always be the segment summary block, which will have, for each block two 4 byte fields. These 2 fields will have different meanings according to their values as they will be used to distinguish between the 4 types of blocks present in the segments - imap blocks, segment summary blocks (a segment can have more than one segment summary block), inode block, file data block.

   Block type     | 1st 4 bytes      |     2nd 4 bytes
---------------------------------------------------------------------------------
imap block        |     0            |     imap block addrs array index + 1
                  |                  |
seg sum block     |     0            |        0
                  |                  |
inode block       |   inode num      |        0
                  |                  |
file data block   |   inode num      |    file data block num 1 indexed


Lets understand what is going on for each of these blocks and how to find out if these blocks are live or not.

A) Imap block - since imap blocks don't belong to any particular file, the first 4 bytes are 0 and 2nd 4 bytes are the index value of the imap block addrs array in the checkpoint region, which points to this imap block.

To check for liveness, we can see the address in the index mentioned in segment summary block and if they match then its alive, else its dead.

B) Segment summary block - A segment summary block has both blocks as 0, hence it can be mistaken as empty, but we can work around it. The checkpoint header stores the currently being written segment's segment number and filled offset, and we know that at any given time only the currently written segment can be partially written. Every other segment must be either full or empty (untouched).

 So when a segment is partially filled, its segment summary block is also partially filled as all entries after last filled offset are full zero and this never changes as even if the segment is filled in later, its block related info is filled in another segment summary block. But if we do find such a segment where the primary segment summary block has many full zero entries, and we know that this isn't the current active segment, then we can always conclude that the first full zero entry in the primary segment summary block is another segment summary block.

 Also, we can always know whether a segment is empty or not by referring to the live blocks for segment in the SUT, as live blocks for empty segment will always be 0.

 Since a segment has 512 blocks, and a segment summary block can cover only 1024 / (4+4) = 128 blocks, then at minimum a segment will need 4 segment summary blocks (assuming each segment summary block is full). The rule will be that a segment summary block will be directly followed by all the blocks it represents. For eg: if we want to flush the entire segment at once, it will be structured like - 1st segment summary block, followed by its respective 127 blocks, 2nd segment summary block, followed by its respective 127 blocks, so on and so forth. 
 For another example, lets take a case of partial writes. Lets say we want to flush 150 blocks, then in this case the segment structure will look like - 1st segment summary block (fully filled), followed by its 127 blocks, 2nd segment summary block (only filled 22 entries, rest are full zero), followed by its respective 21 blocks (excluded 1 block for this 2nd flush's segment summary block). Now when we want to flush again, lets say 100 blocks, we again append a new segment summary block filled with 100 entries, followed by its respective 100 blocks. 

C) Inode block - 1st 4 bytes are the inode number of the file and the 2nd 4 bytes are 0 in the segment summary block entry. To check livenes, we use the inode number value to find the inode block address via the imap and then compare this found address to the address of current block. If they match, it means it is live, else its dead.

D) File Data block - 1st 4 bytes are the inode number of the file and the 2nd 4 bytes is the position in the file, in the segment summary block entry. To check livenes, we use the inode number value to find the inode block address via the imap and then check if the block address for file block position given in the inode block matches with the current block, if they do then its alive, else its dead.


## Write Path

The basic algorithm for in-memory write will be like this - 
1) Append data to segment
2) Append inode block to segment
3) Append imap block to segment
4) Update segment summary
5) Update imap block address array in checkpoint region
6) Update segment usage table in Checkpoint region
7) Update timestamp, last updated segment number and segment offset in Checkpoint region

The checkpoint region is just within 2 blocks i.e. 2 kb and hence can easily fit in a 4 kb page. So we can just create a struct for checkpoint region like this - 

uint region_num; // global variable denoting which region (1 or 2) the current struct checkpoint is representing

#define IMAP_BLK_NUM = 64768 / 256 = 253
#define SEG_NUM = 100

struct checkpoint {
   uint timestamp;
   uint segment_num; 
   uint fill_offset;
   uint imap_addr[IMAP_BLK_NUM];
   sut_entry sut[SEG_NUM]  
}

struct sut_entry {
   uint live_count;
   uint last_mod_time;
}


Storing the Segment in-memory is not so straight forward, as a page is just 4 kb and the segment is 512 kb in size. Hence, what we do is we store pointers to the pages having the segment blocks. We need 512 / 4 = 128 pages to store the full segment. Thus with 4 byte address, we will need 4 x 128 = 512 bytes for full pointer array.

struct seg_buf {
  int valid;   // has data been read from disk?
  int disk;    // does disk "own" buf?
  uint dev;
  uint segno;
  struct sleeplock lock;
  uint refcnt;
  uint addrs[128]; // stores the address of the pages which store the actual segment blocks
};







