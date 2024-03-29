/*
 * Copyright (C) 2000-2005 by ETH Zurich
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY ETH ZURICH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL ETH ZURICH
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * For additional information see http://www.btnode.ethz.ch/
 */

/**
 * \author Matthias Ringwald
 *
 * \brief Distributed Sniffer using the MHOP connection-less service
 * 
 * With this application it is possible to sniff Chipcon traffic
 * and forward it over a multi-hop bluetooth connection to a PC/PDA
 *
 * We use a priority queue (sorted by time) to receive packets at the
 * sink partially ordered.
 *
 * Time synchronisation is done using the Bluetooth clock offset between
 * two BTnodes as a reference
 *
 */

#include <string.h>    // libc memcpy
#include <stdlib.h>     // rand, srand
#include <stdio.h>     // nut  printf, reopen, close, ..
#include <sys/heap.h>  // NutHeapAlloc
#include <sys/event.h> 
#include <sys/thread.h>
#include <sys/timer.h>
#include <dev/usartavr.h>  // usart
#include <hardware/btn-hardware.h>
#include <led/btn-led.h>

#include <bt/bt_hci_defs.h>
#include <bt/bt_hci_cmds.h>
#include <bt/bt_acl_defs.h>
#include <bt/bt_psm.h>
#include <bt/l2cap_cl.h>
#include <bt/bt_l2cap.h>
#include <bt/bt_semaphore.h>

#include <cc/sniffer.h>
#include <cc/crc.h>

#include <mhop/mhop_cl.h>
#include <mhop/mhop_init.h>

#include <time/bt_time_sync.h>

#include <support/bt_remoteprog.h>

#include <terminal/btn-terminal.h>
#include <terminal/bt_psm-cmds.h>
#include <terminal/bt-cmds.h>
#include <terminal/log-cmds.h>
#include <terminal/mhop_cl-cmds.h>
#include <terminal/l2cap-cmds.h>

#include <debug/logging.h>
#include <debug/toolbox.h>


// choose your connection manger here!
#include <cm/cm_tree.h>

// access internal functions
u_char _bt_cm_get_nr_reliable_cons(void);


#include "program_version.h"

#define CM_COD              955 // mhop example 933

#define MAX_NR_SERVICES 	16

#define CM_PSM				0x1003
#define MHOP_PSM			0x1005
#define SNIF_L2CAP_PSM      0x1011  // 4113
#define SNIF_CONFIG_PSM		0x1013
#define SNIF_PACKET_PSM     0x1017

#define SNIFFED_PACKETED_BUFFER_SIZE 16

#define MAX_PAYLOAD_SIZE 100

enum SNIF_PACKET_TYPES {
	config = 'c', sniffed = 'p'
};

struct sniffed_packet {
	/** queue */
	u_char    free;
	u_long    key;
	/** data bt_addr MUST be first field */
	bt_addr_t bt_addr;		// 
	u_long    timestamp;	// 
	u_char    len;
	u_char    data[255];
};
#define SNIFFED_PACKET_HEADER_LEN 11 

/** priority sniffed packet queue*/
struct sniffed_packet *  packet_buffers;
struct sniffed_packet ** packet_queue;
u_short packet_count;

/** bt stack */
struct btstack* bt_stack;
struct bt_l2cap_stack* l2cap_stack;
bt_addr_t my_addr;

/** state of l2cap connection */
static u_char connected = 0;
u_short l2cap_channel_id;
u_char l2cap_service;
struct bt_l2cap_acl_pkt * l2cap_pkt;
u_long lastPacketSendToHost = 0;

/** snif sink */
u_char    snif_have_sink;
u_char	  snif_am_sink;
bt_addr_t snif_sink;

/** used to start SNIF sniffer */
HANDLE snif_config_queue;
/** sniffer config */
struct sniffer_config snif_config;


