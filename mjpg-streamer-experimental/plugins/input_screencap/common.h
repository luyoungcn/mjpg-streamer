#include <unistd.h>
#include <inttypes.h>


uint64_t getTimeNanos()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_nsec + ((uint64_t)t.tv_sec * 1000 * 1000 * 1000);
}

uint64_t getTimeMillis()
{
    return getTimeNanos() / 1000000;
}


static int fps_value = 0;
static int frameCount = 0;
static uint64_t lastTime = 0;

int fps()
{
    uint64_t curTime = getTimeMillis();
    ++frameCount;
    if (curTime - lastTime > 1000) // 取固定时间间隔为1秒
    {
        fps_value = frameCount - 1;
        frameCount = 0;
        lastTime = curTime;
        printf("-------------------------------------\n");
        printf("#performance# fps: %d\n", fps_value);
    }
    return fps_value;
}
