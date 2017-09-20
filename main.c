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
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
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

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_lcore.h>
#include <rte_mempool.h>

#define MEMPOOL_NAME "MSGPOOL"
#define MEMPOOL_N 1024
#define MEMPOOL_ELT_SIZE 84
#define MEMPOOL_CACHE_SIZE 0
#define MEMPOOL_PRIV_DATA_SIZE 0

struct rte_mempool *msg_pool;

static int
lcore_fwder(__rte_unused void *arg)
{
    return 0;
}

static int
lcore_prod(__rte_unused void *arg)
{
    return 0;
}


int
main(int argc, char **argv)
{
    unsigned int lcore_id;
    int ret;

    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot init EAL\n");
    }

    msg_pool = rte_mempool_create(MEMPOOL_NAME, MEMPOOL_N, MEMPOOL_ELT_SIZE,
                                  MEMPOOL_CACHE_SIZE, MEMPOOL_PRIV_DATA_SIZE,
                                  NULL, NULL, NULL, NULL, rte_socket_id(), 0);
    if (!msg_pool) {
        rte_exit(EXIT_FAILURE, "Cannot allocate mempool\n");
    }

    lcore_id = rte_get_next_lcore(-1,1,0);
    if (lcore_id == RTE_MAX_LCORE) {
        rte_exit(EXIT_FAILURE, "Not enough lcores\n");
    }
    /* start the forwarder thread */
    rte_eal_remote_launch(lcore_fwder, NULL, lcore_id);

    lcore_id = rte_get_next_lcore(lcore_id,1,0);
    if (lcore_id == RTE_MAX_LCORE) {
        rte_exit(EXIT_FAILURE, "Not enough lcores\n");
    }
    /* start the producer thread */
    rte_eal_remote_launch(lcore_prod, NULL, lcore_id);

    /* wait for the threads to finish their jobs */
    rte_eal_mp_wait_lcore();

    return 0;
}



