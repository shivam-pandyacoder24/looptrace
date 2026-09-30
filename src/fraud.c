/*
 * fraud.c - LoopTrace detection engine
 *
 * See fraud.h for the overview. Complexity, with V accounts and E transfers:
 *   building the graph ........ O(1) per transfer (push to adjacency list)
 *   Tarjan's SCC .............. O(V + E) time, O(V) extra space
 *   cycle search .............. exponential in the worst case, so it is
 *                               bounded by max_len and FD_STEP_BUDGET and
 *                               only runs inside multi-account SCCs
 *   ranking ................... O(C log C) heap sort for C cycles
 */
#include "fraud.h"

/* ------------------------------------------------------------------ */
/* small helpers (no libc, so the file also builds for WebAssembly)    */
/* ------------------------------------------------------------------ */

static double fd_sqrt(double x) { return __builtin_sqrt(x); }

static int edge_ok(const FraudGraph *g, int e, const FdParams *p)
{
    const Transfer *t = &g->tx[e];
    return t->amount >= p->min_amount && t->from != t->to;
}

FdParams fd_default_params(void)
{
    FdParams p;
    p.max_len      = 6;
    p.window_hours = 72.0;
    p.min_amount   = 0.0;
    p.chronological = 1;
    return p;
}

const char *fd_risk_label(int score)
{
    if (score >= FD_RISK_HIGH)   return "HIGH";
    if (score >= FD_RISK_MEDIUM) return "MEDIUM";
    return "LOW";
}

/* ------------------------------------------------------------------ */
/* graph construction                                                  */
/* ------------------------------------------------------------------ */

void fd_init(FraudGraph *g)
{
    g->n_accounts = g->n_transfers = 0;
    g->max_transfer = 0.0;
    g->n_sccs = g->n_cycles = g->truncated = 0;
    g->steps = 0;
    for (int i = 0; i < FD_MAX_ACCOUNTS; i++) {
        g->head[i] = -1;
        g->account_hits[i] = g->account_risk[i] = 0;
        g->scc_id[i] = -1;
    }
}

/* Adds one transfer. Returns its id, or -1 if it was rejected. */
int fd_add_transfer(FraudGraph *g, int from, int to, double amount, double time)
{
    if (from < 0 || to < 0 || from >= FD_MAX_ACCOUNTS || to >= FD_MAX_ACCOUNTS)
        return -1;
    if (g->n_transfers >= FD_MAX_TRANSFERS || !(amount > 0.0))
        return -1;

    int e = g->n_transfers++;
    Transfer *t = &g->tx[e];
    t->from = from;
    t->to = to;
    t->amount = amount;
    t->time = time;
    t->next = g->head[from];      /* O(1) insert at the front of the list */
    g->head[from] = e;

    if (from >= g->n_accounts) g->n_accounts = from + 1;
    if (to   >= g->n_accounts) g->n_accounts = to + 1;
    if (amount > g->max_transfer) g->max_transfer = amount;
    return e;
}

/* ------------------------------------------------------------------ */
/* 1. Tarjan's strongly connected components                           */
/* ------------------------------------------------------------------ */

static void strongconnect(FraudGraph *g, int v, const FdParams *p)
{
    g->index[v] = g->low[v] = g->counter++;
    g->stack[g->sp++] = v;
    g->on_stack[v] = 1;

    for (int e = g->head[v]; e != -1; e = g->tx[e].next) {
        if (!edge_ok(g, e, p)) continue;
        int w = g->tx[e].to;
        if (g->index[w] == -1) {                 /* tree edge: recurse     */
            strongconnect(g, w, p);
            if (g->low[w] < g->low[v]) g->low[v] = g->low[w];
        } else if (g->on_stack[w]) {             /* back edge into the SCC */
            if (g->index[w] < g->low[v]) g->low[v] = g->index[w];
        }
    }

    if (g->low[v] == g->index[v]) {              /* v is the root of an SCC */
        int id = g->n_sccs++, size = 0, w;
        do {
            w = g->stack[--g->sp];
            g->on_stack[w] = 0;
            g->scc_id[w] = id;
            size++;
        } while (w != v);
        g->scc_size[id] = size;
    }
}

int fd_find_sccs(FraudGraph *g, const FdParams *p)
{
    g->n_sccs = g->counter = g->sp = 0;
    for (int v = 0; v < g->n_accounts; v++) {
        g->index[v] = -1;
        g->on_stack[v] = 0;
    }
    for (int v = 0; v < g->n_accounts; v++)
        if (g->index[v] == -1)
            strongconnect(g, v, p);
    return g->n_sccs;
}

/* ------------------------------------------------------------------ */
/* 2. Risk scoring                                                     */
/* ------------------------------------------------------------------ */

