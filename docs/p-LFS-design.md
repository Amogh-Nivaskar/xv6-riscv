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




