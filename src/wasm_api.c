/*
 * wasm_api.c - exposes the LoopTrace engine to JavaScript
 *
 * Built with:  make wasm
 *   clang --target=wasm32 -nostdlib ... src/wasm_api.c src/fraud.c
 *
 * There is no C library in the browser build, so the engine (fraud.c) is
 * written without one, and the two memory helpers the compiler may call
 * for struct copies are defined here. JavaScript keeps the account names
 * and talks to the engine through integer ids and small getter functions.
 */
#include "fraud.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static FraudGraph G;
static FdParams   P;

/* ---- freestanding runtime support ---- */
void *memset(void *dst, int c, __SIZE_TYPE__ n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ---- building the graph ---- */
EXPORT(lt_reset) void lt_reset(void) { fd_init(&G); }

EXPORT(lt_add) int lt_add(int from, int to, double amount, double time)
{
    return fd_add_transfer(&G, from, to, amount, time);
}

EXPORT(lt_max_accounts)  int lt_max_accounts(void)  { return FD_MAX_ACCOUNTS; }
EXPORT(lt_max_transfers) int lt_max_transfers(void) { return FD_MAX_TRANSFERS; }

/* ---- running the analysis ---- */
EXPORT(lt_analyze)
int lt_analyze(int max_len, double window_hours, double min_amount, int chronological)
{
    P.max_len = max_len;
    P.window_hours = window_hours;
    P.min_amount = min_amount;
    P.chronological = chronological;
    return fd_analyze(&G, &P);
}

/* ---- reading results ---- */
EXPORT(lt_accounts)  int  lt_accounts(void)  { return G.n_accounts; }
EXPORT(lt_sccs)      int  lt_sccs(void)      { return G.n_sccs; }
EXPORT(lt_scc_of)    int  lt_scc_of(int v)   { return G.scc_id[v]; }
EXPORT(lt_scc_size)  int  lt_scc_size(int s) { return G.scc_size[s]; }
EXPORT(lt_truncated) int  lt_truncated(void) { return G.truncated; }
EXPORT(lt_steps)     double lt_steps(void)   { return (double)G.steps; }

EXPORT(lt_cycle_len)    int    lt_cycle_len(int i)          { return G.cycles[i].len; }
EXPORT(lt_cycle_edge)   int    lt_cycle_edge(int i, int j)  { return G.cycles[i].edges[j]; }
EXPORT(lt_cycle_score)  int    lt_cycle_score(int i)        { return G.cycles[i].score; }
EXPORT(lt_cycle_flags)  int    lt_cycle_flags(int i)        { return G.cycles[i].flags; }
EXPORT(lt_cycle_span)   double lt_cycle_span(int i)         { return G.cycles[i].span_hours; }
EXPORT(lt_cycle_min)    double lt_cycle_min(int i)          { return G.cycles[i].min_amount; }
EXPORT(lt_cycle_total)  double lt_cycle_total(int i)        { return G.cycles[i].total_amount; }

EXPORT(lt_account_hits) int lt_account_hits(int v) { return G.account_hits[v]; }
EXPORT(lt_account_risk) int lt_account_risk(int v) { return G.account_risk[v]; }
