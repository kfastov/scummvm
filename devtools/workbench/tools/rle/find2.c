// Поиск начала пиксельных данных: сначала дешёвый фильтр по соотношению
// «выход/вход» на первых байтах, потом полная проба.
#include <stdio.h>
#include <stdlib.h>
static unsigned char *buf; static long bufLen;
static void probe(long s,unsigned raw,unsigned lim,unsigned*o,unsigned*u){
  unsigned out=0,in=0;
  while(out<raw&&in<lim&&s+in<bufLen){unsigned char c=buf[s+in++];unsigned n;
    if(c<=0xf5){in++;n=c+3;}else{n=c-0xf5;in+=n;}
    out+=n;}
  *o=out;*u=in;}
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb");fseek(f,0,SEEK_END);bufLen=ftell(f);fseek(f,0,SEEK_SET);
  buf=malloc(bufLen); if(fread(buf,1,bufLen,f)!=(size_t)bufLen)return 1;fclose(f);
  long dib=strtol(argv[2],NULL,0); unsigned raw=strtoul(argv[3],NULL,0),comp=strtoul(argv[4],NULL,0);
  long span=argc>5?strtol(argv[5],NULL,0):0x200000;
  double ratio=(double)raw/comp;
  long from=dib-span; if(from<0)from=0; long to=dib+span; if(to>bufLen)to=bufLen;
  int hits=0;
  for(long s=from;s<to;s++){
    // дешёвый фильтр: 1500 входных байт должны дать примерно ratio*1500 выходных
    unsigned o,u; probe(s,(unsigned)(ratio*1500),1500,&o,&u);
    if(u<1400) continue;
    double r=(double)o/u; if(r<ratio*0.90||r>ratio*1.10) continue;
    probe(s,raw,comp+512,&o,&u);
    if(o!=raw) continue;
    int d=(int)u-(int)comp; if(d<0)d=-d; if(d>32) continue;
    if(hits<12) printf("данные @%#lx (dib%+ld) вход %u (%+d)\n",s,s-dib,u,(int)u-(int)comp);
    hits++;
  }
  printf("совпадений: %d\n",hits);
  return 0;}
