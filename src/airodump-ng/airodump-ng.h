/*
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * is provided AS IS, WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, and
 * NON-INFRINGEMENT.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */

#ifndef _AIRODUMP_NG_H_
#define _AIRODUMP_NG_H_

#include <sys/ioctl.h>
#if !defined(TIOCGWINSZ) && !defined(linux)
#include <sys/termios.h>
#endif

#include "aircrack-ng/defs.h"
#include "aircrack-ng/third-party/eapol.h"
#include "aircrack-ng/support/pcap_local.h"
#include "aircrack-ng/osdep/osdep.h"
#include "aircrack-ng/osdep/queue.h"

enum
{
    invalid_channel = -1,
    invalid_frequency = -1,
    channel_sentinel = 0,
    frequency_sentinel = 0
}; 

typedef enum channel_switching_method_t
{
    channel_switching_method_fifo,
    channel_switching_method_round_robin,
    channel_switching_method_hop_on_last,
    channel_switching_method_COUNT
} channel_switching_method_t;

/* some constants */

#define REFRESH_RATE 100000 /* default delay in us between updates */
#define DEFAULT_HOPFREQ 250 /* default delay in ms between channel hopping */

#define NB_PRB 10 /* size of probed ESSID ring buffer */

#define MAX_CARDS 8 /* maximum number of cards to capture from */

#define STD_OPN 0x0001u
#define STD_WEP 0x0002u
#define STD_WPA 0x0004u
#define STD_WPA2 0x0008u

#define STD_FIELD (STD_OPN | STD_WEP | STD_WPA | STD_WPA2)

#define ENC_WEP 0x0010u
#define ENC_TKIP 0x0020u
#define ENC_WRAP 0x0040u
#define ENC_CCMP 0x0080u
#define ENC_WEP40 0x1000u
#define ENC_WEP104 0x0100u
#define ENC_GCMP 0x4000u
#define ENC_GMAC 0x8000u

#define ENC_FIELD                                                              \
	(ENC_WEP | ENC_TKIP | ENC_WRAP | ENC_CCMP | ENC_WEP40 | ENC_WEP104         \
	 | ENC_GCMP                                                                \
	 | ENC_GMAC)

#define AUTH_OPN 0x0200u
#define AUTH_PSK 0x0400u
#define AUTH_MGT 0x0800u
#define AUTH_CMAC 0x10000u
#define AUTH_SAE 0x20000u
#define AUTH_OWE 0x40000u

#define AUTH_FIELD                                                             \
	(AUTH_OPN | AUTH_PSK | AUTH_CMAC | AUTH_MGT | AUTH_SAE | AUTH_OWE)

#define STD_QOS 0x2000u

#define QLT_TIME 5
#define QLT_COUNT 25

// milliseconds to store last packets
#define BUFFER_TIME_MILLISECS 3000

extern int get_ram_size(void);

extern const unsigned long int crc_tbl[256];
extern const unsigned char crc_chop_tbl[256][4];

#include "aircrack-ng/support/station.h"

/* linked list of detected macs through ack, cts or rts frames */

struct NA_info
{
	TAILQ_ENTRY(NA_info) entry;

	time_t tinit; 
    time_t tlast; /* first and last time seen  */
    mac_address namac; /* the stations MAC address  */
	int power; /* last signal power         */
	int channel; /* captured on channel       */
	int ack; /* number of ACK frames      */
	int ack_old; /* old number of ACK frames  */
	int ackps; /* number of ACK frames/s    */
	int cts; /* number of CTS frames      */
	int rts_r; /* number of RTS frames (rx) */
	int rts_t; /* number of RTS frames (tx) */
	int other; /* number of other frames    */
	struct timeval tv; /* time for ack per second   */
};

size_t get_channel_count(
    int const * const channels,
    bool const count_valid_channels_only);

size_t get_frequency_count(
    int const * const frequencies,
    bool const count_valid_frequencies_only);

int send_probe_request(struct wif * const wi);

#endif
