/*
 * bench_tiktoken.c
 *
 * Throughput benchmark for pg_tiktoken_c: tiktoken_count / tiktoken_encode.
 *
 * Compile:
 *   make -C contrib/pg_tiktoken_c/bench
 *   -- or --
 *   gcc -O2 -o bench_tiktoken bench_tiktoken.c \
 *       -I$(pg_config --includedir) -L$(pg_config --libdir) -lpq -lpthread
 *
 * Usage:
 *   ./bench_tiktoken [options]
 *
 * Options:
 *   -h host      PostgreSQL host (default: localhost)
 *   -p port      PostgreSQL port (default: 5432)
 *   -d dbname    Database name   (default: postgres)
 *   -U user      User name       (default: current user)
 *   -e encoding  cl100k_base|o200k_base|r50k_base|p50k_base|all
 *                (default: all)
 *   -f func      count|encode|all  (default: count)
 *   -j threads   Parallel connections (default: 1)
 *   -t seconds   Benchmark duration  (default: 10)
 *   -w seconds   Warmup duration     (default: 2)
 *   -s size      short|medium|long|vlong|all  (default: all)
 *   -x extname   Extension name (default: pg_tiktoken_c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>
#include <math.h>
#include <sys/time.h>

#include <libpq-fe.h>

/* ----------------------------------------------------------------
 * Constants and defaults
 * ---------------------------------------------------------------- */
#define MAX_THREADS     64
#define MAX_LATENCIES   2000000   /* per thread, ~2M samples at 10s */
#define STMT_NAME       "tiktoken_bench"

static const char *DEFAULT_HOST     = "localhost";
static const int   DEFAULT_PORT     = 5432;
static const char *DEFAULT_DBNAME   = "postgres";
static const char *DEFAULT_EXT      = "pg_tiktoken_c";

/* ----------------------------------------------------------------
 * Text cases
 * ---------------------------------------------------------------- */
typedef struct {
    const char *label;
    const char *text;
    int         approx_tokens;  /* rough estimate for display */
} TextCase;

#define REPEAT5(s)   s " " s " " s " " s " " s
#define REPEAT10(s)  REPEAT5(s) " " REPEAT5(s)
#define REPEAT50(s)  REPEAT10(s) " " REPEAT10(s) " " REPEAT10(s) \
                     " " REPEAT10(s) " " REPEAT10(s)

static const TextCase text_cases[] = {
    {
        "short   (~3 tok)",
        "Hello world!",
        3
    },
    {
        "medium  (~60 tok)",
        REPEAT10("PostgreSQL is a powerful open-source relational database."),
        60
    },
    {
        "long   (~500 tok)",
        REPEAT50("The quick brown fox jumps over the lazy dog near the river bank."),
        500
    },
    {
        "vlong (~2000 tok)",
        REPEAT50(REPEAT10("PostgreSQL supports advanced SQL JSONB full-text search and extensions.")),
        2000
    },
    { NULL, NULL, 0 }
};

/* ----------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------- */
typedef struct {
    char host[128];
    int  port;
    char dbname[128];
    char user[128];
    char encoding[64];     /* "all" or specific */
    char func[16];         /* "count", "encode", "all" */
    char size_filter[16];  /* "all", "short", "medium", "long", "vlong" */
    int  duration_sec;
    int  warmup_sec;
    int  n_threads;
    char extname[64];
} Config;

/* ----------------------------------------------------------------
 * Per-thread context and result
 * ---------------------------------------------------------------- */
typedef struct {
    Config      *cfg;
    const char  *encoding;
    const char  *func_name;   /* "tiktoken_count" or "tiktoken_encode" */
    const TextCase *tc;
    int          thread_id;
    /* written by thread */
    uint64_t     n_calls;
    uint64_t     n_tokens;
    double       elapsed_sec;
    double      *latencies_us;  /* microsecond latencies */
    uint64_t     lat_count;
    int          error;
    char         errmsg[256];
} ThreadCtx;

