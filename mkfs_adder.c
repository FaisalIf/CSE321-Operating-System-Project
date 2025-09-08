
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#define BS          4096u
#define INODE_SIZE  128u
#define ROOT_INO    1u
#define DIRECT_MAX  12
#define MODE_FILE   0100000u
#define MODE_DIR    0040000u
#define MAGIC       0x4D565346u   

#pragma pack(push,1)
typedef struct {
    uint32_t magic;               // 0x4D565346
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

    uint64_t root_inode;          // 1
    uint64_t mtime_epoch;

    uint32_t flags;               // 0
    uint32_t checksum;            
} superblock_t;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
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
    uint64_t inode_crc;           
} inode_t;
#pragma pack(pop)

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;            
    uint8_t  type;              
    char     name[58];            
    uint8_t  checksum;           
} dirent64_t;
#pragma pack(pop)

static uint32_t CRC32_TAB[256];
static void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)? (0xEDB88320u ^ (c>>1)) : (c>>1);
        CRC32_TAB[i]=c;
    }
}
static uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for (size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
static void superblock_crc_finalize_block(superblock_t *sb){
    uint8_t tmp[BS];
    memset(tmp, 0, sizeof(tmp));
    memcpy(tmp, sb, sizeof(*sb));
    ((superblock_t*)tmp)->checksum = 0;
    uint32_t s = crc32(tmp, BS - 4);
    sb->checksum = s;
}
static void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE];
    memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}
static void dirent_checksum_finalize(dirent64_t* de){
    const uint8_t* p=(const uint8_t*)de; uint8_t x=0;
    for(int i=0;i<63;i++) x ^= p[i];
    de->checksum = x;
}


static inline int  get_bit(const uint8_t* bm, uint64_t idx){ return (bm[idx>>3]>>(idx&7)) & 1u; }
static inline void set_bit(uint8_t* bm, uint64_t idx){ bm[idx>>3] |= (1u<<(idx&7)); }

static void die(const char* m){ fprintf(stderr,"Error: %s\n", m); exit(2); }
static const char* base_name(const char* p){
    const char* s = strrchr(p,'/'); if(!s) s = strrchr(p,'\\');
    return s ? s+1 : p;
}

