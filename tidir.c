#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>


/* From http://www.unige.ch/medecine/nouspikel/ti99/disks.htm#Magnetic%20media

Sector 0
VIB

Bytes	Contents	Typical values
>00-09	Disk name	"DISKNAME01"
>0A-0B	# of sectors	
    SS/SD: >168 DS/SD: >2D0
    SS/DD: >2D0 DS/DD: >5A9
>0C	Sectors/track	SD: >09 DD: >12
>0D-0F	DSR mark	"DSK"
>10	Protection	Unprot: " " Protected: "P"
>11	Tracks/side	>23 / >28
>12	Sides	>01 / >02
>13	Density	SS: >01 DS: >02
>14-37	(reserved)	>00
>38-EB	Bitmap	SS/SD: >38-64 
                DS/SD and SS/DD: >38-91 
                DS/DD: >38-EB
>EC-FF	(reserved)	Must be >FF

Sector 2+
FDR
 
Bytes	Contents	Comments
>00-09	File name	"MYFILE01"
>0A-0B	(reserved)	>00
>0C	File type	>80: variable
  >08: write protected
  >02: internal
  >01: program
>0D	Records/sector	>00 for program files
>0E-0F	# of sectors in file	Not counting FDR
>10	Last byte in last sector	>00 for fixed files
>11	Record length	>00 for program files
>12-13	Fixed: number of records
  Var: number of sectors
  Program: >00	! Bytes are swapped !
>14-1B	(reserved)	>00
>1C-FF	Cluster list	>UM >SN >OF == >NUM >OFS
*/

#define isVARIABLE(f) (f & 0x80)
#define isFIXED(f) (!(f & 0x80))
#define isPROTECTED(f) (f & 0x08)
#define isINTERNAL(f) (f & 0x02)
#define isDISPLAY(f) (!(f & 0x02))
#define isPROGRAM(f) (f & 0x01)

#define LESSER_OF(a,b) ((a)<(b)?(a):(b))

typedef unsigned char BYTE;
typedef unsigned short WORD;

typedef struct 
{
  BYTE name[10];
} FILENAME;

typedef struct {
  BYTE  diskName[10];
  WORD sectorCount;
  BYTE  sectorsPerTrack;
  BYTE  dskMark[3];
  BYTE  protect;
  BYTE  tracksPerSide;
  BYTE  sides;
  BYTE  density;
  BYTE  reserved1[36];
  BYTE  bitmap[180];
  BYTE  reserved2[20];
} VIB;


char *copyChars(BYTE *dst, const BYTE *src, int len)
{
    for (int i=0; i<len; ++i) {
        dst[i] = src[i];
    }
    dst[len] = 0;
    return dst;
}

typedef struct {
  BYTE  fileName[10];
  WORD reserved1;
  BYTE  fileType;
  BYTE  recordsPerSector;
  WORD sectorsInFile;
  BYTE  lastByteLastSector;
  BYTE  recordLength;
  WORD numberOfRecords; // number of Sectors if VAR. Little endian
  BYTE  reserved2[8];
  BYTE  clusterList[228];
} FDR;

// Cluster is 12 bit sectorNum, 12 bit offset, 3 bytes, nybbles reordered.
// >1C-FF	Cluster list	>UM >SN >OF == >NUM >OFS
// offset appears to be cumulative
void decodeCluster(BYTE *cluster, int *sectorNum, int *offset)
{
  *sectorNum = cluster[0]  | (cluster[1]&0x0f) << 8;
  *offset = cluster[2]<<4  | (cluster[1] >> 4);
}

typedef struct {
  BYTE* sectors; // raw disk image
  int    numSectors;  // actual sectors in raw disk image
  VIB*   vib;      // sector 0
  WORD* sector1; // sector 1, original
  WORD* fdrPtrs; // sector 1, no zeros
  int    numFdrs;
  FDR**  fdrs;    // pointers into disk->sectors image
} DISK;

int sizeofProgramFile(FDR* fdr)
{
    return 256*(ntohs(fdr->sectorsInFile)) - ((0x100-fdr->lastByteLastSector) & 0xff);
}

