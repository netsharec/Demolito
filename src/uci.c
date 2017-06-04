/*
 * Demolito, a UCI chess engine.
 * Copyright 2015 lucasart.
 *
 * Demolito is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Demolito is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program. If
 * not, see <http://www.gnu.org/licenses/>.
*/
#include "bitboard.h"
#include "eval.h"
#include "gen.h"
#include "htable.h"
#include "move.h"
#include "position.h"
#include "search.h"
#include "smp.h"
#include "uci.h"

// Tuning parameters
int X[] = {};

static thrd_t Timer = 0;

static uint64_t Hash = 1;
static int64_t TimeBuffer = 30;

static void uci_format_score(int score, char *str)
{
    if (is_mate_score(score))
        sprintf(str, "mate %d", score > 0 ? (MATE - score + 1) / 2 : (score - MATE + 1) / 2);
    else
        sprintf(str, "cp %d", score * 100 / EP);
}

static void intro()
{
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

    puts("id name Demolito " VERSION "\nid author lucasart");
    printf("option name UCI_Chess960 type check default %s\n", Chess960 ? "true" : "false");
    printf("option name Hash type spin default %" PRIu64 " min 1 max 1048576\n", Hash);
    printf("option name Threads type spin default %d min 1 max 63\n", WorkersCount);
    printf("option name Contempt type spin default %d min -100 max 100\n", Contempt);
    printf("option name Time Buffer type spin default %" PRId64 " min 0 max 1000\n", TimeBuffer);

    // Declare UCI options for tuning parameters
    for (int i = 0; i < (int)(sizeof(X) / sizeof(int)); i++)
        printf("option name X%d type spin default %d min %d max %d\n", i, X[i],
               X[i] > 0 ? 0 : 2 * X[i], X[i] > 0 ? 2 * X[i] : 0);

    // Prepare .var file for Joona's SPSA tuner with tuning parameters
    for (int i = 0; i < (int)(sizeof(X) / sizeof(int)); i++)
        printf("X%d,%d,%d,%d,%.2f,0.002,0\n", i, X[i], X[i] > 0 ? 0 : 2 * X[i], X[i] > 0 ? 2 * X[i] : 0,
               abs(X[i]) / 8.0);

    puts("uciok");
}

static void setoption(char **linePos)
{
    const char *token = strtok_r(NULL, " \n", linePos);
    char name[32] = "";

    if (strcmp(token, "name"))
        return;

    while ((token = strtok_r(NULL, " \n", linePos)) && strcmp(token,"value"))
        strcat(name, token);

    if (!strcmp(name, "UCI_Chess960"))
        Chess960 = !strcmp(strtok_r(NULL, " \n", linePos), "true");
    else if (!strcmp(name, "Hash")) {
        Hash = atoi(strtok_r(NULL, " \n", linePos));
        Hash = 1ULL << bb_msb(Hash);  // must be a power of two
        hash_resize(Hash);
    } else if (!strcmp(name, "Threads"))
        smp_resize(atoi(strtok_r(NULL, " \n", linePos)));
    else if (!strcmp(name, "Contempt"))
        Contempt = atoi(strtok_r(NULL, " \n", linePos));
    else if (!strcmp(name, "TimeBuffer"))
        TimeBuffer = atoi(strtok_r(NULL, " \n", linePos));
    else if (*name == 'X')
        X[atoi(name + 1)] = atoi(strtok_r(NULL, " \n", linePos));
}

