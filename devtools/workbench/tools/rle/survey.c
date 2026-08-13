// Сколько полноэкранных фонов разворачивается прямо из-за палитры.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *buf; static long bufLen;
static unsigned rd32(long p){return buf[p]|(buf[p+1]<<8)|((unsigned)buf[p+2]<<16)|((unsigned)buf[p+3]<<24);}
static void probe(long s,unsigned raw,unsigned stride,unsigned lim,unsigned*o,unsigned*u){
  unsigned out=0,in=0,col=0;
  while(out<raw&&in<lim&&s+in<bufLen){unsigned char c=buf[s+in++];unsigned n;
    if(c<=0xf5){if(s+in>=bufLen)break;in++;n=c+3;}else{n=c-0xf5;in+=n;}
    out+=n;col+=n; if(col>=stride)col-=stride;}
  *o=out;*u=in;}
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);bufLen=ftell(f);fseek(f,0,SEEK_SET);
  buf=malloc(bufLen); if(fread(buf,1,bufLen,f)!=(size_t)bufLen)return 1;fclose(f);
  int total=0,ok=0,nopair=0;
  for(long i=0;i+40<bufLen;i++){
    if(!(buf[i]==0x28&&buf[i+1]==0&&buf[i+2]==0&&buf[i+3]==0))continue;
    if(rd32(i+4)!=640||rd32(i+8)!=480)continue;
    if(buf[i+12]!=1||buf[i+14]!=8)continue;
    total++;
    unsigned raw=640*480,comp=0;
    for(int back=8;back<=0x140&&i-back>=0;back++)
      if(rd32(i-back)==raw){unsigned c2=rd32(i-back+4); if(c2>0&&c2<=raw*2){comp=c2;break;}}
    if(!comp){nopair++;continue;}
    long pal=i+40+256*4; unsigned o,u; probe(pal,raw,640,comp+512,&o,&u);
    int d=(int)u-(int)comp; if(d<0)d=-d;
    int good = (o==raw && d<=512);
    if(good)ok++;
    printf("%#09lx comp=%-7u выход=%-7u вход=%-7u %s\n",i,comp,o,u,good?"годен":"нет");
  }
  printf("полноэкранных %d, без пары размеров %d, разворачивается из-за палитры %d\n",total,nopair,ok);
  return 0;}
