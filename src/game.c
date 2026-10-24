#define _POSIX_C_SOURCE 200809L

#include "game.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

Game *g_game = NULL;

static const char *team_names[] = {
    "Альфа", "Браво", "Чарли", "Дельта",
    "Эхо", "Фокстрот", "Гольф", "Хотел"
};

static const char *str_name(Strategy s)
{
    switch (s) {
    case STR_AGGRESSIVE: return "агрессивная";
    case STR_RANDOM:     return "случайная";
    case STR_CAUTIOUS:   return "осторожная";
    case STR_BLOCKER:    return "блокирующая";
    default:             return "?";
    }
}

static const char *state_name(CarState s)
{
    switch (s) {
    case CAR_RACING:         return "на трассе";
    case CAR_ZERO_PENDING:   return "нулевой ход";
    case CAR_FINISH_PENDING: return "на финише";
    case CAR_FINISHED:       return "финиш";
    case CAR_RETIRED:        return "сход";
    default:                 return "?";
    }
}

void write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        off += (size_t)w;
    }
}

static void delay_ms(int ms)
{
    struct timespec ts;
    if (ms <= 0)
        return;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

static unsigned rng_next(unsigned *st)
{
    *st = (*st) * 1664525u + 1013904223u;
    return *st;
}

static int rng_range(unsigned *st, int lo, int hi)
{
    unsigned span;
    if (hi <= lo)
        return lo;
    span = (unsigned)(hi - lo + 1);
    return lo + (int)(rng_next(st) % span);
}

void emit_event(Game *g, const char *fmt, ...)
{
    char buf[384];
    va_list ap;
    LogEvent ev;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    memset(&ev, 0, sizeof(ev));
    strncpy(ev.text, buf, sizeof(ev.text) - 1);

    pthread_mutex_lock(&g->log_mu);
    if (g->log_count < LOG_CAP) {
        g->logq[g->log_tail] = ev;
        g->log_tail = (g->log_tail + 1) % LOG_CAP;
        g->log_count++;
        log_post_item();
    }
    pthread_mutex_unlock(&g->log_mu);
}

/* Круг засчитывается только после существенного продвижения по кольцу.
 * Порог «4 клетки» ложно давал круг машинам, стартовавшим перед линией. */
static int lap_threshold(const Track *t, const Node *nn)
{
    int th;
    if (!nn || nn->lane < 0 || nn->lane >= t->lanes)
        return 8;
    th = t->lane_len[nn->lane] / 2;
    if (th < 8)
        th = 8;
    return th;
}

static int occ_at(const Game *g, int r, int c)
{
    if (r < 0 || c < 0 || r >= g->track.rows || c >= g->track.cols)
        return -1;
    return g->occ[r][c];
}

static void rank_key(const Game *g, const Car *car, int *k)
{
    int lane = 99;
    const Node *node = track_node_c(&g->track, car->r, car->c);
    if (node && node->valid)
        lane = node->lane;
    k[0] = (car->state == CAR_FINISHED || car->state == CAR_FINISH_PENDING) ? 1 : 0;
    k[1] = car->laps;
    k[2] = car->dist;
    k[3] = -lane;
    k[4] = -car->id;
}

static int cmp_keys(const int *a, const int *b)
{
    int i;
    for (i = 0; i < 5; i++) {
        if (a[i] > b[i])
            return 1;
        if (a[i] < b[i])
            return -1;
    }
    return 0;
}

static int cmp_cars_lead(const Game *g, int a, int b)
{
    int ka[5], kb[5];
    rank_key(g, &g->cars[a], ka);
    rank_key(g, &g->cars[b], kb);
    return cmp_keys(ka, kb);
}

static int collect_order(Game *g, int *order)
{
    int n = 0, i, j;
    for (i = 0; i < g->n_cars; i++) {
        if (g->cars[i].state == CAR_RACING)
            order[n++] = i;
    }
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (cmp_cars_lead(g, order[j], order[i]) > 0) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    return n;
}

typedef struct {
    int vis[MAX_ROWS][MAX_COLS];
    Cell path[MAX_PATH];
    int best_n;
    int nodes;
    Move best[120];
    const Game *g;
    int car_id;
    int is_leader;
    int budget;
} Search;

static int already_best(const Search *s, const Move *m)
{
    int i;
    for (i = 0; i < s->best_n; i++) {
        const Move *b = &s->best[i];
        if (b->end_r == m->end_r && b->end_c == m->end_c &&
            b->cost == m->cost && b->laps_after == m->laps_after &&
            b->finished == m->finished && b->bump_id == m->bump_id)
            return 1;
    }
    return 0;
}

static void consider(Search *s, int r, int c, int cost, int laps, int dist,
                    int bump, int fin, int plen)
{
    Move m;
    const Node *node;
    int i;

    if (cost <= 0)
        return;
    node = track_node_c(&s->g->track, r, c);
    if (!node || !node->valid)
        return;
    memset(&m, 0, sizeof(m));
    m.cost = cost;
    m.end_r = r;
    m.end_c = c;
    m.laps_after = laps;
    m.dist_after = dist;
    m.lane_after = node->lane;
    m.finished = fin;
    m.bump_id = bump;
    m.path_len = plen;
    for (i = 0; i < plen && i < MAX_PATH; i++)
        m.path[i] = s->path[i];
    if (s->best_n < 120 && !already_best(s, &m))
        s->best[s->best_n++] = m;
}

/* plen — число клеток маршрута; cost — стоимость по правилам
 * (вперёд/внутрь = 1, наружу = 3), поэтому индексировать путь cost нельзя. */
static void dfs(Search *s, int r, int c, int cost, int laps, int dist,
                int bump, int since, int finished_now, int plen)
{
    const Node *node;
    Cell opts[3];
    int costs[3];
    int nopt = 0;
    int i;

    s->nodes++;
    if (s->nodes > 8000)
        return;
    if (cost > s->budget)
        return;
    node = track_node_c(&s->g->track, r, c);
    if (!node || !node->valid)
        return;

    if (cost > 0) {
        int fin = (laps >= s->g->laps_to_go) || finished_now;
        consider(s, r, c, cost, laps, dist, bump, fin, plen);
        if (fin)
            return;
    }
    if (cost == s->budget)
        return;

    if (node->fwd.r >= 0) {
        opts[nopt] = node->fwd;
        costs[nopt] = 1;
        nopt++;
    }
    if (node->inward.r >= 0) {
        opts[nopt] = node->inward;
        costs[nopt] = 1;
        nopt++;
    }
    if (node->outward.r >= 0) {
        opts[nopt] = node->outward;
        costs[nopt] = 3;
        nopt++;
    }

    for (i = 0; i < nopt; i++) {
        int nr = opts[i].r, nc = opts[i].c;
        int ncost = cost + costs[i];
        const Node *nn;
        int occ, new_laps, new_dist, new_bump, new_since, is_fwd, fin_now;

        if (ncost > s->budget)
            continue;
        if (plen >= MAX_PATH - 1)
            continue;
        if (s->vis[nr][nc])
            continue;
        nn = track_node_c(&s->g->track, nr, nc);
        if (!nn || !nn->valid)
            continue;

        is_fwd = (nr == node->fwd.r && nc == node->fwd.c);
        occ = occ_at(s->g, nr, nc);
        new_bump = bump;
        if (occ >= 0 && occ != s->car_id) {
            const Car *other = &s->g->cars[occ];
            if (s->is_leader && s->g->lap_retire_on &&
                other->state == CAR_RACING &&
                nn->lane == 0 &&
                s->g->cars[s->car_id].laps > other->laps) {
                new_bump = occ;
            } else {
                continue;
            }
        }

        new_laps = laps;
        new_dist = dist;
        new_since = since;
        fin_now = 0;
        if (is_fwd) {
            new_dist++;
            new_since++;
            if (nn->fin && new_since >= lap_threshold(&s->g->track, nn)) {
                new_laps++;
                new_since = 0;
                if (new_laps >= s->g->laps_to_go)
                    fin_now = 1;
            }
        }

        s->vis[nr][nc] = 1;
        s->path[plen] = opts[i];
        dfs(s, nr, nc, ncost, new_laps, new_dist, new_bump, new_since, fin_now,
            plen + 1);
        s->vis[nr][nc] = 0;
    }
}

static int search_moves(Game *g, int car_id, int budget, int is_leader, Move *out, int cap)
{
    Search s;
    Car *car = &g->cars[car_id];
    int i, n;

    memset(&s, 0, sizeof(s));
    s.g = g;
    s.car_id = car_id;
    s.is_leader = is_leader;
    s.budget = budget;
    for (i = 0; i < MAX_PATH; i++) {
        s.path[i].r = -1;
        s.path[i].c = -1;
    }
    s.vis[car->r][car->c] = 1;
    dfs(&s, car->r, car->c, 0, car->laps, car->dist, -1, car->fwd_since_line, 0, 0);

    n = s.best_n;
    if (n > cap)
        n = cap;
    memcpy(out, s.best, sizeof(Move) * (size_t)n);
    return n;
}

static int move_better(const Move *a, const Move *b, Strategy st)
{
    if (a->finished != b->finished)
        return a->finished > b->finished;
    if (a->laps_after != b->laps_after)
        return a->laps_after > b->laps_after;
    if (a->dist_after != b->dist_after)
        return a->dist_after > b->dist_after;
    if (st == STR_BLOCKER) {
        if (a->lane_after != b->lane_after)
            return a->lane_after > b->lane_after;
        return a->cost <= b->cost;
    }
    if (st == STR_CAUTIOUS) {
        if (a->lane_after != b->lane_after)
            return a->lane_after < b->lane_after;
        return a->cost < b->cost;
    }
    if (a->lane_after != b->lane_after)
        return a->lane_after < b->lane_after;
    return a->cost > b->cost;
}

static int pick_move(Car *car, Move *moves, int n, Move *chosen)
{
    int i, best = 0;
    Strategy st = car->strategy;

    if (n <= 0)
        return 0;
    if (st == STR_RANDOM) {
        *chosen = moves[rng_range(&car->rng, 0, n - 1)];
        return 1;
    }
    if (st == STR_CAUTIOUS) {
        int maxc = moves[0].cost;
        int w = 0;
        for (i = 1; i < n; i++) {
            if (moves[i].cost > maxc)
                maxc = moves[i].cost;
        }
        if (maxc > 1) {
            for (i = 0; i < n; i++) {
                if (moves[i].finished || moves[i].cost < maxc)
                    moves[w++] = moves[i];
            }
            if (w > 0)
                n = w;
        }
    }
    for (i = 1; i < n; i++) {
        if (move_better(&moves[i], &moves[best], st))
            best = i;
    }
    *chosen = moves[best];
    return 1;
}

static int max_move_for(int place_in_round, int leader_cost, int prev_cost)
{
    int base;
    if (place_in_round == 0)
        base = 4;
    else if (place_in_round == 1)
        base = leader_cost + 2;
    else
        base = prev_cost + 1;
    if (base < 1)
        base = 1;
    if (base > MAX_PATH - 1)
        base = MAX_PATH - 1;
    return base;
}

static void format_path(const Move *m, char *buf, size_t cap)
{
    int i, n = 0;
    buf[0] = '\0';
    for (i = 0; i < m->path_len && (size_t)n + 16 < cap; i++) {
        int w = snprintf(buf + n, cap - (size_t)n, "%s(%d,%d)",
                         i ? "->" : "", m->path[i].r, m->path[i].c);
        if (w < 0)
            break;
        n += w;
    }
}

static int step_is_forward(const Track *t, Cell from, Cell to)
{
    const Node *n = track_node_c(t, from.r, from.c);
    if (!n)
        return 0;
    return n->fwd.r == to.r && n->fwd.c == to.c;
}

static void apply_move(Game *g, int car_id, const Move *m)
{
    Car *car = &g->cars[car_id];
    const Node *from = track_node_c(&g->track, car->r, car->c);
    const Node *to;
    int old_lane = from ? from->lane : -1;
    int i, since = car->fwd_since_line;
    Cell cur;

    cur.r = car->r;
    cur.c = car->c;
    g->occ[car->r][car->c] = -1;
    car->r = m->end_r;
    car->c = m->end_c;
    car->laps = m->laps_after;
    car->dist = m->dist_after;
    car->last_cost = m->cost;

    for (i = 0; i < m->path_len; i++) {
        const Node *nn = track_node_c(&g->track, m->path[i].r, m->path[i].c);
        if (step_is_forward(&g->track, cur, m->path[i])) {
            since++;
            if (nn && nn->fin && since >= lap_threshold(&g->track, nn))
                since = 0;
        }
        cur = m->path[i];
    }
    car->fwd_since_line = since;
    g->occ[car->r][car->c] = car_id;

    to = track_node_c(&g->track, car->r, car->c);
    if (to && old_lane >= 0 && to->lane != old_lane) {
        emit_event(g, "  %s меняет дорожку %d -> %d (%s)",
                   car->name, old_lane, to->lane,
                   to->lane < old_lane ? "внутрь" : "наружу");
    }
    if (m->bump_id >= 0) {
        Car *b = &g->cars[m->bump_id];
        emit_event(g, "  %s проходит клетку отстающего %s — сход %s",
                   car->name, b->name, b->name);
        if (g->occ[b->r][b->c] == m->bump_id)
            g->occ[b->r][b->c] = -1;
        b->state = CAR_RETIRED;
    }
}

static void *logger_thread(void *arg)
{
    Game *g = arg;
    for (;;) {
        LogEvent ev;
        int stop_empty;

        log_wait_item(g);
        pthread_mutex_lock(&g->log_mu);
        if (g->log_count == 0) {
            stop_empty = g->shutdown;
            pthread_mutex_unlock(&g->log_mu);
            if (stop_empty)
                break;
            continue;
        }
        ev = g->logq[g->log_head];
        g->log_head = (g->log_head + 1) % LOG_CAP;
        g->log_count--;
        pthread_mutex_unlock(&g->log_mu);

        {
            char line[400];
            int n = snprintf(line, sizeof(line), "%s\n", ev.text);
            if (n > 0) {
                write_all(STDOUT_FILENO, line, (size_t)n);
                if (g->log_fd >= 0)
                    write_all(g->log_fd, line, (size_t)n);
            }
        }
        delay_ms(g->delay_ms);
    }
    return NULL;
}

void print_board(Game *g)
{
    int r, c;
    char line[1024];
    int n;

    pthread_mutex_lock(&g->log_mu);
    n = snprintf(line, sizeof(line), "--- Трасса (раунд %d) ---\n", g->round_no);
    write_all(STDOUT_FILENO, line, (size_t)n);
    if (g->log_fd >= 0)
        write_all(g->log_fd, line, (size_t)n);

    for (r = 0; r < g->track.rows; r++) {
        n = 0;
        for (c = 0; c < g->track.cols; c++) {
            int id = g->occ[r][c];
            const Node *node = &g->track.map[r][c];
            if (id >= 0)
                n += snprintf(line + n, sizeof(line) - (size_t)n, "%s", g->cars[id].name);
            else if (!node->valid)
                n += snprintf(line + n, sizeof(line) - (size_t)n, "  ");
            else if (node->fin)
                n += snprintf(line + n, sizeof(line) - (size_t)n, "==");
            else
                n += snprintf(line + n, sizeof(line) - (size_t)n, ".%d", node->lane);
        }
        n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
        write_all(STDOUT_FILENO, line, (size_t)n);
        if (g->log_fd >= 0)
            write_all(g->log_fd, line, (size_t)n);
    }
    pthread_mutex_unlock(&g->log_mu);
}

static void print_standings(Game *g, const char *title)
{
    int order[MAX_CARS];
    int i;
    emit_event(g, "%s", title);
    for (i = 0; i < g->n_cars; i++)
        order[i] = i;
    for (i = 0; i < g->n_cars; i++) {
        int j;
        for (j = i + 1; j < g->n_cars; j++) {
            if (cmp_cars_lead(g, order[j], order[i]) > 0) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    for (i = 0; i < g->n_cars; i++) {
        Car *car = &g->cars[order[i]];
        const Node *node = track_node_c(&g->track, car->r, car->c);
        emit_event(g, "  %2d. %s  %s  круг %d/%d  dist=%d  дор.%d  %s  [%s]",
                   i + 1, car->name, team_names[car->team],
                   car->laps, g->laps_to_go, car->dist,
                   node && node->valid ? node->lane : -1,
                   str_name(car->strategy),
                   state_name(car->state));
    }
}

static int points_for_place(int teams, int place)
{
    static const int p3[] = {0, 9, 6, 4, 3, 2, 1};
    static const int p4[] = {0, 5, 4, 3, 2, 1};
    if (teams <= 3) {
        if (place >= 1 && place <= 6)
            return p3[place];
        return 0;
    }
    if (place >= 1 && place <= 5)
        return p4[place];
    return 0;
}

static void final_classification(Game *g)
{
    int i, t;
    int team_pts[MAX_TEAMS];
    int team_best[MAX_TEAMS];
    Car *sorted[MAX_CARS];
    int n = g->n_cars;

    for (i = 0; i < n; i++)
        sorted[i] = &g->cars[i];

    for (i = 0; i < n; i++) {
        int j;
        for (j = i + 1; j < n; j++) {
            int better = 0;
            if (sorted[j]->state == CAR_FINISHED && sorted[i]->state != CAR_FINISHED)
                better = 1;
            else if (sorted[j]->state == CAR_FINISHED && sorted[i]->state == CAR_FINISHED)
                better = sorted[j]->finish_order < sorted[i]->finish_order;
            else if (sorted[j]->state != CAR_FINISHED && sorted[i]->state != CAR_FINISHED) {
                if (sorted[j]->dist != sorted[i]->dist)
                    better = sorted[j]->dist > sorted[i]->dist;
                else {
                    const Node *nj = track_node_c(&g->track, sorted[j]->r, sorted[j]->c);
                    const Node *ni = track_node_c(&g->track, sorted[i]->r, sorted[i]->c);
                    int lj = nj ? nj->lane : 99, li = ni ? ni->lane : 99;
                    better = lj < li;
                }
            }
            if (better) {
                Car *tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    emit_event(g, "===== ИТОГОВАЯ КЛАССИФИКАЦИЯ МАШИН =====");
    for (i = 0; i < n; i++) {
        sorted[i]->place = i + 1;
        emit_event(g, "  %d место: %s (%s), круги %d, dist=%d, очки %d, статус %s",
                   i + 1, sorted[i]->name, team_names[sorted[i]->team],
                   sorted[i]->laps, sorted[i]->dist,
                   points_for_place(g->teams, i + 1),
                   state_name(sorted[i]->state));
    }

    memset(team_pts, 0, sizeof(team_pts));
    for (t = 0; t < g->teams; t++)
        team_best[t] = 99;
    for (i = 0; i < n; i++) {
        int tm = sorted[i]->team;
        team_pts[tm] += points_for_place(g->teams, i + 1);
        if (i + 1 < team_best[tm])
            team_best[tm] = i + 1;
    }
    emit_event(g, "===== КЛАССИФИКАЦИЯ КОМАНД =====");
    {
        int order_t[MAX_TEAMS];
        for (t = 0; t < g->teams; t++)
            order_t[t] = t;
        for (t = 0; t < g->teams; t++) {
            int j;
            for (j = t + 1; j < g->teams; j++) {
                int a = order_t[t], b = order_t[j];
                int better = team_pts[b] > team_pts[a] ||
                    (team_pts[b] == team_pts[a] && team_best[b] < team_best[a]);
                if (better) {
                    order_t[t] = b;
                    order_t[j] = a;
                }
            }
        }
        for (t = 0; t < g->teams; t++) {
            int tm = order_t[t];
            emit_event(g, "  %d. %s: %d очков (лучшая машина — место %d)",
                       t + 1, team_names[tm], team_pts[tm], team_best[tm]);
        }
    }
    emit_event(g, "Раундов проведено: %d. Прерывание: %s.",
               g->round_no, g->interrupted ? "да" : "нет");
}

static int racing_left(const Game *g)
{
    int i, n = 0;
    for (i = 0; i < g->n_cars; i++) {
        if (g->cars[i].state == CAR_RACING)
            n++;
    }
    return n;
}

static void end_round_cleanup(Game *g)
{
    int i;
    int leader_id = -1;
    int best_laps = -1, best_dist = -1, best_lane = 99;

    for (i = 0; i < g->n_cars; i++) {
        Car *car = &g->cars[i];
        if (car->state == CAR_FINISH_PENDING) {
            g->occ[car->r][car->c] = -1;
            car->state = CAR_FINISHED;
            emit_event(g, "Судья: %s убрана с финишной линии (финиш засчитан).",
                       car->name);
        }
    }
    for (i = 0; i < g->n_cars; i++) {
        Car *car = &g->cars[i];
        if (car->state == CAR_ZERO_PENDING) {
            emit_event(g, "Судья: %s сходит с дистанции после нулевого хода.",
                       car->name);
            g->occ[car->r][car->c] = -1;
            car->state = CAR_RETIRED;
        }
    }

    if (!g->lap_retire_on)
        return;

    for (i = 0; i < g->n_cars; i++) {
        const Node *n;
        int ln;
        if (g->cars[i].state != CAR_RACING)
            continue;
        n = track_node_c(&g->track, g->cars[i].r, g->cars[i].c);
        ln = n ? n->lane : 99;
        if (leader_id < 0 || g->cars[i].laps > best_laps ||
            (g->cars[i].laps == best_laps && g->cars[i].dist > best_dist) ||
            (g->cars[i].laps == best_laps && g->cars[i].dist == best_dist && ln < best_lane)) {
            leader_id = i;
            best_laps = g->cars[i].laps;
            best_dist = g->cars[i].dist;
            best_lane = ln;
        }
    }
    if (leader_id < 0)
        return;
    for (i = 0; i < g->n_cars; i++) {
        if (g->cars[i].state != CAR_RACING)
            continue;
        if (g->cars[leader_id].laps >= g->cars[i].laps + 1) {
            emit_event(g, "Судья: %s отстала на круг от лидера %s и сходит.",
                       g->cars[i].name, g->cars[leader_id].name);
            g->occ[g->cars[i].r][g->cars[i].c] = -1;
            g->cars[i].state = CAR_RETIRED;
        }
    }
}

static void maybe_stall_penalty(Game *g)
{
    int order[MAX_CARS];
    int no;
    if (!g->stall_retire_on || g->stall_moves < 3)
        return;
    no = collect_order(g, order);
    if (no > 0 && g->cars[order[0]].state == CAR_RACING) {
        emit_event(g, "Судья: нет движения вперёд 3 хода подряд, лидер %s снимается",
                   g->cars[order[0]].name);
        g->occ[g->cars[order[0]].r][g->cars[order[0]].c] = -1;
        g->cars[order[0]].state = CAR_RETIRED;
    }
    g->stall_moves = 0;
}

static void do_car_turn(Game *g, int car_id)
{
    Car *self = &g->cars[car_id];
    Move moves[120];
    Move chosen;
    int n, budget, used_bonus = 0;
    char pbuf[256];
    int old_dist, old_laps, old_lane;
    const Node *node;
    int place = self->turn_slot;
    int leader_cost = g->last_fwd_cost;
    int prev_cost = g->last_fwd_cost;
    int base;

    if (g->shutdown || self->state != CAR_RACING)
        return;

    node = track_node_c(&g->track, self->r, self->c);
    old_lane = node ? node->lane : -1;
    old_dist = self->dist;
    old_laps = self->laps;

    base = max_move_for(place, leader_cost, prev_cost);
    budget = base;
    if (g->bonuses_on)
        budget += self->bonus;
    if (budget > MAX_PATH - 1)
        budget = MAX_PATH - 1;

    emit_event(g, "Ход %s | очередь %d | дорожка %d | круг %d | dist %d | лимит %d%s | %s",
               self->name, place + 1, old_lane, self->laps, self->dist, budget,
               self->bonus && g->bonuses_on ? " (есть премия)" : "",
               str_name(self->strategy));

    n = search_moves(g, car_id, budget, place == 0, moves, 120);
    if (n <= 0) {
        emit_event(g, "  блокирование: нет допустимого хода, объявлен нулевой ход");
        self->state = CAR_ZERO_PENDING;
        self->last_cost = 0;
        g->last_fwd_cost = 0;
        g->stall_moves++;
        maybe_stall_penalty(g);
        return;
    }

    pick_move(self, moves, n, &chosen);
    format_path(&chosen, pbuf, sizeof(pbuf));
    emit_event(g, "  маршрут (стоимость %d): %s", chosen.cost, pbuf);

    if (g->bonuses_on && self->bonus > 0 && chosen.cost > base) {
        used_bonus = chosen.cost - base;
        if (used_bonus > self->bonus)
            used_bonus = self->bonus;
        self->bonus -= used_bonus;
        emit_event(g, "  использована премия: %d (остаток %d)", used_bonus, self->bonus);
    }

    apply_move(g, car_id, &chosen);
    g->last_fwd_cost = chosen.cost;

    if (self->dist > old_dist)
        g->stall_moves = 0;
    else {
        g->stall_moves++;
        emit_event(g, "  %s не продвинулась вперёд", self->name);
        maybe_stall_penalty(g);
    }

    if (self->laps > old_laps) {
        emit_event(g, "  %s проходит круг! Теперь: %d/%d",
                   self->name, self->laps, g->laps_to_go);
        if (g->bonuses_on && self->laps < g->laps_to_go) {
            self->bonus = 3;
            emit_event(g, "  начислена круговая премия +3 клетки для %s", self->name);
        }
    }

    if (chosen.finished || self->laps >= g->laps_to_go) {
        self->state = CAR_FINISH_PENDING;
        self->finish_order = ++g->finish_seq;
        emit_event(g, "  ФИНИШ: %s останавливается на линии (порядок %d)",
                   self->name, self->finish_order);
    }
}

static void *car_main(void *arg)
{
    Car *self = arg;
    Game *g = g_game;

    for (;;) {
        car_turn_wait(self->id);
        if (g->shutdown)
            break;
        pthread_mutex_lock(&g->board_mu);
        do_car_turn(g, self->id);
        pthread_mutex_unlock(&g->board_mu);
        done_post();
    }
    return NULL;
}

static void *judge_thread(void *arg)
{
    Game *g = arg;
    int i;

    emit_event(g, "Судья: гонка началась. Кругов: %d, машин: %d, команд: %d, seed=%d.",
               g->laps_to_go, g->n_cars, g->teams, g->seed);
    emit_event(g, "Судья: дорожек %d, поле %dx%d. Премии: %s, сход отстающих: %s.",
               g->track.lanes, g->track.rows, g->track.cols,
               g->bonuses_on ? "да" : "нет",
               g->lap_retire_on ? "да" : "нет");
    for (i = 0; i < g->n_cars; i++) {
        emit_event(g, "  старт %s команда %s стратегия %s клетка (%d,%d)",
                   g->cars[i].name, team_names[g->cars[i].team],
                   str_name(g->cars[i].strategy), g->cars[i].r, g->cars[i].c);
    }
    if (!g->quiet)
        print_board(g);
    print_standings(g, "Стартовый порядок (после жеребьёвки):");

    while (g->running && !g->shutdown) {
        int order[MAX_CARS];
        int n;

        pthread_mutex_lock(&g->board_mu);
        if (g->round_no >= g->max_rounds) {
            emit_event(g, "Судья: достигнут предел раундов (%d).", g->max_rounds);
            g->running = 0;
            pthread_mutex_unlock(&g->board_mu);
            break;
        }
        if (racing_left(g) == 0) {
            emit_event(g, "Судья: на трассе не осталось участников.");
            g->running = 0;
            pthread_mutex_unlock(&g->board_mu);
            break;
        }
        g->round_no++;
        n = collect_order(g, order);
        emit_event(g, "===== РАУНД %d | в гонке %d машин =====", g->round_no, n);
        {
            char buf[256];
            int k, pos = 0;
            pos = snprintf(buf, sizeof(buf), "Порядок хода:");
            for (k = 0; k < n && pos < (int)sizeof(buf) - 8; k++)
                pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, " %s",
                                g->cars[order[k]].name);
            emit_event(g, "%s", buf);
        }
        g->last_fwd_cost = 4;
        pthread_mutex_unlock(&g->board_mu);

        for (i = 0; i < n && !g->shutdown; i++) {
            int id = order[i];
            pthread_mutex_lock(&g->board_mu);
            if (g->cars[id].state != CAR_RACING) {
                pthread_mutex_unlock(&g->board_mu);
                continue;
            }
            g->cars[id].turn_slot = i;
            pthread_mutex_unlock(&g->board_mu);
            car_turn_post(id);
            done_wait();
        }

        pthread_mutex_lock(&g->board_mu);
        end_round_cleanup(g);
        if (!g->quiet)
            print_board(g);
        print_standings(g, "Положение после раунда:");
        if (racing_left(g) == 0)
            g->running = 0;
        pthread_mutex_unlock(&g->board_mu);
    }

    pthread_mutex_lock(&g->board_mu);
    final_classification(g);
    pthread_mutex_unlock(&g->board_mu);

    g->shutdown = 1;
    wake_all_for_shutdown(g);
    return NULL;
}

static Strategy strategy_for(int car_index)
{
    static const Strategy cycle[] = {
        STR_AGGRESSIVE, STR_BLOCKER, STR_CAUTIOUS, STR_RANDOM
    };
    return cycle[car_index % 4];
}

int game_setup(Game *g)
{
    int i, k, r, c;
    unsigned rng;
    int perm[MAX_TEAMS];
    int placed = 0;
    int outer, olen;

    g_game = g;
    if (track_init(&g->track, g->track.rows, g->track.cols, g->track.lanes) != 0)
        return -1;

    g->n_cars = g->teams * g->cars_per_team;
    if (g->n_cars <= 0 || g->n_cars > MAX_CARS)
        return -1;

    pthread_mutex_init(&g->board_mu, NULL);
    pthread_mutex_init(&g->log_mu, NULL);
    g->log_head = g->log_tail = g->log_count = 0;
    g->round_no = 0;
    g->finish_seq = 0;
    g->next_place = g->n_cars;
    g->running = 1;
    g->shutdown = 0;
    g->stall_moves = 0;
    g->last_fwd_cost = 0;

    for (r = 0; r < MAX_ROWS; r++)
        for (c = 0; c < MAX_COLS; c++)
            g->occ[r][c] = -1;

    rng = (unsigned)g->seed * 747796405u + 2891336453u;
    outer = g->track.lanes - 1;
    olen = g->track.lane_len[outer];
    if (g->n_cars >= olen)
        return -1;

    for (i = 0; i < g->teams; i++)
        perm[i] = i;
    for (i = g->teams - 1; i > 0; i--) {
        int j = (int)(rng_next(&rng) % (unsigned)(i + 1));
        int t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }

    /* Сначала по одной машине от команды, затем вторые в том же порядке.
     * Лидер жеребьёвки стоит на финишной клетке внешней дорожки, остальные — сзади. */
    for (k = 0; k < g->cars_per_team; k++) {
        for (i = 0; i < g->teams; i++) {
            int id = placed;
            int team = perm[i];
            int idx = (olen - placed) % olen;
            Cell cell = g->track.lane_cell[outer][idx];
            Car *car = &g->cars[id];

            memset(car, 0, sizeof(*car));
            car->id = id;
            car->team = team;
            car->num = k;
            snprintf(car->name, sizeof(car->name), "%c%d", 'A' + team, k + 1);
            car->strategy = strategy_for(id);
            car->state = CAR_RACING;
            car->r = cell.r;
            car->c = cell.c;
            car->laps = 0;
            car->dist = -placed;
            car->last_cost = 0;
            car->turn_slot = 0;
            car->bonus = 0;
            car->fwd_since_line = 0;
            car->pending_bump = -1;
            car->rng = rng_next(&rng) ^ (unsigned)(id + 1) * 0x9E3779B9u;
            g->occ[car->r][car->c] = id;
            placed++;
        }
    }

    if (sync_init(g) != 0)
        return -1;

    g->log_fd = -1;
    if (g->logfile[0]) {
        g->log_fd = open(g->logfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g->log_fd < 0)
            g->log_fd = -1;
    }
    return 0;
}

void game_destroy(Game *g)
{
    sync_destroy(g);
    pthread_mutex_destroy(&g->board_mu);
    pthread_mutex_destroy(&g->log_mu);
    if (g->log_fd >= 0) {
        close(g->log_fd);
        g->log_fd = -1;
    }
}

void game_request_stop(Game *g, int interrupted)
{
    g->interrupted = interrupted;
    g->running = 0;
    g->shutdown = 1;
    wake_all_for_shutdown(g);
}

int game_run(Game *g)
{
    int i;
    g_game = g;

    if (pthread_create(&g->logger_th, NULL, logger_thread, g) != 0)
        return -1;
    for (i = 0; i < g->n_cars; i++) {
        if (pthread_create(&g->cars[i].thread, NULL, car_main, &g->cars[i]) != 0)
            return -1;
    }
    if (pthread_create(&g->judge_th, NULL, judge_thread, g) != 0)
        return -1;

    pthread_join(g->judge_th, NULL);
    for (i = 0; i < g->n_cars; i++)
        pthread_join(g->cars[i].thread, NULL);

    pthread_mutex_lock(&g->log_mu);
    g->shutdown = 1;
    pthread_mutex_unlock(&g->log_mu);
    log_post_item();
    pthread_join(g->logger_th, NULL);
    return 0;
}
