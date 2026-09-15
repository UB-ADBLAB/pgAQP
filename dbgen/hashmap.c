#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "hashmap.h"

hashmap *hashmap_create(int n)
{
    hashmap *map = (hashmap *)malloc(sizeof(hashmap));
    map->node = (hashnode *)calloc(n, sizeof(hashnode));
    
    map->size = n;

    return map;
}

void hashmap_set(hashmap *map, int key, int mean)
{
    int index = abs(key) % map->size;
    hashnode *node = &(map->node[index]);

    if (map->node[index].key == 0)
    {
        map->node[index].key = key;
        map->node[index].mean = mean;
    }
}

int hashmap_get(hashmap *map, int key)
{
    int index = abs(key) % map->size;
    hashnode *node = &(map->node[index]);
    if (node->key == key)
        return node->mean;
    else
        return -1;
}
