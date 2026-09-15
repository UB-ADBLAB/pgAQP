#include "aqp.h"

#include <catalog/pg_type.h>
#include <utils/array.h>
#include <utils/float.h>

#include "aqp_math.h"

PG_FUNCTION_INFO_V1(aqp_erf_inv_pg);
Datum
aqp_erf_inv_pg(PG_FUNCTION_ARGS)
{
    double y = PG_GETARG_FLOAT8(0);
    double x = aqp_erf_inv(y);
    PG_RETURN_FLOAT8(x);
}

PG_FUNCTION_INFO_V1(aqp_clt_half_ci_finalfunc);
Datum
aqp_clt_half_ci_finalfunc(PG_FUNCTION_ARGS)
{
    ArrayType   *transarray = PG_GETARG_ARRAYTYPE_P(0);
    float8      *transvalue;
    float8      confidence = PG_GETARG_FLOAT8(1);
    float8      nsamples = PG_GETARG_FLOAT8(2);
    float8      Sxx;
    float8      ci;
    
    if (ARR_NDIM(transarray) != 1 ||
        ARR_DIMS(transarray)[0] != 3 ||
        ARR_HASNULL(transarray) ||
        ARR_ELEMTYPE(transarray) != FLOAT8OID)
    {
        elog(ERROR, "aqp_clt_half_ci_finalfunc: expected 3-element float8 array");
    }
    transvalue = (float8 *) ARR_DATA_PTR(transarray);
    
    /* 
     * N = transvalue[0] = num_accepted_samples
     * N' = nsamples
     * Sx = transvalue[1] = \sum_{i = 1}^N Xi = \sum_{i = 1}^N' Xi
     * Sxx = transvalue[2] = \sum_{i = 1}^N (Xi - Sx/N)^2
     *
     * The following is essentially calling float8_combine() over the
     * transarray and another transarray with values {N' - N, 0, 0} where the
     * second transition array corresponds to the aggregated values of the
     * rejected samples. (see backend/utils/adt/float.c).
     */
    if (transvalue[0] == 0.0)
    {
        Sxx = 0.0;
    }
    else
    {
        float8 N2 = nsamples - transvalue[0];
        if (N2 <= 0.0)
        {
            Sxx = transvalue[2];
        }
        else
        {
            float8 tmp;

            /* Sxx1 + Sxx2 + N1 * N2 * (Sx1 / N1 - Sx2 / N2)^2 / (N1 + N2) */
            tmp = transvalue[1] / transvalue[0];
            Sxx = transvalue[2]
                + transvalue[0] * (nsamples - transvalue[0]) / nsamples
                    * tmp * tmp;
            if (unlikely(isinf(Sxx)))
                float_overflow_error();
        }
    }

    if (nsamples <= 1)
        PG_RETURN_NULL();
    
    elog(INFO, "old = %f", Sxx);
    ci = aqp_erf_inv(confidence) *
        sqrt(2 * Sxx / (((float8) nsamples) * (nsamples - 1)));
    PG_RETURN_FLOAT8(ci); 
}