/** used to signal SNIF worker state machine */ 
HANDLE snif_event_queue;
/** config   has to be sent by thread */
u_char snif_send_config;
/** set sniffer config */
u_char snif_set_config;

/** info on network */
bt_hci_con_handle_t rel_cons[20]; 

/** problem with data forwarding */
u_char packet_queue_warning = 0;

// deal with missing functions in unix-emulation
#ifdef __BTN_UNIX__
void sniffer_config(struct sniffer_config *config){
}
#endif


///////////////////////////////////////////////////////////////////////////////////////////////////////
// 
//  priorty queue of sniffed packets 
//

/**
 * init packet queue
 */
 void packet_buffer_init(void){
	int i;
	packet_buffers = NutHeapAlloc( sizeof(struct sniffed_packet)   * SNIFFED_PACKETED_BUFFER_SIZE );
	packet_queue   = NutHeapAlloc( sizeof(struct sniffed_packet *) * SNIFFED_PACKETED_BUFFER_SIZE );
	packet_queue[0] = (void*)0;
	for (i=0;i<SNIFFED_PACKETED_BUFFER_SIZE;i++){
		packet_buffers[i].free = 1;
		packet_queue[i] = (void*)0;
	}
	packet_count = 0;
}
/**
 * get free packet buffer, buffer is marked as non-free
 */
struct sniffed_packet * packet_queue_get_empty(void){
	int i;
	for (i=0;i<SNIFFED_PACKETED_BUFFER_SIZE;i++){
		if (packet_buffers[i].free){
			packet_buffers[i].free = 0;
			return &packet_buffers[i];
		}
	}
	return (void*) 0;
}

/**
 * insert packet in queue by key
 * @assert packet was retrieved by packet_queue_get_empty, #free in queue >= # free in buffer
 */
void   packet_queue_insert( struct sniffed_packet *pkt, u_long key) {
	int pos = 0;
	int last = SNIFFED_PACKETED_BUFFER_SIZE-1;
	while (pos < SNIFFED_PACKETED_BUFFER_SIZE && packet_queue[pos] && packet_queue[pos]->key < key){
		pos++;
	}
	while (last - 1 >= pos){
		packet_queue[last] = packet_queue[last-1];
		last--;
	}
	packet_queue[pos] = pkt;
	pkt->key = key;
	packet_count++;
}

/**
 * get the first packet in queue
 * @return packet or null if no packet in queue 
 */
 struct sniffed_packet * packet_queue_get_next(void){
	int i;
	struct sniffed_packet * buffer = packet_queue[0];
	if (buffer) {
		for(i=0;i<SNIFFED_PACKETED_BUFFER_SIZE - 1;i++){
			packet_queue[i] = packet_queue[i+1];
		}
		packet_count--;
	}
	return buffer;
}

/**
 * free a packet returned by packet_queue_next
 */
void packet_buffer_free(struct sniffed_packet * pkt){
	pkt->free = 1;
}



///////////////////////////////////////////////////////////////////////////////////////////////////////
// 
//  l2cap-cl (mhop) and l2cap data and connection handler
//

void print_hex_data( char *format, u_char *data, u_char len) {
    int i;
    printf(format, len);
    for (i=0;i<len;i++) {
        printf("%02x ", data[i]); 
    }
    printf("\n");
}

/**
* \brief Data callbacks for the various snif packet services
 * 
 * This callback is called each time a snif cl packed arrives.
 * 
 * \param msg Pointer to the received packet
 * \param data Pointer to the payload of the received packet
 * \param data_len Size of the payload
 * \param service_nr The service number this callback corresponds to
 * \param cb_arg Callback argument: could be defined when registering at
 * the protocol/service multiplexor (see #mhop_blink_service_register).
 * Unused in this example.
 */

