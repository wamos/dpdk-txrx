/*
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
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

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>
#include <rte_debug.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_mempool.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 512

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

static const char *_MSG_POOL = "MSG_POOL";
static const char *_SEC_2_PRI = "SEC_2_PRI";
static const char *_PRI_2_SEC = "PRI_2_SEC";
const unsigned string_size = 64;

struct rte_ring *send_ring, *recv_ring;
struct rte_mempool *message_pool;
volatile int quit = 0;

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

static int
lcore_recv(__attribute__((unused)) void *arg)
{
	unsigned lcore_id = rte_lcore_id();
	
	recv_ring = rte_ring_lookup(_PRI_2_SEC);
       	send_ring = rte_ring_lookup(_SEC_2_PRI);
       	message_pool = rte_mempool_lookup(_MSG_POOL);

	printf("Starting core %u\n", lcore_id);
	printf("quit flag %d", quit);
	//#TODO: while loop is not entered?
	while (!quit){
		void *msg;
		if (rte_ring_dequeue(recv_ring, &msg) < 0){
			usleep(5);
			continue;
		}
		printf("core %u: Received '%s'\n", lcore_id, (char *)msg);
		rte_mempool_put(message_pool, msg);
	}
	printf("after while");

	return 0;
}

/* basicfwd.c: Basic DPDK skeleton forwarding example. */

/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */
static inline int
port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = 1, tx_rings = 1;
	int retval;
	uint16_t q;

	if (port >= rte_eth_dev_count())
		return -1;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(port), NULL);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			(unsigned)port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

	/* Enable RX in promiscuous mode for the Ethernet device. */
	rte_eth_promiscuous_enable(port);

	return 0;
}

/*
 * The lcore main. This is the main thread that does the work, reading from
 * an input port and writing to an output port.
 */
static __attribute__((noreturn)) void
lcore_main(void)
{
	const uint8_t nb_ports = rte_eth_dev_count();
	uint8_t port;

	/*
	 * Check that the port is on the same NUMA node as the polling thread
	 * for best performance.
	 */
	for (port = 0; port < nb_ports; port++)
		if (rte_eth_dev_socket_id(port) > 0 &&
				rte_eth_dev_socket_id(port) !=
						(int)rte_socket_id())
			printf("WARNING, port %u is on remote NUMA node to "
					"polling thread.\n\tPerformance will "
					"not be optimal.\n", port);

	printf("\nCore %u forwarding packets. [Ctrl+C to quit]\n",
			rte_lcore_id());
	
	if (nb_ports != 1)
		rte_exit(EXIT_FAILURE, "ST: Now there must be only a port\n");
	/* on the current machine, mellanox NIC is on port 0, so we enforce port=0 here*/
	port=0;
	uint16_t count=0;
	struct rte_mempool *m_pool;
	/* Run until the application is quit or killed. */
	for (;;) {

		/* Get burst of RX packets */
		struct rte_mbuf *bufs[BURST_SIZE];
		void *msg; //=NULL?, then ret_mempool_get fails
		const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
		count=count+nb_rx;
		m_pool = rte_mempool_lookup(_MSG_POOL);
		if(m_pool == NULL) {
      			printf("Where is my Message pool, pool creation failed\n");
   		}
					
		//rte_mempool:
		//Note that it can return -ENOENT when the local cache and common pool are empty, even if cache from other lcores are full.					
		//if (rte_mempool_get(message_pool, &msg) < 0)
                //	rte_panic("Failed to get message buffer\n");

		if(nb_rx>0){
			//TODO: fix the char printing issues 
			char lo = (char) count & 0xFF;
 			char hi = count >> 8;
			printf("low:%s",lo);
			// we need a char msg[2];
			char* msgchars;
			msgchars = (char *) malloc(sizeof(char)*2);
			msgchars[0] = lo;
			msgchars[1] = hi;
			
			if (rte_mempool_get(message_pool, &msg) < 0)
                        	rte_panic("Failed to get message buffer\n");	

			snprintf((char *)msg, string_size, "%s", msgchars );
			//printf("%s\n", msgchars);
			printf("packet #%" PRIu16 ":%s",count, (char *) msg);
			//printf("rte: %d\n", rte_ring_enqueue(send_ring, msg));
        		if (rte_ring_enqueue(send_ring, msg) < 0) {
                		printf("Failed to send message - message discarded\n");
                		rte_mempool_put(message_pool, msg);
			}
        	}
		
		/*if (fp != NULL){
 			//fprintf(fp, "Port number %d \n", port);
			for(int i=0;i < nb_rx;i++)
 				rte_pktmbuf_dump(fp, bufs[i], sizeof(bufs[i]));
		}
		//if (unlikely(nb_rx == 0))
		//	continue;*/
		for(int i=0;i< nb_rx;i++)
			rte_pktmbuf_free(bufs[i]);
	}
}

