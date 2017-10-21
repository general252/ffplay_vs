#include "play.h"
#include <stdio.h>

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
    struct VideoState* is = NULL;

    int times = 0;
    play(&is, "http://live.hkstv.hk.lxdns.com/live/hks/playlist.m3u8");

    while (1) {
        event_loop(is);

        if (100 == times++) {
            stream_seek_percent(is, 0.5);
        }
    }

    return 0;
}