/** 
* config data received
* - store config
* - store sink address
*/
static bt_acl_pkt_buf* cl_config(bt_acl_pkt_buf* pkt_buf,
                                 u_char* data,
                                 u_short data_len,
                                 u_short service_nr,
                                 void* cb_arg)
{
	u_char * source;
	source = (u_char*) mhop_cl_get_source_addr(pkt_buf->pkt);
	// printf("cl_config, source "ADDR_FMT"\n", ADDR(source));
    
	// store config
    // print_hex_data( "SNIFFER: cl_data config data(%u): ", (void*) data, sizeof(struct sniffer_config));
	memcpy ( (void*) &snif_config, data, sizeof(struct sniffer_config));
    // print_hex_data( "SNIFFER: cl_data config snif_config (%u): ", (void*) &snif_config, sizeof(struct sniffer_config));
	snif_set_config =  1;
	
	// store route to sink
	memcpy( (void*) &snif_sink, (void*) source, 6);
	if (snif_have_sink == 0) {
        // disable inquiry now
        con_mgr_inq_disable();
        snif_have_sink = 1;
	}
    
	// ping worker 
	NutEventPost(&snif_event_queue);
	
	// free the received message
	return pkt_buf;
}


/**
* sniffed packet received 
 * - copy into queue
 */
static bt_acl_pkt_buf* cl_sniffed(bt_acl_pkt_buf* pkt_buf,
                                  u_char* data,
                                  u_short data_len,
                                  u_short service_nr,
                                  void* cb_arg)
{
	struct sniffed_packet * packet;
	u_char * source;
	source = (u_char*) mhop_cl_get_source_addr(pkt_buf->pkt);
	// printf("cl_sniffed, source "ADDR_FMT"\n", ADDR(source));
	
	// store sniffed packet and enter timestamp
	packet = packet_queue_get_empty();
	if (packet) {
		memcpy( (void*) &packet->bt_addr, (void *) data, data_len);
        packet->timestamp = bt_time_sync_get_time( pkt_buf );
		packet_queue_insert( packet, packet->timestamp);
        
		// ping worker
		NutEventPost(&snif_event_queue);
	} else {
		// printf("cl_sniffed: packet queue full, dropping packet\n");
	}
 	
	// free the received message
	return pkt_buf;
}

static void _snif_co_data_cb(struct bt_l2cap_acl_pkt *pkt, u_char service_nr, u_short channel_id, void *arg)
{
	enum SNIF_PACKET_TYPES type = pkt->payload[0];
    // print_hex_data( "SNIFFER: L2CAP DATA received (%u): ", (u_char*) &pkt->payload, pkt->len[0] | (pkt->len[1] << 8));
    
	switch (type) {
		case config:
    		// store config, set and broadcast it
            // dump 
			memcpy( (void *) &snif_config, (void *) &pkt->payload[1], sizeof(struct sniffer_config));
            // print_hex_data( "SNIFFER: copied config (%u): ", (u_char*) &snif_config, sizeof(struct sniffer_config));
			snif_send_config = 1;
			NutEventPost(&snif_event_queue);
			break;
		case sniffed:
		default:
			printf("L2CAP DATA CB: Unknown packet type %c\n", (u_char) type);
			// should not happen, just ignore
			break;
	} 
    bt_l2cap_complete_pkt(pkt);
}

static void _snif_con_cb(u_char type, u_char detail, u_char service_nr, u_short channel_id, void *arg)
{
    if (type == BT_L2CAP_CONNECT) {
        connected = 1;
		snif_am_sink = 1;
        l2cap_channel_id = channel_id;
        printf("L2CAP connect. Unique handle: %04x\n", channel_id);
    } else {
        connected = 0;
		snif_am_sink = 0;
    	snif_have_sink = 0;
        printf("L2CAP disconnect. Unique handle: %04x\n", channel_id);
    }
}