unsigned long fsize(FILE *file)
{
    fseek(file, 0, SEEK_END);
    unsigned long len = (unsigned long)ftell(file);
    fseek(file, 0, SEEK_SET);
    return len;
}

// no longer used
void readSectorFromDSK(FILE *file, int sectorNum, void *buf)
{
  char fcn[256];
  sprintf(fcn, "readSector(file, %d, buf)", sectorNum);
  
  int n = fseek(file, sectorNum * 256, SEEK_SET);
  if (n != 0) {
    fprintf(stderr, "Error: %s seeking sector %d: %s\n", fcn, sectorNum, strerror(errno));
    exit(1);
  }
  n = fread(buf, 1, 256, file);
  if (n != 256) {
    fprintf(stderr, "Error: %s got %ld bytes from sector: %s\n", fcn, n, strerror(errno));
    exit(1);
  }
  fprintf(stderr, "Success: %s %s\n", fcn, buf);
}

// Copy sectors already in memory
size_t copySectors(DISK* disk, int sectorNum, void *buf, int numSectors)
{
  char fcn[256];
  sprintf(fcn, "copySectors(disk, %d, %x, %d)", sectorNum, buf, numSectors);

  size_t size = 256 * numSectors;
    if (sectorNum + numSectors > disk->numSectors) {
        fprintf(stderr, "attempt to copy sectors %d+%d goes beyond last sector %d\n", sectorNum, numSectors, disk->numSectors);
        return 0;
    }
    memcpy(buf, disk->sectors + (256 * sectorNum), size);
    fprintf(stderr, "Success: %s\n", fcn);
    return size;
}

// read the whole disk into memory and set up some catalog structures and pointers
void readDisk(FILE *file, DISK* disk)
{
  char fcn[256];
  sprintf(fcn, "readDisk(file, disk)");
  
  size_t size   = fsize(file);
  disk->sectors = (BYTE*)malloc(size);
  disk->numSectors = size / 256;
  disk->vib     = (VIB*)   disk->sectors;
  disk->sector1 = (WORD*) (disk->sectors + 256);
  disk->fdrPtrs = (WORD*) malloc(256); // cleaned up catalog FDR pointers

  size_t n = fread(disk->sectors, 1, size, file);
  if (n != size) {
    fprintf(stderr, "Error: %s got %ld bytes, expected %ld: %s\n", fcn, n, size, strerror(errno));
    exit(1);
  }
  
  int numFdrs = 0;
  // count all file pointers even if catalog is weird (contains 0s)
  for (int i=0; i<128; ++i) {
    if (disk->sector1[i] != 0) {
      disk->fdrPtrs[numFdrs] = ntohs(disk->sector1[i]);
      ++numFdrs;
      //fprintf(stderr, "FDR pointer %d\n", disk->fdrPtrs[i]);
    }
  }
  
  disk->fdrs = (FDR**)malloc(sizeof(FDR*) * numFdrs);
  disk->numFdrs = numFdrs;
  for (int i=0; i<numFdrs; ++i) {
    // fdrs[i] is a pointer into memory of raw disk image
    disk->fdrs[i] = (FDR*)(disk->sectors + 256 * disk->fdrPtrs[i]);
//    readSector(file, disk->fdrPtrs[i], (void*)&disk->fdrs[i]);
  }
}

int countSetBits(char c)
{
  c = c - ((c >> 1) & 0x55);
  c = (c & 0x33) + ((c >> 2) & 0x33);
  return (c & 0x0f) + (c >> 4);
}

int countZeroBits(char *bits, int len)
{
    int count = 0;
    for (int i=0; i<len; ++i) {
      char c = bits[i];
      count += (8 - countSetBits(c));
    }
}

void testCountBits()
{
    for (int i=0; i<256; i++) {
        printf("%2x %3d\n", i, countSetBits(i));
    }
}

// filename padded with spaces up to 10 chars

void padFilename(BYTE* in, BYTE* out) 
{
   BYTE *p1 = in;
   for(int i=0; i<10; ++i)
   {  
      if(*p1) {
        out[i] = *p1++;
      } else {
          out[i] = ' ';
      }
   }       
}

