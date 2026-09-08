#include "game.h"
#include "game_font.h"
#include "game_sprites.h"
#include <stdio.h>
#include <stdlib.h>
static uint16_t *fb;
static uint16_t color(unsigned rgb) {unsigned v=((rgb>>8)&0xf800)|((rgb>>5)&0x7e0)|((rgb>>3)&31);return (v>>8)|(v<<8);}
static void rect(int x,int y,int w,int h,unsigned c) {
    int x1=x+w,y1=y+h;if(x<0)x=0;if(y<0)y=0;if(x1>240)x1=240;if(y1>320)y1=320;
    uint16_t p=color(c);for(int j=y;j<y1;j++)for(int i=x;i<x1;i++)fb[j*240+i]=p;
}
static void ellipse(int x,int y,int rx,int ry,unsigned c) {
    for(int j=-ry;j<=ry;j++)for(int i=-rx;i<=rx;i++)
        if(i*i*ry*ry+j*j*rx*rx<=rx*rx*ry*ry)rect(x+i,y+j,1,1,c);
}
static void line(int x,int y,int x2,int y2,int width,unsigned c) {
    int dx=abs(x2-x),sx=x<x2?1:-1,dy=-abs(y2-y),sy=y<y2?1:-1,err=dx+dy;
    for(;;){rect(x-width/2,y-width/2,width,width,c);if(x==x2&&y==y2)break;int e=2*err;if(e>=dy){err+=dy;x+=sx;}if(e<=dx){err+=dx;y+=sy;}}
}
static unsigned utf8(const char **s) {
    const unsigned char *p=(const unsigned char*)*s;unsigned c=*p++;
    if(c>=0xe0){c=((c&15)<<12)|((p[0]&63)<<6)|(p[1]&63);p+=2;}
    else if(c>=0xc0){c=((c&31)<<6)|(p[0]&63);p++;}
    *s=(const char*)p;return c;
}
static int text_width(const char *s,int scale){int w=0;while(*s)w+=(utf8(&s)<128?8:16)*scale;return w;}
static void text(int x,int y,const char *s,int scale,unsigned c) {
    while(*s){unsigned cp=utf8(&s);const uint16_t *rows=NULL;
        for(unsigned k=0;k<FONT_COUNT;k++)if(game_font[k].code==cp){rows=game_font[k].rows;break;}
        if(rows)for(int j=0;j<16;j++)for(int i=0;i<16;i++)if(rows[j]&(1u<<i))rect(x+i*scale,y+j*scale,scale,scale,c);
        x+=(cp<128?8:16)*scale;
    }
}
static void center(int y,const char *s,int scale,unsigned c){text((240-text_width(s,scale))/2,y,s,scale,c);}
#define INK 0x222630
#define ORANGE 0xf48435
#define WHITE 0xf8f7f2
static void sprite(const uint32_t *pixels,int x,int y,int w,int h) {
    for(int j=0;j<h;j++)for(int i=0;i<w;i++){
        int dx=x+i,dy=y+j;if(dx<0||dx>=240||dy<0||dy>=320)continue;
        unsigned p=pixels[(j*120/h)*80+i*80/w],a=p>>24;if(!a)continue;
        if(a>=250){fb[dy*240+dx]=color(p);continue;}
        unsigned old=fb[dy*240+dx];old=((old>>8)|(old<<8))&65535;
        unsigned r=((p>>16)&255)*a+((old>>11)&31)*255/31*(255-a);
        unsigned gr=((p>>8)&255)*a+((old>>5)&63)*255/63*(255-a);
        unsigned b=(p&255)*a+(old&31)*255/31*(255-a);
        fb[dy*240+dx]=color(((r/255)<<16)|((gr/255)<<8)|(b/255));
    }
}
static void ball(int x,int y,int r) {
    ellipse(x+1,y+2,r,r,0x845139);ellipse(x,y,r,r,0xf58a36);ellipse(x-3,y-4,r-5,r-6,0xffa957);
    line(x-r+2,y,x+r-2,y,1,0x5d3a2b);line(x,y-r+1,x,y+r-1,1,0x5d3a2b);
    line(x-r/2,y-r+2,x-r/3,y,1,0x5d3a2b);line(x-r/3,y,x-r/2,y+r-2,1,0x5d3a2b);
    line(x+r/2,y-r+2,x+r/3,y,1,0x5d3a2b);line(x+r/3,y,x+r/2,y+r-2,1,0x5d3a2b);
}
static void net(int64_t age) {
    int sway=age>=0&&age<350?(int)((350-age)/70)%3-1:0;
    for(int i=0;i<5;i++) {
        line(169+i*10,103,178+i*6+sway,126,1,0xe9ecee);
        if(i<4)line(169+i*10,103,184+i*6+sway,126,1,0xf9faf9);
    }
    line(173,111,207,111,1,0xd5dadf);line(176,119,204,119,1,0xf9faf9);line(178,126,202,126,1,0xf9faf9);
}
void game_render(const game_t *g,int64_t now,uint16_t *pixels) {
    fb=pixels;
    // Neutral grey practice studio, like the original basketball clip.
    rect(0,0,240,320,0xadb1b4);
    for(int y=46;y<248;y++){int v=164+(y-46)/6;rect(0,y,240,1,(v<<16)|(v<<8)|v);}
    line(12,47,12,246,1,0xc9ccce);line(86,47,86,246,1,0xbfc3c6);line(157,47,157,246,1,0xc9ccce);
    rect(0,242,240,39,0x92969a);line(0,243,240,243,2,0x73797e);
    line(0,278,70,244,1,0xdadbd8);line(240,277,192,244,1,0xdadbd8);
    ellipse(124,262,87,14,0xc5c7c5);ellipse(124,262,84,12,0x92969a);
    ellipse(93,266,54,6,0x656d74);
    // Hoop is well above the player's back, leaving the entire trajectory visible.
    rect(218,66,7,167,0x626a72);rect(157,61,76,43,0x69727c);rect(160,64,70,37,0xf2f2ec);
    rect(178,79,27,21,0xd47a4c);rect(181,82,21,17,0xf2f2ec);
    line(164,103,216,103,4,0x954921);net(now-g->feedback_at);
    int64_t age=now-g->shot_at;
    bool bump=g->flying&&age<380;
    int lift=bump?(age<140?(int)age/18:(int)(380-age)/30):0;
    // The original two back-facing cutouts are used unchanged, scaled at draw time.
    const uint32_t *frame=bump?back_left:back_right;
    int lean=bump?10:0;
    sprite(frame,31+lean,96-lift,120,180);
    if(bump){line(25,166,47,160,2,WHITE);line(24,177,45,173,2,WHITE);}
    if(g->flying&&age<SHOT_FLIGHT_MS) {
        for(int k=1;k<=3;k++){int x=(int)g->ball_x-k*7,y=(int)g->ball_y+2*k;ellipse(x,y,2,2,0xdfd7c9);}
    }
    ball((int)g->ball_x,(int)g->ball_y,12);
    // Front lip occludes the ball naturally while it passes through the basket.
    if(g->flying&&age>=SHOT_FLIGHT_MS&&age<SHOT_FLIGHT_MS+150)net(now-g->feedback_at);
    line(166,104,214,104,3,ORANGE);
    rect(0,0,240,46,0x191d27);text(9,15,"只因你太美",1,0xffc76c);
    char s[80];snprintf(s,sizeof(s),"%03d",g->score);text(177,6,s,2,WHITE);
    snprintf(s,sizeof(s),"进球 %d",g->baskets);text(169,30,s,1,0xc4c7cf);
    if(now-g->feedback_at<900){
        rect(8,67,136,25,0x28342e);text(14,71,g->baskets%5==0?"漂亮！你太美":"好球！+2",1,0x9aefb0);
        int step=(int)((now-g->feedback_at)/30);
        for(int k=0;k<8;k++){int x=171+(k*13)%53,y=129+step+(k%3)*7;rect(x,y,2,4,k%2?WHITE:ORANGE);}
    } else if(!g->flying) {text(14,65,"练习时长两年半",1,0x515760);}
    rect(0,282,240,38,0x191d27);
    center(285,g->flying?"好球，再来一个！":"轻晃一下 · 顶球进篮",1,0xffce7d);
    snprintf(s,sizeof(s),"M 顶球  Q 重来  R 音量%d",g->volume);center(304,s,1,0xc3c6cf);
    if(!g->sensor_ok){rect(8,68,136,23,0x28313c);text(14,72,"按 M 也能顶球",1,WHITE);}
}
