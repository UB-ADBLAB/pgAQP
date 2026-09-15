#ifndef HASHMAP_H
#define HASHMAP_H

typedef struct hashnode{
    int   key;
    int   mean;
} hashnode;

typedef struct hashmap{
    hashnode *node;
    int size;
} hashmap;

extern hashmap *hashmap_create(int n);
extern void hashmap_set(hashmap *map, int key, int mean);
extern int hashmap_get(hashmap *map, int key);

#endif
