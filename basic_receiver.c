/*-
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

#define RX_RING_SIZE 128
#define TX_RING_SIZE 512

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

static const struct rte_eth_conf port_conf_default = {
	.rxmode = { .max_rx_pkt_len = ETHER_MAX_LEN }
};

static uint64_t
get_ns_time(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t) ts.tv_nsec;	
}


static void 
print_eth_stats(uint8_t portid, uint64_t timediff, uint64_t rx_count)
{
	struct rte_eth_stats stats;

	if (rte_eth_stats_get(portid, &stats)) {
		rte_exit(EXIT_FAILURE, "Couldn't get stats for port %d\n", portid);
	}

	//printf("TX port %"PRIu8 ":\n", portid);
	printf("time diff: %"PRIu64 "ns \n", timediff);
	printf("stats ipackets %"PRIu64 "\n", stats.ipackets);
	printf("stats opackets %"PRIu64 "\n", stats.opackets);
	printf("stats ibytes %"PRIu64 "\n", stats.ibytes);
	printf("stats obytes %"PRIu64 "\n", stats.obytes);
	printf("count opackets %"PRIu64 "\n", rx_count);
	printf("count obytes %"PRIu64 "\n", rx_count*64);
	double timediff_in_second = (double) timediff/ (double) 1000000000;
	double stats_thru = (double) stats.ibytes/ timediff_in_second;
	double count_thru = rx_count*64/timediff_in_second;
	printf("throughput on stats: %f \n", stats_thru);
	printf("throughput on counts: %f \n", count_thru);

}

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
	/* on the current machine, mellanox NIC is on port 0, so we enforce the port=0 here*/
	port=0;
	//FILE *fp;
	//fp = fopen("/tmp/dump.txt", "w");

	
	uint64_t rx_count=0;
	uint64_t counter=0;
	uint8_t flag=0;
	uint64_t start_time=0;
	/* Run until the application is quit or killed. */
	for (;;) {
	//for(int j = 0; j < 65536; j++){	
		/* Get burst of RX packets */
		struct rte_mbuf *bufs[BURST_SIZE];
		/* pull mode devices, so most the time nb_rx can be 0 */ 
		uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
		rx_count +=(uint64_t)nb_rx;
		if(nb_rx>0 && flag==0){
			//printf("%" PRIu64 "\n", rx_count);
			start_time=get_ns_time();
			printf("timer starts!\n");
			flag=1;
		}
		/*else //if(nb_rx>0 && flag==1)
			printf("%" PRIu64 "\n", rx_count);
		*/
		// 2^24	
		if(counter!= 0 && counter%16777216 == 0 && flag == 1){
			uint64_t end_time=get_ns_time();
			print_eth_stats(port, end_time-start_time, rx_count);
		}
		counter++;
		/*if (fp != NULL){
 			//fprintf(fp, "Port number %d \n", port);
			for(int i=0;i < nb_rx;i++){
 				rte_pktmbuf_dump(fp, bufs[i], sizeof(bufs[i]));
				count++;
			}
			printf("%" PRIu16 "\n",count);
		}
		if (unlikely(nb_rx == 0))
			continue;
		*/
		for(int i=0;i< nb_rx;i++)
			rte_pktmbuf_free(bufs[i]);
	}
	
	
	//fclose(fp);
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
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * 64,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initialize all ports. */
	for (portid = 0; portid < nb_ports; portid++)
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n",
					portid);

	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");

	/* Call lcore_main on the master core only. */
	lcore_main();

	return 0;
}
