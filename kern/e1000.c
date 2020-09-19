#include <kern/e1000.h>
#include <kern/pmap.h>
#include <inc/string.h>

// LAB 6: Your driver code here
volatile void *bar_va;
#define E1000REG(offset) (void *)(bar_va + offset)

// transmit related
struct e1000_tdh *tdh;
struct e1000_tdt *tdt;
struct e1000_tx_desc tx_desc_array[TXDESCS];
char tx_buffer_array[TXDESCS][TX_PKT_SIZE];

// receive related
struct e1000_rdh *rdh;
struct e1000_rdt *rdt;
struct e1000_rx_desc rx_desc_array[RXDESCS];
char rx_buffer_array[RXDESCS][RX_PKT_SIZE];

// QEMU's default MAC address
// written from lowest-order byte to highest-order byte
uint32_t E1000_MAC[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

int
e1000_attachfn(struct pci_func *pcif)
{
	pci_func_enable(pcif);
	cprintf("reg_base:%x, reg_size:%x\n", pcif->reg_base[0], pcif->reg_size[0]);

    bar_va = mmio_map_region(pcif->reg_base[0], pcif->reg_size[0]);

	uint32_t *status_reg = (uint32_t *)E1000REG(E1000_STATUS);
	assert(*status_reg == 0x80080783);

    e1000_transmit_init();
    e1000_receive_init();

    /*
	 * transmit test
	 */
	//char *data = "transmit test";
	//e1000_transmit(data, 13);

	return 0;
}

static void
e1000_transmit_init()
{
    int i;
	for (i = 0; i < TXDESCS; i++) {
		tx_desc_array[i].addr = PADDR(tx_buffer_array[i]);
		tx_desc_array[i].cmd = 0;
        // If a descriptor's DD bit is set, you know it's safe
        // to recycle and use it to transmit another packet.
		tx_desc_array[i].status |= E1000_TXD_STAT_DD;
	}

    struct e1000_tdlen *tdlen = (struct e1000_tdlen *)E1000REG(E1000_TDLEN);
	tdlen->len = TXDESCS;

    uint32_t *tdbal = (uint32_t *)E1000REG(E1000_TDBAL);
	*tdbal = PADDR(tx_desc_array);

    // We are using 32-bit address, so high bits part are 0
	uint32_t *tdbah = (uint32_t *)E1000REG(E1000_TDBAH);
	*tdbah = 0;

    // Head and tail are initially index 0 to indicate
    // transmit queue is empty (because all packets have been sent)
    tdh = (struct e1000_tdh *)E1000REG(E1000_TDH);
	tdh->tdh = 0;

	tdt = (struct e1000_tdt *)E1000REG(E1000_TDT);
	tdt->tdt = 0;

    // section 14.5
    struct e1000_tctl *tctl = (struct e1000_tctl *)E1000REG(E1000_TCTL);
	tctl->en = 1;
	tctl->psp = 1;
	tctl->ct = 0x10;
	tctl->cold = 0x40;

    // table 13-77 of section 13.4.34
	struct e1000_tipg *tipg = (struct e1000_tipg *)E1000REG(E1000_TIPG);
	tipg->ipgt = 10;
	tipg->ipgr1 = 4;
	tipg->ipgr2 = 6;
}

int
e1000_transmit(void *data, size_t len)
{
    // you can't just use the TDH (transmit descriptor head) register to check if full
    // Use RS and DD bit to check instead (in hint)
	uint32_t current = tdt->tdt;
	if (!(tx_desc_array[current].status & E1000_TXD_STAT_DD)) {
		return -E_TRANSMIT_RETRY;
	}

	tx_desc_array[current].length = len;
	tx_desc_array[current].status &= ~E1000_TXD_STAT_DD;
	tx_desc_array[current].cmd |= (E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS);
	memcpy(tx_buffer_array[current], data, len);
	uint32_t next = (current + 1) % TXDESCS;
	tdt->tdt = next;
	return 0;
}


static void
get_ra_address(uint32_t mac[], uint32_t *ral, uint32_t *rah)
{
	uint32_t low = 0, high = 0;
	int i;

	for (i = 0; i < 4; i++) {
		low |= mac[i] << (8 * i);
	}

	for (i = 4; i < 6; i++) {
		high |= mac[i] << (8 * i);
	}

    // ral for the lower 32 bits
	*ral = low;
    // rah for the higher 16 bits
    // Set E1000_RAH_AV to compare the high bits against the incoming packet
	*rah = high | E1000_RAH_AV;
}

static void
e1000_receive_init()
{
	uint32_t *rdbal = (uint32_t *)E1000REG(E1000_RDBAL);
	*rdbal = PADDR(rx_desc_array);
	uint32_t *rdbah = (uint32_t *)E1000REG(E1000_RDBAH);
	*rdbah = 0;

	int i;
	for (i = 0; i < RXDESCS; i++) {
		rx_desc_array[i].addr = PADDR(rx_buffer_array[i]);
	}

	struct e1000_rdlen *rdlen = (struct e1000_rdlen *)E1000REG(E1000_RDLEN);
	rdlen->len = RXDESCS;

    // The receive queue is very similar to the transmit queue, 
    // except that it consists of empty packet buffers waiting to be filled with incoming packets
    // rdh points to the first position that you can receive and rdt point to the last position to receive
	rdh = (struct e1000_rdh *)E1000REG(E1000_RDH);
	rdh->rdh = 0;
	rdt = (struct e1000_rdt *)E1000REG(E1000_RDT);
	rdt->rdt = RXDESCS - 1;

	uint32_t *rctl = (uint32_t *)E1000REG(E1000_RCTL);
	*rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC;

	uint32_t *ra = (uint32_t *)E1000REG(E1000_RA);
	uint32_t ral, rah;
	get_ra_address(E1000_MAC, &ral, &rah);
	ra[0] = ral;
	ra[1] = rah;
}

int
e1000_receive(void *addr, size_t *len)
{
    // RDH register cannot be reliably read from software
    // so to determine if a packet has been delivered to the buffer,
    // you'll have to read the DD status bit
	static int32_t next = 0;
	if (!(rx_desc_array[next].status & E1000_RXD_STAT_DD)) {
        // If the DD bit isn't set, then no packet has been received
		return -E_RECEIVE_RETRY;
	}
	if (rx_desc_array[next].errors) {
		cprintf("receive errors\n");
		return -E_RECEIVE_RETRY;
	}
	*len = rx_desc_array[next].length;
	memcpy(addr, rx_buffer_array[next], *len);

    // tell the card that the descriptor is free by updating the RDT
	rdt->rdt = (rdt->rdt + 1) % RXDESCS;

	next = (next + 1) % RXDESCS;
	return 0;
}