// filenames must be padded
int compareFilenames(BYTE* filename1, BYTE* filename2)
{
  
  for (int i=0; i<10; ++i) {
      int d  = filename1[i] - filename2[i];
      if (d<0) {
          return -1;
      } else if (d > 0) {
          return 1;
      }
  }
  return 0;
}

FDR* findFile(DISK* disk, BYTE* filename)
{
    for (int i=0; i<disk->numFdrs; ++i) {
        if (!compareFilenames(disk->fdrs[i]->fileName, filename)) {
            return disk->fdrs[i];
        }
    }
    return NULL;
}

// Return the offset in the last cluster (total sectors counting from 0)
int lastClusterOffset(FDR* fdr)
{
    int sectorNum, offset;
    int result = 0;
    for (int i = 0; i < 228; i += 3) {
        decodeCluster(fdr->clusterList + i, &sectorNum, &offset);
        if (sectorNum != 0) {
            result = offset;
        } else {
          break;
        }
    }
    return result;
}

// Load all the sectors of a file into buf. size is the raw sector buffer size.
void loadFile(DISK* disk, FDR* fdr, BYTE **buf, size_t *size)
{
    int lastOffset = lastClusterOffset(fdr);
    int sectorsInFile = ntohs( fdr->sectorsInFile);
    if (1 + lastOffset != sectorsInFile) {
        fprintf(stderr, "Warning: last Cluster Offset %d not equal to FDR sectors in file %d\n", lastOffset, sectorsInFile);
    }
    *size = 256 * (1 + lastOffset);
    *buf = (BYTE*) malloc(*size);
    
    // Read clusters
    BYTE* bufPtr = *buf;
    int sectorNum, offset;
    int runningOffset = 0;
    for (int i = 0; i < 228; i += 3) {
        decodeCluster(fdr->clusterList + i, &sectorNum, &offset);
        if (sectorNum != 0) {
            fprintf(stderr, "%02x%02x%02x %3x(%x) \n", fdr->clusterList[i], fdr->clusterList[i+1], fdr->clusterList[i+2] ,sectorNum, offset);
            copySectors(disk, sectorNum, bufPtr, offset - runningOffset); 
            runningOffset = offset;
            bufPtr += 256 * (offset - runningOffset);
        } else {
            break;
        }
    }
}

void catFile(FDR* fdr, BYTE *buf, size_t size)
{
    // PGM is not printable but if it were it's a single bytestream.
    // Record oriented formats:
    // DISplay format is supposed to be printable.
    // INT is not supposed to be printable (binary, floating point number format, or anything)
    // VAR starts with a length byte, FIX does not.
    
    BYTE fileType = fdr->fileType;
    if (isPROGRAM(fileType)) {
      fwrite(buf, 1, sizeofProgramFile(fdr), stdout);
    } else if (isVARIABLE(fileType)) {
      int sectorsInFile = ntohs(fdr->sectorsInFile);
      for (int i=0; i<sectorsInFile; ++i) {
        BYTE *bufptr = buf + 256*i;
        int bytesConsumed = 0;
        int limit = 256;
        if (i+1 == sectorsInFile) {
            limit = fdr->lastByteLastSector;
        }
        // output all records in one sector
        while(bytesConsumed < limit) {
          int len = *bufptr++;
          if (bytesConsumed+len>limit) break;
          //printf("len byte=%d, '%.*s'\n", len, len, bufptr); 
          fwrite(bufptr, 1, len, stdout);
          fputc('\n', stdout);
          bytesConsumed += 1+len;
          bufptr += len;
        }
      }          
    } else if (isFIXED(fileType)) {
      int recordsRemaining = le16toh(fdr->numberOfRecords);
      int sectorsInFile = ntohs(fdr->sectorsInFile);
      for (int i=0; i<sectorsInFile; ++i) {
        BYTE *bufptr = buf + 256*i;
        int n = LESSER_OF(recordsRemaining, fdr->recordsPerSector);
        fwrite(bufptr, fdr->recordLength, n, stdout);
        recordsRemaining -= n;
      }
    }
}

