#include "postgres.h"

#include "nodes/pg_list.h"
#include "utils/walker.h"


static inline uint64 aqp_get_bucket_weight(uint64 Q, uint64 R, uint32 i)
{
    return (i < R) ? (Q + 1) : Q;
}

aqp_alias_u64 *
aqp_alias_u64_create(
    uint64 *weights,
    uint32 N)
{
    aqp_alias_u64 *alias;
    uint64 S, Q;
    uint32 R;
    uint32 *aliases = palloc(sizeof(uint32) * N);
    uint32 *remitems = palloc(sizeof(uint32) * N);
    uint32 i, w, w2;
    uint32 pi, ni;
    
    S = 0;
    for (i = 0; i < N; ++i)
    {
        S += weights[i];
    }

    Q = S / N;
    R = S % N;

    memset(aliases, 0xff, sizeof(uint32) * N);
    
    pi = 0;
    ni = N;
    for (i = 0; i < N; ++i)
    {
        w = aqp_get_bucket_weight(Q, R, i);
        if (weights[i] < w)
        {
            remitems[pi++] = i;
        }
        else if (weights[i] > w)
        {
            remitems[--ni] = i;
        }
    }

    while (pi > 0)
    {
        uint32 i, j;
        Assert(ni < N);
        
        i = remitems[pi - 1];
        j = remitems[ni];

        w = aqp_get_bucket_weight(Q, R, i);
        w2 = aqp_get_bucket_weight(Q, R, j);

        Assert(weights[i] < w);
        Assert(weights[j] > w2);

        w = w - weights[i]; 
        /*
         * This is always true because (1) the max{w} = Q + 1 iff an underfull
         * item happens to be zero weight and falls into a bucket with a weight
         * of Q + 1; (2) min{weights[j]} = Q + 1 iff an overfull item happens
         * to have weight Q + 1 and falls into a bucket with a weight of Q.
         */
        Assert(w <= weights[j]);

        Assert(aliases[i] = 0xffffffffu);
        aliases[i] = j;
        weights[j] -= w;

        if (weights[j] < w2)
        {
            ++ni;
            remitems[pi - 1] = j;
        }
        else if (weights[j] == w2)
        {
            ++ni;
            --pi;
        }
        else
        {
            /* otherwise, item j remains as an overfull item */
            --pi;
        }
    }
    Assert(ni == N);

    pfree(remitems);
    
    alias = (aqp_alias_u64 *) palloc(sizeof(aqp_alias_u64));    
    alias->cutoffs = weights;
    alias->aliases = aliases;
    alias->Q = Q;
    alias->R = R;
    alias->N = N;
    return alias;
}

void
aqp_alias_u64_destroy(aqp_alias_u64 *alias)
{
    pfree(alias->cutoffs);
    pfree(alias->aliases);
    pfree(alias);
}

