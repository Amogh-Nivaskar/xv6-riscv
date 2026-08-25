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
 - imap blocks address array
 - Segment usage table i.e. for each segment it records num of live blocks and most recent modified time of any block in segment

The layout of the checkpoint region is as follows:
 - Last update timestamp - 4 bytes
 - Segment number of the last updated segment - 4 bytes
 - For imap block address array - 
    1 block = 1024 bytes
    block address = 4 bytes

    Hence, no. of inodes per imap block = 1024 / 4 = 256

    Space left in checkpoint block = 1024 - 4 - 4 = 1016

    Total no. of inodes = 1016 / 4 x 256 = 65024
    
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
   0 - 255              Addr 1
   256 - 511            Addr 2
   512 - 767            Addr 3
       .                   .
       .                   .
       .                   .  


So, given an inode number (i), we can find the index to get its imap block address like this -
   imap_index = i // 256

Given an index (idx), the range of inode nums covered in the imap at its block address can be found like this - 
   [idx * 256, (idx + 1) * 256 - 1]

In our design, since 1 inode takes up one block, we don't need any offset to find it. Just the block address is sufficient. Hence the imap is just an array of 4 byte inode block addresses.

Once we have the imap block containing the inode block address, we find the inode block address by offsetting in imap block by - 
   i % 256


Thus we have found the block address of an inode, for a given inode number.


## Segments structure

Now that we have seen how to read a file data block, we can design the write path, but before that lets see how a segment is designed in the first place as that info is important in the writing of data in a segment.

A segment has 100 blocks. The first block will always be the segment summary block, which will have, for each block two 4 byte fields. These 2 fields will have different meanings according to their values as they will be used to distinguish between the 4 types of blocks present in the segments - imap blocks, segment summary blocks (a segment can have more than one segment summary block), inode block, file data block.

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

B) Segment summary block - A segment summary block has both blocks as 0, hence it can be mistaken as empty, but we can work around it. For 1st block of a segment, if its value in segment summary block is full 0, but its next entries have values, then its not empty. Also