/* ----------------------------------------------------------------
 * Timing helpers
 * ---------------------------------------------------------------- */
static inline double
now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ----------------------------------------------------------------
 * Percentile computation (in-place sort not needed: use order stats)
 * ---------------------------------------------------------------- */
static int
cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double
percentile(double *arr, uint64_t n, double pct)
{
    if (n == 0) return 0.0;
    uint64_t i = (uint64_t)(pct * (double)(n - 1) / 100.0);
    if (i >= n) i = n - 1;
    return arr[i];
}

/* ----------------------------------------------------------------
 * Worker thread
 * ---------------------------------------------------------------- */
static void *
worker_thread(void *arg)
{
    ThreadCtx *ctx = (ThreadCtx *)arg;
    Config    *cfg = ctx->cfg;

    /* Build connection string */
    char connstr[512];
    snprintf(connstr, sizeof(connstr),
             "host=%s port=%d dbname=%s%s",
             cfg->host, cfg->port, cfg->dbname,
             cfg->user[0] ? "" : "");
    if (cfg->user[0])
    {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s user=%s", connstr, cfg->user);
        strncpy(connstr, tmp, sizeof(connstr));
    }

    PGconn *conn = PQconnectdb(connstr);
    if (PQstatus(conn) != CONNECTION_OK)
    {
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "connect failed: %s", PQerrorMessage(conn));
        ctx->error = 1;
        PQfinish(conn);
        return NULL;
    }

    /* Suppress NOTICE messages */
    {
        PGresult *r = PQexec(conn, "SET client_min_messages = WARNING");
        PQclear(r);
    }

    /* Create extension if needed (ignore errors) */
    {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "CREATE EXTENSION IF NOT EXISTS %s", cfg->extname);
        PGresult *r = PQexec(conn, sql);
        PQclear(r);
    }

    /* Prepare statement */
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT %s($1, $2)", ctx->func_name);

    PGresult *pr = PQprepare(conn, STMT_NAME, sql, 0, NULL);
    if (PQresultStatus(pr) != PGRES_COMMAND_OK)
    {
        snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                 "prepare failed: %s", PQerrorMessage(conn));
        ctx->error = 1;
        PQclear(pr);
        PQfinish(conn);
        return NULL;
    }
    PQclear(pr);

    const char *params[2] = { ctx->encoding, ctx->tc->text };

    /* Allocate latency array */
    ctx->latencies_us = malloc(MAX_LATENCIES * sizeof(double));
    if (!ctx->latencies_us)
    {
        ctx->error = 1;
        snprintf(ctx->errmsg, sizeof(ctx->errmsg), "OOM");
        PQfinish(conn);
        return NULL;
    }

    /* ---- Warmup ---- */
    double t_warmup_end = now_sec() + cfg->warmup_sec;
    while (now_sec() < t_warmup_end)
    {
        PGresult *res = PQexecPrepared(conn, STMT_NAME, 2, params,
                                       NULL, NULL, 0);
        PQclear(res);
    }

    /* ---- Benchmark ---- */
    uint64_t calls   = 0;
    uint64_t tokens  = 0;
    uint64_t lat_cnt = 0;

    double t_start = now_sec();
    double t_end   = t_start + cfg->duration_sec;

    while (now_sec() < t_end)
    {
        double t0 = now_sec();
        PGresult *res = PQexecPrepared(conn, STMT_NAME, 2, params,
                                       NULL, NULL, 0);
        double t1 = now_sec();

        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            snprintf(ctx->errmsg, sizeof(ctx->errmsg),
                     "query error: %s", PQerrorMessage(conn));
            ctx->error = 1;
            PQclear(res);
            break;
        }

        /* For tiktoken_count, parse the count to get token throughput */
        if (strcmp(ctx->func_name, "tiktoken_count") == 0 &&
            PQntuples(res) > 0)
        {
            tokens += (uint64_t)atoll(PQgetvalue(res, 0, 0));
        }
        PQclear(res);

        calls++;
        if (lat_cnt < MAX_LATENCIES)
            ctx->latencies_us[lat_cnt++] = (t1 - t0) * 1e6;
    }

    double elapsed = now_sec() - t_start;

    ctx->n_calls    = calls;
    ctx->n_tokens   = tokens;
    ctx->elapsed_sec = elapsed;
    ctx->lat_count  = lat_cnt;

    /* Sort latencies for percentile computation */
    if (lat_cnt > 0)
        qsort(ctx->latencies_us, lat_cnt, sizeof(double), cmp_double);

    PQfinish(conn);
    return NULL;
}

