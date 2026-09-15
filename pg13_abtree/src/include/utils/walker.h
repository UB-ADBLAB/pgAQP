/*-------------------------------------------------------------------------
 * 
 * walker.h
 *  Walker's alias structure implementation's header file
 *
 * This is an uint64_t variant of the alias method. Given N items, and N
 * uint64_t weights w_i, assuming the sum of weights S = \sum w_i can be
 * represented as a uint64_t, we assign S / N to each of the N buckets and
 * an additional 1 to each of the first S % N buckets.
 * 
 * src/include/utils/walker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WALKER_H
#define WALKER_H

typedef struct aqp_alias_u64 {
    uint64 *cutoffs;
    uint32 *aliases;
    uint64 Q;
    uint32 R;
    uint32 N;
} aqp_alias_u64;

/*
 * Weights store the initial weights of all items initially, which will be
 * used to store the cutoffs in the constructed array.
 */
aqp_alias_u64 *aqp_alias_u64_create(
    uint64 *weights, /* IN: weights & OUT: cutoff values */
    uint32 N);

/*
 * Destroys and deallocates all allocated space for this alias structure,
 * including the original weights array (the cutoffs).
 */
void aqp_alias_u64_destroy(aqp_alias_u64 *alias);

/*
 * Returns the total item weights in this alias structure.
 */
static inline uint64
aqp_alias_u64_get_sum_weights(aqp_alias_u64 *alias)
{
    return alias->R + alias->Q * alias->N;
}

/*
 * Given a random uint64 x \in [0, alias->S), sample one item from the alias
 * structure.
 */
static inline uint32
aqp_alias_u64_sample_unif_u64(aqp_alias_u64 *alias, uint64 x)
{
    uint64 i, r;
    if (x < alias->R)
    {
        i = x;
        r = alias->Q + 1;
    }
    else
    {
        i = (x - alias->R) / alias->Q;
        r = (x - alias->R) % alias->Q;
    }

    if (r > alias->cutoffs[i])
        return alias->aliases[i];
    return i;
}

/*
 * Given a random real number r \in [0, 1], sample one item from the alias
 * structure.
 */
static inline uint32
aqp_alias_u64_sample_unif_real(aqp_alias_u64 *alias, double r)
{
    uint64 S = aqp_alias_u64_get_sum_weights(alias);
    uint64 x = r * S;
    if (x == S) --x;
    return aqp_alias_u64_sample_unif_u64(alias, x);
}

#endif  /* WALKER_H */

