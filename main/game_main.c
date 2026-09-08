#include "game.h"
#include "box2.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <assert.h>
#include <math.h>
#include <string.h>
static const char *TAG="kun_game";
static game_t state;
static SemaphoreHandle_t state_mutex;
static nvs_handle_t save;
static bool save_ok;
extern const uint8_t bgm_start[] asm("_binary_bgm_pcm_start");
extern const uint8_t bgm_end[] asm("_binary_bgm_pcm_end");
extern const uint8_t voice_start[] asm("_binary_voice_pcm_start");
extern const uint8_t voice_end[] asm("_binary_voice_pcm_end");
static int64_t millis(void){return esp_timer_get_time()/1000;}
static game_t snapshot(void){game_t s;xSemaphoreTake(state_mutex,portMAX_DELAY);s=state;xSemaphoreGive(state_mutex);return s;}
void game_selftest(void);
static void display_task(void *arg) {
    (void)arg;
    uint16_t *pixels=heap_caps_malloc(240*320*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);assert(pixels);
    TickType_t wake=xTaskGetTickCount();
    for(;;){game_t s=snapshot();game_render(&s,millis(),pixels);ESP_ERROR_CHECK(box2_lcd_draw_bitmap(0,0,240,320,pixels));vTaskDelayUntil(&wake,pdMS_TO_TICKS(33));}
}
static int pcm_sample(const uint8_t *bytes,size_t index){return (int16_t)((unsigned)bytes[index*2]|((unsigned)bytes[index*2+1]<<8));}
static void audio_task(void *arg) {
    (void)arg;
    int16_t pcm[240];uint32_t noise=0x12345678;int volume=-1;
    const size_t music_length=(bgm_end-bgm_start)/2,voice_length=(voice_end-voice_start)/2;
    assert(music_length>0&&voice_length>0);
    size_t music_at=0,voice_at=voice_length;int last_baskets=0;
    int64_t seen_feedback=-10000,seen_shot=-10000;int swish_left=0,bump_left=0;
    for(;;){
        game_t s=snapshot();
        if(volume!=s.volume){if(box2_audio_set_output_volume(s.volume)==ESP_OK)volume=s.volume;}
        if(s.feedback_at!=seen_feedback){seen_feedback=s.feedback_at;if(s.feedback_at>=0)swish_left=3600;}
        if(s.shot_at!=seen_shot){seen_shot=s.shot_at;if(s.shot_at>=0)bump_left=1800;}
        if(s.baskets!=last_baskets){if(s.baskets>0&&s.baskets%5==0)voice_at=0;last_baskets=s.baskets;}
        for(int i=0;i<240;i++) {
            int sample=pcm_sample(bgm_start,music_at++);if(music_at>=music_length)music_at=0;
            if(voice_at<voice_length)sample=sample/4+pcm_sample(voice_start,voice_at++)*3/4;
            else sample=sample*3/4;
            noise^=noise<<13;noise^=noise>>17;noise^=noise<<5;
            if(swish_left>0){sample+=(int16_t)(noise&65535)*swish_left/36000;swish_left--;}
            if(bump_left>0){sample+=(int)(sinf((1800-bump_left)*6.2831853f*95/24000.f)*1800.f*bump_left/1800.f);bump_left--;}
            if(sample>32767)sample=32767;
            if(sample<-32768)sample=-32768;
            pcm[i]=(int16_t)sample;
        }
        esp_err_t err=box2_audio_write(pcm,240);
        if(err!=ESP_OK){ESP_LOGE(TAG,"Audio stopped: %s",esp_err_to_name(err));xSemaphoreTake(state_mutex,portMAX_DELAY);state.audio_ok=false;xSemaphoreGive(state_mutex);vTaskDelete(NULL);}
    }
}
void app_main(void) {
    game_selftest();game_init(&state);
    if(nvs_flash_init()==ESP_OK&&nvs_open("kun_basket",NVS_READWRITE,&save)==ESP_OK){
        save_ok=true;int32_t v=0;if(nvs_get_i32(save,"record",&v)==ESP_OK&&v>=0)state.high_score=v;
    }
    ESP_ERROR_CHECK(box2_board_init());
    state.sensor_ok=box2_motion_init(box2_board_i2c_bus())==ESP_OK;
    state.audio_ok=box2_audio_init(box2_board_i2c_bus())==ESP_OK;
    state.music_ok=bgm_end-bgm_start>48000;
    ESP_ERROR_CHECK(box2_lcd_init());
    state_mutex=xSemaphoreCreateMutex();assert(state_mutex);
    assert(xTaskCreate(display_task,"game_display",6144,NULL,3,NULL)==pdPASS);
    if(state.audio_ok)assert(xTaskCreate(audio_task,"game_audio",4096,NULL,5,NULL)==pdPASS);
    vTaskPrioritySet(NULL,4);
    ESP_LOGI(TAG,"BACK-BUMP GAME READY | sensor=%d audio=%d music=%.2fs voice=%.2fs | shake/M=bump Q=reset R=volume",state.sensor_ok,state.audio_ok,(bgm_end-bgm_start)/48000.0,(voice_end-voice_start)/48000.0);
    TickType_t wake=xTaskGetTickCount();unsigned raw_before=0,stable=0;
    int64_t changed_at=0,last_log=0;int failures=0,saved_record=state.high_score;
    for(;;){
        int64_t now=millis();box2_board_state_t board={0};unsigned raw=stable;
        if(box2_board_read_state(&board,false)==ESP_OK)raw=(board.left_pressed?1:0)|(board.q_pressed?2:0)|(board.middle_pressed?4:0)|(board.right_pressed?8:0);
        if(raw!=raw_before){raw_before=raw;changed_at=now;}
        unsigned pressed=0;
        if(now-changed_at>=40){pressed=raw&~stable;stable=raw;}
        box2_motion_state_t m={0};bool valid=false;
        if(state.sensor_ok){valid=box2_motion_read(&m)==ESP_OK;if(valid)failures=0;else failures++;}
        bool bumped=false,score_changed=false;int record;
        xSemaphoreTake(state_mutex,portMAX_DELAY);
        if(failures>=10&&state.sensor_ok){state.sensor_ok=false;ESP_LOGE(TAG,"Sensor unavailable; M still works");}
        if(pressed&8)state.volume=state.volume==0?35:state.volume==35?55:state.volume==55?75:0;
        if(pressed&2)game_restart(&state,now);
        else {
            if(valid){float a[3]={m.x_mg,m.y_mg,m.z_mg};bumped=game_motion(&state,a,now);}
            if(pressed&5)bumped=game_bump(&state,now)||bumped;
        }
        int previous_baskets=state.baskets;game_tick(&state,now);score_changed=state.baskets!=previous_baskets;record=state.high_score;
        if(bumped)ESP_LOGI(TAG,"BUMP: energy=%.0f",state.energy);
        if(score_changed)ESP_LOGI(TAG,"BASKET! count=%d score=%d",state.baskets,state.score);
        if(now-last_log>=5000){ESP_LOGI(TAG,"running sensor=%d audio=%d xyz=%d,%d,%d baskets=%d heap=%u",state.sensor_ok,state.audio_ok,m.x_mg,m.y_mg,m.z_mg,state.baskets,(unsigned)esp_get_free_heap_size());last_log=now;}
        bool save_now=save_ok&&!state.flying&&record>saved_record;
        xSemaphoreGive(state_mutex);
        // Persist after the ball animation; flash writes never stall the ball in flight.
        if(save_now){esp_err_t e=nvs_set_i32(save,"record",record);if(e==ESP_OK)e=nvs_commit(save);if(e==ESP_OK)saved_record=record;else ESP_LOGW(TAG,"Record save failed: %s",esp_err_to_name(e));}
        vTaskDelayUntil(&wake,pdMS_TO_TICKS(20));
    }
}