/* ----------------------------------------------------------------
 * Run one benchmark case (encoding × func × text_case)
 * ---------------------------------------------------------------- */
typedef struct {
    double rows_per_sec;
    double tokens_per_sec;
    double lat_p50;
    double lat_p95;
    double lat_p99;
    double lat_max;
    uint64_t total_calls;
} BenchResult;

static int
run_bench(Config *cfg,
          const char *encoding,
          const char *func_name,
          const TextCase *tc,
          BenchResult *out)
{
    pthread_t   threads[MAX_THREADS];
    ThreadCtx   ctxs[MAX_THREADS];
    int         j;

    for (j = 0; j < cfg->n_threads; j++)
    {
        memset(&ctxs[j], 0, sizeof(ctxs[j]));
        ctxs[j].cfg       = cfg;
        ctxs[j].encoding  = encoding;
        ctxs[j].func_name = func_name;
        ctxs[j].tc        = tc;
        ctxs[j].thread_id = j;
    }

    for (j = 0; j < cfg->n_threads; j++)
        pthread_create(&threads[j], NULL, worker_thread, &ctxs[j]);

    for (j = 0; j < cfg->n_threads; j++)
        pthread_join(threads[j], NULL);

    /* Check errors */
    for (j = 0; j < cfg->n_threads; j++)
    {
        if (ctxs[j].error)
        {
            fprintf(stderr, "Thread %d error: %s\n", j, ctxs[j].errmsg);
            for (int k = 0; k < cfg->n_threads; k++)
                free(ctxs[k].latencies_us);
            return -1;
        }
    }

    /* Aggregate */
    uint64_t total_calls  = 0;
    uint64_t total_tokens = 0;
    double   total_elapsed = 0;

    /* Merge all latency arrays for percentiles */
    uint64_t total_lat = 0;
    for (j = 0; j < cfg->n_threads; j++)
        total_lat += ctxs[j].lat_count;

    double *all_lat = malloc(total_lat * sizeof(double));
    uint64_t off = 0;
    for (j = 0; j < cfg->n_threads; j++)
    {
        total_calls   += ctxs[j].n_calls;
        total_tokens  += ctxs[j].n_tokens;
        total_elapsed += ctxs[j].elapsed_sec;
        if (ctxs[j].lat_count > 0)
        {
            memcpy(all_lat + off, ctxs[j].latencies_us,
                   ctxs[j].lat_count * sizeof(double));
            off += ctxs[j].lat_count;
        }
        free(ctxs[j].latencies_us);
    }

    double avg_elapsed = total_elapsed / cfg->n_threads;

    qsort(all_lat, off, sizeof(double), cmp_double);

    out->total_calls    = total_calls;
    out->rows_per_sec   = (avg_elapsed > 0)
                          ? (double)total_calls / avg_elapsed : 0;
    out->tokens_per_sec = (avg_elapsed > 0)
                          ? (double)total_tokens / avg_elapsed : 0;
    out->lat_p50 = percentile(all_lat, off, 50.0);
    out->lat_p95 = percentile(all_lat, off, 95.0);
    out->lat_p99 = percentile(all_lat, off, 99.0);
    out->lat_max = (off > 0) ? all_lat[off - 1] : 0.0;

    free(all_lat);
    return 0;
}