void processFile(DISK* disk, BYTE *arg, BYTE flag)
{
  char fileName[10];
  padFilename(arg, fileName);
  FDR* fdr = findFile(disk, fileName);
  if (fdr) {
    BYTE* buf;
    size_t size;
    loadFile(disk, fdr, &buf, &size);
    if (flag == 'c') {
        catFile(fdr, buf, size);
    }
  } else {
    fprintf(stderr, "File not found on disk: %s\n", arg);
  }
    
}

void printCatalog(DISK* disk)
{
  char diskName[11], dskMark[4];
  copyChars(&diskName[0], disk->vib->diskName, 10);
  copyChars(&dskMark[0],  disk->vib->dskMark, 3);
    
  int secTotal        = ntohs(disk->vib->sectorCount);
  int secUsed         = countZeroBits(((char*)disk->vib)+0x38, 0xc8);
  int secFree         = secTotal - secUsed;
  char  protect         = disk->vib->protect;
  int   sectorsPerTrack = disk->vib->sectorsPerTrack;
  int   tracksPerSide   = disk->vib->tracksPerSide;
  int   sides           = disk->vib->sides;
  int   density         = disk->vib->density;  
  
  printf("%3s.%10s  Free=%4d Used=%4d %c %dS,%dT,%dS,%dD\n", dskMark, diskName, secFree, secUsed, protect,
          sectorsPerTrack, tracksPerSide, sides, density);
  printf("FDR Name       Size Type        P Sector(Offset)\n");
  printf("--- ---------- ---- ----------- - --------------\n");
  
  for (int i=0; i < disk->numFdrs; ++i) {
    FDR* fdr = disk->fdrs[i];
    char name[11], typestr[12], protect;
    copyChars(name, fdr->fileName, 10);
    int sectors = ntohs(fdr->sectorsInFile);
    
    int fileType = fdr->fileType;
    if (fileType & 0x1) {
        int totalBytes = sizeofProgramFile(fdr);
        sprintf(typestr, "Pgm   %5d", totalBytes);
    } else {
        sprintf(typestr, "%3s/%3s %3d", (fileType&0x02 ? "Int"  : "Dis"),
                                        (fileType&0x80 ? "Var"  : "Fix"),
                                        fdr->recordLength);
    }
    protect = fileType & 0x8 ? 'P' : ' ';
    printf("%3x %-10s %4d %-11s  %c ", disk->fdrPtrs[i], name, 1+sectors, typestr, protect);
    // print cluster list
    int sectorNum, offset;
    for (int i = 0; i < 228; i += 3) {
        decodeCluster(fdr->clusterList + i, &sectorNum, &offset);
        if (sectorNum != 0) {
            printf("%02x%02x%02x %3x(%x) ", fdr->clusterList[i], fdr->clusterList[i+1], fdr->clusterList[i+2] ,sectorNum, offset);
        } else {
            break;
        }
    }
    printf("\n");
  }
}


int main(int argc, char **argv)
{
  //printf("size of VIB is %ld\n", sizeof(VIB));
  //printf("size of FDR is %ld\n", sizeof(FDR));
  //printf("offsetof(density) is %ld\n", offsetof(VIB, density));
  if (argc<2) {
      fprintf(stderr, "Usage: %s file.dsk [TIFILE ...]\n", argv[0]);
      exit(1);
  }
  FILE *file = fopen(argv[1], "r");
  if (!file) {
    fprintf(stderr, "can't read file %s\n", argv[1]);
    exit(1);
  }
  unsigned int len = fsize(file);
  fprintf(stderr, "File size is %d\n", len);
  
  DISK* disk = (DISK*)malloc(sizeof(DISK));
  readDisk(file, disk);
  fclose(file);
  BYTE *dskMark = disk->vib->dskMark;
  if (dskMark[0] == 'D' && dskMark[1] == 'S' && dskMark[2] == 'K') {
    if (argc == 2) {
      printCatalog(disk);
    } else {
      for (int i=2; i<argc; ++i) {
        processFile(disk, argv[i], 'c'); // future -c cat
      }
    }
    
  } else {
    fprintf(stderr, "Disk sector 0 does not look like a volume, not cataloging.\n");
  }
  
}

  
