/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
/* Inspect snapshots under the production mutex without I2C or stdout work. */
#include "../app/lcd.c"

static void *publisher(void *argument)
{
    struct lcd_display *display = argument;
    for (unsigned int i = 0; i < 10000; ++i)
    {
        unsigned int level = i % 6;
        lcd_publish_environment(display, true, level, level, false, true, true,
                                (int32_t)(level * 1000));
    }
    return NULL;
}
static void *observer(void *argument)
{
    struct lcd_display *display = argument;
    for (unsigned int i = 0; i < 10000; ++i)
    {
        pthread_mutex_lock(&display->lock);
        assert(display->level == display->leds);
        assert(display->temperature_mc == (int32_t)(display->level * 1000));
        pthread_mutex_unlock(&display->lock);
        assert(lcd_error(display) == 0);
    }
    return NULL;
}
int main(void)
{
    struct lcd_display display = {0};
    assert(pthread_mutex_init(&display.lock, NULL) == 0);
    assert(pthread_cond_init(&display.changed, NULL) == 0);
    pthread_t threads[4];
    for (unsigned int i = 0; i < 3; ++i)
        assert(pthread_create(&threads[i], NULL, publisher, &display) == 0);
    assert(pthread_create(&threads[3], NULL, observer, &display) == 0);
    for (unsigned int i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    pthread_mutex_lock(&display.lock);
    display.stopping = true;
    unsigned int level = display.level;
    pthread_mutex_unlock(&display.lock);
    lcd_publish(&display, true, 5, 8);
    assert(display.level == level);
    pthread_cond_destroy(&display.changed);
    pthread_mutex_destroy(&display.lock);
    puts("PASS LCD synchronization: 30000 publishes and concurrent snapshots, coherent fields, stop rejects updates");
    return 0;
}
