#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// Exercises the LFS segment cleaner (garbage collector).
//
// The disk has a fixed number of segments; nothing ever reuses one
// until the cleaner reclaims it. This test writes-then-deletes far
// more total data than the disk's raw capacity while keeping one
// "keeper" file alive throughout. If the cleaner is broken (or never
// fires), the filesystem runs out of segments and panics well before
// this completes. If it works, this finishes cleanly and the keeper
// file's content survives, byte for byte, across however many
// cleaning cycles happened underneath it.
//
// Files are churned in batches rather than one at a time: a single
// 64KB file's writes don't come close to the dirty-block threshold
// that triggers a real flush, so a lone create+write+unlink can pop
// its data straight out of the cache before any of it ever reaches
// disk -- never touching a real segment at all, and never giving the
// cleaner anything to do. Keeping a whole batch's files alive at once
// means their combined dirty data reliably crosses that threshold and
// gets flushed to real segment addresses before any of them are
// deleted, so unlinking the batch afterward creates real garbage in
// real segments.

static void die(const char *msg)
{
    printf("FAILED: %s\n", msg);
    exit(1);
}

#define WRITE_CHUNK (4 * 1024)       // bytes per write() call
#define KEEPER_BLOCKS 20             // > NDIRECT(12): forces an indirect block
#define KEEPER_SIZE (KEEPER_BLOCKS * 1024)
#define CHURN_FILE_SIZE (64 * 1024)  // bytes per throwaway file
#define BATCH_FILES 10               // files kept alive together per batch
#define NUM_BATCHES 240              // ~150MB total, well past the ~50MB disk

char wbuf[WRITE_CHUNK];
char rbuf[WRITE_CHUNK];
char keeper_expected[KEEPER_SIZE];

// deterministic, position-dependent so any corruption (wrong block,
// torn write, stale content from another file) is detectable exactly.
static char pattern_byte(int pos)
{
    return (char)((pos * 131 + 17) & 0xff);
}

static void write_keeper(void)
{
    int fd = open("gckeeper", O_CREATE | O_RDWR | O_TRUNC);
    if (fd < 0)
        die("could not create gckeeper");

    for (int i = 0; i < KEEPER_SIZE; i++)
        keeper_expected[i] = pattern_byte(i);

    int off = 0;
    while (off < KEEPER_SIZE)
    {
        int n = KEEPER_SIZE - off < WRITE_CHUNK ? KEEPER_SIZE - off : WRITE_CHUNK;
        if (write(fd, keeper_expected + off, n) != n)
            die("short write to gckeeper");
        off += n;
    }
    close(fd);
}

static void verify_keeper(const char *when)
{
    int fd = open("gckeeper", O_RDONLY);
    if (fd < 0)
        die("gckeeper missing during verify");

    int off = 0;
    while (off < KEEPER_SIZE)
    {
        int n = KEEPER_SIZE - off < WRITE_CHUNK ? KEEPER_SIZE - off : WRITE_CHUNK;
        int r = read(fd, rbuf, n);
        if (r != n)
            die("short read from gckeeper during verify");
        if (memcmp(rbuf, keeper_expected + off, n) != 0)
        {
            printf("FAILED: gckeeper content mismatch %s (offset %d)\n", when, off);
            exit(1);
        }
        off += r;
    }
    close(fd);
}

static void name_for(int idx, char *name)
{
    name[0] = 'g';
    name[1] = 'c';
    name[2] = 'c';
    name[3] = '0' + (idx / 1000) % 10;
    name[4] = '0' + (idx / 100) % 10;
    name[5] = '0' + (idx / 10) % 10;
    name[6] = '0' + idx % 10;
    name[7] = '\0';
}

static void write_churn_file(int idx, const char *name)
{
    int fd = open(name, O_CREATE | O_RDWR | O_TRUNC);
    if (fd < 0)
        die("could not create churn file");

    for (int i = 0; i < WRITE_CHUNK; i++)
        wbuf[i] = pattern_byte(idx * 7 + i);

    int off = 0;
    while (off < CHURN_FILE_SIZE)
    {
        if (write(fd, wbuf, WRITE_CHUNK) != WRITE_CHUNK)
            die("short write to churn file");
        off += WRITE_CHUNK;
    }
    close(fd);
}

static void churn_batch(int batch)
{
    char names[BATCH_FILES][16];

    for (int i = 0; i < BATCH_FILES; i++)
    {
        int idx = batch * BATCH_FILES + i;
        name_for(idx, names[i]);
        write_churn_file(idx, names[i]);
    }

    // all BATCH_FILES are still alive here -- their combined dirty
    // data comfortably exceeds the flush threshold, so this batch is
    // guaranteed to hit real disk before any of it becomes garbage.
    for (int i = 0; i < BATCH_FILES; i++)
    {
        if (unlink(names[i]) < 0)
            die("unlink of churn file failed");
    }
}

int main(void)
{
    printf("gctest: writing keeper file\n");
    write_keeper();
    verify_keeper("right after writing");

    printf("gctest: churning ~%dMB (%d batches x %d files x %dKB) through a 100-segment, ~50MB disk\n",
           (NUM_BATCHES * BATCH_FILES * CHURN_FILE_SIZE) / (1024 * 1024),
           NUM_BATCHES, BATCH_FILES, CHURN_FILE_SIZE / 1024);

    for (int b = 0; b < NUM_BATCHES; b++)
    {
        churn_batch(b);

        if (b % 20 == 19)
        {
            verify_keeper("mid-churn");
            printf("gctest: %d/%d batches done, keeper still intact\n", b + 1, NUM_BATCHES);
        }
    }

    verify_keeper("after all churn");

    // filesystem must still be fully usable: segments were actually
    // reclaimed, not just "didn't crash by luck".
    int fd = open("gcfinal", O_CREATE | O_RDWR | O_TRUNC);
    if (fd < 0)
        die("could not create file after churn");
    if (write(fd, "still alive", 11) != 11)
        die("write failed after churn");
    close(fd);

    fd = open("gcfinal", O_RDONLY);
    if (fd < 0)
        die("could not reopen file after churn");
    char tail[12];
    memset(tail, 0, sizeof(tail));
    if (read(fd, tail, 11) != 11 || memcmp(tail, "still alive", 11) != 0)
        die("post-churn file content wrong");
    close(fd);
    unlink("gcfinal");

    unlink("gckeeper");

    printf("gctest: OK\n");
    exit(0);
}