int main(int argc, char** argv){
    const char *in_img=NULL,*out_img=NULL,*path=NULL;

    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--input")  && i+1<argc) in_img  = argv[++i];
        else if(!strcmp(argv[i],"--output") && i+1<argc) out_img = argv[++i];
        else if(!strcmp(argv[i],"--file")   && i+1<argc) path    = argv[++i];
        else if(!strcmp(argv[i],"--help")){ fprintf(stderr,"Usage: %s --input in.img --output out.img --file <file>\n", argv[0]); return 2; }
        else { fprintf(stderr,"Unknown option: %s\n", argv[i]); return 2; }
    }
    if(!in_img || !out_img || !path){
        fprintf(stderr,"Usage: %s --input in.img --output out.img --file <file>\n", argv[0]); return 2;
    }

    crc32_init();

    FILE* f=fopen(in_img,"rb"); if(!f){ perror("open input"); return 2; }
    if(fseek(f,0,SEEK_END)!=0){ perror("fseek"); fclose(f); return 2; }
    long szL=ftell(f); if(szL<=0){ perror("ftell"); fclose(f); return 2; }
    if(fseek(f,0,SEEK_SET)!=0){ perror("fseek"); fclose(f); return 2; }
    if(szL % (long)BS) die("Invalid input image size");
    size_t sz=(size_t)szL;
    uint8_t* img=(uint8_t*)malloc(sz); if(!img) die("OOM");
    if(fread(img,1,sz,f)!=sz){ perror("fread"); fclose(f); free(img); return 2; }
    fclose(f);

    superblock_t* sb=(superblock_t*)img;
    if(sb->magic!=MAGIC) die("Bad superblock magic");
    if(sb->block_size!=BS) die("Unexpected block size");

    uint8_t*  inode_bm = img + sb->inode_bitmap_start*BS;
    uint8_t*  data_bm  = img + sb->data_bitmap_start*BS;
    inode_t*  inodes   = (inode_t*)(img + sb->inode_table_start*BS);

    struct stat st;
    if(stat(path,&st)!=0){ perror("stat"); free(img); return 2; }
    if(!(st.st_mode & S_IFREG)){ free(img); die("--file must be a regular file"); }
    FILE* fp=fopen(path,"rb"); if(!fp){ perror("open file"); free(img); return 2; }
    uint64_t fsize=(uint64_t)st.st_size;
    uint64_t need=(fsize+BS-1)/BS;
    if(need>DIRECT_MAX){ fclose(fp); free(img); die("File too large for MiniVSFS"); }

    const char* base = base_name(path);
    size_t namelen = strlen(base);
    if(namelen==0 || namelen>58){ fclose(fp); free(img); die("Filename length must be 1..58"); }
    char fname[59]={0}; memcpy(fname, base, namelen);

    inode_t* root=&inodes[ROOT_INO-1];
    if(root->mode!=MODE_DIR) { fclose(fp); free(img); die("Root inode is not a directory"); }
    if(root->direct[0]==0)   { fclose(fp); free(img); die("Root has no data block"); }
    uint32_t root_block = root->direct[0];
    uint8_t* rootblk = img + (uint64_t)root_block*BS;

    size_t slots = BS/sizeof(dirent64_t);
    for(size_t i=0;i<slots;i++){
        dirent64_t* de=(dirent64_t*)(rootblk + i*sizeof(dirent64_t));
        if(de->inode_no==0) continue;
        char exist[59]; memcpy(exist,de->name,58); exist[58]='\0';
        if(strncmp(exist,fname,58)==0){
            fclose(fp); free(img); die("File with same name already exists in root");
        }
    }

    int free_ino=-1;
    for(uint64_t i=0;i<sb->inode_count;i++){
        if(!get_bit(inode_bm,i)){ free_ino=(int)i; break; }
    }
    if(free_ino<0){ fclose(fp); free(img); die("No free inode"); }

    
    uint32_t blocks[DIRECT_MAX]={0}; uint64_t got=0;
    for(uint64_t i=0;i<sb->data_region_blocks && got<need; i++){
        if(!get_bit(data_bm,i)){
            blocks[got++] = (uint32_t)(sb->data_region_start + i);
            set_bit(data_bm,i);                                    
        }
    }
    if(got<need){ fclose(fp); free(img); die("No space for data blocks"); }

    for(uint64_t bi=0; bi<need; ++bi){
        uint8_t* blk = img + (uint64_t)blocks[bi]*BS;
        memset(blk,0,BS);
        size_t toread = (size_t)((bi+1)*BS<=fsize? BS : (fsize - bi*BS));
        if(toread>0 && fread(blk,1,toread,fp)!=toread){ perror("read file"); fclose(fp); free(img); return 2; }
    }
    fclose(fp);


    inode_t* ino = &inodes[free_ino];
    memset(ino,0,sizeof(*ino));
    ino->mode = MODE_FILE;
    ino->links = 1;
    ino->size_bytes = fsize;
    uint64_t now=(uint64_t)time(NULL);
    ino->atime = ino->mtime = ino->ctime = now;
    for(int i=0;i<DIRECT_MAX;i++) ino->direct[i]=0;
    for(uint64_t i=0;i<need;i++)   ino->direct[i]=blocks[i];
    inode_crc_finalize(ino);
    set_bit(inode_bm,(uint64_t)free_ino);
    dirent64_t de; memset(&de,0,sizeof(de));
    de.inode_no = (uint32_t)(free_ino+1);
    de.type = 1;                         
    memcpy(de.name, fname, namelen);
    dirent_checksum_finalize(&de);
    int placed=0;
    for(size_t i=0;i<slots;i++){
        dirent64_t* slot=(dirent64_t*)(rootblk + i*sizeof(dirent64_t));
        if(slot->inode_no==0){ *slot=de; placed=1; break; }
    }
    if(!placed){ free(img); die("Root directory full"); }

    root->links += 1;
    root->mtime = now;
    inode_crc_finalize(root);

    sb->mtime_epoch = now;
    superblock_crc_finalize_block(sb); 

    
    FILE* fo=fopen(out_img,"wb"); if(!fo){ perror("open output"); free(img); return 2; }
    if(fwrite(img,1,sz,fo)!=sz){ perror("write output"); fclose(fo); free(img); return 2; }
    fclose(fo);
    free(img);

    fprintf(stderr,"Added %s -> inode %d, %" PRIu64 " blocks\n", fname, free_ino+1, need);
    return 0;
}