/*
 * The main function, which does initialization and calls the per-lcore
 * functions.
 */
int
main(int argc, char *argv[])
{
	struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint8_t portid;
	const unsigned flags = 0;
	const unsigned ring_size = 64;
	const unsigned pool_size = 1024;
	const unsigned pool_cache = 32;
	const unsigned priv_data_sz = 0;
	unsigned lcore_id;

	/* Initialize the Environment Abstraction Layer (EAL). */
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

	argc -= ret;
	argv += ret;

	/* Check that there is an even number of ports to send/receive on. */
	nb_ports = rte_eth_dev_count();
	printf("\nNnumber of Ports: %d\n", nb_ports);
	if (nb_ports != 1)
		rte_exit(EXIT_FAILURE, "ST: Now there must be only a port\n");

	/* Creates a new mempool in memory to hold the mbufs. */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
	for (portid = 0; portid < nb_ports; portid++)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n",
					portid);

	//if (rte_lcore_count() > 1)
	//	printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	/*multi-thread*/
	send_ring = rte_ring_create(_PRI_2_SEC, ring_size, rte_socket_id(), flags);
        recv_ring = rte_ring_create(_SEC_2_PRI, ring_size, rte_socket_id(), flags);
        message_pool = rte_mempool_create(_MSG_POOL, pool_size,
       				string_size, pool_cache, priv_data_sz,
                        	NULL, NULL, NULL, NULL,rte_socket_id(), flags);
	/*	
        if (rte_eal_process_type() == RTE_PROC_PRIMARY){
                send_ring = rte_ring_create(_PRI_2_SEC, ring_size, rte_socket_id(), flags);
                recv_ring = rte_ring_create(_SEC_2_PRI, ring_size, rte_socket_id(), flags);
                message_pool = rte_mempool_create(_MSG_POOL, pool_size,
                                string_size, pool_cache, priv_data_sz,
                                NULL, NULL, NULL, NULL,
                                rte_socket_id(), flags);
        } else {
                recv_ring = rte_ring_lookup(_PRI_2_SEC);
                send_ring = rte_ring_lookup(_SEC_2_PRI);
                message_pool = rte_mempool_lookup(_MSG_POOL);
        }
        if (send_ring == NULL)
                rte_exit(EXIT_FAILURE, "Problem getting sending ring\n");
        if (recv_ring == NULL)
                rte_exit(EXIT_FAILURE, "Problem getting receiving ring\n");
        if (message_pool == NULL)
                rte_exit(EXIT_FAILURE, "Problem getting message pool\n");
	
        //RTE_LOG(INFO, APP, "Finished Process Init.\n");
	*/
        //call lcore_recv() on every slave lcore
        RTE_LCORE_FOREACH_SLAVE(lcore_id) {
                rte_eal_remote_launch(lcore_recv, NULL, lcore_id);
        }
	

	/* Call lcore_main on the master core only. */
	lcore_main();

	//rte_eal_mp_wait_lcore();

	return 0;
}
