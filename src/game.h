#ifndef GAME_H
#define GAME_H

#include "track.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CAR_RACING = 0,
    CAR_ZERO_PENDING,   /* нулевой ход, сход после раунда */
    CAR_FINISH_PENDING, /* стоит на финише до конца раунда */
    CAR_FINISHED,
    CAR_RETIRED
} CarState;

typedef enum {
    STR_AGGRESSIVE = 0, /* максимум прогресса, затем внутренняя дорожка */
    STR_RANDOM,
    STR_CAUTIOUS,       /* не максимальный ход, запас по манёвру */
    STR_BLOCKER         /* держится ближе к внешней, мешает обгону */
} Strategy;

typedef struct {
    int id;
    int team;
    int num;            /* номер в команде 0.. */
    char name[16];
    Strategy strategy;
    CarState state;
    int r, c;
    int laps;           /* число пересечений финиша */
    int dist;           /* накопленный прогресс вперёд (без боковых) */
    int last_cost;      /* стоимость последнего хода */
    int turn_slot;      /* место в очереди текущего раунда (0 = лидер) */
    int bonus;          /* оставшиеся премиальные клетки (макс. 3 за круг) */
    int fwd_since_line; /* шагов вперёд с последнего пересечения финиша */
    int pending_bump;
    int finish_order;
    int place;
    unsigned rng;
    pthread_t thread;
} Car;

typedef struct {
    char text[384];
} LogEvent;

#define LOG_CAP 4096

typedef struct {
    Track track;
    int teams;
    int cars_per_team;
    int n_cars;
    int laps_to_go;     /* дистанция в кругах */
    int max_rounds;
    int delay_ms;
    int bonuses_on;
    int lap_retire_on;
    int stall_retire_on;
    int quiet;
    int seed;
    char logfile[256];

    Car cars[MAX_CARS];
    int occ[MAX_ROWS][MAX_COLS]; /* id машины или -1 */

    int round_no;
    int last_fwd_cost;  /* ход предыдущей машины в текущем раунде */
    int stall_moves;    /* подряд ходов без продвижения вперёд */
    int finish_seq;
    int next_place;
    volatile int running;
    volatile int shutdown;
    volatile int interrupted;

    /* Очередь событий для потока журнала. */
    LogEvent logq[LOG_CAP];
    int log_head, log_tail, log_count;

    pthread_mutex_t board_mu;
    pthread_mutex_t log_mu;
    pthread_t judge_th;
    pthread_t logger_th;

    int log_fd; /* дескриптор файла журнала, -1 если нет */
} Game;

typedef struct {
    Cell path[MAX_PATH];
    int path_len;
    int cost;
    int end_r, end_c;
    int laps_after;
    int dist_after;
    int lane_after;
    int finished;
    int bump_id; /* -1 если нет */
} Move;

int game_setup(Game *g);
void game_destroy(Game *g);
int game_run(Game *g);
void game_request_stop(Game *g, int interrupted);
void emit_event(Game *g, const char *fmt, ...);
void print_board(Game *g);
void write_all(int fd, const char *buf, size_t n);

/* Глобальный указатель на текущую гонку: потоки машин и
 * синхронизация на условных переменных читают shutdown отсюда. */
extern Game *g_game;

/* Реализации синхронизации (semaphore или condvar). */
int sync_init(Game *g);
void sync_destroy(Game *g);
void car_turn_wait(int car_id);
void car_turn_post(int car_id);
void done_wait(void);
void done_post(void);
void log_wait_item(Game *g);
void log_post_item(void);
void wake_all_for_shutdown(Game *g);

#endif
