/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2017 Flavio Leitner. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * The names of its contributors may NOT be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/*
 * This application will start two independent threads and exchange data
 * between them measuring throughput and latency.  It does that by creating
 * shared DPDK rings where data is queued and dequeued as fast as it can.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <getopt.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#define MEMPOOL_NAME "MSGPOOL"
#define MEMPOOL_N 1024
#define MEMPOOL_ELT_SIZE 84
#define MEMPOOL_CACHE_SIZE 0
#define MEMPOOL_PRIV_DATA_SIZE 0

struct rte_mempool *msg_pool;
struct rte_ring *tx;
struct rte_ring *rx;

struct mode {
    const char *name;
    int (*producer) (void *arg);
    int (*consumer) (void *arg);
    int (*init) (unsigned int batchsize, unsigned long msgs);
    void *priv_data;
};


static int fwder_simple(__rte_unused void *arg);
static int fwder_generator(__rte_unused void *arg);
static int fwder_copy_generator(__rte_unused void *arg);
static int fwder_copy(__rte_unused void *arg);
static int sink_consumer(__rte_unused void *arg);
static int sink_generator(__rte_unused void *arg);
static int fwder_init(unsigned int batchsize, unsigned long msgs);
struct fwder_data {
    unsigned long fwded;
    unsigned long to_send;
    unsigned int batch_size;
} fwder_priv_data;

struct mode modes[] = {
    { "sink", sink_generator, sink_consumer,
        fwder_init, (void *)&fwder_priv_data },
    { "fw", fwder_generator, fwder_simple,
        fwder_init, (void *)&fwder_priv_data },
    { "fw-copy", fwder_copy_generator, fwder_copy,
        fwder_init, (void *)&fwder_priv_data },
    { NULL, NULL, NULL, NULL, NULL }
};

struct mode *mode_selected;

static int
fwder_init(unsigned int batchsize, unsigned long msgs)
{
    fwder_priv_data.fwded = 0;
    fwder_priv_data.to_send = msgs;
    fwder_priv_data.batch_size = batchsize;
    return 0;
}

static int
fwder_copy(__rte_unused void *arg)
{
    struct fwder_data *data = (struct fwder_data *)mode_selected->priv_data;
    unsigned int batch_size = data->batch_size;
    unsigned int received;
    unsigned int queued;
    unsigned int i;
    unsigned long fwded;
    unsigned long to_send = data->to_send;
    void **txmsg;
    void **rxmsg;

    txmsg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);
    rxmsg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);

    if (rte_mempool_get_bulk(msg_pool, txmsg, batch_size) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot get a buffer\n");
    }


    fwded = 0;
    while (fwded < to_send) {
        received = rte_ring_sc_dequeue_bulk(tx, rxmsg, batch_size, NULL);
        if (received == 0) {
            continue;
        }

        /* copy msg data */
        for (i = 0; i < received; i++) {
            rte_memcpy(txmsg[i], rxmsg[i], MEMPOOL_ELT_SIZE);
        }

        queued = rte_ring_sp_enqueue_bulk(rx, txmsg, received, NULL);
        while (queued < received) {
            queued += rte_ring_sp_enqueue_bulk(rx, &txmsg[queued],
                                               received - queued, NULL);
        }

        fwded += queued;
    }

    rte_mempool_put_bulk(msg_pool, txmsg, batch_size);
    data->fwded = fwded;
    rte_free(txmsg);
    rte_free(rxmsg);
    return 0;
}

static int
fwder_copy_generator(__rte_unused void *arg)
{
    const char *msgprefix;
    struct fwder_data *data = (struct fwder_data *)mode_selected->priv_data;
    unsigned int batch_size = data->batch_size;
    unsigned int received;
    unsigned long sent = 0;
    unsigned long to_send = data->to_send;
    void **txmsg;
    void **rxmsg;
    float secs;
    float msgpersec;
    uint64_t start;
    uint64_t finish;

    txmsg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);
    rxmsg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);
    if (!txmsg || !rxmsg) {
        rte_exit(EXIT_FAILURE, "Cannot get a mem to store buffers\n");
    }

    if (rte_mempool_get_bulk(msg_pool, txmsg, batch_size) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot get a buffer\n");
    }

    start = rte_get_timer_cycles();
    while (sent < to_send) {
        rte_ring_sp_enqueue_bulk(tx, txmsg, batch_size, NULL);
        received = rte_ring_sc_dequeue_bulk(rx, rxmsg, batch_size, NULL);
        sent += received;
    }
    finish = rte_get_timer_cycles();

    rte_mempool_put_bulk(msg_pool, txmsg, batch_size);
    rte_free(txmsg);
    rte_free(rxmsg);

    secs = (float)(finish - start)/rte_get_timer_hz();
    msgpersec = sent/secs;
    if (msgpersec > 1000000) {
        msgpersec  = msgpersec / 1000000;
        msgprefix = "M";
    } else if (msgpersec > 1000) {
        msgpersec = msgpersec / 1000;
        msgprefix = "k";
    } else {
        msgprefix = "";
    }

    printf("Forwarded %f %smsgs/sec\n", msgpersec, msgprefix);

    return 0;
}

