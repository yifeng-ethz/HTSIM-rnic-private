#include <cstdlib>
#include <climits>
#include <random>
#include <cstdint>

#include "config.h"

using namespace std;

static mt19937 random_engine;

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
    random_engine = mt19937(seed);
}

int rand()
{
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
