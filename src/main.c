#define _POSIX_C_SOURCE 200809L

#include "game.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Game *g_sig;

static void on_signal(int sig)
{
    char msg[] = "\nПолучен сигнал прерывания, судья завершает гонку...\n";
    (void)sig;
    write_all(STDOUT_FILENO, msg, sizeof(msg) - 1);
    if (g_sig)
        game_request_stop(g_sig, 1);
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0')
        return -1;
    *out = (int)v;
    return 0;
}

static void usage(const char *argv0)
{
    char buf[1200];
    int n = snprintf(buf, sizeof(buf),
        "Использование: %s [опции]\n"
        "  -c файл     конфигурационный файл (ключ=значение)\n"
        "  -s seed     начальное значение ГПСЧ (воспроизводимость)\n"
        "  -d мс       задержка отображения события\n"
        "  -l файл     журнал результатов\n"
        "  -n команд   число команд (2..8)\n"
        "  -p машин    машин в команде (1..2)\n"
        "  -k кругов   дистанция\n"
        "  -m раундов  предел раундов\n"
        "  -g дорожек  число дорожек (2..8)\n"
        "  -R строк -C столбцов   размер поля\n"
        "  -q          без ASCII-трассы (только события)\n"
        "  -i          интерактивный ввод параметров\n"
        "  --no-bonus  без круговых премий\n"
        "  --no-lap-retire  не снимать отстающих на круг\n"
        "  -h          справка\n",
        argv0);
    write_all(STDOUT_FILENO, buf, (size_t)n);
}

static int read_line_fd(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char ch;
        ssize_t r = read(fd, &ch, 1);
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ch == '\n')
            break;
        if (ch == '\r')
            continue;
        buf[n++] = ch;
    }
    buf[n] = '\0';
    return (int)n;
}

static void prompt_int(const char *q, int *dst)
{
    char line[64];
    char out[128];
    int n = snprintf(out, sizeof(out), "%s [%d]: ", q, *dst);
    write_all(STDOUT_FILENO, out, (size_t)n);
    if (read_line_fd(STDIN_FILENO, line, sizeof(line)) > 0) {
        int v;
        if (parse_int(line, &v) == 0)
            *dst = v;
    }
}

static void apply_kv(Game *g, const char *key, const char *val)
{
    int v;
    if (strcmp(key, "teams") == 0 && parse_int(val, &v) == 0)
        g->teams = v;
    else if (strcmp(key, "cars_per_team") == 0 && parse_int(val, &v) == 0)
        g->cars_per_team = v;
    else if (strcmp(key, "laps") == 0 && parse_int(val, &v) == 0)
        g->laps_to_go = v;
    else if (strcmp(key, "max_rounds") == 0 && parse_int(val, &v) == 0)
        g->max_rounds = v;
    else if (strcmp(key, "lanes") == 0 && parse_int(val, &v) == 0)
        g->track.lanes = v;
    else if (strcmp(key, "rows") == 0 && parse_int(val, &v) == 0)
        g->track.rows = v;
    else if (strcmp(key, "cols") == 0 && parse_int(val, &v) == 0)
        g->track.cols = v;
    else if (strcmp(key, "seed") == 0 && parse_int(val, &v) == 0)
        g->seed = v;
    else if (strcmp(key, "delay_ms") == 0 && parse_int(val, &v) == 0)
        g->delay_ms = v;
    else if (strcmp(key, "bonuses") == 0 && parse_int(val, &v) == 0)
        g->bonuses_on = v;
    else if (strcmp(key, "lap_retire") == 0 && parse_int(val, &v) == 0)
        g->lap_retire_on = v;
    else if (strcmp(key, "stall_retire") == 0 && parse_int(val, &v) == 0)
        g->stall_retire_on = v;
    else if (strcmp(key, "quiet") == 0 && parse_int(val, &v) == 0)
        g->quiet = v;
    else if (strcmp(key, "log") == 0)
        strncpy(g->logfile, val, sizeof(g->logfile) - 1);
}

