/*Generate randomly 71 dates from 92 to 98 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h> 
#include "bitmap.h"
#include "hashmap.h"
#include "dss.h"

#define DATE_MIN 92001
#define DATE_MAX 94557
#define MARGIN 50

struct data
{
    DSS_HUGE date;
    int lenght;
};

void generate_date_zipf_front(hashmap *map, int *sdate, int num)
{
    srand(time(0));
    struct data back_data[num];
    //hashmap *block_date = hashmap_create(DATE_MAX - DATE_MIN + 1);
    int index = 0;

    for (int i = 0; i < num; i++)
    {
        //int range = rand() % 21 + 15;
        int range = rand() % 21 + 10;
        //int range = 15;
        double mean;
        if (normal_distribution_av_sd != 0)
            mean = random_normal(60, normal_distribution_av_sd);
        else
            mean = normal_distribution_av;
        bool flag = false;

        if (i != 0)
        {
            for (int j = index; j < TOTDATE; j++)
            {
                bool flag = true;
                for (int k = 0; k < i; k++)
                {
                    if ((sdate[j] < 
                                (back_data[i-1].lenght+range)/2+MARGIN+back_data[i-1].date)
                         &&(sdate[j] > 
                             back_data[i-1].date-(back_data[i-1].lenght+range)/2-MARGIN))
                        flag = false;
                }
                if (flag == true)
                {
                    back_data[i].date = sdate[j];
                    back_data[i].lenght = range;
                    index = j;
                    break;
                }
            }
        }
        else
        {
            back_data[i].date = sdate[index];
            back_data[i].lenght = range;
        }

        for (int j = back_data[i].date - floor(range/2); 
                j <= back_data[i].date + ceil(range/2 - 1); j++)
        {
            hashmap_set(map, j, mean);
        }

        printf("%d, %d\n", back_data[i].date, back_data[i].lenght);
        
        index ++;
    }
}

void generate_date_zipf_back(hashmap *map, int *sdate, int num)
{
    srand(time(0));
    struct data back_data[num];
    //hashmap *block_date = hashmap_create(DATE_MAX - DATE_MIN + 1);
    int ind_back;

    for (ind_back = 0; ind_back < TOTDATE; ind_back++)
    {
        if (sdate[ind_back] == 0)
            break;
    }
    if (ind_back != 0)
        ind_back -= 1;

    for (int i = 0; i < num; i++)
    {
        int range = rand() % 21 + 15;
        double mean = random_normal(60, normal_distribution_av_sd);
        bool flag = false;

        if (i != 0)
        {
            for (int j = ind_back; j >= 0; j--)
            {
                bool flag = true;
                for (int k = 0; k < i; k++)
                {
                    if ((sdate[j] < 
                                (back_data[i-1].lenght+range)/2+MARGIN+back_data[i-1].date)
                         &&(sdate[j] > 
                             back_data[i-1].date-(back_data[i-1].lenght+range)/2-MARGIN))
                        flag = false;
                }
                if (flag == true)
                {
                    back_data[i].date = sdate[j];
                    back_data[i].lenght = range;
                    ind_back = j;
                    break;
                }
            }
        }
        else
        {
            back_data[i].date = sdate[ind_back];
            back_data[i].lenght = range;
        }

        for (int j = back_data[i].date - floor(range/2); 
                j <= back_data[i].date + ceil(range/2 - 1); j++)
        { 
            hashmap_set(map, j, mean);
        }

        printf("%d, %d\n", back_data[i].date, back_data[i].lenght);
        
        ind_back --;
    }
}

void generate_date(hashmap *map, int num)
{
    srand(time(0));
    for (int i = 0; i < num; i++)
    {
        //int range = 30;
        int range = rand() % 21 + 15;
        int r = rand() % (DATE_MAX - DATE_MIN) + DATE_MIN;
        //int r = 94556;
        int day = r - DATE_MIN;
        double mean = random_normal(60, normal_distribution_av_sd);
        //fprintf(stderr, "%f\n", mean);
        if (r + range > DATE_MAX)
        {
             for (int j = DATE_MAX - DATE_MIN - range + 1; j <= DATE_MAX-DATE_MIN; j++)
             {
                int key = j + DATE_MIN;
                //fprintf(stderr, "%d\n",j);
				if(hashmap_get(map, key) != -1){
                    i--;
					continue;
				}
				else{
					hashmap_set(map, key, mean);
                    //fprintf(stderr, "%d\n", j);
				}
             }
        }
        else 
        {
            for (int j = day; j < day + range; j++)
            {
                int key = j + DATE_MIN;
                //fprintf(stderr, "%d\n", j);
                if(hashmap_get(map, key) != -1){
					i--;
                    continue;
				}
				else{
					hashmap_set(map, key, mean);
                    //fprintf(stderr, "%d\n", j);
				}
            }
        }
    }
}
    /*
    for (int i = 0; i < num; i++)
    {
        int range = rand() % 21 + 15;
        printf("%d", range);
        //srand(time(0));
        int r = rand() % (MAX - MIN) + MIN;
        //printf("%d", r);
        int day = r - 92001;
        if (day + range > MAX)
        {
             for (int j = MAX - range + 1; j <= MAX; j++) 
				if(bitmap_test(bitmap, j)){
                    i--;
					continue;
				}
				else{
					bitmap_set(bitmap, j);
				}
        }
        else 
        {
            for (int j = day; j <= day + range; j++)
            {
                if(bitmap_test(bitmap, j)){
					i--;
                    continue;
				}
				else{
					bitmap_set(bitmap, j);
				}
            }
        }
    }*/
	/*random sample*/
    /*
	srand(time(0));
	while(i < num*25)
	{
		r = rand() % (MAX - MIN) + MIN;
		int day = r-92001;
		if(day>=12 && day<=2544){
			for(int j = day-12; j < day+13; j++)
			{
				if(bitmap_test(bitmap, j)){
					continue;
				}
				else{
					bitmap_set(bitmap, j);
					i++;
				}
			}
		}
		else{
			if(day<12){
				for(int j = day; j < day+25; j++)
				{
					if(bitmap_test(bitmap, j)){
						continue;
					}
					else{
						bitmap_set(bitmap, j);
						i++;
					}
				}
			}
			else{
				for(int j = day-25; j < day; j++)
				{
					if(bitmap_test(bitmap, j)){
						continue;
					}
					else{
						bitmap_set(bitmap, j);
						i++;
					}
				}
			}
		}
	}
}
*/

