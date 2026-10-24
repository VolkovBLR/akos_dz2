#define _POSIX_C_SOURCE 200809L

#include "game.h"

#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* POSIX-семафоры: право хода каждой машины, сигнал «ход выполнен»,
 * уведомление потока журнала. На Linux — unnamed sem_init; на macOS
 * unnamed семафоры не реализованы, поэтому используем named sem_open. */

#ifdef __APPLE__
static sem_t *sem_turn[MAX_CARS];
static sem_t *sem_done;
static sem_t *sem_log;
static char name_turn[MAX_CARS][64];
static char name_done[64];
static char name_log[64];
#else
static sem_t sem_turn[MAX_CARS];
static sem_t sem_done;
static sem_t sem_log;
#endif

static int n_cars_sync;

#ifdef __APPLE__
static void make_name(char *dst, size_t cap, const char *tag, int id)
{
    snprintf(dst, cap, "/akos_dz2_%d_%s_%d", (int)getpid(), tag, id);
}
#endif

int sync_init(Game *g)
{
    int i;
    n_cars_sync = g->n_cars;
#ifdef __APPLE__
    for (i = 0; i < g->n_cars; i++) {
        make_name(name_turn[i], sizeof(name_turn[i]), "t", i);
        sem_unlink(name_turn[i]);
        sem_turn[i] = sem_open(name_turn[i], O_CREAT | O_EXCL, 0600, 0);
        if (sem_turn[i] == SEM_FAILED)
            return -1;
    }
    make_name(name_done, sizeof(name_done), "d", 0);
    make_name(name_log, sizeof(name_log), "l", 0);
    sem_unlink(name_done);
    sem_unlink(name_log);
    sem_done = sem_open(name_done, O_CREAT | O_EXCL, 0600, 0);
    sem_log = sem_open(name_log, O_CREAT | O_EXCL, 0600, 0);
    if (sem_done == SEM_FAILED || sem_log == SEM_FAILED)
        return -1;
#else
    for (i = 0; i < g->n_cars; i++) {
        if (sem_init(&sem_turn[i], 0, 0) != 0)
            return -1;
    }
    if (sem_init(&sem_done, 0, 0) != 0)
        return -1;
    if (sem_init(&sem_log, 0, 0) != 0)
        return -1;
#endif
    return 0;
}

void sync_destroy(Game *g)
{
    int i;
#ifdef __APPLE__
    for (i = 0; i < n_cars_sync; i++) {
        if (sem_turn[i] && sem_turn[i] != SEM_FAILED) {
            sem_close(sem_turn[i]);
            sem_unlink(name_turn[i]);
        }
        sem_turn[i] = NULL;
    }
    if (sem_done && sem_done != SEM_FAILED) {
        sem_close(sem_done);
        sem_unlink(name_done);
    }
    if (sem_log && sem_log != SEM_FAILED) {
        sem_close(sem_log);
        sem_unlink(name_log);
    }
    sem_done = NULL;
    sem_log = NULL;
#else
    for (i = 0; i < n_cars_sync; i++)
        sem_destroy(&sem_turn[i]);
    sem_destroy(&sem_done);
    sem_destroy(&sem_log);
#endif
    (void)g;
}

void car_turn_wait(int car_id)
{
#ifdef __APPLE__
    sem_wait(sem_turn[car_id]);
#else
    sem_wait(&sem_turn[car_id]);
#endif
}

void car_turn_post(int car_id)
{
#ifdef __APPLE__
    sem_post(sem_turn[car_id]);
#else
    sem_post(&sem_turn[car_id]);
#endif
}

void done_wait(void)
{
#ifdef __APPLE__
    sem_wait(sem_done);
#else
    sem_wait(&sem_done);
#endif
}

void done_post(void)
{
#ifdef __APPLE__
    sem_post(sem_done);
#else
    sem_post(&sem_done);
#endif
}

void log_wait_item(Game *g)
{
    (void)g;
#ifdef __APPLE__
    sem_wait(sem_log);
#else
    sem_wait(&sem_log);
#endif
}

void log_post_item(void)
{
#ifdef __APPLE__
    sem_post(sem_log);
#else
    sem_post(&sem_log);
#endif
}

void wake_all_for_shutdown(Game *g)
{
    int i;
    for (i = 0; i < g->n_cars; i++)
        car_turn_post(i);
    done_post();
    log_post_item();
}
