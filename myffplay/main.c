#include "play.h"

#include <SDL.h>
#include <SDL_thread.h>
#define SDL_main main

#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "avdevice.lib")
#pragma comment(lib, "avfilter.lib")
#pragma comment(lib, "postproc.lib")
#pragma comment(lib, "swresample.lib")
#pragma comment(lib, "swscale.lib")

#pragma comment(lib, "SDL2.lib")

int main(int argc, char *argv[])
{
    play("../xibushijie.mp4");

    return 0;
}