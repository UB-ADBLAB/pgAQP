/*
* $Id: rnd.c,v 1.7 2006/07/31 17:23:09 jms Exp $
*
* Revision History
* ===================
* $Log: rnd.c,v $
* Revision 1.7  2006/07/31 17:23:09  jms
* fix to parallelism problem
*
* Revision 1.6  2005/10/25 17:26:38  jms
* check in integration between microsoft changes and baseline code
*
* Revision 1.5  2005/10/14 23:16:54  jms
* fix for answer set compliance
*
* Revision 1.4  2005/09/23 22:29:35  jms
* fix to assume 64b support in the 32b RNG calls. Should speed generation, and corrects a problem with FK between Customer and Orders
*
* Revision 1.3  2005/03/04 21:43:23  jms
* correct segfult in random()
*
* Revision 1.2  2005/01/03 20:08:59  jms
* change line terminations
*
* Revision 1.1.1.1  2004/11/24 23:31:47  jms
* re-establish external server
*
* Revision 1.7  2004/04/08 17:34:15  jms
* cleanup SOLARIS/SUN ifdefs; now all use SUN
*
* Revision 1.6  2004/03/26 20:22:56  jms
* correct Solaris header
*
* Revision 1.5  2004/03/02 20:50:50  jms
* MP/RAS porting changes
*
* Revision 1.4  2004/02/18 16:37:33  jms
* add int32_t for solaris
*
* Revision 1.3  2004/02/18 16:26:49  jms
* 32/64 bit changes for overflow handling needed additional changes when ported back to windows
*
* Revision 1.2  2004/02/18 16:17:32  jms
* add 32bit specific changes to UnifInt
*
* Revision 1.1.1.1  2003/08/08 21:50:34  jms
* recreation after CVS crash
*
* Revision 1.3  2003/08/08 21:35:26  jms
* first integration of rng64 for o_custkey and l_partkey
*
* Revision 1.2  2003/08/07 17:58:34  jms
* Convery RNG to 64bit space as preparation for new large scale RNG
*
* Revision 1.1.1.1  2003/04/03 18:54:21  jms
* initial checkin
*
*
*/
/* 
 * RANDOM.C -- Implements Park & Miller's "Minimum Standard" RNG
 * 
 * (Reference:  CACM, Oct 1988, pp 1192-1201)
 * 
 * NextRand:  Computes next random integer
 * UnifInt:   Yields an long uniformly distributed between given bounds 
 * UnifReal: ields a real uniformly distributed between given bounds   
 * Exponential: Yields a real exponentially distributed with given mean
 * 
 */

#include "config.h"
#include <stdio.h>
#include <math.h>
# include <string.h>
# include <stdlib.h>
#ifdef LINUX
#include <stdint.h>
#endif
#ifdef IBM
#include <inttypes.h>
#endif
#ifdef SUN
#include <inttypes.h>
#endif
#ifdef ATT
#include <sys/bitypes.h>
#endif
#ifdef WIN32
#define int32_t	__int32
#endif
#include "dss.h"
#include "rnd.h" 
#include "rnglib.h"
/*ranlib.h & rnglib.h adopted from https://people.sc.fsu.edu/~jburkardt/c_src/ranlib/ranlib.html*/


char *env_config PROTO((char *tag, char *dflt));
void NthElement(DSS_HUGE, DSS_HUGE *);

void
dss_random(DSS_HUGE *tgt, DSS_HUGE lower, DSS_HUGE upper, long stream, DSS_HUGE numtuples)
{
	*tgt = skew_zipf_factor == 0 ? UnifInt(lower, upper, stream) : ZipfInt(lower, upper, stream, numtuples);
	Seed[stream].usage += 1;

	return;
}

void dss_random_unif(DSS_HUGE* tgt, DSS_HUGE lower, DSS_HUGE upper, long stream)
{
	*tgt = UnifInt(lower, upper, stream);
	Seed[stream].usage += 1;

	return;
}

void
row_start(int t)	\
{
	int i;
	for (i=0; i <= MAX_STREAM; i++) 
		Seed[i].usage = 0 ; 
	
	return;
}

