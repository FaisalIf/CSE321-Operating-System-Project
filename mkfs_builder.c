// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

uint64_t g_random_seed = 0; 

#pragma pack(push,1)
typedef struct {
    uint32_t magic;               // 0x4D565346 ("MVFS")
    uint32_t version;             // 1
    uint32_t block_size;          // 4096
    uint64_t total_blocks;
    uint64_t inode_count;

    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;

    uint64_t root_inode;          // always 1
    uint64_t mtime_epoch;         // creation time

    uint32_t flags;               // 0
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;        // 0100000 (file) or 0040000 (dir)
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc;   // crc32 of first 120 bytes
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t) == INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;         // 1=file, 2=dir
    char name[58];
    uint8_t checksum;     // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t) == 64, "dirent size mismatch");

// ==========================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}


typedef struct {
    char image[256];
    uint64_t size_kib;
    uint64_t inodes;
} options_t;

void parse_cli(int argc, char** argv, options_t* opts) {
    if (argc < 7) {
        fprintf(stderr, "Usage: %s --image out.img --size-kib <180..4096> --inodes <128..512>\n", argv[0]);
        exit(1);
    }

    opts->image[0] = '\0';
    opts->size_kib = 0;
    opts->inodes = 0;

    for (int i=1;i<argc;i++) {
        if (strcmp(argv[i],"--image")==0 && i+1<argc) {
            const char *s = argv[++i];
            strncpy(opts->image, s, sizeof(opts->image) - 1);
            opts->image[sizeof(opts->image) - 1] = '\0';
        } else if (strcmp(argv[i],"--size-kib")==0 && i+1<argc) {
            opts->size_kib = strtoull(argv[++i],NULL,10);
        } else if (strcmp(argv[i],"--inodes")==0 && i+1<argc) {
            opts->inodes = strtoull(argv[++i],NULL,10);
        }
    }
    if (opts->size_kib < 180 || opts->size_kib > 4096 || opts->size_kib % 4 != 0) {
        fprintf(stderr,"Invalid size-kib\n"); exit(1);
    }
    if (opts->inodes < 128 || opts->inodes > 512) {
        fprintf(stderr,"Invalid inodes count\n"); exit(1);
    }
    if (opts->image[0] == '\0') {
        fprintf(stderr,"No image filename provided\n"); exit(1);
    }
}

// =============== Builder ===============
void build_image(const options_t* opts) {
    uint64_t total_blocks = (opts->size_kib * 1024ULL) / BS;

    // Layout
    uint64_t inode_bitmap_start = 1;
    uint64_t data_bitmap_start  = 2;
    uint64_t inode_table_start  = 3;
    uint64_t inode_table_blocks = ((opts->inodes * INODE_SIZE) + BS - 1)/BS;
    uint64_t data_region_start  = inode_table_start + inode_table_blocks;
    uint64_t data_region_blocks = total_blocks - data_region_start;

    // Superblock
    superblock_t sb = {0};
    sb.magic = 0x4D565346; // 'MVFS'
    sb.version = 1;
    sb.block_size = BS;
    sb.total_blocks = total_blocks;
    sb.inode_count = opts->inodes;
    sb.inode_bitmap_start = inode_bitmap_start;
    sb.inode_bitmap_blocks = 1;
    sb.data_bitmap_start = data_bitmap_start;
    sb.data_bitmap_blocks = 1;
    sb.inode_table_start = inode_table_start;
    sb.inode_table_blocks = inode_table_blocks;
    sb.data_region_start = data_region_start;
    sb.data_region_blocks = data_region_blocks;
    sb.root_inode = ROOT_INO;
    sb.mtime_epoch = (uint64_t)time(NULL);
    sb.flags = 0;
    superblock_crc_finalize(&sb);

    // Buffers
    uint8_t* block = calloc(1, BS);
    if (!block) { perror("calloc"); exit(1); }
    FILE* f = fopen(opts->image,"wb"); if (!f){perror("fopen"); free(block); exit(1);}

    // Write superblock
    memset(block,0,BS);
    memcpy(block,&sb,sizeof(sb));
    if (fwrite(block,BS,1,f) != 1) { perror("write superblock"); fclose(f); free(block); exit(1); }

    // Inode bitmap (mark root inode)
    memset(block,0,BS);
    block[0] = 0x01; 
    if (fwrite(block,BS,1,f) != 1) { perror("write inode bitmap"); fclose(f); free(block); exit(1); }

    memset(block,0,BS);
    block[0] = 0x01; 
    if (fwrite(block,BS,1,f) != 1) { perror("write data bitmap"); fclose(f); free(block); exit(1); }

    // Inode table
    memset(block,0,BS);
    inode_t root = {0};
    root.mode = 040000; // dir
    root.links = 2;
    root.uid = 0; root.gid = 0;
    root.size_bytes = BS;
    root.atime = root.mtime = root.ctime = (uint64_t)time(NULL);
    root.direct[0] = (uint32_t)data_region_start; // first data block absolute number
    root.proj_id = 2; 
    root.uid16_gid16=0; root.xattr_ptr=0;
    inode_crc_finalize(&root);
    memcpy(block,&root,sizeof(root));
    if (fwrite(block,BS,1,f) != 1) { perror("write inode table (first)"); fclose(f); free(block); exit(1); }

    for (uint64_t i=1;i<inode_table_blocks;i++) {
        memset(block,0,BS);
        if (fwrite(block,BS,1,f) != 1) { perror("write inode table pad"); fclose(f); free(block); exit(1); }
    }

    // Root directory block
    memset(block,0,BS);
    dirent64_t dot = {0}; dot.inode_no=1; dot.type=2; strcpy(dot.name,"."); dirent_checksum_finalize(&dot);
    dirent64_t dotdot = {0}; dotdot.inode_no=1; dotdot.type=2; strcpy(dotdot.name,".."); dirent_checksum_finalize(&dotdot);
    memcpy(block,&dot,sizeof(dot));
    memcpy(block+sizeof(dot),&dotdot,sizeof(dotdot));
    if (fwrite(block,BS,1,f) != 1) { perror("write root dir"); fclose(f); free(block); exit(1); }

    for (uint64_t i=1;i<data_region_blocks;i++) {
        memset(block,0,BS);
        if (fwrite(block,BS,1,f) != 1) { perror("write data pad"); fclose(f); free(block); exit(1); }
    }

    free(block);
    fclose(f);
    printf("Image %s created: %llu blocks (%llu KiB), %llu inodes\n",
           opts->image,(unsigned long long)total_blocks,
           (unsigned long long)opts->size_kib,
           (unsigned long long)opts->inodes);
}

int main(int argc, char** argv) {
    crc32_init();
    options_t opts={0};
    parse_cli(argc,argv,&opts);
    build_image(&opts);
    return 0;
}
