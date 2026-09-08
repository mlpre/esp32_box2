#include "game.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
void game_selftest(void) {
    game_t g;game_init(&g);
    assert(g.baskets==0&&!g.flying);assert(game_bump(&g,1000));assert(!game_bump(&g,1020));
    game_tick(&g,1500);assert(g.ball_y<BALL_START_Y&&g.ball_x>BALL_START_X&&g.score==0);
    game_tick(&g,2100);assert(g.baskets==1&&g.score==2&&fabsf(g.ball_x-HOOP_X)<0.01f&&fabsf(g.ball_y-HOOP_Y)<0.01f);
    game_tick(&g,2120);assert(g.score==2); // Exactly one score per shot.
    game_tick(&g,2650);assert(!g.flying&&g.ball_x==BALL_START_X);
    assert(game_bump(&g,3000));game_tick(&g,9000);assert(g.score==4&&!g.flying); // Late frame still scores only once.
    game_restart(&g,10000);assert(g.score==0&&g.high_score==4&&!g.flying);
    const float flat[3]={0,0,1000},shake[3]={0,0,1450};
    game_init(&g);assert(!game_motion(&g,flat,0));
    for(int i=1;i<60;i++)assert(!game_motion(&g,flat,i*20)); // No calibration and no idle false hits.
    assert(game_motion(&g,shake,1500));assert(!game_motion(&g,shake,1520));
    for(int i=1;i<80;i++){game_motion(&g,flat,1520+i*20);game_tick(&g,1520+i*20);}
    game_tick(&g,3200);assert(!g.flying);assert(game_motion(&g,shake,3300));
    game_init(&g);const float sideways[3]={1000,0,0},side_bump[3]={1450,0,0};
    assert(!game_motion(&g,sideways,0));assert(game_motion(&g,side_bump,1000)); // Any grip works.
    puts("BUMP SELFTEST PASS: arc, basket crossing, single scoring, rearm, any grip, restart");
}
