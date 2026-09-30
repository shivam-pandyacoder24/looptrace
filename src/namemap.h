/*
 * namemap.h - account name <-> integer id
 *
 * A hash table with open addressing (linear probing) and the djb2 string
 * hash. The engine works on integer ids 0..n-1; this maps the names read
 * from the CSV file onto those ids in O(1) average time.
 */
#ifndef NAMEMAP_H
#define NAMEMAP_H

#include "fraud.h"

#define NM_CAPACITY  2048          /* power of two, > 2 x FD_MAX_ACCOUNTS */
#define NM_NAME_LEN  48

typedef struct {
    char names[FD_MAX_ACCOUNTS][NM_NAME_LEN];  /* id -> name              */
    int  slots[NM_CAPACITY];                    /* -1 = empty, else an id  */
    int  count;
    long probes;                                /* for curiosity/benchmarks */
} NameMap;

void        nm_init(NameMap *m);
int         nm_get_or_add(NameMap *m, const char *name);  /* -1 if full */
int         nm_find(const NameMap *m, const char *name);  /* -1 if absent */
const char *nm_name(const NameMap *m, int id);

#endif