/* ----------------------------------------------------------------
 * Pretty printing
 * ---------------------------------------------------------------- */
static void
print_header(const char *func_name, const char *encoding, int threads,
             int duration, int warmup)
{
    printf("\n");
    printf("┌─────────────────────────────────────────────────────"
           "───────────────────────────────────────────┐\n");
    printf("│  Function: %-16s  Encoding: %-14s  "
           "Threads: %-3d  Duration: %ds  Warmup: %ds  │\n",
           func_name, encoding, threads, duration, warmup);
    printf("├──────────────────┬───────────┬────────────────┬──────────"
           "┬──────────┬──────────┬──────────┤\n");
    printf("│ %-16s │ %9s │ %14s │ %8s │ %8s │ %8s │ %8s │\n",
           "Text Size", "rows/sec", "tokens/sec",
           "p50 µs", "p95 µs", "p99 µs", "max µs");
    printf("├──────────────────┼───────────┼────────────────┼──────────"
           "┼──────────┼──────────┼──────────┤\n");
}

static void
print_row(const TextCase *tc, const BenchResult *r)
{
    char tok_buf[32];
    if (r->tokens_per_sec > 0)
        snprintf(tok_buf, sizeof(tok_buf), "%14.0f", r->tokens_per_sec);
    else
        snprintf(tok_buf, sizeof(tok_buf), "%14s", "n/a");

    printf("│ %-16s │ %9.0f │ %s │ %8.1f │ %8.1f │ %8.1f │ %8.1f │\n",
           tc->label,
           r->rows_per_sec,
           tok_buf,
           r->lat_p50,
           r->lat_p95,
           r->lat_p99,
           r->lat_max);
}

static void
print_footer(void)
{
    printf("└──────────────────┴───────────┴────────────────┴──────────"
           "┴──────────┴──────────┴──────────┘\n");
}

/* ----------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------- */
static void
usage(const char *prog)
{
    fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"Connection options:\n"
"  -h HOST      host (default: localhost)\n"
"  -p PORT      port (default: 5432)\n"
"  -d DBNAME    database (default: postgres)\n"
"  -U USER      username\n"
"\n"
"Benchmark options:\n"
"  -e ENC       encoding: cl100k_base|o200k_base|r50k_base|p50k_base|all\n"
"               (default: all)\n"
"  -f FUNC      function: count|encode|all  (default: count)\n"
"  -s SIZE      text size: short|medium|long|vlong|all  (default: all)\n"
"  -j THREADS   parallel connections (default: 1)\n"
"  -t SECS      benchmark duration in seconds (default: 10)\n"
"  -w SECS      warmup duration in seconds (default: 2)\n"
"  -x EXTNAME   extension name (default: pg_tiktoken_c)\n"
"\n",
            prog);
}

