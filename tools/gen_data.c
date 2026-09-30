/*
 * gen_data.c - synthetic transaction generator for testing and benchmarks
 *
 *   ./gen_data <accounts> <transfers> <rings> [seed] > big.csv
 *
 * Produces mostly one-directional "normal" traffic plus `rings` planted
 * laundering loops (3-6 hops, similar amounts, fast and time-ordered),
 * so you can check that LoopTrace finds them and measure its speed.
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long state = 88172645463325252ULL;

static unsigned long long rnd(void)          /* xorshift64 */
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
static int    rand_int(int lo, int hi) { return lo + (int)(rnd() % (unsigned long long)(hi - lo + 1)); }
static double rand01(void)             { return (double)(rnd() % 1000000ULL) / 1000000.0; }

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <accounts> <transfers> <rings> [seed]\n", argv[0]);
        return 1;
    }
    int n = atoi(argv[1]), m = atoi(argv[2]), rings = atoi(argv[3]);
    if (argc > 4) state ^= (unsigned long long)atoll(argv[4]) * 2654435761ULL;
    if (n < 10) n = 10;

    printf("from,to,amount,time_hours\n");

    /* Normal traffic: mostly from lower to higher ids (a near-DAG, like
       employers -> people -> merchants), with a few reverse payments. */
    for (int i = 0; i < m; i++) {
        int a = rand_int(0, n - 2);
        int b = rand_int(a + 1, n - 1);
        if (rnd() % 20 == 0) { int t = a; a = b; b = t; }      /* 5% reverse */
        double amount = 200.0 + rand01() * rand01() * 30000.0;
        printf("ACC%04d,ACC%04d,%.2f,%.1f\n", a, b, amount, rand01() * 720.0);
    }

    /* Planted rings */
    for (int r = 0; r < rings; r++) {
        int len = rand_int(3, 6);
        int members[6];
        for (int k = 0; k < len; k++) {
            int dup;
            do {
                members[k] = rand_int(0, n - 1);
                dup = 0;
                for (int j = 0; j < k; j++) if (members[j] == members[k]) dup = 1;
            } while (dup);
        }
        double amount = 20000.0 + rand01() * 30000.0;
        double t = rand01() * 650.0;
        for (int k = 0; k < len; k++) {
            printf("ACC%04d,ACC%04d,%.2f,%.1f\n",
                   members[k], members[(k + 1) % len], amount, t);
            amount *= 0.96 + rand01() * 0.03;        /* 1-4% skimmed per hop */
            t += 1.0 + rand01() * 10.0;
        }
    }
    return 0;
}
