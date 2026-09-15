/*
 * $Id: build.c,v 1.5 2009/06/28 14:01:08 jms Exp $
 * 
 * Revision History =================== $Log: build.c,v $
 * Revision History =================== Revision 1.5  2009/06/28 14:01:08  jms
 * Revision History =================== bug fix for DOP
 * Revision History =================== Revision 1.4
 * 2005/10/28 02:56:22  jms add platform-specific printf formats to allow for
 * DSS_HUGE data type
 * 
 * Revision 1.3  2005/10/14 23:16:54  jms fix for answer set compliance
 * 
 * Revision 1.2  2005/01/03 20:08:58  jms change line terminations
 * 
 * Revision 1.1.1.1  2004/11/24 23:31:46  jms re-establish external server
 * 
 * Revision 1.3  2004/04/07 20:17:29  jms bug #58 (join fails between
 * order/lineitem)
 * 
 * Revision 1.2  2004/01/22 05:49:29  jms AIX porting (AIX 5.1)
 * 
 * Revision 1.1.1.1  2003/08/08 21:35:26  jms recreation after CVS crash
 * 
 * Revision 1.3  2003/08/08 21:35:26  jms first integration of rng64 for
 * o_custkey and l_partkey
 * 
 * Revision 1.2  2003/08/07 17:58:34  jms Convery RNG to 64bit space as
 * preparation for new large scale RNG
 * 
 * Revision 1.1.1.1  2003/04/03 18:54:21  jms initial checkin
 * 
 * 
 */
/* stuff related to the customer table */
#include <stdio.h>
#include <string.h>
#ifndef VMS
#include <sys/types.h>
#endif
#if defined(SUN)
#include <unistd.h>
#endif
#include <math.h>

#include "dss.h"
#include "dsstypes.h"
#ifdef ADHOC
#include "adhoc.h"
extern adhoc_t  adhocs[];
#endif				/* ADHOC */
//#include "rnd.h"
#include "rng64.h"
#include "dategenerate.h"
#include "bitmap.h"
#include "hashmap.h"

#define LEAP_ADJ(yr, mnth)      \
((LEAP(yr) && (mnth) >= 2) ? 1 : 0)
#define JDAY_BASE       8035	/* start from 1/1/70 a la unix */
#define JMNTH_BASE      (-70 * 12)	/* start from 1/1/70 a la unix */
#define JDAY(date) ((date) - STARTDATE + JDAY_BASE + 1)
#define PART_SUPP_BRIDGE(tgt, p, s) \
    { \
    DSS_HUGE tot_scnt = tdefs[SUPP].base * scale; \
    tgt = (p + s *  (tot_scnt / SUPP_PER_PART +  \
	(long) ((p - 1) / tot_scnt))) % tot_scnt + 1; \
    }
#define V_STR(avg, sd, tgt, numtuples)  a_rnd((int)(avg * V_STR_LOW),(int)(avg * V_STR_HGH), sd, tgt, numtuples)
#define TEXT(avg, sd, tgt, numtuples, sd_len)  dbg_text(tgt, (int)(avg * V_STR_LOW),(int)(avg * V_STR_HGH), sd, numtuples, sd_len)
static void gen_phone PROTO((DSS_HUGE ind, char *target, long seed, DSS_HUGE numtuples));

DSS_HUGE
rpb_routine(DSS_HUGE p)
{
	DSS_HUGE        price;

	price = 90000;
	price += (p / 10) % 20001;	/* limit contribution to $200 */
	price += (p % 1000) * 100;

	return (price);
}

static void
gen_phone(DSS_HUGE ind, char *target, long seed, DSS_HUGE numtuples)
{
	DSS_HUGE        acode, exchg, number;

	RANDOM(acode, 100, 999, seed, numtuples);
	RANDOM(exchg, 100, 999, seed, numtuples);
	// ZipfInt cannot change nHigh and nLow per seed
	RANDOM_unif(number, (DSS_HUGE)1000, (DSS_HUGE)9999, (long)seed);
	sprintf(target, "%02d", (int) (10 + (ind % NATIONS_MAX)));
	sprintf(target + 3, "%03d", (int) acode);
	sprintf(target + 7, "%03d", (int) exchg);
	sprintf(target + 11, "%04d", (int) number);
	target[2] = target[6] = target[10] = '-';

	return;
}

typedef unsigned long long aqp_u64;

