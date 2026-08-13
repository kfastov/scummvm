// Распаковать картинку из книги с заданного места и записать BMP.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *buf; static long bufLen; static int RUNADD=3; static int TRUNC=1;
int main(int argc,char**argv){
  if(argc<8){fprintf(stderr,"dump <книга> <dib> <data> <w> <h> <comp> <out.bmp>\n");return 2;}
  FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);bufLen=ftell(f);fseek(f,0,SEEK_SET);
  buf=malloc(bufLen); if(fread(buf,1,bufLen,f)!=(size_t)bufLen)return 1; fclose(f);
  long dib=strtol(argv[2],NULL,0), data=strtol(argv[3],NULL,0);
  int w=atoi(argv[4]),h=atoi(argv[5]); unsigned comp=strtoul(argv[6],NULL,0);
  if(getenv("RUNADD"))RUNADD=atoi(getenv("RUNADD"));
  if(getenv("TRUNC"))TRUNC=atoi(getenv("TRUNC"));
  unsigned stride=((w*8+31)/32)*4, raw=stride*h;
  unsigned char*px=calloc(raw,1);
  unsigned out=0,in=0,col=0;
  while(out<raw && in<comp+512 && data+in<bufLen){
    unsigned char c=buf[data+in++]; unsigned n;
    if(c<=0xf5){ unsigned char v=buf[data+in++]; n=c+RUNADD; if(TRUNC&&n>stride-col)n=stride-col;
      if(out+n>raw)n=raw-out; memset(px+out,v,n); }
    else { unsigned k=c-0xf5; n=k; if(TRUNC&&n>stride-col)n=stride-col; if(out+n>raw)n=raw-out;
      memcpy(px+out,buf+data+in,n); in+=k; }
    out+=n; col+=n; if(col>=stride)col=0;
  }
  FILE*o=fopen(argv[7],"wb");
  unsigned off=14+40+1024, sz=off+raw;
  fwrite("BM",1,2,o); fwrite(&sz,4,1,o); unsigned z=0; fwrite(&z,4,1,o); fwrite(&off,4,1,o);
  fwrite(buf+dib,1,40+1024,o); fwrite(px,1,raw,o); fclose(o);
  fprintf(stderr,"выход %u из %u, вход %u из %u\n",out,raw,in,comp);
  return 0;
}
