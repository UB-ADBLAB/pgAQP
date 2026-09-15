#ifndef AQP_SAMPLEPATH_H
#define AQP_SAMPLEPATH_H

#include "aqp.h"

#include <nodes/extensible.h>
#include <nodes/pathnodes.h>

typedef struct AQPSWRPath {
    CustomPath      cpath;

    IndexOptInfo    *indexinfo;
    List            *indexclauses;
    bool            index_only_scan;

    uint64          sample_size;
    Expr            *repeatable_expr;
    Expr            *sample_size_expr;

    Cost            indextotalcost;
    Selectivity     indexselectivity;
} AQPSWRPath;


typedef struct AQPPSWRPath {
    CustomPath      cpath;

    List            *swrpaths;

    uint64          sample_size;
    Expr            *repeatable_expr;
    Expr            *sample_size_expr;

    Cost            indextotalcost;
    Selectivity     indexselectivity;
} AQPPSWRPath;

extern void aqp_fix_swrscan_exprs(Plan *stmt);
extern void aqp_fix_swrscan(CustomScan *cscan);
extern void aqp_fix_swrindexonlyscan(CustomScan *cscan);
extern void aqp_fix_progressive_swrscan(CustomScan *cscan);

#endif  /* AQP_SAMPLEPATH_H */