/**
* \brief Registers the "snif" service at the protocol/service multiplexor
 * 
 * For that the "snif" service is accessible by remote devices, it has
 * to be registered at the protocol/service multiplexor: On a receiving device,
 * the protocol/service multiplexor uses the unique identifier \param psm
 * found in a received packet to deliver it to the service registered at
 * psm \param psm.
 * 
 * \param psmux Pointer to the protocol/service multiplexor
 * \param psm PSM to use for the "snif" service
 */
void snif_cl_service_register(bt_psm_t* psmux)
{
	long snif_service_nr;
	
	// register "snif_config" service at psmux
	snif_service_nr = bt_psm_service_register(psmux, SNIF_CONFIG_PSM, cl_config, NULL);
	bt_psm_service_set_buffers(psmux, snif_service_nr, NULL);
    
	// register "snif_sniffed" service at psmux
	snif_service_nr = bt_psm_service_register(psmux, SNIF_PACKET_PSM, cl_sniffed, NULL);
	bt_psm_service_set_buffers(psmux, snif_service_nr, NULL);
}

/**
* register l2cap snif service
 */
void snif_co_service_register(struct bt_l2cap_stack *stack, u_char nr_buffer, u_short min_mtu, u_short max_mtu)
{
    l2cap_service = bt_l2cap_register_service(SNIF_L2CAP_PSM, nr_buffer, min_mtu, max_mtu, _snif_con_cb, _snif_co_data_cb, NULL);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////
// 
//  SNIF state machine 
//


/**
 * forward sniffed packet
 * if gateway forward over l2cap, otherwise use mhop
 */
void sendSniffedPacket( struct sniffed_packet *pkt){
	u_short packet_size = SNIFFED_PACKET_HEADER_LEN + pkt->len;
    if (packet_size > MAX_PAYLOAD_SIZE) {
        printf("====> packet_size %u > MAX_PAYLOAD_SIZE(%u),  discarding packet!!!\n", packet_size, MAX_PAYLOAD_SIZE);
        return;
    }
	void *  packet_content =  (void*) &pkt->bt_addr;
	// printf("sendSniffedPacket from "ADDR_FMT", t = %lu \n", ADDR(pkt->bt_addr), pkt->timestamp);

	if (snif_am_sink) {
		// send packet over l2cap
		memcpy( &l2cap_pkt->payload[0] ,  packet_content, packet_size);
		bt_l2cap_send( l2cap_channel_id, l2cap_pkt, packet_size);
		lastPacketSendToHost = NutGetMillis();
        // print_hex_data("data (%u): ", (void*)packet_content, packet_size);
	} else if (snif_have_sink) {
		// send packet over mhop
		bt_time_sync_send_mhop_pkt(pkt->timestamp, packet_content, packet_size, snif_sink, SNIF_PACKET_PSM, MHOP_CL_UNICAST, MHOP_CL_TTL_INFINITE);
	}
}

/**
 * send tick (and bt clock) to host
 * 
 * the tick is an EMPTY packet (only the DSN sniffed header)
 */

void sendTick( void ){
	u_long timestamp =  NutGetMillis();
	// send packet over l2cap
	u_short packet_size = SNIFFED_PACKET_HEADER_LEN;
	// set bt addr (->bt_addr)
	bt_hci_get_local_bt_addr( bt_stack, (void *) &l2cap_pkt->payload[0]);
	// convert to root time (->timestamp)
	* (u_long*) &l2cap_pkt->payload[6] = timestamp;
	// set data (->len)
	l2cap_pkt->payload[10] = 0;
	// printf("sendTick, t = %lu \n", timestamp);
	bt_l2cap_send( l2cap_channel_id, l2cap_pkt, packet_size);
	lastPacketSendToHost = NutGetMillis();
}

/** 
 * broadcast snif config 
 */
void broadcastConfig(void){
	// printf("broadcastConfig\n");
	mhop_cl_send_pkt((void*)&snif_config, sizeof(struct sniffer_config), bt_addr_null, SNIF_CONFIG_PSM, MHOP_CL_BROADCAST, MHOP_CL_TTL_INFINITE);
}

/**
 * snif sniffer thread
 * 
 * wait for config
 * sniff packets and store in packet queue
 *
 */

void prettyPrintConfig(void) {
    // dump 
    print_hex_data( "SNIFFER: snif config received (%u): ", (u_char *) &snif_config, sizeof(struct sniffer_config) );
    printf("> freq %lu\n", snif_config.freq);
    printf("> sopLength %u\n", snif_config.sopLength);
    if (snif_config.sopLength > 1) {
        printf("> sopWord %02x%02x\n", snif_config.sopFirst, snif_config.sopSecond);
    } else {
        printf("> sopByte %x\n", snif_config.sopFirst);
    }
    if (snif_config.fixedSize) {
        printf("> packetSize = %u\n", snif_config.headerSize);
    } else {
        printf("> headerSize = %u\n", snif_config.headerSize);
        printf("> lengthPos = %u\n", snif_config.lengthPos);
        printf("> lengthOffset = %u\n", snif_config.lengthOffset);
    }
    if (snif_config.crcLength == 2) {
        printf("> crc len  = %u\n", snif_config.crcLength);
        printf("> crc word = %02x\n", snif_config.crcPoly);
        printf("> crc pos  = %u\n", snif_config.crcPos);
    } 
}


void packetGenerator(void){
	struct sniffed_packet * packet;
	u_long fake_timestamp = 1;

	while(1){
	
		NutSleep( 20 );

		// create sniffer packet	
		packet = packet_queue_get_empty();
		if (packet) {
			// set timestampp
			packet->timestamp = fake_timestamp;
			// set bt addr
			bt_hci_get_local_bt_addr( bt_stack, packet->bt_addr);
			// set data
			packet->len = 4;
			packet->timestamp = NutGetMillis();
			// insert
			packet_queue_insert( packet, packet->timestamp);
			// ping worker
			NutEventPost(&snif_event_queue);
		} else {
			printf("SNIFFER: packet queue full, dropping packet\n");
		}
	}
}

/**
 * sniffer timestamps are os ticks, with a 1024 hz default
 */
u_long convertNutTicksToMillis(u_long ticks) {
    // carefully stay within 32 bit values, explicitly use 1024 hz ticks
    u_long seconds = ticks >> 10;
    ticks          &= 0x3ff;
    return seconds * 1000 + (ticks * 125) / 128;
}


THREAD ( SNIFFER, arg){
#if (defined __BTNODE3__) && (!defined FAKE_DATA)
    u_short length;
    u_short result;
    u_short src;
    u_short dst;
    u_short packetCRC;
    u_short calcCRC;
	struct sniffed_packet * packet;

	printf("SNIFFER: started\n");
	snif_config_queue = 0;
	NutEventWait(&snif_config_queue, NUT_WAIT_INFINITE);
	sniffer_init();

    // INSOMNIA! sleep mode causes chipcon reception to collapse
    NutThreadSetSleepMode(SLEEP_MODE_NONE);

	printf("SNIFFER: config set, ready\n");

    while(1){
        // reserve empty sniffer packet	
        do {
            packet = packet_queue_get_empty();
            if (packet == NULL){
                NutSleep(100); 
                if (packet_queue_warning == 0) {
                    printf("SNIFFER: packet queue full!\n");
                    packet_queue_warning = 1;
                }
            }
        } while (packet == NULL);
        if ( packet_queue_warning ) {
            printf("SNIFFER: packet queue recovered. :)!\n");
            packet_queue_warning = 0;
        }
    
        // sniff packet
        do {
            length = 100;
            result = sniffer_receive_extra(&src, &dst, &packet->data[0], &length, 1000, NULL, NULL, &packet->timestamp);
            if (result == 0) {
                // check crc
                packetCRC = packet->data[length-1] | (((u_short) packet->data[length-2]) << 8);
                calcCRC = crc_ccitt_compute(&packet->data[0], length-2);
//                if (packetCRC != calcCRC) {
//
// disable CRC CHECK!
//                    result = -1;
//                }
                if (packetCRC == calcCRC) {
                    printf("CRC ok! (%04x)", packetCRC);
                } else {
                    printf("CRC WRONG! packet %04x, calc %04x\n", packetCRC, calcCRC);
                }
                print_hex_data( "PACKET (%u): ", (u_char *) &packet->data[0], length);

            }
        } while (result != 0);
        // set bt addr
        bt_hci_get_local_bt_addr( bt_stack, packet->bt_addr);
        // set data
        packet->len = (u_char) length;
        // TODO: convert to system time (packet->timestamp is in nut os ticks)
        packet->timestamp = convertNutTicksToMillis( packet->timestamp);
        // insert
        packet_queue_insert( packet, packet->timestamp);
        // ping worker
        NutEventPost(&snif_event_queue);
    }	
#else
	printf("SNIFFER: started\n");
	snif_config_queue = 0;
	NutEventWait(&snif_config_queue, NUT_WAIT_INFINITE);
	printf("SNIFFER: config set, ready\n");
    packetGenerator();
#endif
    while (1) { NutSleep(1000); };
}


/**
 * snif worker thread
 * 
 * config local MAC sniffer
 * broadcast snif configuration to mhop
 * sent timestamp to neighbours
 * forward sniffed packets 
 */
 
THREAD ( WORKER, arg){

	struct sniffed_packet * packet;

	snif_event_queue = 0;
	snif_send_config = 0;
	snif_set_config = 0;

	printf("WORKER: started\n");
	
	while(1){
	
		// poll at least every second
		NutEventWait(&snif_event_queue, 1000 );
		
		// configure MAC sniffer
		if (snif_set_config){
            // prettyPrintConfig();
			sniffer_config(&snif_config);
			snif_set_config = 0;
			NutEventPost( &snif_config_queue);
		}
		
		// check networking. if not sink and no reliable connections, enable periodic enquiry
        if (snif_have_sink && snif_am_sink == 0) {
            if (_bt_cm_get_nr_reliable_cons() == 0) {
                con_mgr_inq_enable();
                snif_have_sink = 0;
            }
        }
        
        // forward packets in packet queue
		while (packet_count > 0){
			packet = packet_queue_get_next();
			sendSniffedPacket( packet );
			packet_buffer_free( packet );
		}

		// SINK ONLY
        
		// broadcast MAC sniffer config on mhop
		if (snif_send_config){
			broadcastConfig();
			snif_send_config = 0;
		}
       
		// send time info to host after 900 ticks (1024 ticks per second)
		if (snif_am_sink && ((lastPacketSendToHost + 900) < NutGetMillis())){
			sendTick();
		}
	}
}

/**
 * \brief Initialization routine
 * 
 * Initialization routinge that initializes the hardware, the led's the
 * terminal, as well as the bluetooth and the multi-hop communication
 * stacks.
 * 
 * At the end of the routine, the "blink" service is registered at
 * the protocol/service multiplexor, and the command for sending a
 * "blink" request is registered at the terminal.
 */
int main(void)
{	
    // serial baud rate
    u_long baud = 57600;

    // hardware init
    btn_hardware_init();
    btn_led_init(1);
    
    // init terminal app uart
    NutRegisterDevice(&APP_UART, 0, 0);
    freopen(APP_UART.dev_name, "r+", stdout);
    _ioctl(_fileno(stdout), UART_SETSPEED, &baud);
        
    // init event logger
    log_init();
    
    // hello world!
    printf("\n# --------------------------------------------");
    printf("\n# Welcome to BTnut (c) 2006 ETH Zurich\n");
    printf("# program version: %s\n", PROGRAM_VERSION);
    printf("# --------------------------------------------");
    printf("\nbooting bluetooth module... ");

    // bluetooth module on (takes a while)
    btn_hardware_bt_on();
    printf("ok.\n\r");
    
    // Start bt-stack and let the initialization begin
    printf("init bt-stack... ");
    bt_stack = bt_hci_init(&BT_UART);
    printf("done.\n");

    // use addr for random
    bt_hci_get_local_bt_addr(bt_stack, my_addr);
    srand( * (u_short *) &my_addr[0] );

    // init l2cap using same packet types as l2cap_cl
    printf("init l2cap... ");
    l2cap_stack =
    	bt_l2cap_init(bt_stack, 8, 8, BT_HCI_PACKET_TYPE_DM3);
    printf("done.\n");

	// Init remote prog
    printf("init remote programmming... ");
    bt_remoteprog_init( l2cap_stack, NULL);
    printf("done.\n");

	// Initialize connection-less multi-hop layer
    // Set ACL packet types
    printf("setting acl pkt types... ");
    bt_acl_init(bt_stack, BT_HCI_PACKET_TYPE_DM3);
    printf("done.\n");

	// Init protocol/service multiplexor
    printf("init protcol/service mux... ");
    bt_psm_t* psmux = bt_psm_init(bt_stack, MAX_NR_SERVICES, 4);
    printf("done.\n");

    // Init connectionless l2cap stack
    printf("init connectionless l2cap... ");
    l2cap_cl_init(bt_stack, psmux);
    printf("done.\n");
    
	// init terminal & give hint
	btn_terminal_init(stdout, "[snif@btnode]$");
    // printf("hit tab twice for a list of commands\n\r");

    // Init connection manager -- requires Teminal agk!
    printf("init connection manager... ");
    con_mgr_init(bt_stack, psmux, CM_PSM, bt_hci_register_con_table_cb, CM_COD);
	// -- was used for testing: con_mgr_inq_disable();
    printf("done.\n");
    
    // Init connectionless multihop protocol
    printf("init connectionless multi-hop protocol... ");
    mhop_cl_init(bt_stack,
    				psmux,
    				MHOP_PSM,
    				6,
    				con_mgr_register_con_table_cb);
    printf("done.\n");
    
    // start time sync service
    bt_time_sync_mhop_init( bt_stack, psmux, MHOP_PSM);
    
    
	// register the "snif" service at the CL service / protocol multiplexor
	snif_cl_service_register(psmux);
    
	// register the "snif" service at the CO service / protocol multiplexor
	snif_co_service_register(l2cap_stack, 1, BT_L2CAP_MIN_MTU, BT_L2CAP_MTU_DEFAULT);

	// init terminal & give hint
	// btn_terminal_init(stdout, "[mblink@btnode]$");
    printf("hit tab twice for a list of commands\n\r");

	// register con mgr terminal commands
	con_mgr_register_cmds();

	// init and register bt terminal commands
	bt_cmds_init(bt_stack);
	bt_cmds_register_cmds();
    bt_extra_cmds_register_cmds();	   
    // bt_semaphore_register_cmds();
   
    // init and register l2cap cmds at terminal
    l2cap_cmds_register_cmds();
	l2cap_cmds_init(l2cap_stack, 1, BT_L2CAP_MIN_MTU, BT_L2CAP_MTU_DEFAULT);
			
	// prepare for packet forwarding
	packet_buffer_init();				
	snif_have_sink = 0;
	snif_am_sink = 0;
    
	l2cap_pkt = NutHeapAlloc( BT_L2CAP_ACL_SIZE_DH3 );
	
    // worker thread
	NutThreadCreate("worker", WORKER, 0, 1024);

    // sniffer thread
	NutThreadCreate("sniffer", SNIFFER, 0, 1024);

    // terminal mode
    btn_terminal_run(BTN_TERMINAL_NOFORK, 0);

    return 0;
}