/*
 * Score = (amount + timing + shape) * volume
 *
 *   amount  0..45  laundered money keeps most of its value as it moves,
 *                  so min/max amount around the loop stays close to 1
 *   timing  0..35  the hops happen one after another, ideally fast
 *   shape   5..20  3-5 hop "layering" loops are the classic pattern;
 *                  2-hop round trips are often innocent refunds
 *   volume  x0.4..x1.0  scaled by the loop's bottleneck amount compared
 *                  with the largest transfer in the dataset
 */
static void score_cycle(const FraudGraph *g, Cycle *c, const FdParams *p)
{
    double tmin = g->tx[c->edges[0]].time, tmax = tmin;
    int ordered = 1;

    c->min_amount = c->max_amount = g->tx[c->edges[0]].amount;
    c->total_amount = 0.0;
    c->flags = 0;

    for (int i = 0; i < c->len; i++) {
        const Transfer *t = &g->tx[c->edges[i]];
        if (t->amount < c->min_amount) c->min_amount = t->amount;
        if (t->amount > c->max_amount) c->max_amount = t->amount;
        if (t->time < tmin) tmin = t->time;
        if (t->time > tmax) tmax = t->time;
        c->total_amount += t->amount;
        if (i > 0 && t->time < g->tx[c->edges[i - 1]].time) ordered = 0;
    }
    c->span_hours = tmax - tmin;

    double similarity = c->min_amount / c->max_amount;
    double s_amount = 45.0 * similarity;
    if (similarity >= 0.85) c->flags |= FD_FLAG_SIMILAR_AMT;

    double s_time = 0.0;
    if (ordered) {
        c->flags |= FD_FLAG_TIME_ORDERED;
        if (c->span_hours <= p->window_hours) {
            c->flags |= FD_FLAG_FAST;
            s_time = 35.0;
        } else {
            s_time = 17.5 + 17.5 * p->window_hours / c->span_hours;
        }
    }

    double s_shape;
    if (c->len == 2) { s_shape = 5.0; c->flags |= FD_FLAG_ROUND_TRIP; }
    else if (c->len <= 5) s_shape = 20.0;
    else s_shape = 12.0;

    double volume = fd_sqrt(c->min_amount / g->max_transfer);
    double score = (s_amount + s_time + s_shape) * (0.4 + 0.6 * volume);

    int s = (int)(score + 0.5);
    c->score = s < 0 ? 0 : (s > 100 ? 100 : s);
}

/* ------------------------------------------------------------------ */
/* 3. Cycle enumeration (depth-bounded DFS inside each SCC)            */
/* ------------------------------------------------------------------ */

/*
 * A loop is "chronological" when some rotation of it has non-decreasing
 * timestamps, i.e. money leaves the first account and comes back to it
 * hop by hop. Going round the loop, the time can then drop only once
 * (where the last hop wraps back to the first). This returns where that
 * drop ends, which is the natural starting hop, or -1 if the time drops
 * more than once.
 */
static int chronological_start(const FraudGraph *g, const int *edges, int len)
{
    int drops = 0, start = 0;
    for (int i = 0; i < len; i++) {
        double t_prev = g->tx[edges[(i + len - 1) % len]].time;
        if (g->tx[edges[i]].time < t_prev) { drops++; start = i; }
    }
    return drops <= 1 ? start : -1;
}

/* Copies the current DFS path into a Cycle and keeps it if it ranks. */
static void record_cycle(FraudGraph *g, int len, const FdParams *p)
{
    Cycle c = {0};
    int first = chronological_start(g, g->path, len);
    if (first < 0) {                        /* unordered: start at earliest */
        first = 0;
        for (int i = 1; i < len; i++)
            if (g->tx[g->path[i]].time < g->tx[g->path[first]].time)
                first = i;
    }
    c.len = len;
    for (int i = 0; i < len; i++)
        c.edges[i] = g->path[(first + i) % len];
    score_cycle(g, &c, p);

    if (g->n_cycles < FD_MAX_CYCLES) {
        g->cycles[g->n_cycles++] = c;
        return;
    }
    /* Storage full: replace the weakest kept loop if this one is riskier. */
    g->truncated = 1;
    int weakest = 0;
    for (int i = 1; i < g->n_cycles; i++)
        if (g->cycles[i].score < g->cycles[weakest].score)
            weakest = i;
    if (c.score > g->cycles[weakest].score)
        g->cycles[weakest] = c;
}

/*
 * Extends the path start -> ... -> v by one transfer at a time.
 *   depth   transfers already on the path
 *   tmin/tmax/tlast/drops   timing of the path so far (chronological mode)
 *
 * Only accounts with a larger id than `start` may be visited, so every
 * loop is reported once, from its smallest account id.
 *
 * In chronological mode a branch is cut as soon as its hops span more than
 * the time window or the time goes backwards twice. On real data this is
 * what keeps the search fast: most paths die after one or two hops.
 */