static int load_config(Game *g, const char *path)
{
    char buf[4096];
    ssize_t n, off = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    while (off < (ssize_t)sizeof(buf) - 1) {
        n = read(fd, buf + off, sizeof(buf) - 1 - (size_t)off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        off += n;
    }
    close(fd);
    buf[off] = '\0';

    {
        char *save = buf;
        char *line;
        while ((line = strtok(save, "\n")) != NULL) {
            char key[64], val[192];
            char *hash;
            save = NULL;
            hash = strchr(line, '#');
            if (hash)
                *hash = '\0';
            if (sscanf(line, " %63[^=]=%191s", key, val) == 2)
                apply_kv(g, key, val);
        }
    }
    return 0;
}

static void defaults(Game *g)
{
    memset(g, 0, sizeof(*g));
    g->teams = 3;
    g->cars_per_team = 2;
    g->laps_to_go = 2;
    g->max_rounds = 80;
    g->track.lanes = 4;
    g->track.rows = 12;
    g->track.cols = 20;
    g->seed = 1;
    g->delay_ms = 0;
    g->bonuses_on = 1;
    /* По умолчанию выкл.: иначе после первого круга лидера пачка сходит. */
    g->lap_retire_on = 0;
    g->stall_retire_on = 1;
    g->quiet = 0;
    strncpy(g->logfile, "race.log", sizeof(g->logfile) - 1);
}

static int validate(const Game *g)
{
    if (g->teams < 2 || g->teams > MAX_TEAMS)
        return -1;
    if (g->cars_per_team < 1 || g->cars_per_team > 2)
        return -1;
    if (g->laps_to_go < 1 || g->laps_to_go > 20)
        return -1;
    if (g->max_rounds < 1)
        return -1;
    if (g->track.lanes < 2 || g->track.lanes > MAX_LANES)
        return -1;
    if (g->track.rows < 2 * g->track.lanes + 2 ||
        g->track.cols < 2 * g->track.lanes + 2)
        return -1;
    return 0;
}

int main(int argc, char **argv)
{
    Game game;
    struct sigaction sa;
    int i;
    int interactive = 0;
    char err[128];

    defaults(&game);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-i") == 0) {
            interactive = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            game.quiet = 1;
        } else if (strcmp(argv[i], "--no-bonus") == 0) {
            game.bonuses_on = 0;
        } else if (strcmp(argv[i], "--no-lap-retire") == 0) {
            game.lap_retire_on = 0;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            if (load_config(&game, argv[++i]) != 0) {
                snprintf(err, sizeof(err), "Не удалось прочитать конфиг %s\n", argv[i]);
                write_all(STDERR_FILENO, err, strlen(err));
                return 1;
            }
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.seed);
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.delay_ms);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            strncpy(game.logfile, argv[++i], sizeof(game.logfile) - 1);
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.teams);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.cars_per_team);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.laps_to_go);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.max_rounds);
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.track.lanes);
        } else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.track.rows);
        } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            parse_int(argv[++i], &game.track.cols);
        } else {
            snprintf(err, sizeof(err), "Неизвестная опция: %s\n", argv[i]);
            write_all(STDERR_FILENO, err, strlen(err));
            usage(argv[0]);
            return 1;
        }
    }

    if (interactive) {
        prompt_int("Число команд", &game.teams);
        prompt_int("Машин в команде", &game.cars_per_team);
        prompt_int("Кругов", &game.laps_to_go);
        prompt_int("Предел раундов", &game.max_rounds);
        prompt_int("Seed", &game.seed);
        prompt_int("Задержка мс", &game.delay_ms);
    }

    if (validate(&game) != 0) {
        const char *m = "Некорректные параметры трассы или состава. См. -h.\n";
        write_all(STDERR_FILENO, m, strlen(m));
        return 1;
    }

    g_sig = &game;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (game_setup(&game) != 0) {
        const char *m = "Ошибка инициализации гонки (проверьте размеры трассы).\n";
        write_all(STDERR_FILENO, m, strlen(m));
        return 1;
    }
    if (game_run(&game) != 0) {
        const char *m = "Ошибка запуска потоков.\n";
        write_all(STDERR_FILENO, m, strlen(m));
        game_destroy(&game);
        return 1;
    }
    game_destroy(&game);
    return 0;
}
