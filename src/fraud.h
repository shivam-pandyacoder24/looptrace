/*
 * fraud.h - LoopTrace: money-laundering loop detection on a transfer graph
 *
 * Model
 *   Every bank account is a node. Every transfer is a directed, weighted
 *   edge  from -> to  carrying an amount and a timestamp.
 *
 * Pipeline
 *   1. Tarjan's algorithm splits the graph into strongly connected
 *      components (SCCs) in O(V + E). Money can only travel in a circle
 *      inside one SCC, so every account in a single-account SCC is cleared
 *      immediately without further work.
 *   2. A depth-bounded DFS enumerates the simple cycles inside each
 *      multi-account SCC. Each cycle is found exactly once by only starting
 *      from its smallest account id. In chronological mode, branches whose
 *      hops are out of time order or too far apart are pruned early.
 *   3. Each cycle gets a 0-100 risk score from amount similarity, timing,
 *      loop length and volume, and the cycles are heap-sorted best first.
 *
 * The engine does no dynamic allocation and uses no C library functions,
 * so the same file compiles natively (CLI) and to WebAssembly (website).
 */
#ifndef FRAUD_H
#define FRAUD_H

#define FD_MAX_ACCOUNTS   1024
#define FD_MAX_TRANSFERS  8192
#define FD_MAX_CYCLES     500       /* keep the top 500 loops by score      */
#define FD_MAX_CYCLE_LEN  8         /* longest loop the DFS will follow     */
#define FD_STEP_BUDGET    3000000L  /* DFS work cap, keeps worst case bounded */

/* Cycle flags (bit mask) */
#define FD_FLAG_TIME_ORDERED  1  /* each hop happens after the previous one   */
#define FD_FLAG_FAST          2  /* whole loop closes inside the time window  */
#define FD_FLAG_SIMILAR_AMT   4  /* every hop is within 15% of the largest    */
#define FD_FLAG_ROUND_TRIP    8  /* two-account loop: A -> B -> A             */

/* Risk levels */
#define FD_RISK_HIGH    70
#define FD_RISK_MEDIUM  40

typedef struct {
    int    from, to;
    double amount;
    double time;      /* hours since the start of the dataset          */
    int    next;      /* next transfer leaving `from`, -1 = end of list */
} Transfer;

typedef struct {
    int    len;                       /* number of transfers in the loop     */
    int    edges[FD_MAX_CYCLE_LEN];   /* transfer ids, earliest one first    */
    double min_amount, max_amount, total_amount;
    double span_hours;                /* latest minus earliest timestamp     */
    int    flags;
    int    score;                     /* 0..100                              */
} Cycle;

typedef struct {
    int    max_len;       /* longest loop to look for (2..FD_MAX_CYCLE_LEN) */
    double window_hours;  /* loops closing inside this window count as fast */
    double min_amount;    /* transfers below this amount are ignored        */
    int    chronological; /* 1 = only loops whose hops follow each other in
                             time and close inside window_hours (fast, the
                             laundering pattern); 0 = any loop at all       */
} FdParams;

typedef struct {
    /* --- graph (adjacency list stored in arrays) --- */
    int      n_accounts, n_transfers;
    int      head[FD_MAX_ACCOUNTS];         /* first transfer out of each account */
    Transfer tx[FD_MAX_TRANSFERS];
    double   max_transfer;

    /* --- Tarjan SCC output --- */
    int n_sccs;
    int scc_id[FD_MAX_ACCOUNTS];
    int scc_size[FD_MAX_ACCOUNTS];

    /* --- cycles, sorted best first by fd_analyze --- */
    int   n_cycles;
    int   truncated;      /* 1 = more loops exist than were kept/explored */
    long  steps;          /* DFS edge visits used                         */
    long  step_limit;     /* budget for the current start account         */
    Cycle cycles[FD_MAX_CYCLES];

    /* --- per-account results --- */
    int account_hits[FD_MAX_ACCOUNTS];   /* loops the account appears in   */
    int account_risk[FD_MAX_ACCOUNTS];   /* highest score of those loops   */

    /* --- scratch space for the algorithms --- */
    int index[FD_MAX_ACCOUNTS], low[FD_MAX_ACCOUNTS], on_stack[FD_MAX_ACCOUNTS];
    int stack[FD_MAX_ACCOUNTS], sp, counter;
    int on_path[FD_MAX_ACCOUNTS];
    int path[FD_MAX_CYCLE_LEN];
} FraudGraph;

FdParams fd_default_params(void);
void     fd_init(FraudGraph *g);
int      fd_add_transfer(FraudGraph *g, int from, int to, double amount, double time);

int      fd_find_sccs(FraudGraph *g, const FdParams *p);    /* Tarjan, O(V+E)      */
int      fd_find_cycles(FraudGraph *g, const FdParams *p);  /* bounded DFS         */
int      fd_analyze(FraudGraph *g, const FdParams *p);      /* both + score + sort */

const char *fd_risk_label(int score);

#endif /* FRAUD_H */