static void dfs(FraudGraph *g, int start, int v, int depth, const FdParams *p,
                double tmin, double tmax, double tlast, int drops)
{
    for (int e = g->head[v]; e != -1; e = g->tx[e].next) {
        if (g->steps >= g->step_limit) { g->truncated = 1; return; }
        g->steps++;

        if (!edge_ok(g, e, p)) continue;
        int w = g->tx[e].to;
        if (g->scc_id[w] != g->scc_id[start]) continue;  /* loops never leave an SCC */
        if (w != start && (w < start || g->on_path[w])) continue;

        double t = g->tx[e].time;
        double nmin = tmin, nmax = tmax;
        int ndrops = drops;
        if (depth == 0) {
            nmin = nmax = t;
        } else {
            if (t < nmin) nmin = t;
            if (t > nmax) nmax = t;
            if (t < tlast) ndrops++;
        }
        if (p->chronological) {
            if (nmax - nmin > p->window_hours || ndrops > 1) continue;
        }

        g->path[depth] = e;
        if (w == start) {
            /* closing the loop: the wrap back to the first hop counts too */
            int wrap = depth > 0 && t > g->tx[g->path[0]].time;
            if (!p->chronological || ndrops + wrap <= 1)
                record_cycle(g, depth + 1, p);
        } else if (depth + 1 < p->max_len) {
            g->on_path[w] = 1;
            dfs(g, start, w, depth + 1, p, nmin, nmax, t, ndrops);
            g->on_path[w] = 0;
        }
    }
}

int fd_find_cycles(FraudGraph *g, const FdParams *p)
{
    g->n_cycles = g->truncated = 0;
    g->steps = 0;

    int starts_left = 0;
    for (int v = 0; v < g->n_accounts; v++) {
        g->on_path[v] = 0;
        if (g->scc_size[g->scc_id[v]] >= 2) starts_left++;
    }

    for (int s = 0; s < g->n_accounts; s++) {
        if (g->scc_size[g->scc_id[s]] < 2) continue;      /* no loop possible */
        /* share the remaining budget fairly between the remaining starts,
           so one dense corner of the graph cannot starve the rest */
        g->step_limit = g->steps + (FD_STEP_BUDGET - g->steps) / starts_left--;
        g->on_path[s] = 1;
        dfs(g, s, s, 0, p, 0.0, 0.0, 0.0, 0);
        g->on_path[s] = 0;
    }
    return g->n_cycles;
}

/* ------------------------------------------------------------------ */
/* 4. Ranking: heap sort, riskiest loop first                          */
/* ------------------------------------------------------------------ */

static int worse(const Cycle *a, const Cycle *b)
{
    if (a->score != b->score) return a->score < b->score;
    return a->total_amount < b->total_amount;
}

static void swap_cycles(Cycle *a, Cycle *b) { Cycle t = *a; *a = *b; *b = t; }

static void sift_down(Cycle *c, int root, int n)
{
    for (;;) {
        int child = 2 * root + 1;
        if (child >= n) return;
        if (child + 1 < n && worse(&c[child + 1], &c[child])) child++;
        if (!worse(&c[child], &c[root])) return;
        swap_cycles(&c[root], &c[child]);
        root = child;
    }
}

/* Max-heap on "worseness": the worst loop sinks to the end each round. */
static void sort_cycles(Cycle *c, int n)
{
    for (int i = n / 2 - 1; i >= 0; i--) sift_down(c, i, n);
    for (int end = n - 1; end > 0; end--) {
        swap_cycles(&c[0], &c[end]);
        sift_down(c, 0, end);
    }
}

/* ------------------------------------------------------------------ */
/* full pipeline                                                       */
/* ------------------------------------------------------------------ */

int fd_analyze(FraudGraph *g, const FdParams *params)
{
    FdParams p = *params;
    if (p.max_len < 2) p.max_len = 2;
    if (p.max_len > FD_MAX_CYCLE_LEN) p.max_len = FD_MAX_CYCLE_LEN;
    if (p.window_hours <= 0.0) p.window_hours = 1.0;

    fd_find_sccs(g, &p);
    fd_find_cycles(g, &p);
    sort_cycles(g->cycles, g->n_cycles);

    for (int v = 0; v < g->n_accounts; v++)
        g->account_hits[v] = g->account_risk[v] = 0;
    for (int i = 0; i < g->n_cycles; i++) {
        const Cycle *c = &g->cycles[i];
        for (int j = 0; j < c->len; j++) {
            int a = g->tx[c->edges[j]].from;
            g->account_hits[a]++;
            if (c->score > g->account_risk[a]) g->account_risk[a] = c->score;
        }
    }
    return g->n_cycles;
}
