#pragma once

typedef struct VideoState VideoState;

int play(VideoState** video, const char* filename);
void event_loop(VideoState *is);

void show_paly_help();