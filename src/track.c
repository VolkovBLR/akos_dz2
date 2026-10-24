#include "track.h"

#include <string.h>

static int in_bounds(const Track *t, int r, int c)
{
    return r >= 0 && c >= 0 && r < t->rows && c < t->cols;
}

int track_is_cell(const Track *t, int r, int c)
{
    if (!in_bounds(t, r, c))
        return 0;
    /* Внутреннее отверстие кольца: всё, что дальше, чем lanes клеток от края. */
    if (r >= t->lanes && r < t->rows - t->lanes &&
        c >= t->lanes && c < t->cols - t->lanes)
        return 0;
    return 1;
}

int track_lane(const Track *t, int r, int c)
{
    int d;
    if (!track_is_cell(t, r, c))
        return -1;
    d = r;
    if (t->rows - 1 - r < d)
        d = t->rows - 1 - r;
    if (c < d)
        d = c;
    if (t->cols - 1 - c < d)
        d = t->cols - 1 - c;
    /* d = 0 на внешнем крае => самая внешняя дорожка. */
    return t->lanes - 1 - d;
}

Node *track_node(Track *t, int r, int c)
{
    if (!in_bounds(t, r, c))
        return NULL;
    return &t->map[r][c];
}

const Node *track_node_c(const Track *t, int r, int c)
{
    if (!in_bounds(t, r, c))
        return NULL;
    return &t->map[r][c];
}

static void set_cell(Cell *cell, int r, int c)
{
    cell->r = r;
    cell->c = c;
}

static void invalid_cell(Cell *cell)
{
    cell->r = -1;
    cell->c = -1;
}

/* Обход прямоугольника дорожки по часовой стрелке, индекс 0 — верхняя сторона. */
static int build_lane(Track *t, int lane)
{
    int inset = t->lanes - 1 - lane;
    int r0 = inset;
    int c0 = inset;
    int r1 = t->rows - 1 - inset;
    int c1 = t->cols - 1 - inset;
    int n = 0;
    int r, c;

    if (r1 - r0 < 2 || c1 - c0 < 2)
        return -1;

    for (c = c0; c < c1; c++) {
        t->lane_cell[lane][n].r = r0;
        t->lane_cell[lane][n].c = c;
        n++;
    }
    for (r = r0; r < r1; r++) {
        t->lane_cell[lane][n].r = r;
        t->lane_cell[lane][n].c = c1;
        n++;
    }
    for (c = c1; c > c0; c--) {
        t->lane_cell[lane][n].r = r1;
        t->lane_cell[lane][n].c = c;
        n++;
    }
    for (r = r1; r > r0; r--) {
        t->lane_cell[lane][n].r = r;
        t->lane_cell[lane][n].c = c0;
        n++;
    }

    if (n >= MAX_LANE_LEN || n <= 0)
        return -1;
    t->lane_len[lane] = n;
    return 0;
}

int track_init(Track *t, int rows, int cols, int lanes)
{
    int lane, i, r, c;

    memset(t, 0, sizeof(*t));
    if (lanes < 2 || lanes > MAX_LANES)
        return -1;
    if (rows < 2 * lanes + 2 || cols < 2 * lanes + 2)
        return -1;
    if (rows > MAX_ROWS || cols > MAX_COLS)
        return -1;

    t->rows = rows;
    t->cols = cols;
    t->lanes = lanes;

    for (r = 0; r < MAX_ROWS; r++) {
        for (c = 0; c < MAX_COLS; c++) {
            t->map[r][c].valid = 0;
            t->map[r][c].lane = -1;
            invalid_cell(&t->map[r][c].fwd);
            invalid_cell(&t->map[r][c].back);
            invalid_cell(&t->map[r][c].inward);
            invalid_cell(&t->map[r][c].outward);
        }
    }

    for (lane = 0; lane < lanes; lane++) {
        if (build_lane(t, lane) != 0)
            return -1;
        for (i = 0; i < t->lane_len[lane]; i++) {
            Cell cell = t->lane_cell[lane][i];
            Cell nxt = t->lane_cell[lane][(i + 1) % t->lane_len[lane]];
            Cell prv = t->lane_cell[lane][(i + t->lane_len[lane] - 1) % t->lane_len[lane]];
            Node *node = &t->map[cell.r][cell.c];
            node->valid = 1;
            node->lane = lane;
            node->idx = i;
            /* Финиш — первая клетка верхней прямой каждой дорожки (idx == 0). */
            node->fin = (i == 0);
            node->fwd = nxt;
            node->back = prv;
        }
    }

    /* Боковые соседи: ортогональный шаг на соседнюю дорожку. */
    for (r = 0; r < t->rows; r++) {
        for (c = 0; c < t->cols; c++) {
            Node *node;
            const int dr[4] = {-1, 1, 0, 0};
            const int dc[4] = {0, 0, -1, 1};
            int k;

            if (!t->map[r][c].valid)
                continue;
            node = &t->map[r][c];
            for (k = 0; k < 4; k++) {
                int nr = r + dr[k];
                int nc = c + dc[k];
                int nlane;
                if (!track_is_cell(t, nr, nc) || !t->map[nr][nc].valid)
                    continue;
                if ((nr == node->fwd.r && nc == node->fwd.c) ||
                    (nr == node->back.r && nc == node->back.c))
                    continue;
                nlane = t->map[nr][nc].lane;
                if (nlane == node->lane - 1) {
                    set_cell(&node->inward, nr, nc);
                } else if (nlane == node->lane + 1) {
                    set_cell(&node->outward, nr, nc);
                }
            }
        }
    }
    return 0;
}
