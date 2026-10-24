#define _POSIX_C_SOURCE 200809L

#include "game.h"

#include <string.h>

/* Взаимодействие потоков через мьютекс и условные переменные
 * вместо семафоров (дополнительное требование задания). */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_turn[MAX_CARS];
static int token_turn[MAX_CARS];
static pthread_cond_t cv_done;
static int token_done;
static pthread_cond_t cv_log;
static int token_log;
static int n_cars_sync;
static int inited;

int sync_init(Game *g)
{
    int i;
    n_cars_sync = g->n_cars;
    memset(token_turn, 0, sizeof(token_turn));
    token_done = 0;
    token_log = 0;
    for (i = 0; i < g->n_cars; i++)
        pthread_cond_init(&cv_turn[i], NULL);
    pthread_cond_init(&cv_done, NULL);
    pthread_cond_init(&cv_log, NULL);
    inited = 1;
    return 0;
}

void sync_destroy(Game *g)
{
    int i;
    (void)g;
    if (!inited)
        return;
    for (i = 0; i < n_cars_sync; i++)
        pthread_cond_destroy(&cv_turn[i]);
    pthread_cond_destroy(&cv_done);
    pthread_cond_destroy(&cv_log);
    inited = 0;
}

void car_turn_wait(int car_id)
{
    pthread_mutex_lock(&mu);
    while (!token_turn[car_id] && !(g_game && g_game->shutdown))
        pthread_cond_wait(&cv_turn[car_id], &mu);
    token_turn[car_id] = 0;
    pthread_mutex_unlock(&mu);
}

void car_turn_post(int car_id)
{
    pthread_mutex_lock(&mu);
    token_turn[car_id] = 1;
    pthread_cond_signal(&cv_turn[car_id]);
    pthread_mutex_unlock(&mu);
}

void done_wait(void)
{
    pthread_mutex_lock(&mu);
    while (!token_done && !(g_game && g_game->shutdown))
        pthread_cond_wait(&cv_done, &mu);
    token_done = 0;
    pthread_mutex_unlock(&mu);
}

void done_post(void)
{
    pthread_mutex_lock(&mu);
    token_done = 1;
    pthread_cond_signal(&cv_done);
    pthread_mutex_unlock(&mu);
}

void log_wait_item(Game *g)
{
    pthread_mutex_lock(&mu);
    while (!token_log && !g->shutdown)
        pthread_cond_wait(&cv_log, &mu);
    if (token_log > 0)
        token_log--;
    pthread_mutex_unlock(&mu);
}

void log_post_item(void)
{
    pthread_mutex_lock(&mu);
    token_log++;
    pthread_cond_signal(&cv_log);
    pthread_mutex_unlock(&mu);
}

void wake_all_for_shutdown(Game *g)
{
    int i;
    pthread_mutex_lock(&mu);
    for (i = 0; i < g->n_cars; i++) {
        token_turn[i] = 1;
        pthread_cond_signal(&cv_turn[i]);
    }
    token_done = 1;
    pthread_cond_signal(&cv_done);
    token_log++;
    pthread_cond_broadcast(&cv_log);
    pthread_mutex_unlock(&mu);
}