/* ----------------------------------------------------------------
 * main
 * ---------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    Config cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.host,     DEFAULT_HOST,  sizeof(cfg.host) - 1);
    cfg.port = DEFAULT_PORT;
    strncpy(cfg.dbname,   DEFAULT_DBNAME, sizeof(cfg.dbname) - 1);
    strncpy(cfg.encoding, "all",          sizeof(cfg.encoding) - 1);
    strncpy(cfg.func,     "count",        sizeof(cfg.func) - 1);
    strncpy(cfg.size_filter, "all",       sizeof(cfg.size_filter) - 1);
    strncpy(cfg.extname,  DEFAULT_EXT,    sizeof(cfg.extname) - 1);
    cfg.duration_sec = 10;
    cfg.warmup_sec   = 2;
    cfg.n_threads    = 1;

    int c;
    while ((c = getopt(argc, argv, "h:p:d:U:e:f:s:j:t:w:x:?")) != -1)
    {
        switch (c) {
        case 'h': strncpy(cfg.host,    optarg, sizeof(cfg.host)-1);    break;
        case 'p': cfg.port = atoi(optarg);                              break;
        case 'd': strncpy(cfg.dbname,  optarg, sizeof(cfg.dbname)-1);  break;
        case 'U': strncpy(cfg.user,    optarg, sizeof(cfg.user)-1);    break;
        case 'e': strncpy(cfg.encoding,optarg, sizeof(cfg.encoding)-1);break;
        case 'f': strncpy(cfg.func,    optarg, sizeof(cfg.func)-1);    break;
        case 's': strncpy(cfg.size_filter,optarg,sizeof(cfg.size_filter)-1); break;
        case 'j': cfg.n_threads   = atoi(optarg);                      break;
        case 't': cfg.duration_sec = atoi(optarg);                     break;
        case 'w': cfg.warmup_sec   = atoi(optarg);                     break;
        case 'x': strncpy(cfg.extname, optarg, sizeof(cfg.extname)-1); break;
        default:  usage(argv[0]); return 1;
        }
    }

    if (cfg.n_threads < 1)  cfg.n_threads = 1;
    if (cfg.n_threads > MAX_THREADS) cfg.n_threads = MAX_THREADS;

    /* Encoding list */
    static const char *all_encs[] = {
        "cl100k_base", "o200k_base", "r50k_base", "p50k_base", NULL
    };
    const char *enc_list[8] = { NULL };
    int n_enc = 0;
    if (strcmp(cfg.encoding, "all") == 0)
        for (int i = 0; all_encs[i]; i++) enc_list[n_enc++] = all_encs[i];
    else
        enc_list[n_enc++] = cfg.encoding;

    /* Function list */
    static const char *all_funcs[] = {
        "tiktoken_count", "tiktoken_encode", NULL
    };
    const char *func_list[4] = { NULL };
    int n_func = 0;
    if (strcmp(cfg.func, "all") == 0)
        for (int i = 0; all_funcs[i]; i++) func_list[n_func++] = all_funcs[i];
    else if (strcmp(cfg.func, "count") == 0)
        func_list[n_func++] = "tiktoken_count";
    else if (strcmp(cfg.func, "encode") == 0)
        func_list[n_func++] = "tiktoken_encode";
    else {
        fprintf(stderr, "Unknown function: %s\n", cfg.func);
        return 1;
    }

    printf("pg_tiktoken_c throughput benchmark\n");
    printf("Host: %s:%d  DB: %s  Threads: %d  Duration: %ds  Warmup: %ds\n",
           cfg.host, cfg.port, cfg.dbname,
           cfg.n_threads, cfg.duration_sec, cfg.warmup_sec);

    for (int fi = 0; fi < n_func; fi++)
    {
        const char *fn = func_list[fi];
        for (int ei = 0; ei < n_enc; ei++)
        {
            const char *enc = enc_list[ei];
            print_header(fn, enc, cfg.n_threads,
                         cfg.duration_sec, cfg.warmup_sec);

            for (int ti = 0; text_cases[ti].label; ti++)
            {
                const TextCase *tc = &text_cases[ti];

                /* size filter */
                if (strcmp(cfg.size_filter, "all") != 0)
                {
                    if (strcmp(cfg.size_filter, "short")  == 0 && ti != 0) continue;
                    if (strcmp(cfg.size_filter, "medium") == 0 && ti != 1) continue;
                    if (strcmp(cfg.size_filter, "long")   == 0 && ti != 2) continue;
                    if (strcmp(cfg.size_filter, "vlong")  == 0 && ti != 3) continue;
                }

                fprintf(stderr, "  running %-16s / %-14s / %-16s …\n",
                        fn, enc, tc->label);
                fflush(stderr);

                BenchResult r;
                int rc = run_bench(&cfg, enc, fn, tc, &r);
                if (rc != 0)
                {
                    printf("│ %-16s │ ERROR                                   "
                           "                                          │\n",
                           tc->label);
                    continue;
                }
                print_row(tc, &r);
            }
            print_footer();
        }
    }

    return 0;
}