void
row_stop(int t)	\
	{ 
	int i;
	
	/* need to allow for handling the master and detail together */
	if (t == ORDER_LINE)
		t = ORDER;
	if (t == PART_PSUPP)
		t = PART;
	
	for (i=0; i <= MAX_STREAM; i++)
		if ((Seed[i].table == t) || (Seed[i].table == tdefs[t].child))
			{ 
			if (set_seeds && (Seed[i].usage > Seed[i].boundary))
				{
				fprintf(stderr, "\nSEED CHANGE: seed[%d].usage = %I64d\n", 
					i, Seed[i].usage); 
				Seed[i].boundary = Seed[i].usage;
				} 
			else 
				{
				NthElement((Seed[i].boundary - Seed[i].usage), &Seed[i].value);
#ifdef RNG_TEST
				Seed[i].nCalls += Seed[i].boundary - Seed[i].usage;
#endif
				}
			} 
		return;
	}

void
dump_seeds(int tbl)
{
	int i;

	for (i=0; i <= MAX_STREAM; i++)
		if (Seed[i].table == tbl)
#ifdef RNG_TEST
			printf("%d(%I64d):\t%I64d\n", i, Seed[i].nCalls, Seed[i].value);
#else
			printf("%d:\t%I64d\n", i, Seed[i].value);
#endif
	return;
}

/******************************************************************

   NextRand:  Computes next random integer

*******************************************************************/

/*
 * long NextRand( long nSeed )
 */
DSS_HUGE
NextRand(DSS_HUGE nSeed)

/*
 * nSeed is the previous random number; the returned value is the 
 * next random number. The routine generates all numbers in the 
 * range 1 .. nM-1.
 */

{
	nSeed = (nSeed * 16807) % 2147483647;
    return (nSeed);
}

/******************************************************************

   UnifInt:  Yields an long uniformly distributed between given bounds

*******************************************************************/

/*
 * long UnifInt( long nLow, long nHigh, long nStream )
 */
DSS_HUGE
UnifInt(DSS_HUGE nLow, DSS_HUGE nHigh, long nStream)

/*
 * Returns an integer uniformly distributed between nLow and nHigh, 
 * including * the endpoints.  nStream is the random number stream.   
 * Stream 0 is used if nStream is not in the range 0..MAX_STREAM.
 */

{
    double          dRange;
    DSS_HUGE            nTemp,
		nRange;
    int32_t	nLow32 = (int32_t)nLow,
		nHigh32 = (int32_t)nHigh;
	
    if (nStream < 0 || nStream > MAX_STREAM)
        nStream = 0;
	
	if ((nHigh == MAX_LONG) && (nLow == 0))
	{
		dRange = dM; //  DOUBLE_CAST(nHigh32 - nLow32 + 1); // SK: this overflows; wtf? why not just dM
		nRange = nHigh32 - nLow32 + 1;
	}
	else
	{
		dRange = DOUBLE_CAST (nHigh - nLow + 1);
		nRange = nHigh - nLow + 1;
	}

    Seed[nStream].value = NextRand(Seed[nStream].value);
#ifdef RNG_TEST
	Seed[nStream].nCalls += 1;
#endif
	nTemp = (DSS_HUGE) (((double) Seed[nStream].value / dM) * (dRange));
	if (nTemp < 0) nTemp *= -1;

	DSS_HUGE retval = nTemp+nLow;
	if (retval < nLow || retval > nHigh)
	{
		fprintf(stderr, "WARN! UnifInt is croaking!\n");
	}
    return (nLow + nTemp);
}


int FindAmongSortedValues(struct ZipfMetaData *zmd, DSS_HUGE val)
{
	int lowRank = 0;
	int highRank = zmd->numRanksUsed - 1;
	
	while (lowRank <= highRank)
	{
		int rankToTry = (lowRank + highRank) / 2;

		if (zmd->sortedValues[rankToTry] == val) return 1;
		else if (zmd->sortedValues[rankToTry] < val) lowRank = rankToTry + 1;
		else highRank = rankToTry - 1;
	}

	return 0;
}

int compare_dss_huge(const void* left, const void* right)
{
	DSS_HUGE l = *((DSS_HUGE*)left);
	DSS_HUGE r = *((DSS_HUGE*)right);
	return (l - r) < 0 ? -1 : (l == r ? 0 : 1);
}