PG_FUNCTION_INFO_V1(aqp_progressive_float8_accum);
Datum
aqp_progressive_float8_accum(PG_FUNCTION_ARGS)
{
    ArrayType  *transarray = PG_GETARG_ARRAYTYPE_P(0);
    float8      newval = PG_GETARG_FLOAT8(1);
	float8		nsamples = PG_GETARG_DATUM(2);
    float8      nbudget = PG_GETARG_DATUM(3);
	float8      ni = PG_GETARG_DATUM(4);
    float8	   *transvalues;
	float8		Sx,
                bx,
                Sxx,
                nb,
                ns,
                nc;

    if (ARR_NDIM(transarray) != 1 ||
            ARR_DIMS(transarray)[0] != 6 ||
            ARR_HASNULL(transarray) ||
            ARR_ELEMTYPE(transarray) != FLOAT8OID)
            elog(ERROR, "aqp_progressive_float8_accum: expected 6-element float8 array");
    
    transvalues = (float8 *) ARR_DATA_PTR(transarray);

	/* n_accepted_samples = transvalues[0]; */ /* num_accepted_samples */
	/*S = transvalues[1];*/ /* \sum_{l=1}^m Xi */
    Sx = transvalues[0]; /* m/(m-1)\sum + 1/(m+1)Xi */
    bx = transvalues[1];
    Sxx = transvalues[2]; /* bx / n11(n11-1) */
    nb = transvalues[3]; /* save for nbudgets */
    ns = transvalues[4]; /* save for nsamples in partition */
    nc = transvalues[5]; /* count 0 value */

	/*
	 * Use the Youngs-Cramer algorithm to incorporate the new value into the
	 * transition values.
	 */

    if (nsamples == 1)
    {
        if(ns != 0)   
        //if (ns != 0 && ns<nb)
        {
            elog(INFO, "nb = %f, bx = %f, ns = %f", nb, bx, ns);
            elog(INFO, "%f", nb * nb * bx / (ns * (ns - 1)));
            elog(INFO, "non-null value count = %f, rho = %f", ns-nc, (ns-nc)/ns);
            Sxx = Sxx + nb*nb * bx / (ns * (ns - 1));
            //Sxx = Sxx +nb*nb*bx;
        }
        //clean up
        Sx = 0;
        bx = 0;
        nc = 0;
    }

    nb = nbudget;
    ns = nsamples;

    if (PG_ARGISNULL(1))
    {
        newval = 0;
        nc ++;
    }

    newval = newval * (ni/nbudget);
    //if (nsamples >1)
        //bx = (nsamples-2)/nsamples *bx + 1/(nsamples*nsamples)*(newval-Sx)*(newval-Sx);
    //else
        //bx = 0;

    //elog(INFO, "bx = %f , Sx = %f, newval = %f", bx, Sx, newval);
    bx += (nsamples-1)/nsamples * (newval - Sx) * (newval - Sx);
    Sx = (nsamples-1)/nsamples * Sx + 1/nsamples * newval;

		/*
		 * Overflow check.  We only report an overflow error when finite
		 * inputs lead to infinite results.  Note also that Sxx should be NaN
		 * if any of the inputs are infinite, so we intentionally prevent Sxx
		 * from becoming infinite.
		 */

        /*NOTE: do not do overflow check now, but should be fixed.*/
		/*if (isinf(Sxx) || isinf(bx) || isinf(Sx))
		{
			if (!isinf(transvalues[1]) && !isinf(newval))
				float_overflow_error();

			bx = get_float8_nan();
		}*/ 

    /*
     * If we're invoked as an aggregate, we can cheat and modify our first
	 * parameter in-place to reduce palloc overhead. Otherwise we construct a
     * new array with the updated transition data and return it.
     */

    if (AggCheckCallContext(fcinfo, NULL))
	{
		transvalues[0] = Sx;
        /*transvalues[2] = Sx;*/
        transvalues[1] = bx;
		transvalues[2] = Sxx;
        transvalues[3] = nb;
        transvalues[4] = ns;
        transvalues[5] = nc;

		PG_RETURN_ARRAYTYPE_P(transarray);
	}
	else
	{
		Datum		transdatums[6];
		ArrayType  *result;

		/*transdatums[0] = Float8GetDatumFast(n_accepted_samples);*/
		transdatums[0] = Float8GetDatumFast(Sx);
		/*transdatums[2] = Float8GetDatumFast(Sx);*/
        transdatums[1] = Float8GetDatumFast(bx);
        transdatums[2] = Float8GetDatumFast(Sxx);
        transdatums[3] = Float8GetDatumFast(nb);
        transdatums[4] = Float8GetDatumFast(ns);
        transdatums[5] = Float8GetDatumFast(nc);

		result = construct_array(transdatums, 6,
								 FLOAT8OID,
								 sizeof(float8), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);

		PG_RETURN_ARRAYTYPE_P(result);
	}
}

PG_FUNCTION_INFO_V1(aqp_progressive_clt_half_ci_finalfunc);
Datum
aqp_progressive_clt_half_ci_finalfunc(PG_FUNCTION_ARGS)
{
    ArrayType   *transarray = PG_GETARG_ARRAYTYPE_P(0);
    float8      *transvalue;
    float8      confidence = PG_GETARG_FLOAT8(1);
    float8      nsamples = PG_GETARG_FLOAT8(2);
    float8      Sxx;
    float8      ci;
    
    if (ARR_NDIM(transarray) != 1 ||
        ARR_DIMS(transarray)[0] != 6 ||
        ARR_HASNULL(transarray) ||
        ARR_ELEMTYPE(transarray) != FLOAT8OID)
    {
        elog(ERROR, "aqp_progressive_clt_half_ci_finalfunc: expected 6-element float8 array");
    }
    transvalue = (float8 *) ARR_DATA_PTR(transarray);
    
    /* 
     * N = transvalue[0] = num_accepted_samples
     * N' = nsamples
     * Sx = transvalue[1] = \sum_{i = 1}^N Xi = \sum_{i = 1}^N' Xi
     * Sxx = transvalue[2] = \sum_{i = 1}^N (Xi - Sx/N)^2
     * 
     * The following is essentially calling float8_combine() over the
     * transarray and another transarray with values {N' - N, 0, 0} where the
     * second transition array corresponds to the aggregated values of the
     * rejected samples. (see backend/utils/adt/float.c).
     */

    elog(INFO, "nb = %f, bx = %f, ns = %f", transvalue[3], transvalue[1], transvalue[4]);
    elog(INFO, "%f", transvalue[3] * transvalue[3] * transvalue[1] / (transvalue[4] * (transvalue[4] - 1)));
    elog(INFO, "non-null value count = %f, rho = %f", transvalue[4]-transvalue[5], (transvalue[4]-transvalue[5])/transvalue[4]);
    Sxx = transvalue[2] + 
        transvalue[3] * transvalue[3] * transvalue[1] / (transvalue[4] * (transvalue[4] - 1));
    //Sxx = transvalue[2] + 
        //transvalue[3]*transvalue[3]*transvalue[1];

    if (nsamples <= 1)
        PG_RETURN_NULL();
    elog(INFO, "Sxx = %f", Sxx);    
    ci = aqp_erf_inv(confidence) * sqrt(2*Sxx/(nsamples*nsamples));
    //ci = aqp_erf_inv(confidence) * sqrt(2*Sxx/(nsamples*nsamples/4));
    //ci = aqp_erf_inv(confidence) * sqrt(2*Sxx/((nsamples-aqp_initial_phase_sample_size)*(nsamples-aqp_initial_phase_sample_size)));
    PG_RETURN_FLOAT8(ci); 
}
