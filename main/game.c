#include "game.h"
#include <math.h>
#include <string.h>
void game_init(game_t *g) {
    memset(g,0,sizeof(*g));g->volume=55;g->motion_ready=true;
    g->shot_at=g->feedback_at=-10000;
    g->ball_x=BALL_START_X;g->ball_y=BALL_START_Y;
}
void game_restart(game_t *g,int64_t now) {
    g->score=g->baskets=0;g->flying=g->scored=false;
    g->shot_at=g->feedback_at=-10000;g->last_tick=now;
    g->ball_x=BALL_START_X;g->ball_y=BALL_START_Y;
    g->motion_ready=false;g->last_motion=now;
}
bool game_motion(game_t *g,const float a[3],int64_t now) {
    if(!g->sample_ready){memcpy(g->filtered,a,sizeof(g->filtered));g->sample_ready=true;return false;}
    float energy=0;
    for(int i=0;i<3;i++) {g->filtered[i]+=0.18f*(a[i]-g->filtered[i]);float d=a[i]-g->filtered[i];energy+=d*d;}
    g->energy=sqrtf(energy);
    if(g->energy<100)g->motion_ready=true;
    if(g->motion_ready&&g->energy>230&&now-g->last_motion>=450){
        g->motion_ready=false;g->last_motion=now;return game_bump(g,now);
    }
    return false;
}
bool game_bump(game_t *g,int64_t now) {
    if(g->flying)return false;
    g->flying=true;g->scored=false;g->shot_at=now;
    g->ball_x=BALL_START_X;g->ball_y=BALL_START_Y;
    return true;
}
void game_tick(game_t *g,int64_t now) {
    g->last_tick=now;
    if(!g->flying)return;
    int64_t elapsed=now-g->shot_at;
    if(elapsed<0)return;
    float t=elapsed/1000.f,T=SHOT_FLIGHT_MS/1000.f;
    const float gravity=300.f;
    const float vx=(HOOP_X-BALL_START_X)/T;
    const float vy=(HOOP_Y-BALL_START_Y-0.5f*gravity*T*T)/T;
    if(elapsed<SHOT_FLIGHT_MS){
        g->ball_x=BALL_START_X+vx*t;
        g->ball_y=BALL_START_Y+vy*t+0.5f*gravity*t*t;
    } else {
        if(!g->scored){g->scored=true;g->baskets++;g->score+=2;g->feedback_at=now;if(g->score>g->high_score)g->high_score=g->score;}
        float drop=(elapsed-SHOT_FLIGHT_MS)/1000.f;
        g->ball_x=HOOP_X;g->ball_y=HOOP_Y+125*drop+150*drop*drop;
    }
    if(elapsed>=SHOT_CYCLE_MS){g->flying=false;g->ball_x=BALL_START_X;g->ball_y=BALL_START_Y;}
}