void dss_setup_zipf(struct zdef curr_zdef)
{
	DSS_HUGE nLow = curr_zdef.nLow, nHigh = curr_zdef.nHigh, numtuples = curr_zdef.numtuples;
	long nStream = curr_zdef.seed;

	struct ZipfMetaData* zmd = &zmdPerStream[nStream];

	fprintf(zipf_debug_file, "|---------\n| Setting zipf manifest for stream %ld\n|-------\n", nStream);
	if (nHigh < nLow || numtuples <= 0)
	{
		fprintf(zipf_debug_file, "-- ERR invalid inputs; high %I64d low %I64d #tuples %I64d\n", nHigh, nLow, numtuples);
		exit(2);
	}

	DSS_HUGE num_dv = nHigh - nLow + 1;
	DSS_HUGE rank_at_which_weight_below_epsilon = (DSS_HUGE)ceil(pow(skew_zmd_epsilon, -1.0 / skew_zipf_factor));
	DSS_HUGE numranks_dh = MIN(MIN(num_dv, numtuples), rank_at_which_weight_below_epsilon);

	if (rank_at_which_weight_below_epsilon < 0 || rank_at_which_weight_below_epsilon >(1 << 30))
	{
		fprintf(zipf_debug_file, "-- WARN: ranks to consider before prob dips below pre-defined constant (ZMD_EPSILON= %g) is too large or overflowing 64 bits (= %I64d). If you want such a low zipfian skew, increase ZMD_EPSILON.",
			skew_zmd_epsilon, rank_at_which_weight_below_epsilon);
		exit(2);
	}

	if (MIN(num_dv, numtuples) > rank_at_which_weight_below_epsilon)
	{
		fprintf(zipf_debug_file, "-- WARN: ranks only limited by epsilon; note that we only approx zipf here. #tuples= %I64d #dv= %I64d #beloweps= %I64d\n", 
			numtuples, num_dv, rank_at_which_weight_below_epsilon);
	}

	if (numranks_dh > (1 << 30))
	{
		fprintf(zipf_debug_file, "-- ERR too many ranks; #= %I64d", numranks_dh);
		exit(2);
	}

	int numranks = (int)numranks_dh;

	double denominator = 0;
	int rank;
	// TODO: is there a closed-form expression for denominator?
	for (rank = 1; rank <= numranks; rank++)
	{
		double weight = pow(1.0 / rank, skew_zipf_factor);
		denominator += weight;

		if (weight < skew_zmd_epsilon) break;
		if (rank < NumTopRanksPerStream)
			zmd->weights[rank] = weight;
	}

	zmd->numRanksUsed = (rank < NumTopRanksPerStream ? rank - 1 : NumTopRanksPerStream - 1);	
    double tot_prob = 0;
	for (rank = 1; rank <= zmd->numRanksUsed; rank++)
	{
		zmd->weights[rank] /= denominator;
		tot_prob += zmd->weights[rank];
	}

	zmd->weights[0] = (1 - tot_prob); // probability outside of the top few ranks
	zmd->initNHigh = nHigh;
	zmd->initNLow = nLow;
	zmd->numTuples = numtuples;

	if (zmd->numRanksUsed == num_dv)
	{
		// All distinct values have ranks; take them all in and then permute randomly
		for (rank = 1; rank <= zmd->numRanksUsed; rank++)
			zmd->valuesAtRank[rank] = nLow + rank - 1;

		// permute
		for (rank = 1; rank < zmd->numRanksUsed; rank++)
		{
			DSS_HUGE swapWith;
			RANDOM_unif(swapWith, (DSS_HUGE)rank, (DSS_HUGE)zmd->numRanksUsed, (long)nStream);
			if (swapWith != rank)
			{
				DSS_HUGE temp = zmd->valuesAtRank[rank];
				zmd->valuesAtRank[rank] = zmd->valuesAtRank[swapWith];
				zmd->valuesAtRank[swapWith] = temp;
			}
		}
	}
	else
	{
		// Randomly pick a few values to have ranks
		// TODO: this is a bit sloppy; consider implementing a reservoir so that it will finish in one full pass
		// The following can be faster than a full pass but runs the risk of taking a long time when #dv is
		// just slightly larger than the #ranks needed
		//
		for (rank = 1; rank <= zmd->numRanksUsed; rank++)
		{
			int found_new_val;
			DSS_HUGE val;
			do
			{
				RANDOM_unif(val, (DSS_HUGE)nLow, (DSS_HUGE)nHigh, (long)nStream);
				if (val < nLow || val > nHigh)
				{
					fprintf(stderr, "-- ERR UnifInt croaks low= %I64d high= %I64d val= %I64d\n", nLow, nHigh, val);
					exit(2);
				}

				found_new_val = 1;
				for (int subrank = 1; subrank < rank; subrank++)
					if (zmd->valuesAtRank[subrank] == val)
					{
						found_new_val = 0;
						break;
					}
			} 
			while (found_new_val == 0);
			
			zmd->valuesAtRank[rank] = val;
		}
	}

	if (zmd->numRanksUsed == numtuples || zmd->numRanksUsed == num_dv)
		zmd->weights[0] = 0.0; // to protect against double trouble

	for (rank = 1; rank <= zmd->numRanksUsed; rank++)
	{
		DSS_HUGE val = zmd->valuesAtRank[rank];
		zmd->sortedValues[rank - 1] = val;
	}
	qsort(&zmd->sortedValues, zmd->numRanksUsed, sizeof(DSS_HUGE), compare_dss_huge);

	fprintf(zipf_debug_file, "-- set #ranks= %d low= %I64d high= %I64d remProb= %f\n", 
		zmd->numRanksUsed, zmd->initNLow, zmd->initNHigh, zmd->weights[0]);

	for (int r = 1; r <= zmd->numRanksUsed; r++)
		fprintf(zipf_debug_file, "\t [%d]: val= %I64d wgt= %f\n", r, zmd->valuesAtRank[r], (float)zmd->weights[r]);
}

