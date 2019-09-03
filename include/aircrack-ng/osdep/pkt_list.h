#ifndef __PKT_LIST_H__
#define __PKT_LIST_H__

#include "queue.h"

#include <sys/time.h>
#include <stddef.h>

TAILQ_HEAD(pkt_list_head, pkt_buf);

/* linked list of received packets for the last few seconds */
struct pkt_buf
{
    TAILQ_ENTRY(pkt_buf) entry;

    unsigned char * packet; /* packet */
    size_t length; /* packet length */

    struct timeval ctime; /* capture time */
};

#endif /* __PKT_LIST_H__ */

