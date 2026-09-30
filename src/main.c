/*
 * main.c - LoopTrace command-line tool
 *
 *   ./looptrace data/rings.csv
 *   ./looptrace data/rings.csv --max-len 5 --window 48 --min-amount 1000 --top 10
 *
 * CSV columns: from,to,amount,time_hours   (a header line is optional)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "fraud.h"
#include "namemap.h"

static FraudGraph g;      /* large: keep it off the stack */
static NameMap    names;

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <transactions.csv> [--max-len N] [--window HOURS]\n"
        "                            [--min-amount X] [--top K] [--any-order]\n"
        "  --any-order   also report loops whose hops are not in time order\n", prog);
}

/* Trims spaces/quotes in place and returns the start of the text. */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s) || *s == '"') s++;
    char *end = s + strlen(s);
    while (end > s && (isspace((unsigned char)end[-1]) || end[-1] == '"')) end--;
    *end = '\0';
    return s;
}

/* Splits a CSV line into up to `max` fields. Returns the field count. */
static int split_csv(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    for (int i = 0; i < n; i++) fields[i] = trim(fields[i]);
    return n;
}

static int load_csv(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }

    char line[512];
    int lineno = 0, skipped = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *fields[4];
        if (trim(line)[0] == '\0' || line[0] == '#') continue;
        if (split_csv(line, fields, 4) < 4) { skipped++; continue; }

        char *end;
        double amount = strtod(fields[2], &end);
        if (end == fields[2]) {                 /* header or bad number */
            if (lineno > 1) skipped++;
            continue;
        }
        double t = strtod(fields[3], NULL);

        char from[NM_NAME_LEN], to[NM_NAME_LEN];
        snprintf(from, sizeof from, "%s", fields[0]);
        snprintf(to, sizeof to, "%s", fields[1]);

        int a = nm_get_or_add(&names, from);
        int b = nm_get_or_add(&names, to);
        if (a < 0 || b < 0 || fd_add_transfer(&g, a, b, amount, t) < 0) {
            skipped++;
            continue;
        }
    }
    fclose(f);
    if (skipped) fprintf(stderr, "note: skipped %d malformed/over-limit lines\n", skipped);
    return 0;
}

static void print_money(double x)
{
    /* 1234567.8 -> 1,234,568 */
    char buf[32];
    long v = (long)(x + 0.5);
    int len = snprintf(buf, sizeof buf, "%ld", v);
    for (int i = 0; i < len; i++) {
        putchar(buf[i]);
        int left = len - i - 1;
        if (left > 0 && left % 3 == 0) putchar(',');
    }
}

static void print_flags(int flags)
{
    const char *sep = "";
    printf("[");
    if (flags & FD_FLAG_TIME_ORDERED) { printf("%sordered", sep); sep = ", "; }
    if (flags & FD_FLAG_FAST)         { printf("%sfast", sep);    sep = ", "; }
    if (flags & FD_FLAG_SIMILAR_AMT)  { printf("%ssimilar amounts", sep); sep = ", "; }
    if (flags & FD_FLAG_ROUND_TRIP)   { printf("%sround trip", sep); }
    printf("]");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }

    FdParams p = fd_default_params();
    int top = 15;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--any-order")) { p.chronological = 0; continue; }
        if (i + 1 >= argc) { usage(argv[0]); return 1; }
        if      (!strcmp(argv[i], "--max-len"))    p.max_len = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window"))     p.window_hours = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min-amount")) p.min_amount = atof(argv[++i]);
        else if (!strcmp(argv[i], "--top"))        top = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    fd_init(&g);
    nm_init(&names);
    if (load_csv(argv[1]) != 0) return 1;

    clock_t t0 = clock();
    int n_cycles = fd_analyze(&g, &p);
    double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;

    int big_sccs = 0;
    for (int i = 0; i < g.n_sccs; i++) if (g.scc_size[i] > 1) big_sccs++;

    printf("\nLoopTrace - money-laundering loop detector\n");
    printf("==========================================\n");
    printf("Input      : %s\n", argv[1]);
    printf("Accounts   : %d\n", g.n_accounts);
    printf("Transfers  : %d\n", g.n_transfers);
    printf("SCCs       : %d total, %d with more than one account\n", g.n_sccs, big_sccs);
    printf("Settings   : %s loops, max length %d, window %.0fh, min amount ",
           p.chronological ? "chronological" : "all", p.max_len, p.window_hours);
    print_money(p.min_amount);
    printf("\nSearch     : %ld DFS steps, %.2f ms%s\n\n",
           g.steps, ms, g.truncated ? "  (limit reached, showing the riskiest loops kept)" : "");

    /* Accounts that can take part in a loop, grouped by SCC */
    int shown = 0;
    for (int s = 0; s < g.n_sccs; s++) {
        if (g.scc_size[s] < 2) continue;
        printf("SCC #%d (%d accounts): ", ++shown, g.scc_size[s]);
        int k = 0;
        for (int v = 0; v < g.n_accounts; v++)
            if (g.scc_id[v] == s)
                printf("%s%s", k++ ? ", " : "", nm_name(&names, v));
        printf("\n");
    }
    if (!shown) printf("No strongly connected groups: money never returns to its source.\n");

    int high = 0, medium = 0;
    for (int i = 0; i < n_cycles; i++) {
        if (g.cycles[i].score >= FD_RISK_HIGH) high++;
        else if (g.cycles[i].score >= FD_RISK_MEDIUM) medium++;
    }
    printf("\nLoops found: %d  (%d high risk, %d medium, %d low)\n",
           n_cycles, high, medium, n_cycles - high - medium);

    int limit = n_cycles < top ? n_cycles : top;
    for (int i = 0; i < limit; i++) {
        const Cycle *c = &g.cycles[i];
        printf("\n%2d. %-6s %3d/100  %d hops  ", i + 1, fd_risk_label(c->score), c->score, c->len);
        print_flags(c->flags);
        printf("\n    ");
        for (int j = 0; j < c->len; j++)
            printf("%s -> ", nm_name(&names, g.tx[c->edges[j]].from));
        printf("%s\n    amounts: ", nm_name(&names, g.tx[c->edges[0]].from));
        for (int j = 0; j < c->len; j++) {
            if (j) printf(" > ");
            print_money(g.tx[c->edges[j]].amount);
        }
        printf("   span: %.1fh\n", c->span_hours);
    }

    /* Accounts ranked by the riskiest loop they appear in */
    printf("\nFlagged accounts (in a high-risk loop):\n");
    int any = 0;
    for (int score = 100; score >= FD_RISK_HIGH; score--)
        for (int v = 0; v < g.n_accounts; v++)
            if (g.account_risk[v] == score) {
                printf("  %-28s risk %3d  in %d loop%s\n", nm_name(&names, v),
                       score, g.account_hits[v], g.account_hits[v] == 1 ? "" : "s");
                any = 1;
            }
    if (!any) printf("  none\n");
    printf("\n");
    return 0;
}