DSS_HUGE
ZipfInt(DSS_HUGE nLow, DSS_HUGE nHigh, long nStream, DSS_HUGE numtuples)
{
	if (nStream < 0 || nStream > MAX_STREAM)
		nStream = 0;

	struct ZipfMetaData* zmd = &zmdPerStream[nStream];
	if (zmd->initNHigh != nHigh || zmd->initNLow != nLow || zmd->numTuples != numtuples)
	{
		fprintf(zipf_debug_file, 
			"-- ERR stream %ld not initialized correctly; high %I64d !=? %I64d, low %I64d !=? %I64d numtuples %I64d !=? %I64d", 
			nStream, zmd->initNHigh, nHigh, zmd->initNLow, nLow, zmd->numTuples, numtuples);
		exit(2);
	}

	Seed[nStream].value = NextRand(Seed[nStream].value); // usage is accounted for in dss_random()
#ifdef RNG_TEST
	Seed[nStream].nCalls += 1;
#endif

	num_zipf_rand_calls[nStream] += 1;

	double rv = ((double)Seed[nStream].value / dM);
	if (rv < 0) rv *= -1;
	double probThusFar = 0;
	int foundSample = 0;
	DSS_HUGE sampleVal = 0;
	for (int rank = 0; rank <= zmd->numRanksUsed; rank++)
	{
		probThusFar += zmd->weights[rank];
		if (rv > probThusFar) continue;

		if (rank > 0)
		{
			// read off the manifest
			sampleVal = zmd->valuesAtRank[rank];
		}
		else
		{
			num_zipf_rand_calls_out_of_manifesto[nStream] += 1;

			// pick a random value that is not in the manifest
			double dRange = ((nHigh == MAX_LONG) && (nLow == 0)) ? dM : DOUBLE_CAST(nHigh - nLow + 1);

			int num_tries = 5;
			DSS_HUGE val;
			do {
				num_tries--;
				Seed[nStream].value = NextRand(Seed[nStream].value);
				Seed[nStream].usage += 1; // so that the usage is marked
#ifdef RNG_TEST
				Seed[nStream].nCalls += 1;
#endif
				double rv = ((double)Seed[nStream].value / dM);
				if (rv < 0) rv *= -1;
				val = nLow + (DSS_HUGE)(rv * dRange);
			} while (num_tries > 0 && FindAmongSortedValues(zmd, val) == 1);

			if (num_tries == 0)
				num_zipf_rand_calls_out_of_manifesto_give_up[nStream] += 1;

			sampleVal = val;
		}

		break;
	}

	return sampleVal;
}

double
zipfIntProb(DSS_HUGE val, long nStream)
{
    int i;
	struct ZipfMetaData* zmd = &zmdPerStream[nStream];

    if (val < zmd->initNLow || val > zmd->initNHigh)
        return 0.0;

    for (i = 1; i <= zmd->numRanksUsed; ++i) {
        if (zmd->valuesAtRank[i] == val)
        {
            return zmd->weights[i];
        }
    }
    
    return zmd->weights[0] / (zmd->initNHigh - zmd->initNLow + 1 - zmd->numRanksUsed);
}

int compare_sdate(const void* left, const void* right, void *cxt)
{
    double *p = (double *) cxt;
    int l = *(int *) left - STARTDATE;
    int r = *(int *) right - STARTDATE;

	return (p[l] - p[r]) > 0 ? -1 : (p[l] == p[r] ? 0 : 1);
}

