#include <pcap.h>

#include "common.h"

#include <rte_pci.h>
#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>

#define VER_16

#define DPDKPCAP_MBUF_SIZE       (2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define DPDKPCAP_NB_MBUF         512
#define DPDKPCAP_CACHE_SIZE      32
#define DPDKPCAP_RX_QUEUE_NUMBER 1
#define DPDKPCAP_TX_QUEUE_NUMBER 1
#define DPDKPCAP_IF_NAMESIZE     16

int initFinished = 0;
int portInitFinished[RTE_MAX_ETHPORTS] = {0};
rte_atomic16_t startRx = RTE_ATOMIC16_INIT(0);

struct rte_mempool* rxPool = 0;
#define DPDKPCAP_RX_POOL_NAME "RX_POOL"
#define DPDKPCAP_RX_QUEUE_DESC_NUMBER 128

struct rte_mempool* txPool = 0;
#define DPDKPCAP_TX_POOL_NAME "TX_POOL"
#define DPDKPCAP_TX_QUEUE_DESC_NUMBER 128

char* deviceNames[RTE_MAX_ETHPORTS] = {NULL};

DpdkPcapResultCode_t globalInit(char *errbuf)
{
    char *args[] = {"dpdkpcap_test", "-c 0x03", "-n 2", "-m 128", "--file-prefix=dpdkpcap_test"};

    if (initFinished == 1)
    {
        return DPDKPCAP_OK;
    }

    if (rte_eal_init(sizeof(args)/sizeof(char*), args) < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not initialize DPDK");
        return DPDKPCAP_FAILURE;
    }

#ifdef VER_16
    if (rte_pmd_init_all() < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not init driver");
        return DPDKPCAP_FAILURE;
    }
#endif

    if (rte_eal_pci_probe() < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not probe devices");
        return DPDKPCAP_FAILURE;
    }

    rxPool = rte_mempool_create(DPDKPCAP_RX_POOL_NAME,
             DPDKPCAP_NB_MBUF,
             DPDKPCAP_MBUF_SIZE,
             DPDKPCAP_CACHE_SIZE,
             sizeof(struct rte_pktmbuf_pool_private),
             rte_pktmbuf_pool_init, NULL,
             rte_pktmbuf_init, NULL,
             SOCKET_ID_ANY,
             MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET);

    if(rxPool == NULL)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not allocate RX memory pool");
        return DPDKPCAP_FAILURE;
    }

    txPool = rte_mempool_create(DPDKPCAP_TX_POOL_NAME,
             DPDKPCAP_NB_MBUF,
             DPDKPCAP_MBUF_SIZE,
             DPDKPCAP_CACHE_SIZE,
             sizeof(struct rte_pktmbuf_pool_private),
             rte_pktmbuf_pool_init, NULL,
             rte_pktmbuf_init, NULL,
             SOCKET_ID_ANY,
             MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET);

    if(txPool == NULL)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not allocate TX memory pool");
        return DPDKPCAP_FAILURE;
    }

    initFinished = 1;
    return DPDKPCAP_OK;
}

