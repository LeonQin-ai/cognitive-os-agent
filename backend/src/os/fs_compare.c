#include "os/os_fs.h"
#include <stdlib.h>
#include <string.h>

int fs_files_equal(const char *left,const char *right,size_t max_bytes){
    long long a=fs_file_size(left),b=fs_file_size(right);
    if(a<0||b<0||fs_is_dir(left)||fs_is_dir(right))return -1;
    if(a!=b)return 0;
    if((unsigned long long)a>max_bytes)return -1;
    uint64_t av=fs_file_version(left),bv=fs_file_version(right);
    for(uint64_t off=0;off<(uint64_t)a;){
        size_t count=(uint64_t)a-off>65536?65536:(size_t)((uint64_t)a-off),an=0,bn=0;
        int am=0,bm=0;char *ap=fs_read_file_slice(left,off,count,&an,&am),*bp=fs_read_file_slice(right,off,count,&bn,&bm);
        int result=(!ap||!bp||an!=count||bn!=count)?-1:memcmp(ap,bp,count)?0:1;
        free(ap);free(bp);if(result!=1)return result;off+=count;
    }
    return av==fs_file_version(left)&&bv==fs_file_version(right)?1:-1;
}
