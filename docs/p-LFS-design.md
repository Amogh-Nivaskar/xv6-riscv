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
   I % 256


Thus we have found the block address of an inode, for a given inode number.