DpdkPcapResultCode_t deviceInit(int deviceId, char *errbuf)
{
    struct rte_eth_conf portConf;
    struct rte_eth_rxconf rxConf;
    struct rte_eth_txconf txConf;
    int queueId = 0;
    int ret = 0;

    memset(&portConf, 0, sizeof(portConf));
    memset(&rxConf, 0, sizeof(rxConf));
    memset(&txConf, 0, sizeof(txConf));

    if (initFinished == 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Global DPDK init is not performed yet");
        return DPDKPCAP_FAILURE;
    }

    if (portInitFinished[deviceId] == 1)
    {
        return DPDKPCAP_OK;
    }

    portConf.rxmode.split_hdr_size = 0;
    portConf.rxmode.header_split   = 0;
    portConf.rxmode.hw_ip_checksum = 0;
    portConf.rxmode.hw_vlan_filter = 0;
    portConf.rxmode.jumbo_frame    = 0;
    portConf.rxmode.hw_strip_crc   = 0;
    portConf.txmode.mq_mode = ETH_MQ_TX_NONE;

    if (rte_eth_dev_configure(deviceId, DPDKPCAP_RX_QUEUE_NUMBER, DPDKPCAP_TX_QUEUE_NUMBER, &portConf) < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not configure the device %d", deviceId);
        return DPDKPCAP_FAILURE;
    }

    rxConf.rx_thresh.pthresh = DPDKPCAP_RX_PTHRESH;
    rxConf.rx_thresh.hthresh = DPDKPCAP_RX_HTHRESH;
    rxConf.rx_thresh.wthresh = DPDKPCAP_RX_WTHRESH;

    if (rte_eth_rx_queue_setup(deviceId, queueId, DPDKPCAP_RX_QUEUE_DESC_NUMBER, SOCKET_ID_ANY, &rxConf, rxPool) < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not setup RX queue of the device %d", deviceId);
        return DPDKPCAP_FAILURE;
    }

    txConf.tx_thresh.pthresh = DPDKPCAP_TX_PTHRESH;
    txConf.tx_thresh.hthresh = DPDKPCAP_TX_HTHRESH;
    txConf.tx_thresh.wthresh = DPDKPCAP_TX_WTHRESH;

    if (rte_eth_tx_queue_setup(deviceId, queueId, DPDKPCAP_TX_QUEUE_DESC_NUMBER, SOCKET_ID_ANY, &txConf) < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not setup TX queue of the device %d", deviceId);
        return DPDKPCAP_FAILURE;
    }

    portInitFinished[deviceId] = 1;

    return DPDKPCAP_OK;
}

DpdkPcapResultCode_t deviceDeInit(int deviceId)
{
    if (portInitFinished[deviceId] == 0)
    {
        return DPDKPCAP_FAILURE;
    }

    rte_eth_dev_stop(deviceId);

    portInitFinished[deviceId] = 0;

    return DPDKPCAP_OK;
}

int findDevice(const char *source, char *errbuf)
{
    int i = 0;

    for (i = 0; i < sizeof(deviceNames); i++)
    {
// TBD replace hard-coded name size
        if (strncmp(source, deviceNames[i], 16) == 0)
            return i;
    }

    snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not find device %s", source);
    return -1;
}

pcap_t* pcap_open_live(const char *source, int snaplen, int promisc, int to_ms, char *errbuf)
{
    pcap_t *p = NULL;
    int deviceId = 0;

    printf("Opening device %s\n", source);

    if (initFinished == 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Global DPDK init is not performed yet");
        return NULL;
    }

    deviceId = findDevice(source, errbuf);
    if (deviceId < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Did not find the device %s", source);
        return NULL;
    }

    if (promisc)
        rte_eth_promiscuous_enable(deviceId);
    else
        rte_eth_promiscuous_disable(deviceId);

    if (rte_eth_dev_start(deviceId) < 0)
    {
        snprintf (errbuf, PCAP_ERRBUF_SIZE, "Could not start the device %d", deviceId);
        return NULL;
    }

    p = malloc (sizeof(pcap_t));
    p->deviceId = deviceId;

    return p;
}

int pcap_loop(pcap_t *p, int cnt, pcap_handler callback, u_char *user)
{
    if (initFinished == 0)
    {
        return DPDKPCAP_FAILURE;
    }

    return DPDKPCAP_FAILURE;
}

void pcap_close(pcap_t *p)
{
    char *deviceName = NULL;

    deviceName = deviceNames[p->deviceId];
    printf("Closing device %s\n", deviceName);

    rte_eth_dev_stop(p->deviceId);
}

