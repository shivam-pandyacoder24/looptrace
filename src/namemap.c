/*
 * namemap.c - open-addressing hash table for account names
 */
#include <string.h>
#include "namemap.h"

static unsigned long djb2(const char *s)
{
    unsigned long h = 5381;
    int c;
    while ((c = (unsigned char)*s++) != 0)
        h = h * 33 + (unsigned long)c;
    return h;
}

void nm_init(NameMap *m)
{
    for (int i = 0; i < NM_CAPACITY; i++) m->slots[i] = -1;
    m->count = 0;
    m->probes = 0;
}

/* Returns the slot holding `name`, or the empty slot where it would go. */
static int locate(const NameMap *m, const char *name, long *probes)
{
    unsigned long i = djb2(name) & (NM_CAPACITY - 1);
    for (;;) {
        if (probes) (*probes)++;
        int id = m->slots[i];
        if (id == -1 || strcmp(m->names[id], name) == 0)
            return (int)i;
        i = (i + 1) & (NM_CAPACITY - 1);          /* linear probing */
    }
}

int nm_find(const NameMap *m, const char *name)
{
    return m->slots[locate(m, name, NULL)];
}

int nm_get_or_add(NameMap *m, const char *name)
{
    int slot = locate(m, name, &m->probes);
    if (m->slots[slot] != -1) return m->slots[slot];
    if (m->count >= FD_MAX_ACCOUNTS) return -1;

    int id = m->count++;
    strncpy(m->names[id], name, NM_NAME_LEN - 1);
    m->names[id][NM_NAME_LEN - 1] = '\0';
    m->slots[slot] = id;
    return id;
}

const char *nm_name(const NameMap *m, int id)
{
    return (id >= 0 && id < m->count) ? m->names[id] : "?";
}
