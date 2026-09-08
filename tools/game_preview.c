#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
void game_selftest(void);
static void output(game_t *g,const char *path,int64_t now){
    static uint16_t storage[240*320+2];storage[0]=0x1234;storage[240*320+1]=0x5678;
    uint16_t *pixels=storage+1;game_render(g,now,pixels);assert(storage[0]==0x1234&&storage[240*320+1]==0x5678);
    FILE *f=fopen(path,"wb");if(!f)exit(2);fprintf(f,"P6\n240 320\n255\n");
    for(int i=0;i<240*320;i++){unsigned c=(pixels[i]>>8)|(pixels[i]<<8);unsigned char rgb[3]={((c>>11)&31)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};fwrite(rgb,1,3,f);}fclose(f);
}
int main(void){
    game_selftest();game_t g;game_init(&g);g.sensor_ok=g.audio_ok=g.music_ok=true;
    output(&g,"build/game_idle.ppm",1000);
    game_bump(&g,1000);game_tick(&g,1140);output(&g,"build/game_bump.ppm",1140);
    game_tick(&g,1550);output(&g,"build/game_flight.ppm",1550);
    game_tick(&g,2160);output(&g,"build/game_basket.ppm",2160);return 0;
}
