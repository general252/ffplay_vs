#pragma once

typedef struct VideoState VideoState;

int play(VideoState** video, const char* filename);
void event_loop(VideoState *is);

void toggle_full_screen(VideoState *is);
void toggle_pause(VideoState *is);
void toggle_mute(VideoState* is); // �Ƿ���

// update_volume(is, +1, P_SDL_VOLUME_STEP); // ��������
// update_volume(is, -1, P_SDL_VOLUME_STEP); // ������С
void update_volume(VideoState *is, int sign, double step);

void step_to_next_frame(VideoState *is);

void stream_seek_percent(VideoState *is, double percent);

void toggle_next_show_mode(VideoState *is);

void show_paly_help();