static void position(char **linePos)
{
    Position p[NB_COLOR];
    int idx = 0;

    const char *token = strtok_r(NULL, " \n", linePos);
    char fen[MAX_FEN] = "";

    if (!strcmp(token, "startpos")) {
        strcpy(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        strtok_r(NULL, " \n", linePos);  // consume "moves" token (if present)
    } else if (!strcmp(token, "fen")) {
        while ((token = strtok_r(NULL, " \n", linePos)) && strcmp(token, "moves"))
            strcat(strcat(fen, token), " ");
    } else
        return;

    pos_set(&p[idx], fen);
    stack_clear(&rootStack);
    stack_push(&rootStack, p[idx].key);

    // Parse moves (if any)
    while ((token = strtok_r(NULL, " \n", linePos))) {
        move_t m = string_to_move(&p[idx], token);
        pos_move(&p[idx^1], &p[idx], m);
        idx ^= 1;
        stack_push(&rootStack, p[idx].key);
    }

    rootPos = p[idx];
}

static void go(char **linePos)
{
    memset(&lim, 0, sizeof(lim));
    lim.depth = MAX_DEPTH;
    lim.movestogo = 30;

    const char *token;

    while ((token = strtok_r(NULL, " \n", linePos))) {
        if (!strcmp(token, "depth"))
            lim.depth = atoi(strtok_r(NULL, " \n", linePos));
        else if (!strcmp(token, "nodes"))
            lim.nodes = atoll(strtok_r(NULL, " \n", linePos));
        else if (!strcmp(token, "movetime")) {
            lim.movetime = atoll(strtok_r(NULL, " \n", linePos));
            lim.movetime -= TimeBuffer;
        } else if (!strcmp(token, "movestogo"))
            lim.movestogo = atoi(strtok_r(NULL, " \n", linePos));
        else if ((rootPos.turn == WHITE && !strcmp(token, "wtime")) || (rootPos.turn == BLACK
                 && !strcmp(token, "btime")))
            lim.time = atoll(strtok_r(NULL, " \n", linePos));
        else if ((rootPos.turn == WHITE && !strcmp(token, "winc")) || (rootPos.turn == BLACK
                 && !strcmp(token, "binc")))
            lim.inc = atoll(strtok_r(NULL, " \n", linePos));
    }

    if (lim.time || lim.inc) {
        lim.movetime = ((lim.movestogo - 1) * lim.inc + lim.time) / lim.movestogo;

        if (lim.movetime > lim.time - TimeBuffer)
            lim.movetime = lim.time - TimeBuffer;
    }

    if (Timer) {
        thrd_join(Timer, NULL);
        Timer = 0;
    }

    thrd_create(&Timer, search_wrapper, NULL);
}

static void eval()
{
    pos_print(&rootPos);
    char str[12];
    uci_format_score(evaluate(&rootPos), str);
    printf("score %s\n", str);
}

static void perft(char **linePos)
{
    const int depth = atoi(strtok_r(NULL, " \n", linePos));
    pos_print(&rootPos);
    printf("perft = %" PRIu64 "\n", gen_perft(&rootPos, depth, 0));
}

Info ui;

void uci_loop()
{
    char *line = NULL, *linePos;
    size_t n = 0;

    while (getline(&line, &n, stdin)) {
        const char *token = strtok_r(line, " \n", &linePos);

        if (!strcmp(token, "uci"))
            intro();
        else if (!strcmp(token, "setoption"))
            setoption(&linePos);
        else if (!strcmp(token, "isready"))
            puts("readyok");
        else if (!strcmp(token, "ucinewgame")) {
            hash_resize(Hash);
            memset(HashTable, 0, Hash << 20);
            smp_new_game();
        } else if (!strcmp(token, "position"))
            position(&linePos);
        else if (!strcmp(token, "go"))
            go(&linePos);
        else if (!strcmp(token, "stop"))
            signal = STOP;
        else if (!strcmp(token, "eval"))
            eval();
        else if (!strcmp(token, "perft"))
            perft(&linePos);
        else if (!strcmp(token, "quit"))
            break;
        else
            printf("unknown command: %s\n", line);
    }

    free(line);

    if (Timer) {
        thrd_join(Timer, NULL);
        Timer = 0;
    }
}

void info_create(Info *info)
{
    info->lastDepth = 0;
    info->best = info->ponder = 0;
    timespec_get(&info->start, TIME_UTC);
    mtx_init(&info->mtx, mtx_plain);
}

void info_destroy(Info *info)
{
    mtx_destroy(&info->mtx);
}

void info_update(Info *info, int depth, int score, int64_t nodes, move_t pv[], bool partial)
{
    mtx_lock(&info->mtx);

    if (depth > info->lastDepth) {
        info->best = pv[0];
        info->ponder = pv[1];

        if (partial) {
            mtx_unlock(&info->mtx);
            return;
        }

        info->lastDepth = depth;

        char str[12];
        uci_format_score(score, str);
        printf("info depth %d score %s time %" PRId64 " nodes %" PRId64 " pv",
               depth, str, elapsed_msec(&info->start), nodes);

        // Because of e1g1 notation when Chess960 = false, we need to play the PV, just to
        // be able to print it. This is a defect of the UCI protocol (which should have
        // encoded castling as e1h1 regardless of Chess960 allowing coherent treatement).
        Position p[2];
        int idx = 0;
        p[idx] = rootPos;

        for (int i = 0; pv[i]; i++) {
            move_to_string(&p[idx], pv[i], str);
            printf(" %s", str);
            pos_move(&p[idx^1], &p[idx], pv[i]);
            idx ^= 1;
        }

        puts("");
    }

    mtx_unlock(&info->mtx);
}

void info_print_bestmove(Info *info)
{
    char best[6], ponder[6];

    mtx_lock(&info->mtx);
    move_to_string(&rootPos, info->best, best);
    move_to_string(&rootPos, info->ponder, ponder);
    mtx_unlock(&info->mtx);

    printf("bestmove %s ponder %s\n", best, ponder);
}

move_t info_best(Info *info)
{
    mtx_lock(&info->mtx);
    const move_t best = info->best;
    mtx_unlock(&info->mtx);

    return best;
}

int info_last_depth(Info *info)
{
    mtx_lock(&info->mtx);
    const int lastDepth = info->lastDepth;
    mtx_unlock(&info->mtx);

    return lastDepth;
}
