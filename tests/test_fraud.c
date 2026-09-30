/*
 * test_fraud.c - unit tests for the detection engine
 *   make test
 */
#include <stdio.h>
#include "../src/fraud.h"

static FraudGraph g;
static int failures = 0, checks = 0;

#define CHECK(cond, msg) do {                                   \
        checks++;                                               \
        if (!(cond)) { failures++; printf("  FAIL: %s\n", msg); } \
    } while (0)

static FdParams params(void) { return fd_default_params(); }

static void test_dag_has_no_loops(void)
{
    printf("DAG has no loops\n");
    FdParams p = params();
    fd_init(&g);
    fd_add_transfer(&g, 0, 1, 100, 0);
    fd_add_transfer(&g, 1, 2, 100, 1);
    fd_add_transfer(&g, 0, 2, 100, 2);
    fd_add_transfer(&g, 2, 3, 100, 3);
    fd_analyze(&g, &p);
    CHECK(g.n_sccs == 4, "every account is its own SCC");
    CHECK(g.n_cycles == 0, "no cycles");
}

static void test_triangle(void)
{
    printf("Three-account ring is found once\n");
    FdParams p = params();
    fd_init(&g);
    fd_add_transfer(&g, 0, 1, 10000, 0);
    fd_add_transfer(&g, 1, 2, 9800, 2);
    fd_add_transfer(&g, 2, 0, 9600, 5);
    fd_add_transfer(&g, 2, 3, 50, 6);          /* exit, not part of the loop */
    fd_analyze(&g, &p);
    CHECK(g.n_sccs == 2, "ring SCC + account 3");
    CHECK(g.scc_id[0] == g.scc_id[1] && g.scc_id[1] == g.scc_id[2], "ring shares an SCC");
    CHECK(g.scc_id[3] != g.scc_id[0], "account 3 is outside the ring");
    CHECK(g.n_cycles == 1, "exactly one cycle (no duplicates from rotations)");
    CHECK(g.cycles[0].len == 3, "cycle has three hops");
    CHECK(g.cycles[0].flags & FD_FLAG_TIME_ORDERED, "hops are time ordered");
    CHECK(g.cycles[0].flags & FD_FLAG_FAST, "loop closes within the window");
    CHECK(g.cycles[0].score >= FD_RISK_HIGH, "classic ring scores high");
    CHECK(g.account_hits[3] == 0, "exit account is not flagged");
}

static void test_rotation_starts_at_earliest(void)
{
    printf("Loop is reported from its earliest transfer\n");
    FdParams p = params();
    fd_init(&g);
    fd_add_transfer(&g, 0, 1, 5000, 30);
    fd_add_transfer(&g, 1, 2, 5000, 40);
    fd_add_transfer(&g, 2, 0, 5000, 10);       /* earliest: loop starts at 2 */
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 1, "one cycle");
    CHECK(g.tx[g.cycles[0].edges[0]].from == 2, "starts at account 2");
    CHECK(g.cycles[0].flags & FD_FLAG_TIME_ORDERED, "ordered after rotation");
}

static void test_unordered_loop_scores_low(void)
{
    printf("Unrelated payments that happen to form a loop score low\n");
    FdParams p = params();
    p.chronological = 0;
    fd_init(&g);
    fd_add_transfer(&g, 0, 1, 8000, 3);
    fd_add_transfer(&g, 1, 2, 5000, 40);
    fd_add_transfer(&g, 2, 0, 300, 20);
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 1, "one cycle");
    CHECK(!(g.cycles[0].flags & FD_FLAG_TIME_ORDERED), "not time ordered");
    CHECK(g.cycles[0].score < FD_RISK_MEDIUM, "low risk");
    p.chronological = 1;
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 0, "skipped entirely in chronological mode");
}

static void test_chronological_window(void)
{
    printf("Chronological mode respects the time window\n");
    FdParams p = params();
    fd_init(&g);
    fd_add_transfer(&g, 0, 1, 1000, 0);
    fd_add_transfer(&g, 1, 2, 1000, 50);
    fd_add_transfer(&g, 2, 0, 1000, 100);       /* closes after 100 hours */
    p.window_hours = 72;
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 0, "100h loop pruned with a 72h window");
    p.window_hours = 120;
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 1, "found with a 120h window");
    p.chronological = 0;
    p.window_hours = 72;
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 1, "any-order mode ignores the window when searching");
}

static void test_parallel_transfers_and_two_loops(void)
{
    printf("Two loops sharing an account, plus a round trip\n");
    FdParams p = params();
    fd_init(&g);
    /* loop A: 0 -> 1 -> 2 -> 0 ; loop B: 0 -> 3 -> 4 -> 0 */
    fd_add_transfer(&g, 0, 1, 1000, 0);
    fd_add_transfer(&g, 1, 2, 1000, 1);
    fd_add_transfer(&g, 2, 0, 1000, 2);
    fd_add_transfer(&g, 0, 3, 1000, 3);
    fd_add_transfer(&g, 3, 4, 1000, 4);
    fd_add_transfer(&g, 4, 0, 1000, 5);
    fd_add_transfer(&g, 5, 6, 1000, 6);         /* round trip 5 <-> 6 */
    fd_add_transfer(&g, 6, 5, 1000, 7);
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 3, "three cycles");
    CHECK(g.account_hits[0] == 2, "shared account is in both loops");
    int round_trips = 0;
    for (int i = 0; i < g.n_cycles; i++)
        if (g.cycles[i].flags & FD_FLAG_ROUND_TRIP) round_trips++;
    CHECK(round_trips == 1, "one round trip");
}

static void test_filters(void)
{
    printf("max_len and min_amount filters\n");
    FdParams p = params();
    fd_init(&g);
    for (int i = 0; i < 6; i++)                 /* one 6-hop loop */
        fd_add_transfer(&g, i, (i + 1) % 6, 1000, i);
    p.max_len = 5;
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 0, "6-hop loop skipped when max_len = 5");
    p.max_len = 6;
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 1, "found when max_len = 6");
    p.min_amount = 2000;
    fd_analyze(&g, &p);
    CHECK(g.n_cycles == 0, "small transfers ignored with min_amount");
    CHECK(g.n_sccs == 6, "SCCs also respect min_amount");
}

static void test_sorted_by_score(void)
{
    printf("Results are sorted riskiest first\n");
    FdParams p = params();
    p.chronological = 0;
    fd_init(&g);
    fd_add_transfer(&g, 0, 1, 50000, 0);        /* strong ring */
    fd_add_transfer(&g, 1, 2, 49000, 1);
    fd_add_transfer(&g, 2, 0, 48000, 2);
    fd_add_transfer(&g, 3, 4, 900, 50);         /* weak loop */
    fd_add_transfer(&g, 4, 3, 100, 10);
    fd_add_transfer(&g, 5, 6, 20000, 5);        /* medium round trip */
    fd_add_transfer(&g, 6, 5, 20000, 300);
    fd_analyze(&g, &p);
    int ok = 1;
    for (int i = 1; i < g.n_cycles; i++)
        if (g.cycles[i].score > g.cycles[i - 1].score) ok = 0;
    CHECK(g.n_cycles == 3, "three cycles");
    CHECK(ok, "scores are non-increasing");
    CHECK(g.cycles[0].len == 3, "the ring is ranked first");
}

int main(void)
{
    test_dag_has_no_loops();
    test_triangle();
    test_rotation_starts_at_earliest();
    test_unordered_loop_scores_low();
    test_chronological_window();
    test_parallel_transfers_and_two_loops();
    test_filters();
    test_sorted_by_score();
    printf("\n%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