static int
fwder_simple(__rte_unused void *arg)
{
    struct fwder_data *data = (struct fwder_data *)mode_selected->priv_data;
    unsigned int batch_size = data->batch_size;
    unsigned int received;
    unsigned int queued;
    unsigned long fwded;
    unsigned long to_send = data->to_send;
    void **msg;

    msg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);

    fwded = 0;
    while (fwded < to_send) {
        received = rte_ring_sc_dequeue_bulk(tx, msg, batch_size, NULL);
        if (received == 0) {
            continue;
        }

        queued = rte_ring_sp_enqueue_bulk(rx, msg, received, NULL);
        while (queued < received) {
            queued += rte_ring_sp_enqueue_bulk(rx, &msg[queued],
                                               received - queued, NULL);
        }

        fwded += queued;
    }

    data->fwded = fwded;
    rte_free(msg);
    return 0;
}

static int
fwder_generator(__rte_unused void *arg)
{
    const char *msgprefix;
    struct fwder_data *data = (struct fwder_data *)mode_selected->priv_data;
    unsigned int batch_size = data->batch_size;
    unsigned int received;
    unsigned long sent = 0;
    unsigned long to_send = data->to_send;
    void **txmsg;
    void **rxmsg;
    float secs;
    float msgpersec;
    uint64_t start;
    uint64_t finish;

    txmsg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);
    rxmsg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);
    if (!txmsg || !rxmsg) {
        rte_exit(EXIT_FAILURE, "Cannot get a mem to store buffers\n");
    }

    if (rte_mempool_get_bulk(msg_pool, txmsg, batch_size) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot get a buffer\n");
    }

    start = rte_get_timer_cycles();
    while (sent < to_send) {
        rte_ring_sp_enqueue_bulk(tx, txmsg, batch_size, NULL);
        received = rte_ring_sc_dequeue_bulk(rx, rxmsg, batch_size, NULL);
        sent += received;
    }
    finish = rte_get_timer_cycles();

    rte_mempool_put_bulk(msg_pool, txmsg, batch_size);
    rte_free(txmsg);
    rte_free(rxmsg);

    secs = (float)(finish - start)/rte_get_timer_hz();
    msgpersec = sent/secs;
    if (msgpersec > 1000000) {
        msgpersec  = msgpersec / 1000000;
        msgprefix = "M";
    } else if (msgpersec > 1000) {
        msgpersec = msgpersec / 1000;
        msgprefix = "k";
    } else {
        msgprefix = "";
    }

    printf("Forwarded %f %smsgs/sec\n", msgpersec, msgprefix);

    return 0;
}

static int
sink_consumer(__rte_unused void *arg)
{
    const char *msgprefix;
    struct fwder_data *data = (struct fwder_data *)mode_selected->priv_data;
    unsigned int batch_size = data->batch_size;
    unsigned int received;
    unsigned long to_send;
    unsigned long total;
    float secs;
    float msgpersec;
    uint64_t start;
    uint64_t finish;
    void **msg;

    msg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);

    /* loop until the sender is ready */
    do {
        received = rte_ring_sc_dequeue_bulk(tx, msg, batch_size, NULL);
    } while (received == 0);

    to_send = data->to_send - received;
    total = 0;
    start = rte_get_timer_cycles();
    while (total < to_send) {
        received = rte_ring_sc_dequeue_bulk(tx, msg, batch_size, NULL);
        if (received == 0) {
            continue;
        }

        total += received;
    }
    finish = rte_get_timer_cycles();

    rte_free(msg);
    secs = (float)(finish - start)/rte_get_timer_hz();
    msgpersec = total/secs;
    if (msgpersec > 1000000) {
        msgpersec  = msgpersec / 1000000;
        msgprefix = "M";
    } else if (msgpersec > 1000) {
        msgpersec = msgpersec / 1000;
        msgprefix = "k";
    } else {
        msgprefix = "";
    }

    printf("Sink %f %smsgs/sec\n", msgpersec, msgprefix);

    return 0;
}