void sdate_rank_sort(int *sdate, double *pr)
{
    for (int i = O_ODATE_MIN; i <= O_ODATE_MAX; i++)
    {
        for (int j = L_SDTE_MIN; j<= L_SDTE_MAX; j++)
        {
            pr[i + j - STARTDATE] += 
                zipfIntProb(i, O_ODATE_SD) * zipfIntProb(j, L_SDTE_SD);
        }
    }

    qsort_r(sdate, TOTDATE, sizeof(int), compare_sdate, pr);
    
/*    for (int i = 0; i <= 200; i++)
    {
        printf("%d\n", sdate[i]);
    }*/
}

void dss_random_var(float av, float sd, DSS_HUGE *tgt, DSS_HUGE nLow, DSS_HUGE nHigh)
{
	float r;
	DSS_HUGE d;
	do {
		r = gennor(av, sd);
		d = round(r);
	} while (r<nLow || r>nHigh);
	*tgt = d;
	//Seed[stream].usage += 1;
}


float gennor ( float av, float sd)

/******************************************************************************/
/*
  Purpose:

    GENNOR generates a normal random deviate.

  Discussion:

    This procedure generates a single random deviate from a normal distribution
    with mean AV, and standard deviation SD.

  Licensing:

    This code is distributed under the GNU LGPL license.

  Modified:

    01 April 2013

  Author:

    Original FORTRAN77 version by Barry Brown, James Lovato.
    C version by John Burkardt.

  Reference:

    Joachim Ahrens, Ulrich Dieter,
    Extensions of Forsythe's Method for Random
    Sampling from the Normal Distribution,
    Mathematics of Computation,
    Volume 27, Number 124, October 1973, page 927-937.

  Parameters:

    Input, float AV, the mean.

    Input, float SD, the standard deviation.

    Output, float GENNOR, a random deviate from the distribution.
*/
{
  float value;

  value = sd * snorm ( ) + av;

  return value;
}
/******************************************************************************/

float snorm ( )

