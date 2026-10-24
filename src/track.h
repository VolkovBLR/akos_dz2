#ifndef TRACK_H
#define TRACK_H

/* Кольцевая клеточная трасса: прямоугольное кольцо из нескольких дорожек.
 * Дорожка 0 — внутренняя (кратчайшая), последняя — внешняя (старт). */

#define MAX_ROWS 40
#define MAX_COLS 80
#define MAX_LANES 8
#define MAX_LANE_LEN 256
#define MAX_PATH 40
#define MAX_CARS 16
#define MAX_TEAMS 8

typedef struct {
    int r;
    int c;
} Cell;

typedef struct {
    int valid;
    int lane;
    int idx;           /* индекс клетки вдоль дорожки, 0 = линия старт/финиш */
    int fin;           /* 1, если клетка лежит на финишной линии */
    Cell fwd;
    Cell back;
    Cell inward;       /* r < 0, если хода нет */
    Cell outward;
} Node;

typedef struct {
    int rows;
    int cols;
    int lanes;
    int lane_len[MAX_LANES];
    Cell lane_cell[MAX_LANES][MAX_LANE_LEN];
    Node map[MAX_ROWS][MAX_COLS];
} Track;

int track_init(Track *t, int rows, int cols, int lanes);
int track_is_cell(const Track *t, int r, int c);
int track_lane(const Track *t, int r, int c);
Node *track_node(Track *t, int r, int c);
const Node *track_node_c(const Track *t, int r, int c);

#endif
