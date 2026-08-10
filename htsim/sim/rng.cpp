#include <cstdlib>
#include <climits>
#include <random>
#include <cstdint>
#ifndef _WIN32
#include <cstdio>
#include <dlfcn.h>
#include <execinfo.h>
#endif

#include "config.h"

using namespace std;

static mt19937 random_engine;

// ---------------------------------------------------------------------------
// glibc-compatible mode for the paper-algorithm port (htsim_uec_paper).
//
// The ATLAHS artifact binaries use glibc's rand()/random() (TYPE_3 additive
// feedback generator, where srand == srandom and rand == random).  This file
// normally overrides those libc symbols with a mt19937 for portability, which
// makes seeded runs draw a completely different sequence than the artifact.
// The paper main opts into an exact reimplementation of the glibc generator;
// every other binary keeps the mt19937 (default off => behavior unchanged).
// ---------------------------------------------------------------------------
static bool glibc_compat = false;
static uint32_t glibc_tbl[34];
static uint64_t glibc_i = 0;

static uint32_t glibc_next()
{
    uint32_t v = glibc_tbl[(glibc_i - 31) % 34] + glibc_tbl[(glibc_i - 3) % 34];
    glibc_tbl[glibc_i % 34] = v;
    ++glibc_i;
    return v >> 1;
}

static void glibc_seed(unsigned seed)
{
    int32_t word = (seed == 0) ? 1 : static_cast<int32_t>(seed);
    glibc_tbl[0] = static_cast<uint32_t>(word);
    for (int i = 1; i < 31; ++i) {
        // word = (16807 * word) % 2147483647 via Schrage, matching glibc.
        int32_t hi = word / 127773;
        int32_t lo = word % 127773;
        word = 16807 * lo - 2836 * hi;
        if (word < 0)
            word += 2147483647;
        glibc_tbl[i] = static_cast<uint32_t>(word);
    }
    for (int i = 31; i < 34; ++i)
        glibc_tbl[i] = glibc_tbl[i - 31];
    glibc_i = 34;
    for (int i = 0; i < 310; ++i)
        (void)glibc_next();
}

void rng_use_glibc_compat()
{
    glibc_compat = true;
    glibc_seed(1); // glibc's un-seeded state == srandom(1)
}

#ifndef _WIN32
// Optional draw tracing for fidelity debugging; active only when the paper
// main enabled compat mode AND HTSIM_RNG_TRACE_N is set.  Inert otherwise.
static FILE* rng_trace_out = nullptr;
static long rng_trace_count = -1;
static long rng_trace_limit = 0;
static bool rng_trace_checked = false;

static void rng_trace(const char* fn, long v)
{
    if (!rng_trace_checked) {
        rng_trace_checked = true;
        const char* n = getenv("HTSIM_RNG_TRACE_N");
        if (n) {
            rng_trace_limit = atol(n);
            const char* f = getenv("HTSIM_RNG_TRACE_FILE");
            rng_trace_out = fopen(f ? f : "rng_trace.txt", "w");
        }
    }
    if (!rng_trace_out || rng_trace_count < 0) return;
    if (rng_trace_count < rng_trace_limit) {
        void* bt[6];
        int nfr = backtrace(bt, 6);
        fprintf(rng_trace_out, "#%ld %s %ld |", rng_trace_count, fn, v);
        for (int i = 2; i < nfr; i++) {
            Dl_info info;
            if (dladdr(bt[i], &info) && info.dli_fbase)
                fprintf(rng_trace_out, " %lx", (unsigned long)((char*)bt[i] - (char*)info.dli_fbase));
            else
                fprintf(rng_trace_out, " %p", bt[i]);
        }
        fprintf(rng_trace_out, "\n");
        if (rng_trace_count == rng_trace_limit - 1) fflush(rng_trace_out);
    }
    rng_trace_count++;
}

static void rng_trace_reseed(const char* fn, unsigned s)
{
    if (!rng_trace_checked) rng_trace("", 0), rng_trace_count = -1;
    if (rng_trace_out) { fprintf(rng_trace_out, "%s(%u)\n", fn, s); rng_trace_count = 0; }
}
#else
static void rng_trace(const char*, long) {}
static void rng_trace_reseed(const char*, unsigned) {}
#endif

#ifdef _WIN32
// POSIX drand48 is not provided by the Windows C runtime. Keep the same
// 48-bit linear-congruential generator and default state used by POSIX so
// simulations do not silently switch to a different random sequence.
static uint64_t drand48_state = UINT64_C(0x1234abcd330e);

void srand48(long seed)
{
    drand48_state = (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 16)
        | UINT64_C(0x330e);
}

double drand48()
{
    constexpr uint64_t mask = (UINT64_C(1) << 48) - 1;
    drand48_state = (UINT64_C(0x5deece66d) * drand48_state + UINT64_C(0xb)) & mask;
    return static_cast<double>(drand48_state) / static_cast<double>(UINT64_C(1) << 48);
}
#endif

void srand(unsigned seed)
{
    if (glibc_compat) {
        glibc_seed(seed);
        rng_trace_reseed("SRAND", seed);
        return;
    }
    random_engine = mt19937(seed);
}

int rand()
{
    if (glibc_compat) {
        int v = static_cast<int>(glibc_next());
        rng_trace("rand", v);
        return v;
    }
    return random_engine() & INT_MAX;
}

void srandom(unsigned seed)
{
    srand(seed);
}

long random()
{
    return rand();
}