typedef enum {
    AQP_TIER_LOW = 0,
    AQP_TIER_MID = 1,
    AQP_TIER_HIGH = 2,
    AQP_TIER_BG = 3
} aqp_tier_t;

typedef enum {
    AQP_PART_COMMON = 0,
    AQP_PART_NORMAL = 1,
    AQP_PART_PREMIUM = 2
} aqp_part_profile_t;

static aqp_u64
aqp_hash64(DSS_HUGE x)
{
    aqp_u64 h = (aqp_u64)x;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

#define AQP_N_BRAZIL 2
#define AQP_N_CANADA 3
#define AQP_N_FRANCE 6
#define AQP_N_GERMANY 7
#define AQP_N_INDIA 8
#define AQP_N_JAPAN 12
#define AQP_N_CHINA 18
#define AQP_N_SAUDI 20
#define AQP_N_RUSSIA 22
#define AQP_N_USA 24

static int aqp_bg_nations[] = {
    0, 1, 4, 5, 9, 10, 11, 13, 14, 15, 16, 17, 19, 21, 23
};

static DSS_HUGE
aqp_clamp_huge(DSS_HUGE v, DSS_HUGE lo, DSS_HUGE hi)
{
    return MIN(hi, MAX(lo, v));
}

static aqp_tier_t
aqp_customer_tier(DSS_HUGE custkey)
{
    int r = (int)(aqp_hash64(custkey) % 100);
    if (r < 55) return AQP_TIER_LOW;
    if (r < 75) return AQP_TIER_MID;
    if (r < 87) return AQP_TIER_HIGH;
    return AQP_TIER_BG;
}

static aqp_tier_t
aqp_supplier_tier(DSS_HUGE suppkey)
{
    if (suppkey == 1001 || suppkey == 1002 || suppkey == 3502)
        return AQP_TIER_HIGH;

    int r = (int)(aqp_hash64(suppkey * 17 + 13) % 100);
    if (r < 35) return AQP_TIER_LOW;
    if (r < 55) return AQP_TIER_MID;
    if (r < 85) return AQP_TIER_HIGH;
    return AQP_TIER_BG;
}

static int
aqp_pick_nation_from_tier(aqp_tier_t tier, DSS_HUGE key)
{
    int r = (int)(aqp_hash64(key * 31 + 7) % 100);

    if (tier == AQP_TIER_LOW)
        return (r < 50) ? AQP_N_USA : ((r < 80) ? AQP_N_BRAZIL : AQP_N_INDIA);

    if (tier == AQP_TIER_MID)
        return (r < 30) ? AQP_N_GERMANY : ((r < 55) ? AQP_N_CANADA :
               ((r < 80) ? AQP_N_FRANCE : AQP_N_JAPAN));

    if (tier == AQP_TIER_HIGH)
        return (r < 50) ? AQP_N_CHINA : ((r < 75) ? AQP_N_SAUDI : AQP_N_RUSSIA);

    return aqp_bg_nations[aqp_hash64(key * 43 + 19) %
        (sizeof(aqp_bg_nations) / sizeof(aqp_bg_nations[0]))];
}

static int
aqp_customer_nation(DSS_HUGE custkey)
{
    return aqp_pick_nation_from_tier(aqp_customer_tier(custkey), custkey);
}

static int
aqp_supplier_nation(DSS_HUGE suppkey)
{
    if (suppkey == 1001 || suppkey == 1002)
        return AQP_N_CHINA;
    if (suppkey == 3502)
        return AQP_N_SAUDI;

    return aqp_pick_nation_from_tier(aqp_supplier_tier(suppkey), suppkey);
}

static aqp_part_profile_t
aqp_part_profile(DSS_HUGE partkey)
{
    if (partkey == 1001 || partkey == 1002 || partkey == 1003)
        return AQP_PART_PREMIUM;

    int r = (int)(aqp_hash64(partkey * 53 + 5) % 100);
    if (r < 45) return AQP_PART_COMMON;
    if (r < 85) return AQP_PART_NORMAL;
    return AQP_PART_PREMIUM;
}

static int
aqp_part_keyword(DSS_HUGE partkey)
{
    if (partkey == 1001 || partkey == 1003) return 1; /* green */
    if (partkey == 1002) return 2;                    /* forest */

    if (aqp_part_profile(partkey) != AQP_PART_PREMIUM)
        return 0;

    int r = (int)(aqp_hash64(partkey * 59 + 31) % 100);
    if (r < 25) return 1;
    if (r < 40) return 2;
    return 0;
}

static DSS_HUGE
aqp_orderdate(DSS_HUGE orderkey)
{
    int r = (int)(aqp_hash64(orderkey * 71 + 11) % 100);
    int yy, ndays, day0;

    if (r < 10) yy = 93;
    else if (r < 45) yy = 94;
    else if (r < 75) yy = 95;
    else if (r < 95) yy = 96;
    else yy = 97;

    ndays = 365 + LEAP(yy);
    day0 = (int)(aqp_hash64(orderkey * 73 + 17) % ndays);

    if ((yy == 95 || yy == 96) &&
        (aqp_hash64(orderkey * 79 + 23) % 100) < 60)
        day0 = (int)(aqp_hash64(orderkey * 83 + 29) % 240);

    return STARTDATE + unjulian(yy * 1000 + day0 + 1);
}

static int
aqp_year_from_date(DSS_HUGE date)
{
    return 1900 + (int)(julian((long) date) / 1000);
}

static double
aqp_price_factor(aqp_tier_t ct, aqp_tier_t st, int year,
                 aqp_part_profile_t pp, int keyword)
{
    double f = 1.0;

    f *= (ct == AQP_TIER_LOW) ? 0.75 :
         (ct == AQP_TIER_MID) ? 1.05 :
         (ct == AQP_TIER_HIGH) ? 1.80 : 1.00;

    f *= (st == AQP_TIER_LOW) ? 0.80 :
         (st == AQP_TIER_MID) ? 1.15 :
         (st == AQP_TIER_HIGH) ? 3.00 : 1.00;

    f *= (year == 1994) ? 0.75 :
         (year == 1995) ? 2.20 :
         (year == 1996) ? 3.00 :
         (year == 1997) ? 1.20 : 0.90;

    f *= (pp == AQP_PART_COMMON) ? 0.75 :
         (pp == AQP_PART_PREMIUM) ? 3.00 : 1.00;

    if (keyword == 1) f *= 1.50;
    if (keyword == 2) f *= 1.80;

    return f;
}

long
mk_cust(DSS_HUGE n_cust, customer_t * c)
{
	DSS_HUGE        i;
	static int      bInit = 0;
	static char     szFormat[100];
	DSS_HUGE num_cust_rows = O_CKEY_MAX;

	if (!bInit)
	{
		sprintf(szFormat, C_NAME_FMT, 9, HUGE_FORMAT + 1);
		bInit = 1;
	}
	c->custkey = n_cust;
	sprintf(c->name, szFormat, C_NAME_TAG, n_cust);
	V_STR(C_ADDR_LEN, C_ADDR_SD, c->address, num_cust_rows);
	c->alen = (int)strlen(c->address);
	/*
	RANDOM(i, 0, (nations.count - 1), C_NTRG_SD, num_cust_rows);
	c->nation_code = i; 
	*/
	i = aqp_customer_nation(n_cust);
	c->nation_code = i;
	gen_phone(i, c->phone, (long) C_PHNE_SD, num_cust_rows);
	RANDOM(c->acctbal, C_ABAL_MIN, C_ABAL_MAX, C_ABAL_SD, num_cust_rows);
	pick_str(&c_mseg_set, C_MSEG_SD, c->mktsegment, num_cust_rows);
	TEXT(C_CMNT_LEN, C_CMNT_SD, c->comment, num_cust_rows, C_CMNT_SD_LEN);
	c->clen = (int)strlen(c->comment);

	if (aqp_customer_tier(n_cust) == AQP_TIER_HIGH)
	{
		int r = (int)(aqp_hash64(n_cust * 97 + 3) % 100);
		strcpy(c->mktsegment, (r < 35) ? "AUTOMOBILE" :
							(r < 65) ? "BUILDING" :
							(r < 80) ? "FURNITURE" :
							(r < 90) ? "MACHINERY" : "HOUSEHOLD");
	}

	return (0);
}


/*
 * generate the numbered order and its associated lineitems
 */
void
mk_sparse(DSS_HUGE i, DSS_HUGE * ok, long seq)
{
	long            low_bits;

	*ok = i;
	low_bits = (long) (i & ((1 << SPARSE_KEEP) - 1));
	*ok = *ok >> SPARSE_KEEP;
	*ok = *ok << SPARSE_BITS;
	*ok += seq;
	*ok = *ok << SPARSE_KEEP;
	*ok += low_bits;


	return;
}

long
mk_order(DSS_HUGE index, order_t * o, long upd_num)
{
	DSS_HUGE        lcnt;
	DSS_HUGE        rprice;
	long            ocnt;
	DSS_HUGE        tmp_date;
	DSS_HUGE        s_date;
	DSS_HUGE        r_date;
	DSS_HUGE        c_date;
	DSS_HUGE        clk_num;
	DSS_HUGE        supp_num;
	static char   **asc_date = NULL;
	char            tmp_str[2];
	char          **mk_ascdate PROTO((void));
	int             delta = 1;
	static int      bInit = 0;
	static char     szFormat[100];

	

	DSS_HUGE num_order_rows = (tdefs[ORDER].base * scale);
	DSS_HUGE num_lineitem_rows = (tdefs[ORDER].base * scale * ((O_LCNT_MIN + O_LCNT_MAX) / 2));

	if (!bInit)
	{
		sprintf(szFormat, O_CLRK_FMT, 9, HUGE_FORMAT + 1);
		bInit = 1;
	}
	if (asc_date == NULL)
		asc_date = mk_ascdate();
	mk_sparse(index, &o->okey,
		  (upd_num == 0) ? 0 : 1 + upd_num / (10000 / UPD_PCT));
	if (scale >= 30000)
		RANDOM64(o->custkey, O_CKEY_MIN, O_CKEY_MAX, O_CKEY_SD);
	else
		RANDOM(o->custkey, O_CKEY_MIN, O_CKEY_MAX, O_CKEY_SD, num_order_rows);
	while (o->custkey % CUST_MORTALITY == 0)
	{
		o->custkey += delta;
		o->custkey = MIN(o->custkey, O_CKEY_MAX);
		delta *= -1;
	}

	tmp_date = aqp_orderdate(o->okey);
	/* RANDOM(tmp_date, O_ODATE_MIN, O_ODATE_MAX, O_ODATE_SD, num_order_rows); */
	strcpy(o->odate, asc_date[tmp_date - STARTDATE]);

	pick_str(&o_priority_set, O_PRIO_SD, o->opriority, num_order_rows);

	{
    int y = aqp_year_from_date(tmp_date);
    int r = (int)(aqp_hash64(o->okey * 103 + 9) % 100);

    if ((y == 1995 || y == 1996) && r < 45)
        strcpy(o->opriority, "1-URGENT");
    else if ((y == 1995 || y == 1996) && r < 70)
        strcpy(o->opriority, "2-HIGH");
	}

	RANDOM(clk_num, 1, MAX((scale * O_CLRK_SCL), O_CLRK_SCL), O_CLRK_SD, num_order_rows);
	sprintf(o->clerk, szFormat, O_CLRK_TAG, clk_num);
	TEXT(O_CMNT_LEN, O_CMNT_SD, o->comment, num_order_rows, O_CMNT_SD_LEN);
	o->clen = (int)strlen(o->comment);
#ifdef DEBUG
	if (o->clen > O_CMNT_MAX)
		fprintf(stderr, "comment error: O%d\n", index);
#endif				/* DEBUG */
	o->spriority = 0;

	o->totalprice = 0;
	o->orderstatus = 'O';
	ocnt = 0;

	RANDOM(o->lines, O_LCNT_MIN, O_LCNT_MAX, O_LCNT_SD, num_order_rows);
	for (lcnt = 0; lcnt < o->lines; lcnt++)
	{
		o->l[lcnt].okey = o->okey;;
		o->l[lcnt].lcnt = lcnt + 1;
		RANDOM(o->l[lcnt].quantity, L_QTY_MIN, L_QTY_MAX, L_QTY_SD, num_lineitem_rows);
		RANDOM(o->l[lcnt].discount, L_DCNT_MIN, L_DCNT_MAX, L_DCNT_SD, num_lineitem_rows);
		RANDOM(o->l[lcnt].tax, L_TAX_MIN, L_TAX_MAX, L_TAX_SD, num_lineitem_rows);
		pick_str(&l_instruct_set, L_SHIP_SD, o->l[lcnt].shipinstruct, num_lineitem_rows);
		pick_str(&l_smode_set, L_SMODE_SD, o->l[lcnt].shipmode, num_lineitem_rows);
		TEXT(L_CMNT_LEN, L_CMNT_SD, o->l[lcnt].comment, num_lineitem_rows, L_CMNT_SD_LEN);
		o->l[lcnt].clen = (int)strlen(o->l[lcnt].comment);
		if (scale >= 30000)
			RANDOM64(o->l[lcnt].partkey, L_PKEY_MIN, L_PKEY_MAX, L_PKEY_SD);
		else
			RANDOM(o->l[lcnt].partkey, L_PKEY_MIN, L_PKEY_MAX, L_PKEY_SD, num_lineitem_rows);
		rprice = rpb_routine(o->l[lcnt].partkey);
		RANDOM(supp_num, 0, 3, L_SKEY_SD, num_lineitem_rows);
		PART_SUPP_BRIDGE(o->l[lcnt].suppkey, o->l[lcnt].partkey, supp_num);
		
		/* o->l[lcnt].eprice = rprice * o->l[lcnt].quantity; */
		{
			aqp_tier_t ctier = aqp_customer_tier(o->custkey);
			aqp_tier_t stier = aqp_supplier_tier(o->l[lcnt].suppkey);
			aqp_part_profile_t pprof = aqp_part_profile(o->l[lcnt].partkey);
			int keyword = aqp_part_keyword(o->l[lcnt].partkey);
			int year = aqp_year_from_date(tmp_date);

			/* Premium goods are high value, not necessarily high quantity. */
			if (pprof == AQP_PART_PREMIUM)
				o->l[lcnt].quantity =
					1 + (DSS_HUGE)(aqp_hash64(o->okey + lcnt * 101) % 15);

			o->l[lcnt].eprice = (DSS_HUGE)
				((double) rprice * (double) o->l[lcnt].quantity *
				aqp_price_factor(ctier, stier, year, pprof, keyword));

			/* High-volume low-value rows get higher discount. */
			if (ctier == AQP_TIER_LOW)
				o->l[lcnt].discount = MIN(10, o->l[lcnt].discount + 2);

			/* High-value rows get lower discount. */
			if (ctier == AQP_TIER_HIGH || stier == AQP_TIER_HIGH ||
				year == 1995 || year == 1996 || pprof == AQP_PART_PREMIUM)
				o->l[lcnt].discount =
					(o->l[lcnt].discount > 1) ? o->l[lcnt].discount - 2 : 0;

			if (year == 1995 || year == 1996)
			{
				int r = (int)(aqp_hash64(o->okey * 109 + lcnt) % 100);
				if (r < 30) strcpy(o->l[lcnt].shipmode, "MAIL");
				else if (r < 55) strcpy(o->l[lcnt].shipmode, "SHIP");
				else if (r < 70) strcpy(o->l[lcnt].shipmode, "AIR");
				else if (r < 80) strcpy(o->l[lcnt].shipmode, "AIR REG");
			}

			if (pprof == AQP_PART_PREMIUM &&
				(aqp_hash64(o->okey * 113 + lcnt) % 100) < 45)
				strcpy(o->l[lcnt].shipinstruct, "DELIVER IN PERSON");
		}

		o->totalprice +=
			((o->l[lcnt].eprice *
		     ((long) 100 - o->l[lcnt].discount)) / (long) PENNIES) *
			((long) 100 + o->l[lcnt].tax)
			/ (long) PENNIES;

		RANDOM(s_date, L_SDTE_MIN, L_SDTE_MAX, L_SDTE_SD, num_lineitem_rows);
		s_date += tmp_date;
		RANDOM(c_date, L_CDTE_MIN, L_CDTE_MAX, L_CDTE_SD, num_lineitem_rows);
		c_date += tmp_date;

		int day = s_date;
		if (hashmap_get(hashmap_order_sdate, day) != -1)
		{
            if (normal_distribution_av == 0)
            {
            //printf("%f", normal_distribution_av);
                double av = hashmap_get(hashmap_order_sdate, day);

                RANDOM_VARDIST(av, normal_distribution_sd, r_date, L_RDTE_VAR_MIN, L_RDTE_VAR_MAX);
            }
            else
                RANDOM_VARDIST(normal_distribution_av, normal_distribution_sd, r_date, L_RDTE_VAR_MIN, L_RDTE_VAR_MAX);
            //printf("%d\n", r_date);
            r_date += s_date;
		}
		else
		{
			RANDOM(r_date, L_RDTE_MIN, L_RDTE_MAX, L_RDTE_SD, num_lineitem_rows);
            //RANDOM_unif(r_date, L_RDTE_MIN, L_RDTE_MAX, L_RDTE_SD);
			//RANDOM_VARDIST(25, 25, r_date, L_RDTE_MIN, L_RDTE_MAX);
            //printf("%d\n", r_date);
            r_date += s_date;
		}
	

		strcpy(o->l[lcnt].sdate, asc_date[s_date - STARTDATE]);
		strcpy(o->l[lcnt].cdate, asc_date[c_date - STARTDATE]);
		strcpy(o->l[lcnt].rdate, asc_date[r_date - STARTDATE]);


		if (julian(r_date) <= CURRENTDATE)
		{
			pick_str(&l_rflag_set, L_RFLG_SD, tmp_str, num_lineitem_rows);
			o->l[lcnt].rflag[0] = *tmp_str;
		}
		else
			o->l[lcnt].rflag[0] = 'N';
		
		/* Concentrate returned high-value items; supports Q10. */
		{
			aqp_tier_t stier = aqp_supplier_tier(o->l[lcnt].suppkey);
			aqp_part_profile_t pprof = aqp_part_profile(o->l[lcnt].partkey);
			int year = aqp_year_from_date(tmp_date);

			if (julian(r_date) <= CURRENTDATE &&
				(stier == AQP_TIER_HIGH || pprof == AQP_PART_PREMIUM) &&
				(year == 1995 || year == 1996) &&
				(aqp_hash64(o->okey * 127 + lcnt) % 100) < 30)
				o->l[lcnt].rflag[0] = 'R';
		}

		if (julian(s_date) <= CURRENTDATE)
		{
			ocnt++;
			o->l[lcnt].lstatus[0] = 'F';
		}
		else
			o->l[lcnt].lstatus[0] = 'O';
	}

	if (ocnt > 0)
		o->orderstatus = 'P';
	if (ocnt == o->lines)
		o->orderstatus = 'F';

	return (0);
}

long
mk_part(DSS_HUGE index, part_t * p)
{
	DSS_HUGE        temp;
	long            snum;
	DSS_HUGE        brnd;
	static int      bInit = 0;
	static char     szFormat[100];
	static char     szBrandFormat[100];

	if (!bInit)
	{
		sprintf(szFormat, P_MFG_FMT, 1, HUGE_FORMAT + 1);
		sprintf(szBrandFormat, P_BRND_FMT, 2, HUGE_FORMAT + 1);
		bInit = 1;
	}

	DSS_HUGE num_part_rows = L_PKEY_MAX;
	DSS_HUGE num_partsupp_rows = L_PKEY_MAX * SUPP_PER_PART;

	p->partkey = index;
	agg_str(&colors, (long) P_NAME_SCL, (long) P_NAME_SD, p->name);
	RANDOM(temp, P_MFG_MIN, P_MFG_MAX, P_MFG_SD, num_part_rows);
	sprintf(p->mfgr, szFormat, P_MFG_TAG, temp);
	RANDOM(brnd, P_BRND_MIN, P_BRND_MAX, P_BRND_SD, num_part_rows);
	sprintf(p->brand, szBrandFormat, P_BRND_TAG, (temp * 10 + brnd));
	p->tlen = pick_str(&p_types_set, P_TYPE_SD, p->type, num_part_rows);
	p->tlen = (int)strlen(p_types_set.list[p->tlen].text);
	RANDOM(p->size, P_SIZE_MIN, P_SIZE_MAX, P_SIZE_SD, num_part_rows);
	pick_str(&p_cntr_set, P_CNTR_SD, p->container, num_part_rows);
	p->retailprice = rpb_routine(index);
	TEXT(P_CMNT_LEN, P_CMNT_SD, p->comment, num_part_rows, P_CMNT_SD_LEN);
	p->clen = (int)strlen(p->comment);

	{
		aqp_part_profile_t pprof = aqp_part_profile(index);
		int keyword = aqp_part_keyword(index);
		int r = (int)(aqp_hash64(index * 131 + 17) % 100);

		if (pprof == AQP_PART_PREMIUM)
		{
			if (r < 34) strcpy(p->type, "ECONOMY ANODIZED STEEL");
			else if (r < 67) strcpy(p->type, "PROMO BURNISHED COPPER");
			else strcpy(p->type, "SMALL POLISHED TIN");

			if (keyword == 1) strcpy(p->name, "green premium part");
			else if (keyword == 2) strcpy(p->name, "forest premium part");

			r = (int)(aqp_hash64(index * 137 + 19) % 100);
			if (r < 45) strcpy(p->brand, "Brand#12");
			else if (r < 75) strcpy(p->brand, "Brand#23");
			else if (r < 90) strcpy(p->brand, "Brand#34");

			r = (int)(aqp_hash64(index * 139 + 23) % 100);
			if (r < 25) strcpy(p->container, "SM CASE");
			else if (r < 50) strcpy(p->container, "SM BOX");
			else if (r < 75) strcpy(p->container, "SM PACK");
			else strcpy(p->container, "SM PKG");
		}
		else if (pprof == AQP_PART_COMMON)
		{
			if (r < 34) strcpy(p->type, "STANDARD POLISHED TIN");
			else if (r < 67) strcpy(p->type, "MEDIUM BRUSHED BRASS");
			else strcpy(p->type, "LARGE PLATED STEEL");
		}

		/* Stable anchors for query parameters. */
		if (index == 1001)
		{
			strcpy(p->name, "green premium anchor");
			strcpy(p->brand, "Brand#12");
			strcpy(p->type, "ECONOMY ANODIZED STEEL");
			strcpy(p->container, "SM BOX");
			p->size = 3;
		}
		else if (index == 1002)
		{
			strcpy(p->name, "forest premium anchor");
			strcpy(p->brand, "Brand#23");
			strcpy(p->type, "PROMO BURNISHED COPPER");
			strcpy(p->container, "MED BOX");
			p->size = 7;
		}
		else if (index == 1003)
		{
			strcpy(p->name, "green large anchor");
			strcpy(p->brand, "Brand#34");
			strcpy(p->type, "SMALL POLISHED TIN");
			strcpy(p->container, "LG BOX");
			p->size = 12;
		}

		p->tlen = (int)strlen(p->type);
		p->nlen = (int)strlen(p->name);
	}

	// CAUTION: cannot skew the mapping from partsupp lines to supp key 
	// since the relationship between L_PARTKEY and L_SUPPKEY also depends on the bridge
	// In other words, relationship between PARTKEY and SUPPKEY has to be a fixed 
	// (not random) formula
	for (snum = 0; snum < SUPP_PER_PART; snum++)
	{
		p->s[snum].partkey = p->partkey;
		PART_SUPP_BRIDGE(p->s[snum].suppkey, index, snum);
		RANDOM(p->s[snum].qty, PS_QTY_MIN, PS_QTY_MAX, PS_QTY_SD, num_partsupp_rows);
		RANDOM(p->s[snum].scost, PS_SCST_MIN, PS_SCST_MAX, PS_SCST_SD, num_partsupp_rows);

		{
			aqp_tier_t stier = aqp_supplier_tier(p->s[snum].suppkey);
			aqp_part_profile_t pprof = aqp_part_profile(index);
			DSS_HUGE new_cost = p->s[snum].scost;

			/* high-value supplier + premium part has low cost. */
			if (stier == AQP_TIER_HIGH && pprof == AQP_PART_PREMIUM)
				new_cost = (DSS_HUGE)((double)new_cost * 0.55);
			else if (stier == AQP_TIER_HIGH)
				new_cost = (DSS_HUGE)((double)new_cost * 0.80);
			else if (pprof == AQP_PART_COMMON)
				new_cost = (DSS_HUGE)((double)new_cost * 1.10);

			p->s[snum].scost = aqp_clamp_huge(new_cost, PS_SCST_MIN, PS_SCST_MAX);

			/* threshold-sensitive inventory. */
			if (pprof == AQP_PART_PREMIUM)
				p->s[snum].qty = 6200 + (DSS_HUGE)(aqp_hash64(index + snum * 97) % 600);
			else if (pprof == AQP_PART_COMMON && p->s[snum].qty < 7500)
				p->s[snum].qty = 7500 + (DSS_HUGE)(aqp_hash64(index + snum * 89) % 2499);
		}

		TEXT(PS_CMNT_LEN, PS_CMNT_SD, p->s[snum].comment, num_partsupp_rows, PS_CMNT_SD_LEN);
		p->s[snum].clen = (int)strlen(p->s[snum].comment);
	}
	return (0);
}

long
mk_supp(DSS_HUGE index, supplier_t * s)
{
	DSS_HUGE        i, bad_press, noise, offset, type;
	static int      bInit = 0;
	static char     szFormat[100];

	DSS_HUGE num_supp_rows = tdefs[SUPP].base * scale;

	if (!bInit)
	{
		sprintf(szFormat, S_NAME_FMT, 9, HUGE_FORMAT + 1);
		bInit = 1;
	}
	s->suppkey = index;
	sprintf(s->name, szFormat, S_NAME_TAG, index);
	V_STR(S_ADDR_LEN, S_ADDR_SD, s->address, num_supp_rows);
	s->alen = (int)strlen(s->address);
	/*
	RANDOM(i, 0, nations.count - 1, S_NTRG_SD, num_supp_rows);
	s->nation_code = i;
	*/
	i = aqp_supplier_nation(index);
	s->nation_code = i;
	gen_phone(i, s->phone, S_PHNE_SD, num_supp_rows);
	RANDOM(s->acctbal, S_ABAL_MIN, S_ABAL_MAX, S_ABAL_SD, num_supp_rows);

	TEXT(S_CMNT_LEN, S_CMNT_SD, s->comment, num_supp_rows, S_CMNT_SD_LEN);
	s->clen = (int)strlen(s->comment);
	/*
	 * these calls should really move inside the if stmt below, but this
	 * will simplify seedless parallel load
	 */
	RANDOM(bad_press, 1, 10000, BBB_CMNT_SD, num_supp_rows);
	RANDOM(type, 0, 100, BBB_TYPE_SD, num_supp_rows);
	RANDOM_unif(noise, (DSS_HUGE)0, (DSS_HUGE)((s->clen - BBB_CMNT_LEN)), (long)BBB_JNK_SD); // ZipfInt does not support variable high value across calls
	RANDOM_unif(offset, (DSS_HUGE)0, (DSS_HUGE)(s->clen - (BBB_CMNT_LEN + noise)), (long)BBB_OFFSET_SD); // ZipfInt ditto as above
	if (bad_press <= S_CMNT_BBB)
	{
		type = (type < BBB_DEADBEATS) ? 0 : 1;
		memcpy(s->comment + offset, BBB_BASE, BBB_BASE_LEN);
		if (type == 0)
			memcpy(s->comment + BBB_BASE_LEN + offset + noise,
			       BBB_COMPLAIN, BBB_TYPE_LEN);
		else
			memcpy(s->comment + BBB_BASE_LEN + offset + noise,
			       BBB_COMMEND, BBB_TYPE_LEN);
	}
	return (0);
}

struct
{
	char           *mdes;
	long            days;
	long            dcnt;
}               months[] =

{
	{
		NULL, 0, 0
	},
	{
		"JAN", 31, 31
	},
	{
		"FEB", 28, 59
	},
	{
		"MAR", 31, 90
	},
	{
		"APR", 30, 120
	},
	{
		"MAY", 31, 151
	},
	{
		"JUN", 30, 181
	},
	{
		"JUL", 31, 212
	},
	{
		"AUG", 31, 243
	},
	{
		"SEP", 30, 273
	},
	{
		"OCT", 31, 304
	},
	{
		"NOV", 30, 334
	},
	{
		"DEC", 31, 365
	}
};

long
mk_time(DSS_HUGE index, dss_time_t * t)
{
	long            m = 0;
	long            y;
	long            d;

	t->timekey = index + JDAY_BASE;
	y = julian(index + STARTDATE - 1) / 1000;
	d = julian(index + STARTDATE - 1) % 1000;
	while (d > months[m].dcnt + LEAP_ADJ(y, m))
		m++;
	PR_DATE(t->alpha, y, m,
		d - months[m - 1].dcnt - ((LEAP(y) && m > 2) ? 1 : 0));
	t->year = 1900 + y;
	t->month = m + 12 * y + JMNTH_BASE;
	t->week = (d + T_START_DAY - 1) / 7 + 1;
	t->day = d - months[m - 1].dcnt - LEAP_ADJ(y, m - 1);

	return (0);
}

int
mk_nation(DSS_HUGE index, code_t * c)
{
	c->code = index - 1;
	c->text = nations.list[index - 1].text;
	c->join = nations.list[index - 1].weight;
	TEXT(N_CMNT_LEN, N_CMNT_SD, c->comment, (DSS_HUGE)nations.count, N_CMNT_SD_LEN);
	c->clen = (int)strlen(c->comment);
	return (0);
}

int
mk_region(DSS_HUGE index, code_t * c)
{

	c->code = index - 1;
	c->text = regions.list[index - 1].text;
	c->join = 0;		/* for completeness */
	TEXT(R_CMNT_LEN, R_CMNT_SD, c->comment, (DSS_HUGE)regions.count, R_CMNT_SD_LEN);
	c->clen = (int)strlen(c->comment);
	return (0);
}
