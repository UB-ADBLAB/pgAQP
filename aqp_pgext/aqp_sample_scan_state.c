/*-------------------------------------------------------------------------
 *
 * aqp_sample_scan_state.c
 *      Common routines to support index/index-only sample scans.
 *
 *-------------------------------------------------------------------------
 */
#include "aqp.h"

#include <executor/executor.h>
#include <utils/builtins.h>
#include <utils/guc.h>

#include "aqp_sample_scan_state.h"

static double high_rejection_rate_threshold_multiplier = 1000.0;

void
aqp_sample_scan_init(AQPSampleScanState *node,
                     PlanState *planstate,
                     Expr *repeatable_expr,
                     uint64 sample_size,
                     Expr *sample_size_expr)
{
    if (!sample_size_expr)
    {
        /* constant sample size, use the one cached in the plan. */
        node->SampleSize = sample_size;
        node->SampleSizeExprState = NULL;
    }
    else
    {
        node->SampleSizeExprState =
            ExecInitExpr(sample_size_expr, planstate);
    }

    if (!repeatable_expr)
    {
        node->Seed = random();
        node->RepeatableExprState = NULL;
    }
    else{
        node->RepeatableExprState =
            ExecInitExpr(repeatable_expr, planstate);
    }

    node -> RescanCount = 0;
    node -> IsFirstStart = true;
    
    /* We do not issue warning until there're at least two attempts. */
    node->HighRejectionRateWarningThreshold =
        Max(sample_size * high_rejection_rate_threshold_multiplier, 1.0);
}

void
aqp_sample_scan_start(AQPSampleScanState *node,
                      ExprContext *econtext)
{
    uint32        seed;
    
    /* We shouldn't restart the sampling unless told so. */
    if (node->RescanForRuntimeKeysOnly) return;

    if (node->SampleSizeExprState) {
        bool            isnull;
        Datum            datum;

        datum = ExecEvalExprSwitchContext(
                    node->SampleSizeExprState,
                    econtext,
                    &isnull);

        if (isnull || DatumGetInt64(datum) < 0)
            ereport(ERROR,
                    errcode(ERRCODE_INVALID_TABLESAMPLE_ARGUMENT),
                    errmsg("TABLESAMPLE SWR() parameter cannot be null or "
                           "negative"));

        node->RemSampleSize = DatumGetInt64(datum);
    }
    else
    {
        node->RemSampleSize = node->SampleSize;
    }
    node->NumIndexAccessAttempted = 0;

    if (node->RepeatableExprState)
    {
        bool            isnull;
        Datum            datum;
        
        datum = ExecEvalExprSwitchContext(
                    node->RepeatableExprState,
                    econtext,
                    &isnull);

        if (isnull)
            ereport(ERROR,
                    errcode(ERRCODE_INVALID_TABLESAMPLE_REPEAT),
                    errmsg("TABLESAMPLE REPEATABLLE parameter cannot be null"));

        seed = DatumGetUInt32(DirectFunctionCall1(hashfloat8, datum));
    }
    else
    {
     //   seed = node->Seed
        if(!node -> IsFirstStart) {
            node -> RescanCount++;
        }
        seed = node -> Seed + node -> RescanCount;
        node -> IsFirstStart = false;
    }

    sampler_random_init_state((long) seed, node->RandState);

    /* 
     * Reset the bit here so that next call from ExecIndexOnlyScan() will
     * not re-initialize the sampler.
     */
    node->RescanForRuntimeKeysOnly = false;
}

void
aqp_sample_scan_reset_rem_samplesize(AQPSampleScanState *node,
                                     uint64 new_rem_samplesize)
{
    node->RemSampleSize = new_rem_samplesize;
    node->HighRejectionRateWarningThreshold =
        Max(new_rem_samplesize * high_rejection_rate_threshold_multiplier, 1.0);
}