int pcap_findalldevs(pcap_if_t **alldevsp, char *errbuf)
{
    int       port     = 0;
    pcap_if_t *pPcapIf = NULL;
    pcap_if_t *pPcapPrevious = NULL;
    struct rte_eth_dev_info info;

    if (globalInit(errbuf) != DPDKPCAP_OK)
    {        
        return DPDKPCAP_FAILURE;
    }

    int portsNumber = rte_eth_dev_count();
    if (portsNumber < 1)
    {
        return DPDKPCAP_FAILURE;
    }

    printf ("Discovered %d devices\n", portsNumber);

    for (port = 0; port < portsNumber; port++)
    {
        if (deviceInit(port, errbuf) == DPDKPCAP_FAILURE)
        {
            return DPDKPCAP_FAILURE;
        }

        pPcapIf = malloc(sizeof(pcap_if_t));

        if (pPcapPrevious)
            pPcapPrevious->next = pPcapIf;
        else
            *alldevsp = pPcapIf;

        pPcapPrevious = pPcapIf;

        rte_eth_dev_info_get(port, &info);

        pPcapIf->name = malloc(DPDKPCAP_IF_NAMESIZE);
        snprintf(pPcapIf->name, DPDKPCAP_IF_NAMESIZE, "enp%us%u",
                 info.pci_dev->addr.bus,
                 info.pci_dev->addr.devid);

        deviceNames[port] = pPcapIf->name;

        printf("Allocating memory for %s\n", pPcapIf->name);
    }

    pPcapPrevious->next = NULL;

    return DPDKPCAP_OK;
}

void pcap_freealldevs(pcap_if_t *alldevs)
{
    pcap_if_t *device = NULL;
    pcap_if_t *nextDevice = NULL;

    if (initFinished == 0)
    {
        return;
    }

    for(device = alldevs; device != NULL; device = nextDevice)
    {
        printf("Releasing memory for %s\n", device->name);
        free(device->name);

        nextDevice = device->next;
        free(device);
    }
}

int pcap_sendpacket(pcap_t *p, const u_char *buf, int size)
{
    int ret = 0;
    struct rte_mbuf* mbuf = NULL;

    mbuf = rte_pktmbuf_alloc(rxPool);

    rte_memcpy(rte_pktmbuf_mtod(mbuf, char*), buf, size);

    printf("Sending a packet to port %d\n", p->deviceId);

    ret = rte_eth_tx_burst(p->deviceId, 0, &mbuf, 1);
    if (ret < 1)
    {
        printf("rte_eth_tx_burst failed on port %d\n", p->deviceId);
        rte_pktmbuf_free(mbuf);
        return DPDKPCAP_FAILURE;
    }

    return DPDKPCAP_OK;
}

pcap_dumper_t * pcap_dump_open(pcap_t *p, const char *fname)
{
    return NULL;
}

int pcap_next_ex(pcap_t *p, struct pcap_pkthdr **pkt_header,
    const u_char **pkt_data)
{
    int ret = 0;
    struct rte_mbuf* mbuf = NULL;

    printf("Receiving a packet on port %d\n", p->deviceId);

    ret = rte_eth_rx_burst(p->deviceId, 0, &mbuf, 1);
    if (ret < 1)
    {
        printf("rte_eth_rx_burst failed on port %d\n", p->deviceId);
        return DPDKPCAP_FAILURE;
    }

    *pkt_data = malloc (100);

    rte_memcpy((void*)*pkt_data, rte_pktmbuf_mtod(mbuf, void*), 100);

    rte_pktmbuf_free(mbuf);

    return DPDKPCAP_OK;
}

void pcap_dump(u_char *user, const struct pcap_pkthdr *h, const u_char *sp)
{
}

char* pcap_geterr(pcap_t *p)
{
    return NULL;
}

void pcap_dump_close(pcap_dumper_t *p)
{
}

int pcap_setdirection(pcap_t *p, pcap_direction_t d)
{
    return DPDKPCAP_FAILURE;
}

void pcap_breakloop(pcap_t *p)
{
}

DpdkPcapResultCode_t sendPacket(int deviceId, const u_char *buf, int size)
{
    struct rte_mbuf *mbuf = NULL;

    if (initFinished == 0)
    {
        return DPDKPCAP_FAILURE;
    }

    mbuf = rte_pktmbuf_alloc(rxPool);

    rte_memcpy(rte_pktmbuf_mtod(mbuf, char*), buf, size);

    rte_eth_tx_burst(deviceId, 0, &mbuf, size);

    return DPDKPCAP_FAILURE;
}

void startRxLoop()
{
    rte_atomic16_set(&startRx, 1);
}

void stopRxLoop()
{
    rte_atomic16_set(&startRx, 0);
}

int isRxLoopStarted()
{
    return (rte_atomic16_read(&startRx) == 1);
}

int rxLoop(void* arg)
{
    while(isRxLoopStarted())
    {
    }
}
