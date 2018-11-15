#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char BYTE;
typedef unsigned short WORD;


// Ignore trivial lists that do not have at least 2 frames - sound on and sound off.
#define MIN_FRAMES 2

// Ignore any frame with more than 13 bytes because 
#define MAX_COUNT 13

// Size of hole to print between lists. EVEN and a word address.
#define MAX_HOLE 3

long fsize(FILE *file)
{
    fseek(file, 0, SEEK_END);
    long len = ftell(file);
    fseek(file, 0, SEEK_SET);
    return len;
}

int printSoundList(BYTE *buf, long start, long end, BYTE *dupe)
{
    long i = start;
    int success = 0;
    BYTE count, ticks;
    long totalTicks = 0;
    long frames = 0;
    while(i<end) {
      // When 0 appears in the count field, the list is ended.
      char label[10];
      if (frames == 0) {
          sprintf(label, "\nS$%04X", i);
      } else {
          label[0] = 0;
      }
      count = buf[i++];
      printf("%6s BYTE %d", label, count);
      if (count == 0) {    
          break;
      } else {
          dupe[i-1] = count;
      }
      if (i + count < end) {
          for (long j=0; j<count; ++j) {
              printf(",>%02X", buf[i+j]);
          }
          i += count;
          ++frames;
          ticks = buf[i++];
          totalTicks += ticks;
          printf(",%d\n", ticks);
      } else {
          i += count;
      }
    }
    printf("\n");
    return success;
}        

// returns i on success (one past end of list/buf)
long testSoundList(BYTE *buf, long start, long end)
{
    long i = start;
    long success = 0;
    BYTE count, ticks;
    long totalTicks = 0;
    long frames = 0;
    while(i<end) {
      count = buf[i++];
      if (count > MAX_COUNT) {
          // Skipping impossibly dense sound list. 13 = 1 count + 3x2 cmd tones +1 cmd noise +4 cmd vols +1 ticks.
          break;
      }
      // When 0 appears in the count field, the list is ended.
      if (count == 0) {
          if (frames != 0 && frames >= MIN_FRAMES) {
              success = i;
          }
          break;
      }
      // Out of range error
      if (count + i > end) {
          break;
      }

      // if the first byte isn't latching a register, it's not a sound list. 
      // (first byte of first frame can't be a least significant byte. However, second frame can use one!)
      if (!(buf[i] & 0x80)) {
          break;
      }
      
      i += count;
      if (i < end) {
          ++frames;
          ticks = buf[i++];
          totalTicks += ticks;
      }
    }
    long length = i - start;

    if (success) {
        printf("\nFound possible list at start >%x end >%x length %d frames %d ticks %d\n", start, i-1, length, frames, totalTicks );
    }
    return success;
}

void peekAtHole(BYTE *buf, long lastEnd, long start) 
{
  // print 2 or 3 bytes between lists in case its a linked list
  if (start - lastEnd > 0 && start - lastEnd <= MAX_HOLE) {
      printf("M$%04X BYTE >%02X", lastEnd, buf[lastEnd]);
      for (long i=lastEnd+1; i<start; ++i) {
          printf(",>%02x", buf[i]);
      }
      printf("\n");
  }
}
    
void findSoundLists(BYTE *buf, long len)
{
    
    // dupe array is marked 1 at each count byte printed as part of a sound list. Another search won't start there.
    BYTE* dupe = (BYTE*)malloc(len);
    memset(dupe, 0, len);

    long successes = 0;
    
    long warnings  = 0;
    long lastEnd = 0;

    printf("Searching...\n");
    // Brute force. Start at each location, see if it makes a coherent sound list terminated with a 0.
    for (long i=0; i<len; ++i) {
        if (buf[i] != 0 && !dupe[i]) {
          long end = testSoundList(buf, i, len);
          if (end) {
              if (i <= lastEnd) {
                  printf("Warning: Overlaps preceding list by %d bytes\n", (lastEnd - i));
                  ++warnings;
              }
              peekAtHole(buf, lastEnd, i);
              printSoundList(buf, i, end, dupe);
              ++successes;
              lastEnd = end;
          }
        }
    }

    printf("\nFound %d possible sound lists including %d alternate overlapping forms\n", successes, warnings);
    fprintf(stderr, "Found %d possible sound lists including %d alternate overlapping forms\n", successes, warnings);
}

int main(int argc, char **argv)
{
    if (argc<2) {
        fprintf(stderr, "Usage: %s file\n", argv[0]);
        exit(1);
    }

    FILE *fp = fopen(argv[1], "r");
    if (fp == NULL) {
        fprintf(stderr, "can't read file %s: ", argv[1]);
        perror("");
        exit(1);
    }

    long len = fsize(fp);
    BYTE *buf = (BYTE*)malloc(len);
    size_t n = fread(buf, 1, len, fp);
    if (n < len) {
        fprintf(stderr, "error reading %d bytes from file %s: ", len, argv[1]);
        perror("");
        exit(1);
    }
    fprintf(stderr, "Read %d bytes from %s\n",    len, argv[1]);
    
    findSoundLists(buf, len);
}