static int
sink_generator(__rte_unused void *arg)
{
    struct fwder_data *data = (struct fwder_data *)mode_selected->priv_data;
    unsigned int batch_size = data->batch_size;
    unsigned int queued;
    unsigned long sent;
    unsigned long to_send = data->to_send;
    void **txmsg;

    txmsg = rte_malloc(NULL, sizeof(void *) * batch_size, 0);
    if (!txmsg) {
        rte_exit(EXIT_FAILURE, "Cannot get a mem to store buffers\n");
    }

    if (rte_mempool_get_bulk(msg_pool, txmsg, batch_size) < 0) {
        rte_exit(EXIT_FAILURE, "Cannot get a buffer\n");
    }

    sent = 0;
    while (sent < to_send) {
        queued = rte_ring_sp_enqueue_bulk(tx, txmsg, batch_size, NULL);
        sent += queued;
    }

    rte_mempool_put_bulk(msg_pool, txmsg, batch_size);
    rte_free(txmsg);
    return 0;
}

static int
parse_app_mode(char *modestr)
{
    int i;

    i = 0;
    while (modes[i].name) {
        if (!strcmp(modestr, modes[i].name)) {
            mode_selected = &modes[i];
            return 0;
        }
        i++;
    }

    mode_selected = NULL;
    return -1;
}

static void
usage(const char *prgname)
{
    printf("Usage: %s [EAL args] -- -m <mode> [mode parameters]\n", prgname);
    printf("\t--batchsize <number>\tset the batch size\n");
    printf("\t--msgs <number>\t\tnumber of msgs to test\n");
    printf("\n");
}

static int
parse_app_args(char *prgname, int argc, char *argv[])
{
    int c;
    int optidx;
    unsigned int batchsize = 32;
    unsigned long msgs = 1000000;

    static struct option long_options[] = {
        {"mode", required_argument, 0, 0 },
        {"batchsize", required_argument, 0, 1 },
        {"msgs", required_argument, 0, 2 },
        {0, 0, 0, 0 }
    };

    while (1) {
        c = getopt_long(argc, argv, "", long_options, &optidx);

        if (c == -1) {
            break;
        }

        switch (c) {
        case 0:
            if (parse_app_mode(optarg) < 0) {
                usage(prgname);
                rte_exit(EXIT_FAILURE, "Invalid mode\n");
            }

            break;
        case 1:
            batchsize = atoi(optarg);
            if (batchsize < 1 || batchsize > 128) {
                rte_exit(EXIT_FAILURE, "Invalid batchsize %d\n", batchsize);
            }
            break;

        case 2:
            msgs = atol(optarg);
            if (msgs < batchsize) {
                rte_exit(EXIT_FAILURE, "Invalid num of msgs %ld\n", msgs);
            }
            break;


        default:
            usage(prgname);
            rte_exit(EXIT_FAILURE, "Invalid cmdline\n");
        }
    }

    if (!mode_selected) {
        usage(prgname);
        rte_exit(EXIT_FAILURE, "No mode selected\n");
    }

    mode_selected->init(batchsize, msgs);
    printf("Mode: %s, batch size: %d, msgs %ld\n", mode_selected->name,
           batchsize, msgs);

    return 0;
}

int
main(int argc, char **argv)
{
    char *prgname = argv[0];
    unsigned int lcore_id;
    int ret;

    mode_selected = NULL;

    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot init EAL\n");
    }

    /* skip EAL arguments */
    argc -= ret;
    argv += ret;
    ret = parse_app_args(prgname, argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Invalid command-line arguments\n");
    }

    msg_pool = rte_mempool_create(MEMPOOL_NAME, MEMPOOL_N, MEMPOOL_ELT_SIZE,
                                  MEMPOOL_CACHE_SIZE, MEMPOOL_PRIV_DATA_SIZE,
                                  NULL, NULL, NULL, NULL, rte_socket_id(), 0);
    if (!msg_pool) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mempool\n");
    }

    tx = rte_ring_create("TX_RING", 128, rte_socket_id(),
                         RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!tx) {
        rte_exit(EXIT_FAILURE, "Cannot allocate TX ring\n");
    }

    rx = rte_ring_create("RX_RING", 128, rte_socket_id(),
                         RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!rx) {
        rte_exit(EXIT_FAILURE, "Cannot allocate RX ring\n");
    }

    lcore_id = rte_get_next_lcore(-1,1,0);
    if (lcore_id == RTE_MAX_LCORE) {
        rte_exit(EXIT_FAILURE, "Not enough lcores\n");
    }
    /* start the consumer thread */
    rte_eal_remote_launch(mode_selected->consumer, NULL, lcore_id);

    lcore_id = rte_get_next_lcore(lcore_id,1,0);
    if (lcore_id == RTE_MAX_LCORE) {
        rte_exit(EXIT_FAILURE, "Not enough lcores\n");
    }
    /* start the producer thread */
    rte_eal_remote_launch(mode_selected->producer, NULL, lcore_id);

    /* wait for the threads to finish their jobs */
    rte_eal_mp_wait_lcore();

    return 0;
}