/******************************************************************************/
/*
  Purpose:

    SNORM samples the standard normal distribution.

  Discussion:

    This procedure corresponds to algorithm FL, with M = 5, in the reference.

  Licensing:

    This code is distributed under the GNU LGPL license.

  Modified:

    01 April 2013

  Author:

    Original FORTRAN77 version by Barry Brown, James Lovato.
    C version by John Burkardt.

  Reference:

    Joachim Ahrens, Ulrich Dieter,
    Extensions of Forsythe's Method for Random
    Sampling from the Normal Distribution,
    Mathematics of Computation,
    Volume 27, Number 124, October 1973, page 927-937.

  Parameters:

    Output, float SNORM, a random deviate from the distribution.
*/
{
  const float a[32] = {
        0.0000000, 0.3917609E-01, 0.7841241E-01, 0.1177699, 
        0.1573107, 0.1970991,     0.2372021,     0.2776904, 
        0.3186394, 0.3601299,     0.4022501,     0.4450965, 
        0.4887764, 0.5334097,     0.5791322,     0.6260990, 
        0.6744898, 0.7245144,     0.7764218,     0.8305109, 
        0.8871466, 0.9467818,     1.009990,      1.077516, 
        1.150349,  1.229859,      1.318011,      1.417797, 
        1.534121,  1.675940,      1.862732,      2.153875 };
  float aa;
  const float d[31] = {
        0.0000000, 0.0000000, 0.0000000, 0.0000000, 
        0.0000000, 0.2636843, 0.2425085, 0.2255674, 
        0.2116342, 0.1999243, 0.1899108, 0.1812252, 
        0.1736014, 0.1668419, 0.1607967, 0.1553497, 
        0.1504094, 0.1459026, 0.1417700, 0.1379632, 
        0.1344418, 0.1311722, 0.1281260, 0.1252791, 
        0.1226109, 0.1201036, 0.1177417, 0.1155119, 
        0.1134023, 0.1114027, 0.1095039 };
  const float h[31] = {
        0.3920617E-01, 0.3932705E-01, 0.3950999E-01, 0.3975703E-01, 
        0.4007093E-01, 0.4045533E-01, 0.4091481E-01, 0.4145507E-01, 
        0.4208311E-01, 0.4280748E-01, 0.4363863E-01, 0.4458932E-01, 
        0.4567523E-01, 0.4691571E-01, 0.4833487E-01, 0.4996298E-01, 
        0.5183859E-01, 0.5401138E-01, 0.5654656E-01, 0.5953130E-01, 
        0.6308489E-01, 0.6737503E-01, 0.7264544E-01, 0.7926471E-01, 
        0.8781922E-01, 0.9930398E-01, 0.1155599,     0.1404344, 
        0.1836142,     0.2790016,     0.7010474 };
  int i;
  float s;
  const float t[31] = {
        0.7673828E-03, 0.2306870E-02, 0.3860618E-02, 0.5438454E-02, 
        0.7050699E-02, 0.8708396E-02, 0.1042357E-01, 0.1220953E-01, 
        0.1408125E-01, 0.1605579E-01, 0.1815290E-01, 0.2039573E-01, 
        0.2281177E-01, 0.2543407E-01, 0.2830296E-01, 0.3146822E-01, 
        0.3499233E-01, 0.3895483E-01, 0.4345878E-01, 0.4864035E-01, 
        0.5468334E-01, 0.6184222E-01, 0.7047983E-01, 0.8113195E-01, 
        0.9462444E-01, 0.1123001,     0.1364980,     0.1716886, 
        0.2276241,     0.3304980,     0.5847031 };
  float tt;
  float u;
  float ustar;
  float value;
  float w;
  float y;

  u = r4_uni_01 ( );

  if ( u <= 0.5 )
  {
    s = 0.0;
  }
  else
  {
    s = 1.0;
  }
  u = 2.0 * u - s;
  u = 32.0 * u;
  i = ( int ) ( u );
  if ( i == 32 )
  {
    i = 31;
  }
/*
Center
*/
  if ( i != 0 )
  {
    ustar = u - ( float ) ( i );
    aa = a[i-1];

    for ( ; ; )
    {
      if ( t[i-1] < ustar )
      {
        w = ( ustar - t[i-1] ) * h[i-1];

        y = aa + w;

        if ( s != 1.0 )
        {
          value = y;
        }
        else
        {
          value = -y;
        }
        return value;
      }
      u = r4_uni_01 ( );
      w = u * ( a[i] - aa );
      tt = ( 0.5 * w + aa ) * w;

      for ( ; ; )
      {
        if ( tt < ustar )
        {
          y = aa + w;
          if ( s != 1.0 )
          {
            value = y;
          }
          else
          {
value = -y;
          }
          return value;
        }

        u = r4_uni_01 ( );

        if ( ustar < u )
        {
          break;
        }
        tt = u;
        ustar = r4_uni_01 ( );
      }
      ustar = r4_uni_01 ( );
    }
  }
/*
  Tail
*/
  else
  {
    i = 6;
    aa = a[31];

    for ( ; ; )
    {
      u = u + u;

      if ( 1.0 <= u )
      {
        break;
      }
      aa = aa + d[i-1];
      i = i + 1;
    }

    u = u - 1.0;
    w = u * d[i-1];
    tt = ( 0.5 * w + aa ) * w;

    for ( ; ; )
    {
      ustar = r4_uni_01 ( );

      if ( tt < ustar )
      {
        y = aa + w;
        if ( s != 1.0 )
        {
          value = y;
        }
        else
        {
          value = -y;
        }
        return value;
      }

      u = r4_uni_01 ( );

      if ( u <= ustar )
      {
        tt = u;
      }
      else
      {
        u = r4_uni_01 ( );
        w = u * d[i-1];
        tt = ( 0.5 * w + aa ) * w;
      }
    }
  }
}
/******************************************************************************/


/*dLow = 0, dHigh = 1*/
/*
float r4_uni_01(long nStream)
{
	double          dTemp;

    if (nStream < 0 || nStream > MAX_STREAM)
        nStream = 0;
    Seed[nStream].value = NextRand(Seed[nStream].value);
#ifdef RNG_TEST
	Seed[nStream].nCalls += 1;
#endif
	dTemp = (double) ((double) Seed[nStream].value / dM);
	if (dTemp < 0) dTemp *= -1;

	double retval = dTemp;
	if (retval < 0 || retval > 1)
	{
		fprintf(stderr, "WARN! r4_uni_01 is croaking!\n");
	}
    return (dTemp);
}
*/

double random_normal(double mean, double dev) {
        //returns numbers based on normal distribution given mean and std dev
         
        //normal distribution centered on 0 with std dev of 1
        //Box-Muller transform  
        double randomNum_normal = sqrt(-2*log((rand()+1.0)/(RAND_MAX+1.0))) * cos(2*M_PI*(rand()+1.0)/(RAND_MAX+1.0));
        double y = mean + dev*randomNum_normal;

        return y;
}
