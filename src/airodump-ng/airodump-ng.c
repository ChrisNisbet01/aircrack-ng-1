/*
 *  pcap-compatible 802.11 packet sniffer
 *
 *  Copyright (C) 2006-2020 Thomas d'Otreppe <tdotreppe@aircrack-ng.org>
 *  Copyright (C) 2004, 2005 Christophe Devine
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 *
 *  In addition, as a special exception, the copyright holders give
 *  permission to link the code of portions of this program with the
 *  OpenSSL library under certain conditions as described in each
 *  individual source file, and distribute linked combinations
 *  including the two.
 *  You must obey the GNU General Public License in all respects
 *  for all of the code used other than OpenSSL. *  If you modify
 *  file(s) with this exception, you may extend this exception to your
 *  version of the file(s), but you are not obligated to do so. *  If you
 *  do not wish to do so, delete this exception statement from your
 *  version. *  If you delete this exception statement from all source
 *  files in the program, then also delete it here.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>

#ifndef TIOCGWINSZ
#include <sys/termios.h>
#endif

#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#define _WITH_DPRINTF
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>
#include <limits.h>

#include <sys/wait.h>

#ifdef HAVE_PCRE
#include <pcre.h>
#endif

#include "aircrack-ng/defs.h"
#include "aircrack-ng/version.h"
#include "aircrack-ng/support/pcap_local.h"
#include "aircrack-ng/ce-wep/uniqueiv.h"
#include "aircrack-ng/support/communications.h"
#include "aircrack-ng/crypto/crypto.h"
#include "aircrack-ng/osdep/channel.h"
#include "aircrack-ng/osdep/osdep.h"
#include "airodump-ng.h"
#include "dump_write.h"
#include "aircrack-ng/osdep/common.h"
#include "aircrack-ng/third-party/ieee80211.h"
#include "aircrack-ng/support/common.h"
#include "aircrack-ng/support/mcs_index_rates.h"
#include "aircrack-ng/utf8/verifyssid.h"
#include "aircrack-ng/tui/console.h"
#include "radiotap/radiotap.h"
#include "radiotap/radiotap_iter.h"
#include "strlcpy.h"
#include "ap_list.h"
#include "aircrack-ng/osdep/sta_list.h"
#include "pcap_reader.h"
#include "oui.h"
#include "get_string_time_from_seconds.h"
#include "gps_tracker.h"
#include "ap_compare.h"
#include "channel_hopper.h"
#include "packet_writer.h"
#include "essid_filter.h"
#include "utils.h"
#include "terminal.h"
#include "ivs_log.h"

#define DEFAULT_CHANNEL_WIDTH_MHZ 20

/* Possibly only required so that this will link. Referenced
 * in communications.c.
 */
struct devices dev;

/* Note that these channel arrays can't be made const because 
 * the channel hopper process may modify the list. 
 * :( 
 */
static int abg_chans[]
	= {1,   7,   13,  2,   8,   3,   14,  9,   4,   10,  5,   11,  6,
	   12,  36,  38,  40,  42,  44,  46,  48,  50,  52,  54,  56,  58,
	   60,  62,  64,  100, 102, 104, 106, 108, 110, 112, 114, 116, 118,
	   120, 122, 124, 126, 128, 132, 134, 136, 138, 140, 142, 144, 149,
	   151, 153, 155, 157, 159, 161, 165, 169, 173, channel_list_sentinel};

static int bg_chans[] = {1, 7, 13, 2, 8, 3, 14, 9, 4, 10, 5, 11, 6, 12, channel_list_sentinel};

static int a_chans[]
	= {36,  38,  40,  42,  44,  46,  48,  50,  52,  54,  56,  58,
	   60,  62,  64,  100, 102, 104, 106, 108, 110, 112, 114, 116,
	   118, 120, 122, 124, 126, 128, 132, 134, 136, 138, 140, 142,
	   144, 149, 151, 153, 155, 157, 159, 161, 165, 169, 173, channel_list_sentinel};

struct detected_frequencies_st
{
    size_t count;
    size_t table_size;
    int * frequencies;
};

struct sort_context_st
{
    int do_sort_always;
    bool sort_required;
    ap_sort_context_st * sort_context;
};

TAILQ_HEAD(na_list_head, NA_info);

static struct local_options
{
    int quitting;
    time_t quitting_event_ts;

    struct ap_list_head ap_list;
    struct AP_info * p_selected_ap; 

	struct sta_list_head sta_list;

	struct na_list_head na_list;

	oui_context_st * manufacturer_list;

    mac_address f_bssid;
    mac_address f_netmask; 

    struct shared_key_context_st shared_key; 

    struct essid_filter_context_st essid_filter;

	char * dump_prefix;

    /* TODO: Stick all this card specific state into a structure. */
    int channel[MAX_CARDS]; /* current channel #    */
	int frequency[MAX_CARDS]; /* current frequency #    */
    size_t max_consecutive_failed_interface_reads;
    size_t wi_consecutive_failed_reads[MAX_CARDS];

    size_t num_cards;

    pthread_t input_tid;
    int input_thread_pipe[2];

    int channel_hopper_pipe[2];

    int signal_event_pipe[2];

	gps_tracker_context_st gps_context;

	int * channels;

	int singlechan; /* channel hopping set 1*/
	int singlefreq; /* frequency hopping: 1 */
    channel_switching_method_t channel_switching_method;
	unsigned int encryption_filter;
	int update_interval_seconds;

	volatile int do_exit; /* interrupt flag       */
	struct winsize window_size; /* console window size  */

	char * elapsed_time; /* capture time			*/

	int one_beacon; /* Record only 1 beacon?*/

	int * own_channels; /* custom channel list  */
	int * own_frequencies; /* custom frequency list  */

	int asso_client; /* only show associated clients */

	char message[512];
	char decloak;

	char is_berlin; /* is the switch --berlin set? */
	int maxnumaps; /* maximum nubers of APs on the list */
	int maxaps; /* number of all APs found */
	int berlin; /* number of seconds it takes in berlin to fill the whole screen
				   with APs*/
    /*
	 * The name for this option may look quite strange, here is the story behind
	 * it:
	 * During the CCC2007, 10 august 2007, we (hirte, Mister_X) went to visit
	 * Berlin
	 * and couldn't resist to turn on airodump-ng to see how much access point
	 * we can
	 * get during the trip from Finowfurt to Berlin. When we were in Berlin, the
	 * number
	 * of AP increase really fast, so fast that it couldn't fit in a screen,
	 * even rotated;
	 * the list was really huge (we have a picture of that). The 2 minutes
	 * timeout
	 * (if the last packet seen is higher than 2 minutes, the AP isn't shown
	 * anymore)
	 * wasn't enough, so we decided to create a new option to change that
	 * timeout.
	 * We implemented this option in the highest tower (TV Tower) of Berlin,
	 * eating an ice.
	 */

    int ignore_negative_one;

	int show_ap;
	int show_sta;
	int show_ack;
	int hide_known;

	int frequency_hop_millisecs;

    char * s_file; /* source interface to read from */
    char * s_iface; /* source interface to read from */
	pcap_reader_context_st * pcap_reader_context;

    int detect_anomaly; /* Detect WIPS protecting WEP in action */

	char * freqstring;
	int freqoption;
	int chanoption;
	int active_scan_sim; /* simulates an active scan, sending probe requests */

	/* Airodump-ng start time: for kismet netxml file */
	char * airodump_start_time;

    struct sort_context_st sort;

	enum
	{
		selection_direction_down,
		selection_direction_up,
		selection_direction_no
	} en_selection_direction;

	int mark_cur_ap;

    struct
    {
        bool paused;

        /* Set after the user pauses output. Used to get the 'pause' message to 
         * be printed. 
         */
        bool required; 
    } console_output;

	mac_address selected_bssid; /* bssid that is selected */

	u_int maxsize_essid_seen;
	int show_manufacturer;
	int show_uptime;
	int file_write_interval;
	u_int maxsize_wps_seen;
	int show_wps;

    char sys_name[256];  /* system name value for wifi scanner custom format */
    char loc_name[256];  /* location name value for wifi scanner custom format */

#ifdef CONFIG_LIBNL
	unsigned int htval;
#endif
	int interactive_mode;

	unsigned long min_pkts;

	bool relative_time; /* read PCAP file in psuedo-real-time */

    time_t filter_seconds;
    int file_reset_seconds;

    size_t max_node_age;

    bool record_data;

    struct
    {
        bool needed;
        struct dump_context_st * context;
    } dump[dump_type_COUNT];

    struct
    {
        bool required;
        FILE * fp;
    } log_csv;

    struct
    {
        bool required;
        FILE * fp;
        char * filename;
    } gpsd;

    struct
    {
        bool required;
        FILE * fp;
        mac_address prev_bssid;
    } ivs;

    struct
    {
        bool required;
        struct packet_writer_context_st * writer;
    } pcap_output;

    int no_filename_index;
    int no_shared_key; 

    /* Possibly appended to filenames of output dump files. value 
     * -1 indicates not to. 
     */
    int f_index; 
} lopt;

static void reset_sort_context(struct sort_context_st * const context)
{
    ap_sort_context_free(context->sort_context);
    context->sort_context = ap_sort_context_alloc(SORT_BY_NOTHING);
    context->do_sort_always = 0;
}

static void reset_selections(struct local_options * const options)
{
	options->relative_time = false;
    options->p_selected_ap = NULL;
    options->en_selection_direction = selection_direction_no;
    options->mark_cur_ap = 0;
    options->console_output.paused = false;

    reset_sort_context(&options->sort);

    MAC_ADDRESS_CLEAR(&options->selected_bssid);
}

static void clear_ap_marking(struct ap_list_head * const ap_list)
{
    struct AP_info * ap_cur;

    TAILQ_FOREACH(ap_cur, ap_list, entry)
    {
        ap_cur->marked = 0;
        ap_cur->marked_color = 1;
    }
}

static bool ap_has_required_security(
    unsigned int ap_security, 
    unsigned int required_security)
{
    bool const has_required =
        ap_security == 0
        || required_security == 0
        || (ap_security & required_security) != 0;

    return has_required;
}

static void color_off(struct local_options * const options)
{
    clear_ap_marking(&options->ap_list);

	textcolor_normal();
	textcolor_fg(TEXT_WHITE);
}

static void color_on(struct local_options * const options)
{
	struct AP_info * ap_cur;
	int color = 1;

	color_off(options);

    TAILQ_FOREACH_REVERSE(ap_cur, &options->ap_list, ap_list_head, entry)
	{
		struct ST_info * st_cur;

        if (ap_cur->nb_pkt < options->min_pkts
            || time(NULL) - ap_cur->tlast > options->berlin)
		{
			continue;
		}

        if (!ap_has_required_security(ap_cur->security, options->encryption_filter))
        {
            continue;
        }

		// Don't filter unassociated clients by ESSID
		if (!MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid)
			&& is_filtered_essid(&options->essid_filter, ap_cur->essid))
		{
			continue;
		}

		TAILQ_FOREACH_REVERSE(st_cur, &options->sta_list, sta_list_head, entry)
		{
			if (st_cur->base != ap_cur
                || (time(NULL) - st_cur->tlast) > options->berlin)
			{
				continue;
			}

			if (MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid)
                && options->asso_client)
			{
				continue;
			}

			if (color > TEXT_MAX_COLOR)
			{
				color++; /* FIXME: Is this correct? Set to 1 instead? */
			}

			if (!ap_cur->marked)
			{
				ap_cur->marked = 1;
				if (MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid))
				{
					ap_cur->marked_color = 1;
				}
				else
				{
					ap_cur->marked_color = color;
                    color++;
				}
			}
			else
			{
				ap_cur->marked_color = 1;
			}
		}
	}
}

static void sort_aps(
    struct ap_list_head * const ap_list,
    struct ap_sort_context_st const * const sort_context)
{
	time_t const tt = time(NULL);
	struct ap_list_head sorted_list = TAILQ_HEAD_INITIALIZER(sorted_list);

	/* Sort the aps by WHATEVER first, */
	/* Can't 'sort' (or something better) be used to sort these
     * entries?
     */
	while (TAILQ_FIRST(ap_list) != NULL)
	{
		struct AP_info * ap_cur;
		struct AP_info * ap_max = NULL;

        /* Only the most recently seen entries are sorted. */
		TAILQ_FOREACH(ap_cur, ap_list, entry)
		{
            time_t const seconds_since_last_seen = tt - ap_cur->tlast;
            static long const sorting_age_limit_seconds = 20;
            bool const too_old_to_sort =
                seconds_since_last_seen > sorting_age_limit_seconds;

            if (too_old_to_sort)
			{
                ap_max = ap_cur;
                break;
			}
		}

        if (ap_max == NULL)
		{
			ap_max = TAILQ_FIRST(ap_list);

            TAILQ_FOREACH(ap_cur, ap_list, entry)
			{
                if (ap_max == ap_cur)
                {
                    /* There's no point in comparing an entry with itself. */
                    continue;
                }

                if (ap_sort_compare(sort_context, ap_cur, ap_max) > 0)
				{
					ap_max = ap_cur;
				}
			}
        }
        /* Put sorted entries at the tail of the list. dump_print()
         * works from the tail to the head of the list.
         */
        TAILQ_REMOVE(ap_list, ap_max, entry);
        TAILQ_INSERT_TAIL(&sorted_list, ap_max, entry); 
	}

	/* The original list is now empty.
	 * Concatenate the sorted list to it so that it contains the
	 * sorted entries.
	 */
	TAILQ_CONCAT(ap_list, &sorted_list, entry);
}

static void sort_stas(struct sta_list_head * const sta_list)
{
	time_t const tt = time(NULL);
	struct sta_list_head sorted_list = TAILQ_HEAD_INITIALIZER(sorted_list);

	while (TAILQ_FIRST(sta_list) != NULL)
	{
		struct ST_info * st_cur;
		struct ST_info * st_max = NULL;

		/* Don't sort entries older than 60 seconds. */
		TAILQ_FOREACH(st_cur, sta_list, entry)
		{
            time_t const seconds_since_last_seen = tt - st_cur->tlast;
            static long const sorting_age_limit_seconds = 60;
            bool const too_old_to_sort =
                seconds_since_last_seen > sorting_age_limit_seconds;

            if (too_old_to_sort)
			{
				st_max = st_cur;
                break;
			}
		}

        if (st_max == NULL)
		{
            st_max = TAILQ_FIRST(sta_list);

			/* STAs are always sorted by power. */
            TAILQ_FOREACH(st_cur, sta_list, entry)
			{
                if (st_max == st_cur)
				{
					/* There's no point in comparing an entry with itself. */
					continue;
				}

                if (st_cur->power > st_max->power)
				{
					st_max = st_cur;
				}
			}
		}

        TAILQ_REMOVE(sta_list, st_max, entry);
        TAILQ_INSERT_TAIL(&sorted_list, st_max, entry);
    }

	/* The original list is now empty.
	 * Concatenate the sorted list to it so that it contains the
	 * sorted entries.
	 */
	TAILQ_CONCAT(sta_list, &sorted_list, entry);
}

static void dump_sort(
    struct local_options * const options,
    struct ap_sort_context_st const * const sort_context)
{
    sort_aps(&options->ap_list, sort_context);
    sort_stas(&options->sta_list);
}

static void input_thread_handle_input_key(
    int const key_code, 
    int const update_fd)
{
    switch (key_code)
    {
        case KEY_q:
        case KEY_o:
        case KEY_p:
        case KEY_s:
        case KEY_SPACE:
        case KEY_r:
        case KEY_m:
        case KEY_ARROW_DOWN:
        case KEY_ARROW_UP:
        case KEY_i:
        case KEY_TAB:
        case KEY_a:
        case KEY_d:
            IGNORE_LTZ(write(update_fd, &key_code, sizeof key_code));
            break;
    }
}

static void handle_input_key(
    struct local_options * const options,
    int const keycode)
{
    if (keycode == KEY_q)
    {
        options->quitting_event_ts = time(NULL);
        options->quitting++;
        if (options->quitting > 1)
        {
            options->do_exit = 1;
        }
        else
        {
            snprintf(
                options->message,
                sizeof(options->message),
                "Are you sure you want to quit? Press Q again to quit.");
        }
    }

    if (keycode == KEY_o)
    {
        color_on(options);
        snprintf(options->message, sizeof(options->message), "color on");
    }

    if (keycode == KEY_p)
    {
        color_off(options);
        snprintf(options->message, sizeof(options->message), "color off");
    }

    if (keycode == KEY_s)
    {
        ap_sort_context_next_sort_method(options->sort.sort_context);
        snprintf(options->message,
                 sizeof(options->message),
                 "sorting by %s", 
                 ap_sort_context_description(options->sort.sort_context));
        options->sort.sort_required = true;
    }

    if (keycode == KEY_SPACE)
    {
        options->console_output.paused = !options->console_output.paused;
        if (options->console_output.paused)
        {
            /* So that the 'paused' indication will get displayed. */
            options->console_output.required = true;
        }

        char const * const message = options->console_output.paused ? "paused" : "resumed";

        snprintf(options->message, 
                 sizeof(options->message), 
                 "%s output", 
                 message);
    }

    if (keycode == KEY_r)
    {
        options->sort.do_sort_always = !options->sort.do_sort_always;
        char const * const message = 
            options->sort.do_sort_always ? "activated" : "deactivated";

        snprintf(options->message,
                 sizeof(options->message),
                 "realtime sorting %s", message);
    }

	if (keycode == KEY_m)
	{
		if (options->p_selected_ap != NULL)
		{
			options->mark_cur_ap = 1;
		}
	}

    if (keycode == KEY_ARROW_DOWN)
    {
        if (options->p_selected_ap != NULL
            && TAILQ_PREV(options->p_selected_ap, ap_list_head, entry) != NULL)
        {
            options->p_selected_ap =
                TAILQ_PREV(options->p_selected_ap, ap_list_head, entry);
            options->en_selection_direction = selection_direction_down;
        }
    }

    if (keycode == KEY_ARROW_UP)
    {
        if (options->p_selected_ap != NULL
            && TAILQ_NEXT(options->p_selected_ap, entry) != NULL)
        {
            options->p_selected_ap =
                TAILQ_NEXT(options->p_selected_ap, entry);
            options->en_selection_direction = selection_direction_up;
        }
    }

    if (keycode == KEY_i)
    {
        bool const is_inverted = 
            ap_sort_context_invert_direction(options->sort.sort_context);
        char const * const message = is_inverted ? "inverted" : "normal";

        snprintf(options->message,
                 sizeof(options->message),
                 "%s sorting order", message);

        options->sort.sort_required = true;
    }

    if (keycode == KEY_TAB)
    {
        char const * message;
        if (options->p_selected_ap == NULL)
        {
            options->en_selection_direction = selection_direction_down;
            options->p_selected_ap = TAILQ_LAST(&options->ap_list, ap_list_head);
            message = "enabled";
        }
        else
        {
            options->en_selection_direction = selection_direction_no;
            options->p_selected_ap = NULL;
            message = "disabled";
        }
        snprintf(options->message,
                 sizeof(options->message),
                 "%s AP selection", message); 

        ap_sort_context_assign_sort_method(options->sort.sort_context, 
                                           SORT_BY_NOTHING);
    }

    if (keycode == KEY_a)
    {
        if (options->show_ap == 1 
            && options->show_sta == 1 
            && options->show_ack == 0)
        {
            options->show_ap = 1;
            options->show_sta = 1;
            options->show_ack = 1;
            snprintf(options->message,
                     sizeof(options->message),
                     "display ap+sta+ack");
        }
        else if (options->show_ap == 1 
                 && options->show_sta == 1
                 && options->show_ack == 1)
        {
            options->show_ap = 1;
            options->show_sta = 0;
            options->show_ack = 0;
            snprintf(
                options->message, sizeof(options->message), "display ap only");
        }
        else if (options->show_ap == 1 
                 && options->show_sta == 0
                 && options->show_ack == 0)
        {
            options->show_ap = 0;
            options->show_sta = 1;
            options->show_ack = 0;
            snprintf(
                options->message, sizeof(options->message), "display sta only");
        }
        else if (options->show_ap == 0 
                 && options->show_sta == 1
                 && options->show_ack == 0)
        {
            options->show_ap = 1;
            options->show_sta = 1;
            options->show_ack = 0;
            snprintf(
                options->message, sizeof(options->message), "display ap+sta");
        }
    }

    if (keycode == KEY_d)
    {
        reset_selections(options);
        snprintf(options->message,
                 sizeof(options->message),
                 "reset selection to default");
    }
}

struct input_thread_args_st
{
    volatile int * do_exit;
    int update_fd;
};

static THREAD_ENTRY(input_thread)
{
    struct input_thread_args_st * const input_args = arg;
    volatile int * do_exit = input_args->do_exit;
    int const update_fd = input_args->update_fd;

    while (!(*do_exit))
	{
        int const keycode = mygetch();

        input_thread_handle_input_key(keycode, update_fd);
	}

	return (NULL);
}

static const char usage[] =

	"\n"
	"  %s - (C) 2006-2020 Thomas d\'Otreppe\n"
	"  https://www.aircrack-ng.org\n"
	"\n"
	"  usage: airodump-ng <options> <interface>[,<interface>,...]\n"
	"\n"
	"  Options:\n"
	"      --ivs                 : Save only captured IVs\n"
	"      --gpsd                : Use GPSd\n"
	"      --write      <prefix> : Dump file prefix\n"
	"      -w                    : same as --write \n"
    "      --nodecloak           : Disable decloaking\n"
    "      -D                    : Same as --nodecloak\n"
	"      --beacons             : Record all beacons in dump file\n"
	"      --update       <secs> : Display update delay in seconds\n"
	"      --showack             : Prints ack/cts/rts statistics\n"
	"      -h                    : Hides known stations for --showack\n"
	"      -f            <msecs> : Time in ms between hopping channels\n"
	"      --berlin       <secs> : Time before removing the AP/client\n"
	"                              from the screen when no more packets\n"
	"                              are received (Default: 120 seconds)\n"
	"      -r             <file> : Read packets from that file\n"
	"      -T                    : While reading packets from a file,\n"
	"                              simulate the arrival rate of them\n"
	"                              as if they were \"live\".\n"
	"      -x            <msecs> : Active Scanning Simulation\n"
	"      --manufacturer        : Display manufacturer from IEEE OUI list\n"
	"      --uptime              : Display AP Uptime from Beacon Timestamp\n"
	"      --wps                 : Display WPS information (if any)\n"
	"      --output-format\n"
	"                  <formats> : Output format. Possible values:\n"
    "                              pcap, ivs, csv, gps, kismet, netxml, wifi_scanner"
    ", logcsv\n"
    "      --sys-name            : Unique System Name\n"
    "      --loc-name            : Unique Location Name\n"
    "      --filter-seconds      : Filter time (seconds)\n"
    "      --file-reset-minutes  : File reset time (minutes)\n"
    "      -v          <minutes> : Maximum age of cached entries\n"
    "      --ignore-negative-one : Removes the message that says\n"
	"                              fixed channel <interface>: -1\n"
    "      --no-filename-index   : Don't include an index in the filname\n"
    "      --no-shared-key       : Don't do shared key checks\n"
	"      --write-interval\n"
	"                  <seconds> : Output file(s) write interval in seconds\n"
	"      --background <enable> : Override background detection.\n"
	"      -n              <int> : Minimum AP packets recv'd before\n"
	"                              for displaying it\n"
	"\n"
	"  Filter options:\n"
	"      --encrypt   <suite>   : Filter APs by cipher suite\n"
	"      --netmask <netmask>   : Filter APs by mask\n"
	"      --bssid     <bssid>   : Filter APs by BSSID\n"
	"      --essid     <essid>   : Filter APs by ESSID\n"
#ifdef HAVE_PCRE
	"      --essid-regex <regex> : Filter APs by ESSID using a regular\n"
	"                              expression\n"
#endif
	"      -a                    : Filter unassociated clients\n"
	"\n"
	"  By default, airodump-ng hops on 2.4GHz channels.\n"
	"  You can make it capture on other/specific channel(s) by using:\n"
	"      --ht20                : Set channel to HT20 (802.11n)\n"
	"      --ht40-               : Set channel to HT40- (802.11n)\n"
	"      --ht40+               : Set channel to HT40+ (802.11n)\n"
	"      --channel <channels>  : Capture on specific channels\n"
	"      --band <abg>          : Band on which airodump-ng should hop\n"
	"      -C    <frequencies>   : Uses these frequencies in MHz to hop\n"
	"      --cswitch  <method>   : Set channel switching method\n"
	"                    0       : FIFO (default)\n"
	"                    1       : Round Robin\n"
	"                    2       : Hop on last\n"
	"      -s                    : same as --cswitch\n"
	"\n"
	"      --help                : Displays this usage screen\n"
	"\n";

static void airodump_usage(void)
{
	char * const l_usage = getVersion(
		"Airodump-ng", _MAJ, _MIN, _SUB_MIN, _REVISION, _BETA, _RC);
	printf(usage, l_usage);
	free(l_usage);
}

static void update_ap_rx_quality(
    struct ap_list_head * const ap_list,
    struct timeval const * const current_time)
{
    /* access points */
    struct AP_info * ap_cur;

    TAILQ_FOREACH(ap_cur, ap_list, entry)
    {
        unsigned long const time_diff = 1000000UL * (current_time->tv_sec - ap_cur->ftimer.tv_sec)
            + (current_time->tv_usec - ap_cur->ftimer.tv_usec);

        /* update every `QLT_TIME`seconds if the rate is low, or every 500ms
         * otherwise */
        if ((ap_cur->fcapt >= QLT_COUNT && time_diff > 500000)
            || time_diff > (QLT_TIME * 1000000))
        {
            /* at least one frame captured */
            if (ap_cur->fcapt > 1)
            {
                unsigned long const capt_time
                    = (1000000UL * (ap_cur->ftimel.tv_sec
                                    - ap_cur->ftimef.tv_sec) // time between
                       // first and last
                       // captured frame
                       + (ap_cur->ftimel.tv_usec - ap_cur->ftimef.tv_usec));

                unsigned long const miss_time
                    = (1000000UL * (ap_cur->ftimef.tv_sec
                                    - ap_cur->ftimer.tv_sec) // time between
                       // timer reset and
                       // first frame
                       + (ap_cur->ftimef.tv_usec - ap_cur->ftimer.tv_usec))
                    + (1000000UL * (current_time->tv_sec
                                    - ap_cur->ftimel.tv_sec) // time between
                       // last frame and
                       // this moment
                       + (current_time->tv_usec - ap_cur->ftimel.tv_usec));

                // number of frames missed at the time where no frames were
                // captured; extrapolated by assuming a constant framerate
                if (capt_time > 0 && miss_time > 200000)
                {
                    int const missed_frames =
                        (int)(((float)miss_time / (float)capt_time)
                              * ((float)ap_cur->fcapt
                                 + (float)ap_cur->fmiss));
                    ap_cur->fmiss += missed_frames;
                }

                ap_cur->rx_quality
                    = (int)(((float)ap_cur->fcapt
                             / ((float)ap_cur->fcapt + (float)ap_cur->fmiss))
                            *
#if defined(__x86_64__) && defined(__CYGWIN__)
                            (0.0f + 100.0f));
#else
                            100.0f);
#endif
            }
            else
            {
                ap_cur->rx_quality = 0; /* no packets -> zero quality */
            }

            /* normalize, in case the seq numbers are not iterating */
            if (ap_cur->rx_quality > 100)
            {
                ap_cur->rx_quality = 100;
            }
            if (ap_cur->rx_quality < 0)
            {
                ap_cur->rx_quality = 0;
            }

            /* reset variables */
            ap_cur->fcapt = 0;
            ap_cur->fmiss = 0;
            gettimeofday(&ap_cur->ftimer, NULL);
        }
    }
}

static void update_sta_rx_quality(
    struct sta_list_head * const sta_list, 
    struct timeval const * const current_time)
{
    struct ST_info * st_cur;

    TAILQ_FOREACH(st_cur, sta_list, entry)
    {
        unsigned long const time_diff = 1000000UL * (current_time->tv_sec - st_cur->ftimer.tv_sec)
            + (current_time->tv_usec - st_cur->ftimer.tv_usec);

        if (time_diff > 10000000)
        {
            st_cur->missed = 0;
            gettimeofday(&st_cur->ftimer, NULL);
        }
    }
}

static void update_rx_quality(struct local_options * const options)
{
    struct timeval current_time;

    gettimeofday(&current_time, NULL);

    update_ap_rx_quality(&options->ap_list, &current_time);
    update_sta_rx_quality(&options->sta_list, &current_time);
}

static void update_ap_packets_per_second(
    struct ap_list_head * const ap_list,
    struct timeval const * const tv)
{
    struct AP_info * ap_cur;

    TAILQ_FOREACH(ap_cur, ap_list, entry)
    {
        time_t const sec = tv->tv_sec - ap_cur->tv.tv_sec; 
        suseconds_t const usec = tv->tv_usec - ap_cur->tv.tv_usec; 

#if defined(__x86_64__) && defined(__CYGWIN__)
        float const pause = (sec * (0.0f + 1000000.0f) + usec) / (0.0f + 1000000.0f);
#else
        float const pause = (sec * 1000000.0f + usec) / 1000000.0f;
#endif
        if (pause > 2.0f)
        {
            unsigned long const diff = ap_cur->nb_data - ap_cur->nb_data_old;
            int const ps = (int)(((float)diff) / pause);

            ap_cur->nb_dataps = ps;
            ap_cur->nb_data_old = ap_cur->nb_data;
            gettimeofday(&ap_cur->tv, NULL);
        }
    }
}

static void update_na_packets_per_second(
    struct na_list_head * const na_list, 
    struct timeval * const tv)
{
    struct NA_info * na_cur;

    TAILQ_FOREACH(na_cur, na_list, entry)
    {
        time_t const sec = tv->tv_sec - na_cur->tv.tv_sec;
        suseconds_t const usec = tv->tv_usec - na_cur->tv.tv_usec;

#if defined(__x86_64__) && defined(__CYGWIN__)
        float const pause = (sec * (0.0f + 1000000.0f) + usec) / (0.0f + 1000000.0f);
#else
        float const pause = (sec * 1000000.0f + usec) / 1000000.0f;
#endif
        if (pause > 2.0f)
        {
            unsigned long const diff = (unsigned long)(na_cur->ack - na_cur->ack_old);
            int const ps = (int)(((float)diff) / pause);

            na_cur->ackps = ps;
            na_cur->ack_old = na_cur->ack;
            gettimeofday(&(na_cur->tv), NULL);
        }
    }
}

static void update_data_packets_per_second(struct local_options * const options)
{
    struct timeval tv;

	gettimeofday(&tv, NULL);

    update_ap_packets_per_second(&options->ap_list, &tv);
    update_na_packets_per_second(&options->na_list, &tv); 
}

static void packet_buf_free(struct pkt_buf * const pkt_buf)
{
	free(pkt_buf->packet);
	free(pkt_buf);
}

static void packet_buf_remove(
    struct pkt_list_head * const pkt_list,
    struct pkt_buf * const pkt_buf)
{
    TAILQ_REMOVE(pkt_list, pkt_buf, entry);

    packet_buf_free(pkt_buf);
}

static int packet_list_free(struct pkt_list_head * const pkt_list)
{
	while (TAILQ_FIRST(pkt_list) != NULL)
	{
		struct pkt_buf * const pkt_buf = TAILQ_FIRST(pkt_list);

        packet_buf_remove(pkt_list, pkt_buf);
	}

	return 0;
}

static void ap_purge_old_packets(
	struct AP_info * const ap_cur,
	struct timeval const * const current_time,
	unsigned long const age_limit_millisecs)
{
	struct pkt_buf * pkt_buf;
	struct pkt_buf * temp;
	bool found_old_packet = false;

    /* New packets are always added to the head of the list, so 
     * once the first packet that is too old is found, there is no 
     * need to check the age of the others. 
     */
	TAILQ_FOREACH_SAFE(pkt_buf, &ap_cur->pkt_list, entry, temp)
	{
		if (!found_old_packet)
		{
			unsigned long const time_diff =
			(((current_time->tv_sec - (pkt_buf->ctime.tv_sec)) * 1000000UL)
				 + (current_time->tv_usec - (pkt_buf->ctime.tv_usec)))
				/ 1000;
            bool const too_old = time_diff > age_limit_millisecs;

			if (too_old)
			{
				found_old_packet = true;
			}
		}

		if (found_old_packet)
		{
            packet_buf_remove(&ap_cur->pkt_list, pkt_buf);
		}
	}

}

static void aps_purge_old_packets(
	struct ap_list_head * const ap_list,
	unsigned long const age_limit_millisecs)
{
	struct timeval current_time;

	gettimeofday(&current_time, NULL);

	struct AP_info * ap_cur;

	TAILQ_FOREACH(ap_cur, ap_list, entry)
	{
		ap_purge_old_packets(ap_cur, &current_time, age_limit_millisecs);
	}
}

static int
list_add_packet(
	struct pkt_list_head * const pkt_list,
    uint8_t const * const packet,
    size_t const length)
{
	struct pkt_buf * new_pkt_buf;

	if (length <= 0)
	{
		return 1;
	}

	if (packet == NULL)
	{
		return 1;
	}

    new_pkt_buf = calloc(1, sizeof *new_pkt_buf);
    if (new_pkt_buf == NULL)
    {
        return 1;
    }

    new_pkt_buf->packet = malloc(length);
    if (new_pkt_buf->packet == NULL)
    {
        free(new_pkt_buf);
        return 1;
    }

    memcpy(new_pkt_buf->packet, packet, length);
    new_pkt_buf->length = (uint16_t)length;

    gettimeofday(&new_pkt_buf->ctime, NULL);

	TAILQ_INSERT_HEAD(pkt_list, new_pkt_buf, entry);

	return 0;
}

/*
 * Check if the same IV was used if the first two bytes were the same.
 * If they are not identical, it would complain.
 * The reason is that the first two bytes unencrypted are 'aa'
 * so with the same IV it should always be encrypted to the same thing.
 */
static int
list_check_decloak(struct pkt_list_head * const pkt_list, int length, const uint8_t * packet)
{
	struct pkt_buf * next;
	int i, correct;

	if (packet == NULL)
	{
		return 1;
	}

	if (length <= 0)
	{
		return 1;
	}

	TAILQ_FOREACH(next, pkt_list, entry)
	{
		if ((next->length + 4) == length)
		{
			correct = 1;
			// check for 4 bytes added after the end
			for (i = 28; i < length - 28; i++) // check everything (in the old
			// packet) after the IV
			// (including crc32 at the end)
			{
				if (next->packet[i] != packet[i])
				{
					correct = 0;
					break;
				}
			}
			if (!correct)
			{
				correct = 1;
				// check for 4 bytes added at the beginning
				for (i = 28; i < length - 28; i++) // check everything (in the
				// old packet) after the IV
				// (including crc32 at the
				// end)
				{
					if (next->packet[i] != packet[4 + i])
					{
						correct = 0;
						break;
					}
				}
			}
			if (correct == 1)
			{
				return 0; // found decloaking!
			}
		}
	}

	return 1; // didn't find decloak
}

static void na_info_free(struct NA_info * const na_cur)
{
    free(na_cur);
}

static void na_info_remove(
    struct na_list_head * const na_list,
    struct NA_info * const na_cur)
{
    TAILQ_REMOVE(na_list, na_cur, entry);

    na_info_free(na_cur);
}

static void na_info_list_free(struct na_list_head * const na_list)
{
    while (TAILQ_FIRST(na_list) != NULL)
	{
        struct NA_info * const na_cur = TAILQ_FIRST(na_list);

        na_info_remove(na_list, na_cur);
	}
}

static struct NA_info * na_info_alloc(mac_address const * const mac)
{
    struct NA_info * na_cur = calloc(1, sizeof *na_cur);

    if (na_cur == NULL)
    {
        perror("calloc failed");
        goto done;
    }

    MAC_ADDRESS_COPY(&na_cur->namac, mac);

    gettimeofday(&na_cur->tv, NULL);
    na_cur->tinit = time(NULL);
    na_cur->tlast = time(NULL);

    na_cur->power = -1;
    na_cur->channel = -1;
    na_cur->ack = 0;
    na_cur->ack_old = 0;
    na_cur->ackps = 0;
    na_cur->cts = 0;
    na_cur->rts_r = 0;
    na_cur->rts_t = 0;

done:
    return na_cur;
}

static struct NA_info * na_info_new(
    struct na_list_head * const na_list, 
    mac_address const * const mac)
{
    struct NA_info * const na_cur = na_info_alloc(mac);

    if (na_cur == NULL)
    {
        goto done;
    }

    TAILQ_INSERT_TAIL(na_list, na_cur, entry);

done:
    return na_cur;
}

static struct NA_info * na_info_lookup_existing(
	struct na_list_head * const list,
	mac_address const * const mac)
{
	struct NA_info * na_cur;

	TAILQ_FOREACH(na_cur, list, entry)
	{
		if (MAC_ADDRESS_EQUAL(&na_cur->namac, mac))
		{
			break;
		}
	}

	return na_cur;
}

struct NA_info * na_info_lookup(
    struct na_list_head * const na_list, 
    mac_address const * const mac)
{
    struct NA_info * na_cur;

    na_cur = na_info_lookup_existing(na_list, mac);
    if (na_cur != NULL)
    {
        goto done;
    }

    na_cur = na_info_new(na_list, mac);

done:
    return na_cur;
}

static void remove_namac(
    struct na_list_head * const na_list, 
    mac_address const * const mac)
{
    struct NA_info * const na_cur = na_info_lookup_existing(na_list, mac);

	if (na_cur == NULL)
	{
		goto done;
	}

    na_info_remove(na_list, na_cur);

done:
	return;
}

static void sta_populate_gps(
    struct ST_info * const st_cur,
    float const * const gps_coordinates)
{
    memcpy(st_cur->gps_loc_min, gps_coordinates, sizeof(st_cur->gps_loc_min));
    memcpy(st_cur->gps_loc_max, gps_coordinates, sizeof(st_cur->gps_loc_max));
    memcpy(st_cur->gps_loc_best, gps_coordinates, sizeof(st_cur->gps_loc_best));
}

static struct ST_info * st_info_alloc(mac_address const * const stmac)
{
    struct ST_info * const st_cur = calloc(1, sizeof *st_cur);

    if (st_cur == NULL)
    {
        perror("calloc failed");
        goto done;
    }

    MAC_ADDRESS_COPY(&st_cur->stmac, stmac);

    st_cur->nb_pkt = 0;

    st_cur->tinit = time(NULL);
    st_cur->tlast = st_cur->tinit;
    st_cur->time_printed = 0;

    st_cur->power = -1;
    st_cur->best_power = -1;
    st_cur->rate_to = -1;
    st_cur->rate_from = -1;

    st_cur->probe_index = -1;
    st_cur->missed = 0;
    st_cur->lastseq = 0;
    st_cur->qos_fr_ds = 0;
    st_cur->qos_to_ds = 0;
    st_cur->channel = 0;
    st_cur->old_channel = 0;

    gettimeofday(&st_cur->ftimer, NULL);

    for (size_t i = 0; i < NB_PRB; i++)
    {
        memset(st_cur->probes[i], 0, sizeof(st_cur->probes[i]));
        st_cur->ssid_length[i] = 0;
    }

done:
    return st_cur;
}

static struct ST_info * sta_info_new(
    struct local_options * const options, 
    mac_address const * const mac)
{
    struct ST_info * const st_cur = st_info_alloc(mac);

    if (st_cur == NULL)
    {
        goto done;
    }

    st_cur->manuf =
        get_manufacturer_by_oui(options->manufacturer_list, mac->addr);

    sta_populate_gps(st_cur, options->gps_context.gps_loc);

    TAILQ_INSERT_TAIL(&options->sta_list, st_cur, entry);

done:
    return st_cur;
}

static struct ST_info * sta_info_lookup_existing(
	struct sta_list_head * const sta_list,
	mac_address const * const mac)
{
	struct ST_info * st_cur;

	TAILQ_FOREACH(st_cur, sta_list, entry)
	{
		if (MAC_ADDRESS_EQUAL(&st_cur->stmac, mac))
		{
			break;
		}
	}

	return st_cur;
}

static struct ST_info * sta_info_lookup(
    struct local_options * const options,
    mac_address const * const mac,
    bool * const is_new)
{
    struct ST_info * st_cur;

    st_cur = sta_info_lookup_existing(&options->sta_list, mac);
    if (st_cur != NULL)
    {
        *is_new = false;

        goto done;
    }

    *is_new = true;
    st_cur = sta_info_new(options, mac);

done:
    return st_cur;
}

static void sta_info_free(struct ST_info * const st_cur)
{
	free(st_cur->manuf);
	free(st_cur);
}

static void sta_info_remove(
    struct sta_list_head * const sta_list,
    struct ST_info * const st_cur)
{
    TAILQ_REMOVE(sta_list, st_cur, entry);

    sta_info_free(st_cur);
}

static void sta_list_free(struct sta_list_head * const sta_list)
{
    while (TAILQ_FIRST(sta_list) != NULL)
    {
        struct ST_info * const st_cur = TAILQ_FIRST(sta_list);

        sta_info_remove(sta_list, st_cur);
    }
}

static void free_stas_with_this_base_ap(
	struct sta_list_head * const sta_list,
	struct AP_info * ap_cur)
{
	struct ST_info * st_cur;
	struct ST_info * st_tmp;

	TAILQ_FOREACH_SAFE(st_cur, sta_list, entry, st_tmp)
	{
		if (st_cur->base == ap_cur)
		{
            sta_info_remove(sta_list, st_cur);
		}
	}
}

static void ap_info_free(
    struct AP_info * const ap_cur,
    struct sta_list_head * const sta_list)
{
	free_stas_with_this_base_ap(sta_list, ap_cur);

	uniqueiv_wipe(ap_cur->uiv_root);
	packet_list_free(&ap_cur->pkt_list);
	data_wipe(ap_cur->data_root);
	free(ap_cur->manuf);

    free(ap_cur);
}

static void ap_info_remove(
    struct local_options * const options,
    struct AP_info * const ap_cur)
{
    if (options->p_selected_ap == ap_cur)
    {
        options->p_selected_ap = NULL;
    }

    TAILQ_REMOVE(&options->ap_list, ap_cur, entry);

    ap_info_free(ap_cur, &options->sta_list);
}

static void ap_list_free(struct local_options * const options)
{
    struct ap_list_head * const ap_list = &options->ap_list;

    while (TAILQ_FIRST(ap_list) != NULL)
    {
        struct AP_info * const ap_cur = TAILQ_FIRST(ap_list);

        ap_info_remove(options, ap_cur);
    }
}

static void ap_info_populate_gps(
    struct AP_info * const ap_cur,
    float const * const gps_coordinates)
{
    memcpy(ap_cur->gps_loc_min, gps_coordinates, sizeof(ap_cur->gps_loc_min));
    memcpy(ap_cur->gps_loc_max, gps_coordinates, sizeof(ap_cur->gps_loc_max));
    memcpy(ap_cur->gps_loc_best, gps_coordinates, sizeof(ap_cur->gps_loc_best));
}

static struct AP_info * ap_info_alloc(mac_address const * const bssid)
{
	struct AP_info * const ap_cur = calloc(1, sizeof(*ap_cur));

	if (ap_cur == NULL)
	{
		perror("calloc failed");
		goto done;
	}

	MAC_ADDRESS_COPY(&ap_cur->bssid, bssid);

	ap_cur->nb_pkt = 0;

	ap_cur->tinit = time(NULL);
	ap_cur->tlast = ap_cur->tinit;
	ap_cur->time_printed = 0;

	ap_cur->avg_power = -1;
	ap_cur->best_power = -1;
	ap_cur->power_index = -1;

	for (size_t i = 0; i < NB_PWR; i++)
	{
		ap_cur->power_lvl[i] = -1;
	}

	ap_cur->channel = -1;
	ap_cur->old_channel = -1;
	ap_cur->max_speed = -1;
	ap_cur->security = 0;

	ap_cur->ivbuf = NULL;
	ap_cur->ivbuf_size = 0;

	ap_cur->nb_data = 0;
	ap_cur->nb_dataps = 0;
	ap_cur->nb_data_old = 0;
	gettimeofday(&ap_cur->tv, NULL);

	ap_cur->dict_started = 0;

	ap_cur->key = NULL;

	ap_cur->nb_bcn = 0;

	ap_cur->rx_quality = 0;
	ap_cur->fcapt = 0;
	ap_cur->fmiss = 0;
	ap_cur->last_seq = 0;
	gettimeofday(&ap_cur->ftimef, NULL);
	gettimeofday(&ap_cur->ftimel, NULL);
	gettimeofday(&ap_cur->ftimer, NULL);

	ap_cur->essid_logged = false;
	memset(ap_cur->essid, 0, sizeof ap_cur->essid);
    ap_cur->ssid_length = 0;
    ap_cur->timestamp = 0;

	ap_cur->is_decloak = 0;

	TAILQ_INIT(&ap_cur->pkt_list);

	ap_cur->marked = 0;
	ap_cur->marked_color = 1;

	ap_cur->data_root = NULL;
	ap_cur->EAP_detected = 0;

	/* 802.11n and ac */
	ap_cur->channel_width = CHANNEL_22MHZ; // 20MHz by default
	memset(ap_cur->standard, 0, sizeof ap_cur->standard);

	ap_cur->n_channel.sec_channel = -1;
	ap_cur->n_channel.short_gi_20 = 0;
	ap_cur->n_channel.short_gi_40 = 0;
	ap_cur->n_channel.any_chan_width = 0;
	ap_cur->n_channel.mcs_index = -1;

	ap_cur->ac_channel.center_sgmt[0] = 0;
	ap_cur->ac_channel.center_sgmt[1] = 0;
	ap_cur->ac_channel.mu_mimo = 0;
	ap_cur->ac_channel.short_gi_80 = 0;
	ap_cur->ac_channel.short_gi_160 = 0;
	ap_cur->ac_channel.split_chan = 0;
	ap_cur->ac_channel.mhz_160_chan = 0;
	ap_cur->ac_channel.wave_2 = 0;
	memset(ap_cur->ac_channel.mcs_index, 0, sizeof ap_cur->ac_channel.mcs_index);

done:
	return ap_cur;
}

static struct AP_info * ap_info_new(
    struct local_options * options, 
    mac_address const * const bssid)
{
    struct AP_info * const ap_cur = ap_info_alloc(bssid);

    if (ap_cur == NULL)
    {
        goto done;
    }

    ap_cur->manuf =
        get_manufacturer_by_oui(options->manufacturer_list, bssid->addr);

    if (options->ivs.fp != NULL)
    {
        /* This sucks up loads of memory, and is only required when 
         * logging IVs. 
         */
        ap_cur->uiv_root = uniqueiv_init();
    }

    ap_cur->decloak_detect = options->decloak;
    ap_info_populate_gps(ap_cur, options->gps_context.gps_loc);

    TAILQ_INSERT_TAIL(&options->ap_list, ap_cur, entry);

done:
    return ap_cur;
}

static struct AP_info * ap_info_lookup_existing(
    struct ap_list_head * ap_list,
    mac_address const * const mac)
{
    struct AP_info * ap_cur;

    TAILQ_FOREACH(ap_cur, ap_list, entry)
    {
        if (MAC_ADDRESS_EQUAL(&ap_cur->bssid, mac))
        {
            break;
        }
    }

    return ap_cur;
}

static struct AP_info * ap_info_lookup(
    struct local_options * const options,
    mac_address const * const bssid,
    bool * const is_new)
{
    struct AP_info * ap_cur;

    ap_cur = ap_info_lookup_existing(&options->ap_list, bssid);
    if (ap_cur != NULL)
    {
        *is_new = false;
        goto done;
    }

    *is_new = true;
    ap_cur = ap_info_new(options, bssid);

done:
    return ap_cur;
}

static void purge_old_aps(
    struct local_options * const options,
	time_t const age_limit)
{
    struct ap_list_head * const ap_list = &options->ap_list;
    struct AP_info * ap_cur;
	struct AP_info * ap_tmp;

    TAILQ_FOREACH_SAFE(ap_cur, ap_list, entry, ap_tmp)
	{
		bool const too_old = ap_cur->tlast < age_limit;

		if (too_old)
		{
            ap_info_remove(options, ap_cur);
		}
	}
}

static void purge_old_stas(
	struct sta_list_head * const sta_list,
	time_t const age_limit)
{
	struct ST_info * st_cur;
	struct ST_info * st_tmp;

	TAILQ_FOREACH_SAFE(st_cur, sta_list, entry, st_tmp)
	{
		bool const too_old = st_cur->tlast < age_limit;

		if (too_old)
		{
            sta_info_remove(sta_list, st_cur);
		}
	}
}

static void purge_old_nas(
	struct na_list_head * const na_list,
	time_t const age_limit)
{
	struct NA_info * na_cur;
	struct NA_info * na_tmp;

	TAILQ_FOREACH_SAFE(na_cur, na_list, entry, na_tmp)
	{
		bool const too_old = na_cur->tlast < age_limit;

		if (too_old)
		{
            na_info_remove(na_list, na_cur);
		}
	}
}

static void purge_old_nodes(
    struct local_options * const options,
    size_t const max_age_seconds)
{
    if (max_age_seconds == 0) /* No limit. */
	{
		goto done;
	}

	time_t const current_time = time(NULL);
    time_t const age_limit = current_time - max_age_seconds;

	purge_old_nas(&options->na_list, age_limit);
	purge_old_stas(&options->sta_list, age_limit);
	purge_old_aps(options, age_limit);

done:
	return;
}

static void update_packet_capture_files(
    struct local_options * const options,
    uint8_t const * const packet,
    size_t const packet_length,
    int32_t const ri_power)
{
    if (options->pcap_output.writer != NULL)
    {
        packet_writer_write(options->pcap_output.writer,
                            packet, 
                            packet_length, 
                            ri_power);
    }
}

static void ap_update(
    struct local_options * const options,
    struct AP_info * const ap_cur,
    unsigned char const * const h80211,
    struct rx_info const * const ri)
{
    /* Get the sequence number. */
    int const seq = ((h80211[22] >> 4) + (h80211[23] << 4));

    /* update the last time seen */
    ap_cur->tlast = time(NULL);

    /* only update power if packets comes from
     * the AP: either type == mgmt and SA == BSSID,
     * or FromDS == 1 and ToDS == 0 */

    if (((h80211[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_NODS
         && MAC_ADDRESS_EQUAL((mac_address *)(h80211 + 10), &ap_cur->bssid))
        || ((h80211[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_FROMDS))
    {
        ap_cur->power_index = (ap_cur->power_index + 1) % NB_PWR;
        ap_cur->power_lvl[ap_cur->power_index] = ri->ri_power;

        ap_cur->avg_power = 
            moving_exponential_average(ri->ri_power, ap_cur->avg_power, 0.99f);

        if (ap_cur->avg_power > ap_cur->best_power)
        {
            ap_cur->best_power = ap_cur->avg_power;
            memcpy(ap_cur->gps_loc_best,
                   options->gps_context.gps_loc,
                   sizeof ap_cur->gps_loc_best);
        }

        /* Every packet in here comes from the AP. */

        if (options->gps_context.gps_loc[0] > ap_cur->gps_loc_max[0])
            ap_cur->gps_loc_max[0] = options->gps_context.gps_loc[0];
        if (options->gps_context.gps_loc[1] > ap_cur->gps_loc_max[1])
            ap_cur->gps_loc_max[1] = options->gps_context.gps_loc[1];
        if (options->gps_context.gps_loc[2] > ap_cur->gps_loc_max[2])
            ap_cur->gps_loc_max[2] = options->gps_context.gps_loc[2];

        if (options->gps_context.gps_loc[0] < ap_cur->gps_loc_min[0])
            ap_cur->gps_loc_min[0] = options->gps_context.gps_loc[0];
        if (options->gps_context.gps_loc[1] < ap_cur->gps_loc_min[1])
            ap_cur->gps_loc_min[1] = options->gps_context.gps_loc[1];
        if (options->gps_context.gps_loc[2] < ap_cur->gps_loc_min[2])
            ap_cur->gps_loc_min[2] = options->gps_context.gps_loc[2];

        if (ap_cur->fcapt == 0 && ap_cur->fmiss == 0)
        {
            gettimeofday(&(ap_cur->ftimef), NULL);
        }
        if (ap_cur->last_seq != 0)
        {
            ap_cur->fmiss += (seq - ap_cur->last_seq - 1);
        }
        ap_cur->last_seq = (unsigned int)seq;
        ap_cur->fcapt++;
        gettimeofday(&(ap_cur->ftimel), NULL);

        /* if we are writing to a file and want to make a continuous rolling log save the data here */
        if (options->log_csv.fp != NULL)
        {
            /* Write out our rolling log every time we see data from an AP */
            dump_write_airodump_ng_logcsv_add_ap(options->log_csv.fp,
                                                 ap_cur,
                                                 ri->ri_power,
                                                 &options->gps_context.gps_time,
                                                 options->gps_context.gps_loc);
        }
    }

    switch (h80211[0])
    {
        case IEEE80211_FC0_SUBTYPE_BEACON:
            ap_cur->nb_bcn++;
            break;

        case IEEE80211_FC0_SUBTYPE_PROBE_RESP:
            /* reset the WPS state */
            ap_cur->wps.state = 0xFF;
            ap_cur->wps.ap_setup_locked = 0;
            break;

        default:
            break;
    }

    ap_cur->nb_pkt++;
}

static void sta_update(
    struct local_options * const options,
    struct ST_info * const st_cur,
    struct AP_info * const ap_cur,
    unsigned char const * const h80211,
    struct rx_info const * const ri,
    int const cardnum)
{
    /* Get the sequence number. */
    int const seq = ((h80211[22] >> 4) + (h80211[23] << 4));

    /* Update the last time seen. */
    st_cur->tlast = time(NULL);

    if (st_cur->base == NULL || !MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid))
    {
        st_cur->base = ap_cur;
    }

    // update bitrate to station
    if ((h80211[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_FROMDS)
    {
        st_cur->rate_to = ri->ri_rate;
    }

    /* only update power if packets comes from the
     * client: either type == Mgmt and SA != BSSID,
     * or FromDS == 0 and ToDS == 1 */

    if (((h80211[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_NODS
         && !MAC_ADDRESS_EQUAL((mac_address *)(h80211 + 10), &ap_cur->bssid))
        || ((h80211[1] & IEEE80211_FC1_DIR_MASK) == IEEE80211_FC1_DIR_TODS))
    {
        st_cur->power = ri->ri_power;
        if (ri->ri_power > st_cur->best_power)
        {
            st_cur->best_power = ri->ri_power;
            memcpy(ap_cur->gps_loc_best,
                   options->gps_context.gps_loc,
                   sizeof(st_cur->gps_loc_best));
        }

        st_cur->rate_from = ri->ri_rate;
        if (ri->ri_channel > 0 && ri->ri_channel <= HIGHEST_CHANNEL)
        {
            st_cur->channel = ri->ri_channel;
        }
        else
        {
            st_cur->channel = options->channel[cardnum];
        }

        if (options->gps_context.gps_loc[0] > st_cur->gps_loc_max[0])
            st_cur->gps_loc_max[0] = options->gps_context.gps_loc[0];
        if (options->gps_context.gps_loc[1] > st_cur->gps_loc_max[1])
            st_cur->gps_loc_max[1] = options->gps_context.gps_loc[1];
        if (options->gps_context.gps_loc[2] > st_cur->gps_loc_max[2])
            st_cur->gps_loc_max[2] = options->gps_context.gps_loc[2];

        if (options->gps_context.gps_loc[0] < st_cur->gps_loc_min[0])
            st_cur->gps_loc_min[0] = options->gps_context.gps_loc[0];
        if (options->gps_context.gps_loc[1] < st_cur->gps_loc_min[1])
            st_cur->gps_loc_min[1] = options->gps_context.gps_loc[1];
        if (options->gps_context.gps_loc[2] < st_cur->gps_loc_min[2])
            st_cur->gps_loc_min[2] = options->gps_context.gps_loc[2];

        if (st_cur->lastseq != 0)
        {
            int const missed = seq - st_cur->lastseq - 1;

            if (missed > 0 && missed < 1000)
            {
                st_cur->missed += missed;
            }
        }
        st_cur->lastseq = (unsigned int)seq;

        /* if we are writing to a file and want to make a continuous rolling log save the data here */
        if (options->log_csv.fp != NULL)
        {
            /* Write out our rolling log every time we see data from a client */
            dump_write_airodump_ng_logcsv_add_client(options->log_csv.fp,
                                                     ap_cur, 
                                                     st_cur, 
                                                     ri->ri_power, 
                                                     &options->gps_context.gps_time, 
                                                     options->gps_context.gps_loc);
        }
    }

    st_cur->nb_pkt++;
}

static mac_address const * locate_bssid_in_80211(uint8_t const * const h80211)
{
    mac_address const * bssid;

    /* Locate the access point's MAC address. */

    switch (h80211[1] & IEEE80211_FC1_DIR_MASK)
    {
        case IEEE80211_FC1_DIR_NODS:
            bssid = (mac_address *)(h80211 + 16);
            break; // Adhoc
        case IEEE80211_FC1_DIR_TODS:
            bssid = (mac_address *)(h80211 + 4);
            break; // ToDS
        case IEEE80211_FC1_DIR_FROMDS:
            bssid = (mac_address *)(h80211 + 10);
            break; // FromDS
        case IEEE80211_FC1_DIR_DSTODS:
            bssid = (mac_address *)(h80211 + 10);
            break; // WDS -> Transmitter taken as BSSID
        default:
            /* Can't happen. All cases have been checked. */
            bssid = NULL;
            abort();
    }

    return bssid;
}

static mac_address const * locate_sta_mac_in_80211(
    uint8_t const * const h80211,
    mac_address const * const bssid)
{
    mac_address const * stmac;

    switch (h80211[1] & IEEE80211_FC1_DIR_MASK)
    {
        case IEEE80211_FC1_DIR_NODS:

            /* If management, check that SA != BSSID. */

            if (MAC_ADDRESS_EQUAL((mac_address *)(h80211 + 10), bssid))
            {
                stmac = NULL;
                goto done;
            }
            stmac = (mac_address *)(h80211 + 10);
            break;

        case IEEE80211_FC1_DIR_TODS:

            /* ToDS packet, must come from a client. */

            stmac = (mac_address *)(h80211 + 10);
            break;

        case IEEE80211_FC1_DIR_FROMDS:

            /* FromDS packet, reject broadcast MACs */

            if (MAC_IS_GROUP_ADDRESS(&h80211[4]))
            {
                stmac = NULL;
                goto done;
            }
            stmac = (mac_address *)(h80211 + 4);
            break;

        case IEEE80211_FC1_DIR_DSTODS:

            stmac = NULL;
            break;

        default:
            /* Can't happen. All possible cases have been checked. */
            stmac = NULL;
            abort();
    }

done:
    return stmac;
}

static void parse_probe_request(
    struct ST_info * const st_cur, 
    uint8_t const * const h80211, 
    size_t const caplen)
{
    if (h80211[0] == IEEE80211_FC0_SUBTYPE_PROBE_REQ && st_cur != NULL)
    {
        uint8_t const * const data_end = h80211 + caplen;
        uint8_t const * p = h80211 + 24;

        while (p < data_end)
        {
            if (p + 2 + p[1] > data_end)
            {
                goto done;
            }

            if (p[0] == 0x00
                && p[1] > 0
                && p[2] != '\0'
                && (p[1] > 1 || p[2] != ' '))
            {
                uint8_t const * const essid = p + 2;
                size_t const essid_length = MIN(ESSID_LENGTH, p[1]);

                if (essid_has_control_chars(essid, essid_length))
                {
                    goto done;
                }

                /* Got a valid ASCII probed ESSID. 
                 * Check if it's already in the ring buffer.
                 */
                /* FIXME: This comparison won't work if the ESSID is one that 
                 * would get modified by make_printable() below. 
                 */
                for (size_t i = 0; i < NB_PRB; i++)
                {
                    if (memcmp(st_cur->probes[i], essid, essid_length) == 0)
                    {
                        goto done;
                    }
                }

                st_cur->probe_index = (st_cur->probe_index + 1) % NB_PRB;
                memcpy(st_cur->probes[st_cur->probe_index], essid, essid_length);
                st_cur->probes[st_cur->probe_index][essid_length] = '\0';
                st_cur->ssid_length[st_cur->probe_index] = essid_length;

                if (!verifyssid((uint8_t *)st_cur->probes[st_cur->probe_index]))
                {
                    make_printable((uint8_t *)st_cur->probes[st_cur->probe_index],
                                   st_cur->ssid_length[st_cur->probe_index]);
                }
            }

            p += 2 + p[1];
        }
    }

done:
    return;
}

static bool parse_beacon_or_probe_response(
    struct local_options * const options, 
    struct AP_info * const ap_cur, 
    uint8_t const * const h80211, 
    size_t caplen)
{
    bool success;

    if (h80211[0] == IEEE80211_FC0_SUBTYPE_BEACON
        || h80211[0] == IEEE80211_FC0_SUBTYPE_PROBE_RESP)
    {
        uint8_t const * const data_end = h80211 + caplen;
        uint8_t const * p;

        if (!(ap_cur->security & (STD_OPN | STD_WEP | STD_WPA | STD_WPA2)))
        {
            if ((h80211[34] & 0x10) >> 4)
            {
                ap_cur->security |= STD_WEP | ENC_WEP;
            }
            else
            {
                ap_cur->security |= STD_OPN;
            }
        }

        ap_cur->preamble = (h80211[34] & 0x20) >> 5;

        unsigned long long * tstamp = (unsigned long long *)(h80211 + 24);
        ap_cur->timestamp = letoh64(*tstamp);

        p = h80211 + 36;

        while (p < data_end)
        {
            if (p + 2 + p[1] > data_end)
            {
                break;
            }

            if (p[0] == 0x00 && p[1] > 0 && p[2] != '\0'
                && (p[1] > 1 || p[2] != ' '))
            {
                /* found a non-cloaked ESSID */
                ap_cur->ssid_length = MIN((sizeof ap_cur->essid - 1), p[1]);

                memcpy(ap_cur->essid, p + 2, ap_cur->ssid_length);
                ap_cur->essid[ap_cur->ssid_length] = '\0';

                if (!ivs_log_essid(options->ivs.fp,
                                   &ap_cur->essid_logged,
                                   &ap_cur->bssid,
                                   &options->ivs.prev_bssid,
                                   ap_cur->essid,
                                   ap_cur->ssid_length))
                {
                    success = false;
                    goto done;
                }

                if (!verifyssid(ap_cur->essid))
                {
                    make_printable(ap_cur->essid, ap_cur->ssid_length);
                }
            }

            /* get the maximum speed in Mb and the AP's channel */

            if (p[0] == 0x01 || p[0] == 0x32)
            {
                if (ap_cur->max_speed < (p[1 + p[1]] & 0x7F) / 2)
                {
                    ap_cur->max_speed = (p[1 + p[1]] & 0x7F) / 2;
                }
            }

            if (p[0] == 0x03)
            {
                ap_cur->channel = p[2];
            }
            else if (p[0] == 0x3d)
            {
                if (ap_cur->standard[0] == '\0')
                {
                    strlcpy(ap_cur->standard, "n", sizeof ap_cur->standard);
                }

                /* also get the channel from ht information->primary channel */
                ap_cur->channel = p[2];

                // Get channel width and secondary channel
                switch (p[3] % 4)
                {
                    case 0:
                        // 20MHz
                        ap_cur->channel_width = CHANNEL_20MHZ;
                        break;
                    case 1:
                        // Above
                        ap_cur->n_channel.sec_channel = 1;
                        switch (ap_cur->channel_width)
                        {
                            case CHANNEL_UNKNOWN_WIDTH:
                            case CHANNEL_3MHZ:
                            case CHANNEL_5MHZ:
                            case CHANNEL_10MHZ:
                            case CHANNEL_20MHZ:
                            case CHANNEL_22MHZ:
                            case CHANNEL_30MHZ:
                            case CHANNEL_20_OR_40MHZ:
                                ap_cur->channel_width = CHANNEL_40MHZ;
                                break;
                            default:
                                break;
                        }
                        break;
                    case 2:
                        // Reserved
                        break;
                    case 3:
                        // Below
                        ap_cur->n_channel.sec_channel = -1;
                        switch (ap_cur->channel_width)
                        {
                            case CHANNEL_UNKNOWN_WIDTH:
                            case CHANNEL_3MHZ:
                            case CHANNEL_5MHZ:
                            case CHANNEL_10MHZ:
                            case CHANNEL_20MHZ:
                            case CHANNEL_22MHZ:
                            case CHANNEL_30MHZ:
                            case CHANNEL_20_OR_40MHZ:
                                ap_cur->channel_width = CHANNEL_40MHZ;
                                break;
                            default:
                                break;
                        }
                        break;
                    default:
                        break;
                }

                ap_cur->n_channel.any_chan_width = (uint8_t)((p[3] / 4) % 2);
            }

            // HT capabilities
            if (p[0] == 0x2d && p[1] > 18)
            {
                if (ap_cur->standard[0] == '\0')
                {
                    strlcpy(ap_cur->standard, "n", sizeof ap_cur->standard);
                }

                // Short GI for 20/40MHz
                ap_cur->n_channel.short_gi_20 = (uint8_t)((p[3] / 32) % 2);
                ap_cur->n_channel.short_gi_40 = (uint8_t)((p[3] / 64) % 2);

                // Parse MCS rate
                /*
                 * XXX: Sometimes TX and RX spatial stream # differ and none of
                 * the beacon
                 * have that. If someone happens to have such AP, open an issue
                 * with it.
                 * Ref:
                 * https://www.wireshark.org/lists/wireshark-bugs/201307/msg00098.html
                 * See IEEE standard 802.11-2012 table 8.126
                 *
                 * For now, just figure out the highest MCS rate.
                 */
                if ((unsigned char)ap_cur->n_channel.mcs_index == 0xff)
                {
                    uint32_t rx_mcs_bitmask;

                    memcpy(&rx_mcs_bitmask, p + 5, sizeof(rx_mcs_bitmask));
                    while (rx_mcs_bitmask)
                    {
                        ++ap_cur->n_channel.mcs_index;
                        rx_mcs_bitmask /= 2;
                    }
                }
            }

            // VHT Capabilities
            if (p[0] == 0xbf && p[1] >= 12)
            {
                // Standard is AC
                strlcpy(ap_cur->standard, "ac", sizeof ap_cur->standard);

                ap_cur->ac_channel.split_chan = (uint8_t)((p[3] / 4) % 4);

                ap_cur->ac_channel.short_gi_80 = (uint8_t)((p[3] / 32) % 2);
                ap_cur->ac_channel.short_gi_160 = (uint8_t)((p[3] / 64) % 2);

				/* XXX - How can this result ever be anything other than 0.
				 * 0b11000 % 2 == 0 doesn't it?
				 */
				ap_cur->ac_channel.mu_mimo = (uint8_t)((p[4] & 0x18) % 2);

                // A few things indicate Wave 2: MU-MIMO, 80+80 Channels
                /* FIXME - is use of the || logical operator really what is
                 * wanted? Why the % 2 at the end if the result of the || is
                 * only ever 0 or 1?
                 */
                ap_cur->ac_channel.wave_2
                    = (uint8_t)((ap_cur->ac_channel.mu_mimo
                                 || ap_cur->ac_channel.split_chan)
                                % 2);

                // Maximum rates (16 bit)
                uint16_t tx_mcs;
                memcpy(&tx_mcs, p + 10, sizeof(tx_mcs)); /* XXX - endianness? */

                // Maximum of 8 SS, each uses 2 bits
                for (uint8_t stream_idx = 0; stream_idx < MAX_AC_MCS_INDEX;
                     ++stream_idx)
                {
                    uint8_t mcs = (uint8_t)(tx_mcs % 4);

                    // Unsupported -> No more spatial stream
                    if (mcs == 3)
                    {
                        break;
                    }
                    switch (mcs)
                    {
                        case 0:
                            // support of MCS 0-7
                            ap_cur->ac_channel.mcs_index[stream_idx] = 7;
                            break;
                        case 1:
                            // support of MCS 0-8
                            ap_cur->ac_channel.mcs_index[stream_idx] = 8;
                            break;
                        case 2:
                            // support of MCS 0-9
                            ap_cur->ac_channel.mcs_index[stream_idx] = 9;
                            break;
                        default:
                            break;
                    }

                    // Next spatial stream
                    tx_mcs /= 4;
                }
            }

            // VHT Operations
            if (p[0] == 0xc0 && p[1] >= 3)
            {
                // Standard is AC
                strlcpy(ap_cur->standard, "ac", sizeof ap_cur->standard);

                // Channel width
                switch (p[2])
                {
                    case 0:
                        // 20 or 40MHz
                        ap_cur->channel_width = CHANNEL_20_OR_40MHZ;
                        break;
                    case 1:
                        ap_cur->channel_width = CHANNEL_80MHZ;
                        break;
                    case 2:
                        ap_cur->channel_width = CHANNEL_160MHZ;
                        break;
                    case 3:
                        // 80+80MHz
                        ap_cur->channel_width = CHANNEL_80_80MHZ;
                        ap_cur->ac_channel.split_chan = 1;
                        break;
                    default:
                        break;
                }

                // 802.11ac channel center segments
                ap_cur->ac_channel.center_sgmt[0] = p[3];
                ap_cur->ac_channel.center_sgmt[1] = p[4];
            }

            // Next
            p += 2 + p[1];
        }

        // Now get max rate
        if (strcmp(ap_cur->standard, "n") == 0 || strcmp(ap_cur->standard, "ac") == 0)
        {
            int sgi = 0;
            int width = 0;

            switch (ap_cur->channel_width)
            {
                case CHANNEL_20MHZ:
                    width = 20;
                    sgi = ap_cur->n_channel.short_gi_20;
                    break;
                case CHANNEL_20_OR_40MHZ:
                case CHANNEL_40MHZ:
                    width = 40;
                    sgi = ap_cur->n_channel.short_gi_40;
                    break;
                case CHANNEL_80MHZ:
                    width = 80;
                    sgi = ap_cur->ac_channel.short_gi_80;
                    break;
                case CHANNEL_80_80MHZ:
                case CHANNEL_160MHZ:
                    width = 160;
                    sgi = ap_cur->ac_channel.short_gi_160;
                    break;
                default:
                    break;
            }

            if (width != 0)
            {
                // In case of ac, get the amount of spatial streams
                int amount_ss = 1;

                if (strcmp(ap_cur->standard, "n") != 0)
                {
                    for (amount_ss = 0;
                         amount_ss < MAX_AC_MCS_INDEX
                         && ap_cur->ac_channel.mcs_index[amount_ss] != 0;
                         ++amount_ss)
                    {
                        /* Do nothing. */
                        ;
                    }
                }

                // Get rate
                float max_rate
                    = (strcmp(ap_cur->standard, "n") == 0)
                    ? get_80211n_rate(
                    width, sgi, ap_cur->n_channel.mcs_index)
                    : get_80211ac_rate(
                    width,
                    sgi,
                    ap_cur->ac_channel.mcs_index[amount_ss - 1],
                    amount_ss);

                // If no error, update rate
                if (max_rate > 0)
                {
                    ap_cur->max_speed = (int)max_rate;
                }
            }
        }
    }

    success = true;

done:
    return success;
}

static void parse_beacon_or_probe_response_2(
    struct AP_info * const ap_cur,
    uint8_t const * const h80211,
    size_t caplen)
{
    if ((h80211[0] == IEEE80211_FC0_SUBTYPE_BEACON
         || h80211[0] == IEEE80211_FC0_SUBTYPE_PROBE_RESP)
        && caplen > 38)
    {
        uint8_t const * const data_end = h80211 + caplen;
        uint8_t const * p;

        p = h80211 + 36; // ignore hdr + fixed params

        while (p < data_end)
        {
            if (p + 2 + p[1] > data_end)
            {
                break;
            }

            uint8_t const type = p[0];
            uint8_t const length = p[1];

            // Find WPA and RSN tags
            if ((type == 0xDD && (length >= 8)
                 && (memcmp(p + 2, "\x00\x50\xF2\x01\x01\x00", 6) == 0))
                || (type == 0x30))
            {
                ap_cur->security &= ~(STD_WEP | ENC_WEP | STD_WPA);

                uint8_t const * const org_p = p;
                int offset = 0;

                if (type == 0xDD)
                {
                    // WPA defined in vendor specific tag -> WPA1 support
                    ap_cur->security |= STD_WPA;
                    offset = 4;
                }

                // RSN => WPA2
                if (type == 0x30)
                {
					ap_cur->security |= STD_WPA2;
                    offset = 0;
                }

                if (length < (18 + offset))
                {
                    p += length + 2;
                    continue;
                }

                // Number of pairwise cipher suites
                if (p + 9 + offset > data_end)
                {
                    break;
                }

                size_t const numuni = p[8 + offset] + (p[9 + offset] << 8);

                // Number of Authentication Key Managament suites
                if (p + (11 + offset) + 4 * numuni > data_end)
                {
                    break;
                }

                size_t const numauth = p[(10 + offset) + 4 * numuni]
                    + (p[(11 + offset) + 4 * numuni] << 8);

                p += (10 + offset);

                if (type != 0x30)
                {
                    if (p + (4 * numuni) + (2 + 4 * numauth) > data_end)
                    {
                        break;
                    }
                }
                else
                {
                    if (p + (4 * numuni) + (2 + 4 * numauth) + 2 > data_end)
                    {
                        break;
                    }
                }

                // Get the list of cipher suites
                for (size_t i = 0; i < (size_t)numuni; i++)
                {
                    switch (p[i * 4 + 3])
                    {
                        case 0x01:
                            ap_cur->security |= ENC_WEP;
                            break;
                        case 0x02:
                            ap_cur->security |= ENC_TKIP;
                            ap_cur->security &= ~STD_WPA2;
                            break;
                        case 0x03:
                            ap_cur->security |= ENC_WRAP;
                            break;
                        case 0x0A:
                        case 0x04:
                            ap_cur->security |= ENC_CCMP;
                            ap_cur->security |= STD_WPA2;
                            break;
                        case 0x05:
                            ap_cur->security |= ENC_WEP104;
                            break;
                        case 0x08:
                        case 0x09:
                            ap_cur->security |= ENC_GCMP;
                            ap_cur->security |= STD_WPA2;
                            break;
                        case 0x0B:
                        case 0x0C:
                            ap_cur->security |= ENC_GMAC;
                            ap_cur->security |= STD_WPA2;
                            break;
                        default:
                            break;
                    }
                }

                p += 2 + 4 * numuni;

                // Get the AKM suites
                for (size_t i = 0; i < numauth; i++)
                {
                    switch (p[i * 4 + 3])
                    {
                        case 0x01:
                            ap_cur->security |= AUTH_MGT;
                            break;
                        case 0x02:
                            ap_cur->security |= AUTH_PSK;
                            break;
                        case 0x06:
                        case 0x0d:
                            ap_cur->security |= AUTH_CMAC;
                            break;
                        case 0x08:
                            ap_cur->security |= AUTH_SAE;
                            break;
                        case 0x12:
                            ap_cur->security |= AUTH_OWE;
                            break;
                        default:
                            break;
                    }
                }

                p = org_p + length + 2;
            }
            else if ((type == 0xDD && (length >= 8)
                      && (memcmp(p + 2, "\x00\x50\xF2\x02\x01\x01", 6) == 0)))
            {
                // QoS IE
                ap_cur->security |= STD_QOS;
                p += length + 2;
            }
            else if ((type == 0xDD && (length >= 4)
                      && (memcmp(p + 2, "\x00\x50\xF2\x04", 4) == 0)))
            {
                // WPS IE
                uint8_t const * const org_p = p;

                p += 6;
                int len = length, subtype = 0, sublen = 0;
                while (len >= 4)
                {
                    subtype = (p[0] << 8) + p[1];
                    sublen = (p[2] << 8) + p[3];
                    if (sublen > len)
                        break;
                    switch (subtype)
                    {
                        case 0x104a: // WPS Version
                            ap_cur->wps.version = p[4];
                            break;
                        case 0x1011: // Device Name
                        case 0x1012: // Device Password ID
                        case 0x1021: // Manufacturer
                        case 0x1023: // Model
                        case 0x1024: // Model Number
                        case 0x103b: // Response Type
                        case 0x103c: // RF Bands
                        case 0x1041: // Selected Registrar
                        case 0x1042: // Serial Number
                            break;
                        case 0x1044: // WPS State
                            ap_cur->wps.state = p[4];
                            break;
                        case 0x1047: // UUID Enrollee
                        case 0x1049: // Vendor Extension
                            if (memcmp(&p[4], "\x00\x37\x2A", 3) == 0)
                            {
                                unsigned char const * pwfa = &p[7];
                                int wfa_len = ntohs(load16(&p[2]));

                                while (wfa_len > 0)
                                {
                                    if (*pwfa == 0)
                                    { // Version2
                                        ap_cur->wps.version = pwfa[2];
                                        break;
                                    }
                                    wfa_len -= pwfa[1] + 2;
                                    pwfa += pwfa[1] + 2;
                                }
                            }
                            break;
                        case 0x1054: // Primary Device Type
                            break;
                        case 0x1057: // AP Setup Locked
                            ap_cur->wps.ap_setup_locked = p[4];
                            break;
                        case 0x1008: // Config Methods
                        case 0x1053: // Selected Registrar Config Methods
                            ap_cur->wps.meth = (p[4] << 8) + p[5];
                            break;
                        default: // Unknown type-length-value
                            break;
                    }
                    p += sublen + 4;
                    len -= sublen + 4;
                }
                p = org_p + length + 2;
            }
            else
            {
                p += length + 2;
            }
        }
    }
}

static void parse_authentication_response(
    struct AP_info * const ap_cur, 
    uint8_t const * const h80211, 
    size_t const caplen)
{
    if (h80211[0] == IEEE80211_FC0_SUBTYPE_AUTH && caplen >= 30)
    {
        if (ap_cur->security & STD_WEP)
        {
            // successful step 2 or 4 (coming from the AP)
            if (memcmp(h80211 + 28, "\x00\x00", 2) == 0
                && (h80211[26] == 0x02 || h80211[26] == 0x04))
            {
                ap_cur->security &= ~(AUTH_OPN | AUTH_PSK | AUTH_MGT);
                if (h80211[24] == 0x00)
                    ap_cur->security |= AUTH_OPN;
                if (h80211[24] == 0x01)
                    ap_cur->security |= AUTH_PSK;
            }
        }
    }
}

static bool parse_association_request(
    struct local_options * const options, 
    struct AP_info * const ap_cur, 
    struct ST_info * const st_cur, 
    uint8_t const * const h80211, 
    size_t const caplen)
{
    bool success;

    if (h80211[0] == IEEE80211_FC0_SUBTYPE_ASSOC_REQ && caplen > 28)
    {
        uint8_t const * const data_end = h80211 + caplen;
        uint8_t const * p = h80211 + 28;

        while (p < data_end)
        {
            if (p + 2 + p[1] > data_end)
            {
                break;
            }

            if (p[0] == 0x00 && p[1] > 0 && p[2] != '\0'
                && (p[1] > 1 || p[2] != ' '))
            {
                /* Found a non-cloaked ESSID. */
                ap_cur->ssid_length = MIN((sizeof ap_cur->essid - 1), p[1]);

                memcpy(ap_cur->essid, p + 2, ap_cur->ssid_length);
                ap_cur->essid[ap_cur->ssid_length] = '\0';

                if (!ivs_log_essid(options->ivs.fp,
                                   &ap_cur->essid_logged,
                                   &ap_cur->bssid,
                                   &options->ivs.prev_bssid,
                                   ap_cur->essid,
                                   ap_cur->ssid_length))
                {
                    success = false;
                    goto done;
                }

                if (!verifyssid(ap_cur->essid))
                {
                    make_printable(ap_cur->essid, ap_cur->ssid_length);
                }
            }

            p += 2 + p[1];
        }

        if (st_cur != NULL)
        {
            st_cur->wpa.state = 0;
        }
    }

    success = true;

done:
    return success;
}

static bool parse_packet_data(
    struct local_options * const options,
    struct AP_info * const ap_cur,
    struct ST_info * const st_cur,
    uint8_t const * const h80211,
    size_t const caplen,
    struct rx_info * const ri,
    int const cardnum)
{
    bool success;

    if ((h80211[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_DATA)
    {
        /* update the channel if we didn't get any beacon */

        if (ap_cur->channel == -1)
        {
            if (ri->ri_channel > 0 && ri->ri_channel <= HIGHEST_CHANNEL)
            {
                ap_cur->channel = ri->ri_channel;
            }
            else
            {
                ap_cur->channel = options->channel[cardnum];
            }
        }

        /* check the SNAP header to see if data is encrypted */

        unsigned int z = ((h80211[1] & IEEE80211_FC1_DIR_MASK) != IEEE80211_FC1_DIR_DSTODS)
            ? 24
            : 30;

        /* Check if 802.11e (QoS) */
        if ((h80211[0] & 0x80) == 0x80)
        {
            z += 2;
            if (st_cur != NULL)
            {
                if ((h80211[1] & 3) == 1) // ToDS
                    st_cur->qos_to_ds = 1;
                else
                    st_cur->qos_fr_ds = 1;
            }
        }
        else
        {
            if (st_cur != NULL)
            {
                if ((h80211[1] & 3) == 1) // ToDS
                    st_cur->qos_to_ds = 0;
                else
                    st_cur->qos_fr_ds = 0;
            }
        }

        if (z == 24)
        {
            if (ap_cur->decloak_detect)
            {
                if (list_check_decloak(&ap_cur->pkt_list, caplen, h80211) != 0)
                {
                    list_add_packet(&ap_cur->pkt_list, h80211, caplen);
                }
                else
                {
                    ap_cur->is_decloak = 1;
                    ap_cur->decloak_detect = 0;

                    packet_list_free(&ap_cur->pkt_list);

                    snprintf(options->message,
                             sizeof(options->message),
                             "Decloak: %02X:%02X:%02X:%02X:%02X:%02X ",
                             ap_cur->bssid.addr[0],
                             ap_cur->bssid.addr[1],
                             ap_cur->bssid.addr[2],
                             ap_cur->bssid.addr[3],
                             ap_cur->bssid.addr[4],
                             ap_cur->bssid.addr[5]);
                }
            }
        }

        if (z + 26 > caplen)
        {
            success = true;
            goto done;
        }

        if (h80211[z] == h80211[z + 1] && h80211[z + 2] == 0x03)
        {
            //            if( ap_cur->encryption < 0 )
            //                ap_cur->encryption = 0;

            /* if ethertype == IPv4, find the LAN address */

            if (h80211[z + 6] == 0x08 && h80211[z + 7] == 0x00
                && (h80211[1] & 3) == 0x01)
            {
                memcpy(ap_cur->lanip, &h80211[z + 20], 4);
            }

            if (h80211[z + 6] == 0x08 && h80211[z + 7] == 0x06)
            {
                memcpy(ap_cur->lanip, &h80211[z + 22], 4);
            }
        }
        //        else
        //            ap_cur->encryption = 2 + ( ( h80211[z + 3] & 0x20 ) >> 5
        //            );

        if (ap_cur->security == 0 || (ap_cur->security & STD_WEP))
        {
            if ((h80211[1] & 0x40) != 0x40)
            {
                ap_cur->security |= STD_OPN;
            }
            else
            {
                if ((h80211[z + 3] & 0x20) == 0x20)
                {
                    ap_cur->security |= STD_WPA;
                }
                else
                {
                    ap_cur->security |= STD_WEP;
                    if ((h80211[z + 3] & 0xC0) != 0x00)
                    {
                        ap_cur->security |= ENC_WEP40;
                    }
                    else
                    {
                        ap_cur->security &= ~ENC_WEP40;
                        ap_cur->security |= ENC_WEP;
                    }
                }
            }
        }

        if (z + 10 > caplen)
        {
            success = true;
            goto done;
        }

        if (ap_cur->security & STD_WEP)
        {
            /* WEP: check if we've already seen this IV */

            if (!uniqueiv_check(ap_cur->uiv_root, &h80211[z]))
            {
                /* first time seen IVs */

                if (options->ivs.fp != NULL)
                {
                    unsigned char clear[2048] = { 0 };
                    int weight[16] = { 0 };
                    int clen;

                    /* datalen = caplen - (header+iv+ivs) */
                    size_t dlen = caplen - z - 4 - 4; // original data len

                    if (dlen > 2048)
                    {
                        dlen = 2048;
                    }

                    // get cleartext + len + 4(iv+idx)
                    int num_xor = known_clear(clear, &clen, weight, h80211, dlen);

                    size_t data_size;
                    uint16_t ivs_type;

                    if (num_xor == 1)
                    {
                        data_size = clen;
                        ivs_type = IVS2_XOR;
                        /* reveal keystream (plain^encrypted) */
                        for (size_t n = 0; n < data_size; n++)
                        {
                            clear[n] = (uint8_t)((clear[n] ^ h80211[z + 4 + n])
                                                 & 0xFF);
                        }
                        // clear is now the keystream
                    }
                    else
                    {
                        // do it again to get it 2 bytes higher
                        num_xor = known_clear(
                            clear + 2, &clen, weight, h80211, dlen);
                        ivs_type = IVS2_PTW;

                        // len = 4(iv+idx) + 1(num of keystreams) + 1(len per
                        // keystream) + 32*num_xor + 16*sizeof(int)(weight[16])
                        data_size = 1 + 1 + 32 * num_xor + 16 * sizeof(int);
                        clear[0] = (uint8_t)num_xor;
                        clear[1] = (uint8_t)clen;
                        /* reveal keystream (plain^encrypted) */
                        for (int o = 0; o < num_xor; o++)
                        {
                            for (size_t n = 0; n < data_size; n++)
                            {
                                clear[2 + n + o * 32] = (uint8_t)(
                                                                  (clear[2 + n + o * 32] ^ h80211[z + 4 + n])
                                                                  & 0xFF);
                            }
                        }
                        memcpy(clear + 4 + 1 + 1 + 32 * num_xor,
                               weight,
                               16 * sizeof(int));
                        // clear is now the keystream
                    }

                    if (!ivs_log_keystream(options->ivs.fp,
                                           &ap_cur->bssid,
                                           &options->ivs.prev_bssid,
                                           ivs_type,
                                           h80211 + z, 4,
                                           clear,
                                           data_size))
                    {
                        success = false;
                        goto done;
                    }
                }

                uniqueiv_mark(ap_cur->uiv_root, &h80211[z]);

                ap_cur->nb_data++;
            }

            // Record all data linked to IV to detect WEP Cloaking
            if (options->ivs.fp == NULL && options->detect_anomaly)
            {
                // Only allocate this when seeing WEP AP
                if (ap_cur->data_root == NULL)
                {
                    ap_cur->data_root = data_init();
                }

                // Only works with full capture, not IV-only captures
                if (data_check(ap_cur->data_root, &h80211[z], &h80211[z + 4])
                    == CLOAKING
                    && ap_cur->EAP_detected == 0)
                {

                    // If no EAP/EAP was detected, indicate WEP cloaking
                    snprintf(options->message,
                             sizeof(options->message),
                             "WEP Cloaking: %02X:%02X:%02X:%02X:%02X:%02X ",
                             ap_cur->bssid.addr[0],
                             ap_cur->bssid.addr[1],
                             ap_cur->bssid.addr[2],
                             ap_cur->bssid.addr[3],
                             ap_cur->bssid.addr[4],
                             ap_cur->bssid.addr[5]);
                }
            }
        }
        else
        {
            ap_cur->nb_data++;
        }

        z = ((h80211[1] & IEEE80211_FC1_DIR_MASK) != IEEE80211_FC1_DIR_DSTODS)
            ? 24
            : 30;

        /* Check if 802.11e (QoS) */
        if ((h80211[0] & 0x80) == 0x80)
            z += 2;

        if (z + 26 > caplen)
        {
            success = true;
            goto done;
        }

        z += 6; // skip LLC header

        /* check ethertype == EAPOL */
        if (h80211[z] == 0x88 && h80211[z + 1] == 0x8E
            && (h80211[1] & 0x40) != 0x40)
        {
            ap_cur->EAP_detected = 1;

            z += 2; // skip ethertype

            if (st_cur == NULL)
            {
                success = true;
                goto done;
            }

            /* frame 1: Pairwise == 1, Install == 0, Ack == 1, MIC == 0 */

            if ((h80211[z + 6] & 0x08) != 0 && (h80211[z + 6] & 0x40) == 0
                && (h80211[z + 6] & 0x80) != 0
                && (h80211[z + 5] & 0x01) == 0)
            {
                memcpy(st_cur->wpa.anonce, &h80211[z + 17], 32);

                st_cur->wpa.state = 1;

                if (h80211[z + 99] == 0xdd) // RSN
                {
                    if (h80211[z + 101] == 0x00 && h80211[z + 102] == 0x0f
                        && h80211[z + 103] == 0xac) // OUI: IEEE8021
                    {
                        if (h80211[z + 104] == 0x04) // OUI SUBTYPE
                        {
                            // Got a PMKID value?!
                            memcpy(st_cur->wpa.pmkid, &h80211[z + 105], 16);

                            /* copy the key descriptor version */
                            st_cur->wpa.keyver = (uint8_t)(h80211[z + 6] & 7);

                            MAC_ADDRESS_COPY(&st_cur->wpa.stmac, &st_cur->stmac);
                            snprintf(options->message,
                                     sizeof(options->message),
                                     "PMKID found: "
                                     "%02X:%02X:%02X:%02X:%02X:%02X ",
                                     ap_cur->bssid.addr[0],
                                     ap_cur->bssid.addr[1],
                                     ap_cur->bssid.addr[2],
                                     ap_cur->bssid.addr[3],
                                     ap_cur->bssid.addr[4],
                                     ap_cur->bssid.addr[5]);

                            success = true;
                            goto done;
                        }
                    }
                }
            }

            /* frame 2 or 4: Pairwise == 1, Install == 0, Ack == 0, MIC == 1 */

            if (z + 17 + 32 > caplen)
            {
                success = true;

                goto done;
            }

            if ((h80211[z + 6] & 0x08) != 0 && (h80211[z + 6] & 0x40) == 0
                && (h80211[z + 6] & 0x80) == 0
                && (h80211[z + 5] & 0x01) != 0)
            {
                if (memcmp(&h80211[z + 17], ZERO, 32) != 0)
                {
                    memcpy(st_cur->wpa.snonce, &h80211[z + 17], 32);
                    st_cur->wpa.state |= 2;
                }

                if ((st_cur->wpa.state & 4) != 4)
                {
                    st_cur->wpa.eapol_size
                        = (uint32_t)((h80211[z + 2] << 8) + h80211[z + 3] + 4);

                    if (caplen - z < st_cur->wpa.eapol_size
                        || st_cur->wpa.eapol_size == 0 //-V560
                        || caplen - z < 81 + 16
                        || st_cur->wpa.eapol_size > sizeof(st_cur->wpa.eapol))
                    {
                        // Ignore the packet trying to crash us.
                        st_cur->wpa.eapol_size = 0;

                        success = true;

                        goto done;
                    }

                    memcpy(st_cur->wpa.keymic, &h80211[z + 81], 16);
                    memcpy(
                        st_cur->wpa.eapol, &h80211[z], st_cur->wpa.eapol_size);
                    memset(st_cur->wpa.eapol + 81, 0, 16);
                    st_cur->wpa.state |= 4;
                    st_cur->wpa.keyver = (uint8_t)(h80211[z + 6] & 7);
                }
            }

            /* frame 3: Pairwise == 1, Install == 1, Ack == 1, MIC == 1 */

            if ((h80211[z + 6] & 0x08) != 0 && (h80211[z + 6] & 0x40) != 0
                && (h80211[z + 6] & 0x80) != 0
                && (h80211[z + 5] & 0x01) != 0)
            {
                if (memcmp(&h80211[z + 17], ZERO, 32) != 0)
                {
                    memcpy(st_cur->wpa.anonce, &h80211[z + 17], 32);
                    st_cur->wpa.state |= 1;
                }

                if ((st_cur->wpa.state & 4) != 4)
                {
                    st_cur->wpa.eapol_size
                        = (h80211[z + 2] << 8) + h80211[z + 3] + 4u;

                    if (st_cur->wpa.eapol_size == 0 //-V560
                        || st_cur->wpa.eapol_size
                        >= sizeof(st_cur->wpa.eapol) - 16)
                    {
                        // Ignore the packet trying to crash us.
                        st_cur->wpa.eapol_size = 0;
                        success = true;
                        goto done;
                    }

                    memcpy(st_cur->wpa.keymic, &h80211[z + 81], 16);
                    memcpy(
                        st_cur->wpa.eapol, &h80211[z], st_cur->wpa.eapol_size);
                    memset(st_cur->wpa.eapol + 81, 0, 16);
                    st_cur->wpa.state |= 4;
                    st_cur->wpa.keyver = (uint8_t)(h80211[z + 6] & 7);
                }
            }

            if (st_cur->wpa.state == 7
                && !is_filtered_essid(&options->essid_filter, ap_cur->essid))
            {
                MAC_ADDRESS_COPY(&st_cur->wpa.stmac, &st_cur->stmac);
                snprintf(options->message,
                         sizeof(options->message),
                         "WPA handshake: %02X:%02X:%02X:%02X:%02X:%02X ",
                         ap_cur->bssid.addr[0],
                         ap_cur->bssid.addr[1],
                         ap_cur->bssid.addr[2],
                         ap_cur->bssid.addr[3],
                         ap_cur->bssid.addr[4],
                         ap_cur->bssid.addr[5]);

                if (!ivs_log_wpa_hdsk(options->ivs.fp,
                                      &ap_cur->bssid,
                                      &options->ivs.prev_bssid,
                                      &st_cur->wpa,
                                      sizeof st_cur->wpa))
                {
                    success = false;
                    goto done;
                }
            }
        }
    }

    success = true;

done:
    return success;
}

static bool parse_control_frame(
    struct local_options * const options, 
    uint8_t const * const h80211, 
    size_t const caplen,
    struct rx_info * const ri)
{
    bool success;

    if (caplen < 24 && caplen >= 10 && h80211[0] != 0)
    {
        /* RTS || CTS || ACK || CF-END || CF-END&CF-ACK*/
        //(h80211[0] == 0xB4 || h80211[0] == 0xC4 || h80211[0] == 0xD4 ||
        // h80211[0] == 0xE4 || h80211[0] == 0xF4)

        /* use general control frame detection, as the structure is always the
         * same: mac(s) starting at [4] */
        if ((h80211[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_CTL)
        {
            uint8_t const * const data_end = h80211 + caplen;
            mac_address namac;
            uint8_t const * p = h80211 + 4;

			while ((uintptr_t) p <= adds_uptr((uintptr_t) h80211, 16) && (p + sizeof namac) <= data_end)
            {
                MAC_ADDRESS_COPY(&namac, (mac_address *)p);

                if (MAC_ADDRESS_IS_EMPTY(&namac))
                {
                    p += sizeof namac;
                    continue;
                }

                if (MAC_ADDRESS_IS_BROADCAST(&namac))
                {
                    p += sizeof namac;
                    continue;
                }

                if (options->hide_known)
                {
                    /* Check AP list. */
                    /* If it's an AP, try next mac */
                    if (ap_info_lookup_existing(&options->ap_list, &namac))
                    {
                        p += sizeof namac;
                        continue;
                    }

                    /* If it's a client, try next mac */
                    if (sta_info_lookup_existing(&options->sta_list, &namac))
                    {
                        p += sizeof namac;
                        continue;
                    }
                }

                /* Not found in either AP list or ST list. 
                 * Find or create an entry in the NA list.
                 */
                struct NA_info * const na_cur =
                    na_info_lookup(&options->na_list, &namac);

                if (na_cur == NULL)
                {
                    success = false;
                    goto done;
                }

                /* update the last time seen & power*/
                na_cur->tlast = time(NULL);
                na_cur->power = ri->ri_power;
                na_cur->channel = ri->ri_channel;

                switch (h80211[0] & IEEE80211_FC0_SUBTYPE_MASK)
                {
                    case IEEE80211_FC0_SUBTYPE_RTS:
                        if (p == h80211 + 4)
                        {
                            na_cur->rts_r++;
                        }
                        else if (p == h80211 + 10)
                        {
                            na_cur->rts_t++;
                        }
                        break;

                    case IEEE80211_FC0_SUBTYPE_CTS:
                        na_cur->cts++;
                        break;

                    case IEEE80211_FC0_SUBTYPE_ACK:
                        na_cur->ack++;
                        break;

                    default:
                        na_cur->other++;
                        break;
                }

                /* grab next mac (for rts frames)*/
                p += sizeof namac;
            }
        }
    }

    success = true;

done:
    return success;
}

// NOTE(jbenden): This is also in ivstools.c
static void dump_add_packet(
	unsigned char const * const h80211,
	size_t const caplen,
	struct rx_info * const ri,
	int const cardnum)
{
	REQUIRE(h80211 != NULL);
    mac_address const * bssid;
	mac_address const * stmac;

	struct AP_info * ap_cur = NULL;
	struct ST_info * st_cur = NULL;

	/* skip all non probe response frames in active scanning simulation mode */
    bool const is_probe_response = 
        h80211[0] == IEEE80211_FC0_SUBTYPE_PROBE_RESP;

    if (lopt.active_scan_sim > 0 && !is_probe_response)
	{
		return;
	}

	/* Skip packets smaller than a 802.11 header. */
	if (caplen < sizeof(struct ieee80211_frame))
	{
		goto write_packet;
	}

    /* Skip (uninteresting) control frames. */
    bool const is_control_frame =
        (h80211[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_CTL;

    if (is_control_frame)
	{
		goto write_packet;
	}

	/* if it's a LLC null packet, just forget it (may change in the future) */

	if (caplen > 28)
	{
		static const unsigned char llcnull[] = { 0, 0, 0, 0 };

        if (memcmp(h80211 + 24, llcnull, sizeof llcnull) == 0)
		{
			return;
		}
	}

    bssid = locate_bssid_in_80211(h80211);

    if (bssid_is_filtered(bssid, &lopt.f_bssid, &lopt.f_netmask))
    {
        return; /* FIXME - single exit. */
    }

    bool is_new_ap;

    ap_cur = ap_info_lookup(&lopt, bssid, &is_new_ap);
    if (ap_cur == NULL)
    {
        return;
    }

    if (is_new_ap)
	{
        /* If mac was listed as unknown, remove it. */
        remove_namac(&lopt.na_list, bssid);
	}

    ap_update(&lopt, ap_cur, h80211, ri);

    /* Locate the station MAC in the 802.11 header. */
    stmac = locate_sta_mac_in_80211(h80211, bssid);

    if (stmac != NULL)
    {
        /* Update the list of wireless stations */
        bool is_new;

        st_cur = sta_info_lookup(&lopt, stmac, &is_new);
        if (st_cur == NULL)
        {
            return;
        }

        if (is_new)
        {
            /* If mac was listed as unknown, remove it. */
            remove_namac(&lopt.na_list, stmac);
        }

        sta_update(&lopt, st_cur, ap_cur, h80211, ri, cardnum);
    }

	/* packet parsing: Probe Request */
    parse_probe_request(st_cur, h80211, caplen);

    /* packet parsing: Beacon or Probe Response */
    if (!parse_beacon_or_probe_response(&lopt, ap_cur, h80211, caplen))
    {
        return;
    }

	/* packet parsing: Beacon & Probe response */
    parse_beacon_or_probe_response_2(ap_cur, h80211, caplen);

	/* packet parsing: Authentication Response */
    parse_authentication_response(ap_cur, h80211, caplen);

	/* packet parsing: Association Request */
    if (!parse_association_request(&lopt, ap_cur, st_cur, h80211, caplen))
    {
        return;
    }

	/* packet parsing: some data */
    if (!parse_packet_data(&lopt, ap_cur, st_cur, h80211, caplen, ri, cardnum))
    {
        return;
    }

write_packet:

    if (ap_cur != NULL)
	{
        if (lopt.one_beacon)
        {
            bool const is_a_beacon =
                (h80211[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT
                && (h80211[0] & IEEE80211_FC0_SUBTYPE_MASK) == IEEE80211_FC0_SUBTYPE_BEACON;

            if (is_a_beacon)
            {
                if (!ap_cur->beacon_logged)
                {
                    ap_cur->beacon_logged = 1;
                }
                else
                {
                    return;
                }
            }
        }

        if (!ap_has_required_security(ap_cur->security, lopt.encryption_filter))
        {
            return;
        }

        if (is_filtered_essid(&lopt.essid_filter, ap_cur->essid))
        {
            return;
        }
    }

    if (lopt.record_data && !lopt.no_shared_key)
	{
        if ((h80211[0] & IEEE80211_FC0_TYPE_MASK) == IEEE80211_FC0_TYPE_MGT
            && (h80211[0] & IEEE80211_FC0_SUBTYPE_MASK) == IEEE80211_FC0_SUBTYPE_AUTH)
		{
			/* authentication packet */
            check_shared_key(&lopt.shared_key, 
                             h80211, 
                             caplen, 
                             lopt.dump_prefix, 
                             lopt.f_index, 
                             true);
		}
	}

    if (!parse_control_frame(&lopt, h80211, caplen, ri))
    {
        return;
    }

    update_packet_capture_files(&lopt, h80211, caplen, ri->ri_power);
}

static bool ap_should_not_be_printed(
    struct local_options const * const options, 
    struct AP_info const * const ap_cur)
{
	bool should_skip;

	REQUIRE(ap_cur != NULL);

	if (ap_cur->nb_pkt < options->min_pkts
        || (time(NULL) - ap_cur->tlast) > options->berlin
		|| MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid))
	{
		should_skip = true;
		goto done;
	}

    if (!ap_has_required_security(ap_cur->security, lopt.encryption_filter))
    {
        should_skip = true;
        goto done;
    }

	if (is_filtered_essid(&options->essid_filter, ap_cur->essid))
	{
		should_skip = true;
		goto done;
	}

	should_skip = false;

done:
	return should_skip;
}

#define CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, where)              \
	do                                                                         \
	{                                                                          \
		if (nlines >= (screen_height - 1))                                     \
		{                                                                      \
            goto where;                                                        \
		}                                                                      \
	} while (0)

static void dump_print(
    int const screen_height, 
    int const screen_width, 
    int const if_num)
{
	time_t tt;
	struct tm * lt;
    int nlines; 
    int i; 
	char strbuf[1024];
	char buffer[1024];
	char ssid_list[512];
	struct AP_info * ap_cur;
	struct ST_info * st_cur;
	struct NA_info * na_cur;
	int columns_ap = 83;
	int columns_sta = 74;
	int columns_na = 68;
	size_t len;
    int numaps = 0; /* Only used when 'berlin' mode is active. */

    if (!lopt.singlechan)
	{
		columns_ap -= 4; // no RXQ in scan mode
	}

	if (lopt.show_uptime)
	{
		columns_ap += 15; // show uptime needs more space
	}

	nlines = 2;

    if (nlines >= screen_height)
    {
        return;
    }

	tt = time(NULL);
	lt = localtime(&tt);

	if (lopt.is_berlin)
	{
        lopt.maxaps = 0;

		TAILQ_FOREACH_REVERSE(ap_cur, &lopt.ap_list, ap_list_head, entry)
		{
			lopt.maxaps++;
			if (ap_cur->nb_pkt < 2
                || (time(NULL) - ap_cur->tlast) > lopt.berlin
				|| MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid))
			{
				continue;
			}
			numaps++;
		}

		if (numaps > lopt.maxnumaps)
		{
			lopt.maxnumaps = numaps;
		}
	}

	/*
	 *  display the channel, battery, position (if we are connected to GPSd)
	 *  and current time
	 */

    terminal_move_cursor_to(1, 2);
	textcolor_normal();
	textcolor_fg(TEXT_WHITE);

	if (lopt.freqoption)
	{
		snprintf(strbuf, sizeof(strbuf) - 1, " Freq %4d", lopt.frequency[0]);
		for (i = 1; i < if_num; i++)
		{
			snprintf(buffer, sizeof(buffer), ",%4d", lopt.frequency[i]);
			strlcat(strbuf, buffer, sizeof(strbuf));
		}
	}
	else /* Must be channel option. */
	{
		snprintf(strbuf, sizeof(strbuf) - 1, " CH %2d", lopt.channel[0]);
		for (i = 1; i < if_num; i++)
		{
			memset(buffer, '\0', sizeof(buffer));
			snprintf(buffer, sizeof(buffer) - 1, ",%2d", lopt.channel[i]);
			strlcat(strbuf, buffer, sizeof(strbuf));
		}
	}

    buffer[0] = '\0';

    if (lopt.gpsd.required)
	{
		// If using GPS then check if we have a valid fix or not and report accordingly
		if (lopt.gps_context.gps_loc[0] != 0.0f)
		{
			struct tm * gtime = &lopt.gps_context.gps_time;

			snprintf(buffer,
					 sizeof(buffer) - 1,
					 " %s[ GPS %3.6f,%3.6f %02d:%02d:%02d ][ Elapsed: %s ][ "
					 "%04d-%02d-%02d %02d:%02d ",
					 lopt.gps_context.batt,
					 lopt.gps_context.gps_loc[0],
					 lopt.gps_context.gps_loc[1],
					 gtime->tm_hour,
					 gtime->tm_min,
					 gtime->tm_sec,
					 lopt.elapsed_time,
					 1900 + lt->tm_year,
					 1 + lt->tm_mon,
					 lt->tm_mday,
					 lt->tm_hour,
					 lt->tm_min);
		}
		else
		{
			snprintf(
				buffer,
				sizeof(buffer) - 1,
				" %s[ GPS %-29s ][ Elapsed: %s ][ %04d-%02d-%02d %02d:%02d ",
				lopt.gps_context.batt,
				" *** No Fix! ***",
				lopt.elapsed_time,
				1900 + lt->tm_year,
				1 + lt->tm_mon,
				lt->tm_mday,
				lt->tm_hour,
				lt->tm_min);
		}
	}
	else
	{
		snprintf(buffer,
				 sizeof(buffer) - 1,
				 "][ Elapsed: %s ][ %04d-%02d-%02d %02d:%02d ",
				 lopt.elapsed_time,
				 1900 + lt->tm_year,
				 1 + lt->tm_mon,
				 lt->tm_mday,
				 lt->tm_hour,
				 lt->tm_min);
	}

	strlcat(strbuf, buffer, sizeof(strbuf));
	memset(buffer, '\0', sizeof(buffer));

	if (lopt.is_berlin)
	{
		snprintf(buffer,
				 sizeof(buffer) - 1,
				 " ][%3d/%3d/%4d ",
				 numaps,
				 lopt.maxnumaps,
				 lopt.maxaps);
	}

	strlcat(strbuf, buffer, sizeof(strbuf));
	memset(buffer, '\0', sizeof(buffer));

    static char const message_field_end[] = "]";
    static char const message_field_start[] = "[ ";

    strncat(strbuf, message_field_end, sizeof strbuf - strlen(strbuf) - 1);
    if (strlen(lopt.message) > 0)
	{
        strlcat(strbuf, message_field_start, sizeof strbuf - strlen(strbuf) - 1);
		strlcat(strbuf, lopt.message, sizeof strbuf - strlen(strbuf) - 1);
	}

	strbuf[screen_width - 1] = '\0';

	ALLEGE(strchr(strbuf, '\n') == NULL);

    console_puts(strbuf);
    nlines++;
    CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done);

	/* print some information about each detected AP */

    terminal_clear_line_from_cursor_right();
    terminal_move_cursor_down(1);
    nlines++;
    CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

	if (lopt.show_ap)
	{
		strbuf[0] = 0;
		strlcat(strbuf, " BSSID              PWR ", sizeof(strbuf));

		if (lopt.singlechan)
		{
			strlcat(strbuf, "RXQ ", sizeof(strbuf));
		}

		strlcat(strbuf,
				" Beacons    #Data, #/s  CH   MB   ENC CIPHER  AUTH ",
				sizeof(strbuf));

		if (lopt.show_uptime)
		{
			strlcat(strbuf, "        UPTIME ", sizeof(strbuf));
		}

		if (lopt.show_wps)
		{
			strlcat(strbuf, "WPS   ", sizeof(strbuf));
			if (screen_width > (columns_ap - 4))
			{
				memset(strbuf + strlen(strbuf),
					   ' ',
					   sizeof(strbuf) - strlen(strbuf) - 1);
				snprintf(strbuf + columns_ap + lopt.maxsize_wps_seen - 5,
						 8,
						 "%s",
						 "  ESSID");
				if (lopt.show_manufacturer)
				{
					memset(strbuf + columns_ap + lopt.maxsize_wps_seen + 1,
						   ' ',
						   sizeof(strbuf) - columns_ap - lopt.maxsize_wps_seen
							   - 1);
					snprintf(strbuf + columns_ap + lopt.maxsize_wps_seen
								 + lopt.maxsize_essid_seen
								 - 4,
							 15,
							 "%s",
							 "MANUFACTURER");
				}
			}
		}
		else
		{
			strlcat(strbuf, "ESSID", sizeof(strbuf));

			if (lopt.show_manufacturer && (screen_width > (columns_ap - 4)))
			{
				// write spaces (32).
				memset(strbuf + columns_ap, ' ', lopt.maxsize_essid_seen - 5);
				snprintf(strbuf + columns_ap + lopt.maxsize_essid_seen - 7,
						 15,
						 "%s",
						 "  MANUFACTURER");
			}
		}
		strbuf[screen_width - 1] = '\0';
		console_puts(strbuf);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

        terminal_clear_line_from_cursor_right();
        terminal_move_cursor_down(1);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

		TAILQ_FOREACH_REVERSE(ap_cur, &lopt.ap_list, ap_list_head, entry)
		{
			/* skip APs with only one packet, or those older than 2 min.
			 * always skip if bssid == broadcast*
			 */
            if (ap_should_not_be_printed(&lopt, ap_cur))
			{
				if (lopt.p_selected_ap == ap_cur)
				{ //the selected AP is skipped (will not be printed), we have to go to the next printable AP
					struct AP_info * ap_tmp;

					if (selection_direction_up == lopt.en_selection_direction)
					{
						//UP arrow was last pressed
						ap_tmp = TAILQ_NEXT(ap_cur, entry);
						if (ap_tmp != NULL)
						{
							while ((NULL != (lopt.p_selected_ap = ap_tmp))
                                   && ap_should_not_be_printed(&lopt, ap_tmp))
							{
								ap_tmp = TAILQ_NEXT(ap_tmp, entry);
							}
						}
						if (ap_tmp == NULL) //we have reached the first element in the list, so go in another direction
						{ //upon we have an AP that is not skipped
							ap_tmp = TAILQ_PREV(ap_cur, ap_list_head, entry);
							if (ap_tmp != NULL)
							{
								while ((NULL != (lopt.p_selected_ap = ap_tmp))
                                       && ap_should_not_be_printed(&lopt, ap_tmp))
								{
									ap_tmp = TAILQ_PREV(ap_tmp, ap_list_head, entry);
                                }
							}
						}
					}
					else if (selection_direction_down == lopt.en_selection_direction)
					{
						//DOWN arrow was last pressed
						ap_tmp = TAILQ_PREV(ap_cur, ap_list_head, entry);
						if (ap_tmp != NULL)
						{
							while ((NULL != (lopt.p_selected_ap = ap_tmp))
                                   && ap_should_not_be_printed(&lopt, ap_tmp))
							{
								ap_tmp = TAILQ_PREV(ap_tmp, ap_list_head, entry);
							}
						}
						if (ap_tmp == NULL) //we have reached the last element in the list, so go in another direction
						{ //upon we have an AP that is not skipped
							ap_tmp = TAILQ_NEXT(ap_cur, entry);
							if (ap_tmp != NULL)
							{
								while ((NULL != (lopt.p_selected_ap = ap_tmp))
                                       && ap_should_not_be_printed(&lopt, ap_tmp))
									ap_tmp = TAILQ_NEXT(ap_tmp, entry);
							}
						}
					}
				}

				continue;
			}

			nlines++;
            CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done);

			snprintf(strbuf,
					 sizeof(strbuf),
					 " %02X:%02X:%02X:%02X:%02X:%02X",
					 ap_cur->bssid.addr[0],
                     ap_cur->bssid.addr[1],
                     ap_cur->bssid.addr[2],
                     ap_cur->bssid.addr[3],
                     ap_cur->bssid.addr[4],
                     ap_cur->bssid.addr[5]);

			len = strlen(strbuf);

			if (lopt.singlechan)
			{
				snprintf(strbuf + len,
						 sizeof(strbuf) - len,
						 "  %3d %3d %8lu %8lu %4d",
						 ap_cur->avg_power,
						 ap_cur->rx_quality,
						 ap_cur->nb_bcn,
						 ap_cur->nb_data,
						 ap_cur->nb_dataps);
			}
			else
			{
				snprintf(strbuf + len,
						 sizeof(strbuf) - len,
						 "  %3d %8lu %8lu %4d",
						 ap_cur->avg_power,
						 ap_cur->nb_bcn,
						 ap_cur->nb_data,
						 ap_cur->nb_dataps);
			}

			len = strlen(strbuf);

			if (ap_cur->standard[0] != '\0')
			{
				// In case of 802.11n or 802.11ac, QoS is pretty much implied
				// Short or long preamble is not that useful anymore.
				snprintf(strbuf + len,
						 sizeof(strbuf) - len,
						 " %3d %4d   ",
						 ap_cur->channel,
						 ap_cur->max_speed);
			}
			else
			{
				snprintf(strbuf + len,
						 sizeof(strbuf) - len,
						 " %3d %4d%c%c ",
						 ap_cur->channel,
						 ap_cur->max_speed,
						 (ap_cur->security & STD_QOS) ? 'e' : ' ',
						 (ap_cur->preamble) ? '.' : ' ');
			}

			len = strlen(strbuf);

			if ((ap_cur->security & (STD_FIELD | AUTH_SAE | AUTH_OWE)) == 0)
            {
				snprintf(strbuf + len, sizeof(strbuf) - len, "    ");
            }
			else
			{
				if (ap_cur->security & STD_WPA2)
				{
                    if (ap_cur->security & AUTH_SAE
                        || ap_cur->security & AUTH_OWE)
                    {
						snprintf(strbuf + len, sizeof(strbuf) - len, "WPA3");
                    }
                    else
                    {
						snprintf(strbuf + len, sizeof(strbuf) - len, "WPA2");
                    }
				}
                else if (ap_cur->security & STD_WPA)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "WPA ");
                }
                else if (ap_cur->security & STD_WEP)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "WEP ");
                }
                else if (ap_cur->security & STD_OPN)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "OPN ");
                }
			}

			strlcat(strbuf, " ", sizeof(strbuf));

			len = strlen(strbuf);

            if ((ap_cur->security & ENC_FIELD) == 0)
            {
				snprintf(strbuf + len, sizeof(strbuf) - len, "       ");
            }
			else
			{
				if (ap_cur->security & ENC_CCMP)
					snprintf(strbuf + len, sizeof(strbuf) - len, "CCMP   ");
				else if (ap_cur->security & ENC_WRAP)
					snprintf(strbuf + len, sizeof(strbuf) - len, "WRAP   ");
				else if (ap_cur->security & ENC_TKIP)
					snprintf(strbuf + len, sizeof(strbuf) - len, "TKIP   ");
				else if (ap_cur->security & ENC_WEP104)
					snprintf(strbuf + len, sizeof(strbuf) - len, "WEP104 ");
				else if (ap_cur->security & ENC_WEP40)
					snprintf(strbuf + len, sizeof(strbuf) - len, "WEP40  ");
				else if (ap_cur->security & ENC_WEP)
					snprintf(strbuf + len, sizeof(strbuf) - len, "WEP    ");
			}

			len = strlen(strbuf);

            if ((ap_cur->security & AUTH_FIELD) == 0)
            {
				snprintf(strbuf + len, sizeof(strbuf) - len, "    ");
            }
			else
			{
                if (ap_cur->security & AUTH_SAE)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "SAE ");
                }
                else if (ap_cur->security & AUTH_MGT)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "MGT ");
                }
                else if (ap_cur->security & AUTH_CMAC)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "CMAC");
                }
				else if (ap_cur->security & AUTH_PSK)
				{
                    if (ap_cur->security & STD_WEP)
                    {
						snprintf(strbuf + len, sizeof(strbuf) - len, "SKA ");
                    }
                    else
                    {
						snprintf(strbuf + len, sizeof(strbuf) - len, "PSK ");
                    }
				}
                else if (ap_cur->security & AUTH_OWE)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "OWE ");
                }
                else if (ap_cur->security & AUTH_OPN)
                {
					snprintf(strbuf + len, sizeof(strbuf) - len, "OPN ");
                }
			}

			len = strlen(strbuf);

			if (lopt.show_uptime)
			{
				snprintf(strbuf + len,
						 sizeof(strbuf) - len,
						 " %14s",
						 parse_timestamp(ap_cur->timestamp));
				len = strlen(strbuf);
			}

			if (lopt.p_selected_ap != NULL && lopt.p_selected_ap == ap_cur)
			{
				if (lopt.mark_cur_ap)
				{
					if (ap_cur->marked == 0)
					{
						ap_cur->marked = 1;
					}
					else
					{
						ap_cur->marked_color++;
						if (ap_cur->marked_color > TEXT_MAX_COLOR)
						{
							ap_cur->marked_color = 1;
							ap_cur->marked = 0;
						}
					}
					lopt.mark_cur_ap = 0;
				}
				textstyle(TEXT_REVERSE);
				MAC_ADDRESS_COPY(&lopt.selected_bssid, &ap_cur->bssid);
			}

			if (ap_cur->marked)
			{
				textcolor_fg(ap_cur->marked_color);
			}

			memset(strbuf + len, ' ', sizeof(strbuf) - len - 1);

			if (screen_width > (columns_ap - 4))
			{
				if (lopt.show_wps)
				{
					size_t wps_len = len;

					if (ap_cur->wps.state != 0xFF)
					{
						if (ap_cur->wps.ap_setup_locked) // AP setup locked
							snprintf(
								strbuf + len, sizeof(strbuf) - len, "Locked");
						else
						{
							snprintf(strbuf + len,
									 sizeof(strbuf) - len,
									 " %u.%d",
									 ap_cur->wps.version >> 4,
									 ap_cur->wps.version & 0xF); // Version
							len = strlen(strbuf);
							if (ap_cur->wps.meth) // WPS Config Methods
							{
								char tbuf[64];
								tbuf[0] = '\0';
								int sep = 0;
#define T(bit, name)                                                           \
	do                                                                         \
	{                                                                          \
		if (ap_cur->wps.meth & (1u << (bit)))                                  \
		{                                                                      \
			if (sep) strlcat(tbuf, ",", sizeof(tbuf));                         \
			sep = 1;                                                           \
			strlcat(tbuf, (name), sizeof(tbuf));                               \
		}                                                                      \
	} while (0)
								T(0u, "USB"); // USB method
								T(1u, "ETHER"); // Ethernet
								T(2u, "LAB"); // Label
								T(3u, "DISP"); // Display
								T(4u, "EXTNFC"); // Ext. NFC Token
								T(5u, "INTNFC"); // Int. NFC Token
								T(6u, "NFCINTF"); // NFC Interface
								T(7u, "PBC"); // Push Button
								T(8u, "KPAD"); // Keypad
								snprintf(strbuf + len,
										 sizeof(strbuf) - len,
										 " %s",
										 tbuf);
#undef T
							}
						}
					}
					else
					{
						snprintf(strbuf + len, sizeof(strbuf) - len, " ");
					}
					len = strlen(strbuf);

					if (lopt.maxsize_wps_seen <= len - wps_len)
					{
						lopt.maxsize_wps_seen = MAX(len - wps_len, 6);
					}
					else
					{
						// pad output
						memset(strbuf + len, ' ', sizeof(strbuf) - len - 1);
						len += lopt.maxsize_wps_seen - len - wps_len;
						strbuf[len] = '\0';
					}
				}

				ssize_t essid_len = len;

				if (ap_cur->essid[0] != '\0')
				{
					if (lopt.show_wps)
						snprintf(strbuf + len,
								 sizeof(strbuf) - len - 1,
								 "  %s",
								 ap_cur->essid);
					else
						snprintf(strbuf + len,
								 sizeof(strbuf) - len,
								 " %s",
								 ap_cur->essid);
				}
				else
				{
					if (lopt.show_wps)
						snprintf(strbuf + len,
								 sizeof(strbuf) - len - 1,
								 "  <length:%3zd>%s",
								 ap_cur->ssid_length,
								 "\x00");
					else
						snprintf(strbuf + len,
								 sizeof(strbuf) - len,
								 " <length:%3zd>%s",
								 ap_cur->ssid_length,
								 "\x00");
				}
				len = strlen(strbuf);

				if (lopt.show_manufacturer)
				{
					if (lopt.maxsize_essid_seen <= (u_int)(len - essid_len))
						lopt.maxsize_essid_seen
							= (u_int) MAX(len - essid_len, 5);
					else
					{
						// pad output
						memset(strbuf + len, ' ', sizeof(strbuf) - len - 1);
						len += lopt.maxsize_essid_seen - (len - essid_len);
						strbuf[len] = '\0';
					}

					if (ap_cur->manuf == NULL)
					{
						ap_cur->manuf =
                            get_manufacturer_by_oui(
                                lopt.manufacturer_list,
                                ap_cur->bssid.addr);
                    }

					snprintf(strbuf + len,
							 sizeof(strbuf) - len - 1,
							 " %s",
							 ap_cur->manuf);
				}
			}

			len = strlen(strbuf);

			// write spaces (32) until the end of column
			int len_remaining = screen_width - len;
			if (len_remaining > 0)
			{
				ALLEGE((size_t) len + len_remaining <= sizeof(strbuf));
				memset(strbuf + len, ' ', len_remaining);
			}

			strbuf[screen_width - 1] = '\0';
			console_puts(strbuf);

			if ((lopt.p_selected_ap != NULL && lopt.p_selected_ap == ap_cur)
				|| ap_cur->marked)
			{
				textstyle(TEXT_RESET);
			}
		}

		/* print some information about each detected station */

        terminal_clear_line_from_cursor_right();
        terminal_move_cursor_down(1);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done);
    }

	if (lopt.show_sta)
	{
		strlcpy(strbuf,
				" BSSID              STATION "
				"           PWR   Rate    Lost    Frames  Notes  Probes",
				sizeof(strbuf));
		strbuf[screen_width - 1] = '\0';
		console_puts(strbuf);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

        terminal_clear_line_from_cursor_right();
        terminal_move_cursor_down(1);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

		TAILQ_FOREACH_REVERSE(ap_cur, &lopt.ap_list, ap_list_head, entry)
		{
			if (ap_cur->nb_pkt < 2
				|| (time(NULL) - ap_cur->tlast) > lopt.berlin)
			{
				continue;
			}

            if (!ap_has_required_security(ap_cur->security, lopt.encryption_filter))
            {
                continue;
            }

			// Don't filter unassociated clients by ESSID
			if (!MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid)
				&& is_filtered_essid(&lopt.essid_filter, ap_cur->essid))
			{
				continue;
			}

            CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done);

			if (lopt.p_selected_ap != NULL
				&& MAC_ADDRESS_EQUAL(&lopt.selected_bssid, &ap_cur->bssid))
			{
				textstyle(TEXT_REVERSE);
			}

			if (ap_cur->marked)
			{
				textcolor_fg(ap_cur->marked_color);
			}

			TAILQ_FOREACH_REVERSE(st_cur, &lopt.sta_list, sta_list_head, entry)
			{
				if (st_cur->base != ap_cur
					|| (time(NULL) - st_cur->tlast) > lopt.berlin)
				{
					continue;
				}

				if (MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid)
                    && lopt.asso_client)
				{
					continue;
				}

				nlines++;
                CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

                if (MAC_ADDRESS_IS_BROADCAST(&ap_cur->bssid))
                {
					printf(" (not associated) ");
                }
                else
                {
					printf(" %02X:%02X:%02X:%02X:%02X:%02X",
                           ap_cur->bssid.addr[0],
                           ap_cur->bssid.addr[1],
                           ap_cur->bssid.addr[2],
                           ap_cur->bssid.addr[3],
                           ap_cur->bssid.addr[4],
                           ap_cur->bssid.addr[5]);
                }

				printf("  %02X:%02X:%02X:%02X:%02X:%02X",
					   st_cur->stmac.addr[0],
					   st_cur->stmac.addr[1],
					   st_cur->stmac.addr[2],
					   st_cur->stmac.addr[3],
					   st_cur->stmac.addr[4],
					   st_cur->stmac.addr[5]);

				printf("  %3d ", st_cur->power);
				printf("  %2d", st_cur->rate_to / 1000000);
				printf("%c", (st_cur->qos_fr_ds) ? 'e' : ' ');
				printf("-%2d", st_cur->rate_from / 1000000);
				printf("%c", (st_cur->qos_to_ds) ? 'e' : ' ');
				printf("  %4d", st_cur->missed);
				printf(" %8lu", st_cur->nb_pkt);
				printf("  %-5s",
					   (st_cur->wpa.pmkid[0] != 0)
						   ? "PMKID"
						   : (st_cur->wpa.state == 7 ? "EAPOL" : ""));

				if (screen_width > (columns_sta - 6))
				{
                    size_t n;
                    size_t i;

					memset(ssid_list, 0, sizeof(ssid_list));

					for (i = 0, n = 0; i < NB_PRB; i++)
					{
                        if (st_cur->probes[i][0] == '\0')
                        {
                            continue;
                        }

						snprintf(ssid_list + n,
								 sizeof(ssid_list) - n - 1,
								 "%c%s",
								 (i > 0) ? ',' : ' ',
								 st_cur->probes[i]);

						n += (1 + strlen(st_cur->probes[i]));

                        if (n >= sizeof(ssid_list))
                        {
                            break;
                        }
					}

					snprintf(strbuf, sizeof(strbuf) - 1, "%-256s", ssid_list)
							< 0
						? abort()
						: (void) 0;
					strbuf[MAX(screen_width - 75, 0)] = '\0';
					printf(" %s", strbuf);
				}

                terminal_clear_line_from_cursor_right();
                putchar('\n');
			}

			if ((lopt.p_selected_ap != NULL
				 && MAC_ADDRESS_EQUAL(&lopt.selected_bssid, &ap_cur->bssid))
				|| ap_cur->marked)
			{
				textstyle(TEXT_RESET);
			}
		}
	}

	if (lopt.show_ack)
	{
		/* print some information about each unknown station */

        terminal_clear_line_from_cursor_right();
        terminal_move_cursor_down(1);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

		strlcpy(strbuf,
				" MAC       "
				"          CH PWR    ACK ACK/s    CTS RTS_RX RTS_TX  OTHER",
				sizeof(strbuf));
		strbuf[screen_width - 1] = '\0';
		console_puts(strbuf);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

		memset(strbuf, ' ', (size_t) screen_width - 1);
		strbuf[screen_width - 1] = '\0';
		console_puts(strbuf);
        nlines++;
        CHECK_END_OF_SCREEN_OR_GOTO(nlines, screen_height, done); 

		TAILQ_FOREACH(na_cur, &lopt.na_list, entry)
		{
			if (time(NULL) - na_cur->tlast > 120)
			{
				continue;
			}

			nlines++;

			if (nlines >= (screen_height - 1))
			{
				return;
			}

			printf(" %02X:%02X:%02X:%02X:%02X:%02X",
				   na_cur->namac.addr[0],
				   na_cur->namac.addr[1],
				   na_cur->namac.addr[2],
				   na_cur->namac.addr[3],
				   na_cur->namac.addr[4],
				   na_cur->namac.addr[5]);

			printf("  %3d", na_cur->channel);
			printf(" %3d", na_cur->power);
			printf(" %6d", na_cur->ack);
			printf("  %4d", na_cur->ackps);
			printf(" %6d", na_cur->cts);
			printf(" %6d", na_cur->rts_r);
			printf(" %6d", na_cur->rts_t);
			printf(" %6d", na_cur->other);

            terminal_clear_line_from_cursor_right();
            putchar('\n');
		}
	}

done:
    terminal_clear_to_end_of_screen();
}

static void
sigchld_handler(int signum)
{
    (void)signum;
    /* Reap zombie processes. */
    pid_t pid;
    int const status = wait_proc(-1, &pid);
    (void)status;
}

static void channel_hopper_data_handler(
    struct local_options * const options,
    struct channel_hopper_data_st const * const hopper_data)
{
    if (hopper_data->card >= ArrayCount(options->frequency))
    {
        // invalid received data
        fprintf(stderr,
                "Invalid card value received from hopper process, got %zd\n",
                hopper_data->card);
        goto done;
    }

    if (options->freqoption)
    {
        options->frequency[hopper_data->card] = hopper_data->u.frequency;
    }
    else
    {
        options->channel[hopper_data->card] = hopper_data->u.channel;
    }

done:
    return;
}

static void update_window_size(struct winsize * const window_size)
{
    if (ioctl(0, TIOCGWINSZ, window_size) < 0)
    {
        static unsigned short int const default_windows_rows = 25;
        static unsigned short int const default_windows_cols = 80;

        window_size->ws_row = default_windows_rows;
        window_size->ws_col = default_windows_cols;
    }
}

static void handle_window_changed_event(void)
{
    terminal_clear_to_end_of_screen();
}

static void handle_terminate_event(struct local_options * const options)
{
    if (options->interactive_mode > 0)
	{
		fprintf(stdout, "Quitting...\n");
        fflush(stdout);
	}

    options->do_exit = 1;
}

typedef enum
{
    signal_event_window_changed,
    signal_event_terminate
} signal_event_t;

static void send_event(int const fd, int const event)
{
    if (fd != -1)
    {
        IGNORE_LTZ(write(fd, &event, sizeof event));
    }
}

static void sighandler(int signum)
{
    int const pipe_fd = lopt.signal_event_pipe[1];

	if (signum == SIGINT || signum == SIGTERM)
	{
        send_event(pipe_fd, signal_event_terminate);
    }
    else if (signum == SIGWINCH)
    {
        send_event(pipe_fd, signal_event_window_changed);
    }
    else if (signum == SIGSEGV)
	{
		fprintf(stderr,
				"Caught signal 11 (SIGSEGV). Please"
				" contact the author!\n\n");
		fflush(stdout);

        terminal_restore();

        exit(1);
	}
}

static void signal_event_shutdown(int * const signal_event_pipe)
{
	if (signal_event_pipe[0] != -1)
	{
		int const fd = signal_event_pipe[0];

		signal_event_pipe[0] = -1;
		close(fd);
	}

	if (signal_event_pipe[1] != -1)
	{
		int const fd = signal_event_pipe[1];

		signal_event_pipe[1] = -1;
		close(fd);
	}
}

static void signal_event_initialise(int * const signal_event_pipe)
{
	int const pipe_result = pipe(signal_event_pipe);
	IGNORE_NZ(pipe_result);

	struct sigaction action;
	action.sa_flags = 0;
	action.sa_handler = &sighandler;
	sigemptyset(&action.sa_mask);

	if (sigaction(SIGINT, &action, NULL) == -1)
	{
		perror("sigaction(SIGINT)");
	}
	if (sigaction(SIGSEGV, &action, NULL) == -1)
	{
		perror("sigaction(SIGSEGV)");
	}
	if (sigaction(SIGTERM, &action, NULL) == -1)
	{
		perror("sigaction(SIGTERM)");
	}
	if (sigaction(SIGWINCH, &action, NULL) == -1)
	{
		perror("sigaction(SIGWINCH)");
	}

	/* Using a separate handler for reaping zombies. */
	action.sa_flags = 0;
	action.sa_handler = &sigchld_handler;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGCHLD, &action, NULL) == -1)
	{
		perror("sigaction(SIGCHLD)");
	}
}

int send_probe_request(struct wif * const wi)
{
	REQUIRE(wi != NULL);

	int len;
	uint8_t p[4096], r_smac[6];

	memcpy(p, PROBE_REQ, 24);

	len = 24;

	p[24] = 0x00; // ESSID Tag Number
	p[25] = 0x00; // ESSID Tag Length

	len += 2;

	memcpy(p + len, RATES, 16);

	len += 16;

	r_smac[0] = 0x00;
	r_smac[1] = rand_u8();
	r_smac[2] = rand_u8();
	r_smac[3] = rand_u8();
	r_smac[4] = rand_u8();
	r_smac[5] = rand_u8();

	memcpy(p + 10, r_smac, 6);

	if (wi_write(wi, NULL, LINKTYPE_IEEE802_11, p, len, NULL) == -1)
	{
		switch (errno)
		{
			case EAGAIN:
			case ENOBUFS:
				usleep(10000);
				return (0); /* XXX not sure I like this... -sorbo */
			default:
				break;
		}

		perror("wi_write()");
		return -1;
	}

	return 0;
}

static int send_probe_requests(struct wif * * const wi, size_t num_cards)
{
	REQUIRE(wi != NULL);
    REQUIRE(num_cards > 0);

	for (size_t i = 0; i < num_cards; i++)
	{
		send_probe_request(wi[i]);
	}

	return (0);
}

size_t get_channel_count(
	int const * const channels,
	bool const count_valid_channels_only)
{
	size_t total_channel_count = 0;
    size_t valid_channel_count = 0;

    while (channels[total_channel_count] != channel_sentinel)
	{
		total_channel_count++;
        if (channels[total_channel_count] != invalid_channel)
		{
			valid_channel_count++;
		}
	}

	int const channel_count =
		count_valid_channels_only
			? valid_channel_count
			: total_channel_count;

	return channel_count;
}

size_t get_frequency_count(
    int const * const frequencies,
    bool const count_valid_frequencies_only)
{
    size_t total_frequency_count = 0;
    size_t valid_frequency_count = 0;

    while (frequencies[total_frequency_count] != frequency_sentinel)
	{
        total_frequency_count++;
        if (frequencies[total_frequency_count] != invalid_frequency)
		{
            valid_frequency_count++;
        }
	}

	int const frequency_count =
        count_valid_frequencies_only
            ? valid_frequency_count
            : total_frequency_count;

    return frequency_count;
}

static bool is_invalid_channel(int const channel)
{
    bool is_invalid;
	int i = 0;

    /* XXX - This seems to be a bit of a complicated way to 
     * construct this code. 
     */
	do
	{
        if (channel == abg_chans[i] && channel != channel_sentinel)
        {
            is_invalid = false;
            goto done;
        }
    }
    while (abg_chans[++i] != channel_sentinel);

    is_invalid = true;

done:
    return is_invalid;
}

static bool is_invalid_frequency(
    struct detected_frequencies_st const * const detected_frequencies,
    int const freq)
{
    bool is_invalid;

    for (size_t i = 0; i < detected_frequencies->count; i++)
    {
        if (freq == detected_frequencies->frequencies[i])
        {
            is_invalid = false;
            goto done;
        }
    }

    is_invalid = true;

done:
    return is_invalid;
}

/* parse a string, for example "1,2,3-7,11" */

static int get_channels(
    int * * const own_channels, 
    const char * optarg)
{
    int chan_cur = 0;
    int chan_first = 0;
    int chan_last = 0;
    static size_t const chan_max = 128;
    size_t chan_remain = chan_max;
    char * optchan = NULL;
    char * optc;
	char * token = NULL;
	int * tmp_channels;

	if (optarg == NULL)
	{
        return -1;
    }

	// create a writable string
	optc = optchan = strdup(optarg);
	ALLEGE(optc != NULL);

    tmp_channels = calloc(chan_max + 1, sizeof *tmp_channels);
	ALLEGE(tmp_channels != NULL);

	// split string in tokens, separated by ','
	while ((token = strsep(&optchan, ",")) != NULL)
	{
		// range defined?
		if (strchr(token, '-') != NULL)
		{
			// only 1 '-' ?
			if (strchr(token, '-') == strrchr(token, '-'))
			{
				// are there any illegal characters?
				for (size_t i = 0; i < strlen(token); i++)
				{
					if (((token[i] < '0') || (token[i] > '9'))
						&& (token[i] != '-'))
					{
						free(tmp_channels);
						free(optc);
						return -1;
					}
				}

				if (sscanf(token, "%u-%u", &chan_first, &chan_last) != EOF)
				{
					if (chan_first > chan_last)
					{
						free(tmp_channels);
						free(optc);
						return -1;
					}
					for (int i = chan_first; i <= chan_last; i++)
					{
						if (!is_invalid_channel(i) && chan_remain > 0)
						{
							tmp_channels[chan_max - chan_remain] = i;
							chan_remain--;
						}
					}
				}
				else
				{
					free(tmp_channels);
					free(optc);
					return -1;
				}
			}
			else
			{
				free(tmp_channels);
				free(optc);
				return -1;
			}
		}
		else
		{
			// are there any illegal characters?
			for (size_t i = 0; i < strlen(token); i++)
			{
				if ((token[i] < '0') || (token[i] > '9'))
				{
					free(tmp_channels);
					free(optc);
					return -1;
				}
			}

			if (sscanf(token, "%u", &chan_cur) != EOF)
			{
				if (!is_invalid_channel(chan_cur) && chan_remain > 0)
				{
					tmp_channels[chan_max - chan_remain] = chan_cur;
					chan_remain--;
				}
			}
			else
			{
				free(tmp_channels);
				free(optc);
				return -1;
			}
		}
	}

    size_t const num_channels = chan_max - chan_remain;

    *own_channels
        = malloc((num_channels + 1) * sizeof *own_channels);
    ALLEGE(*own_channels != NULL);

    for (size_t i = 0; i < num_channels; i++) //-V658
    {
        (*own_channels)[i] = tmp_channels[i];
    }

    (*own_channels)[num_channels] = channel_sentinel;

	free(tmp_channels);
	free(optc);

    if (num_channels == 1)
	{
        return (*own_channels)[0];
    }

    if (num_channels == 0)
	{
        return -1;
    }

	return 0;
}

/* parse a string, for example "1,2,3-7,11" */

static int getfrequencies(
    int * * const own_frequencies,
    struct detected_frequencies_st * const detected_frequencies,
    const char * optarg)
{
	unsigned int i = 0, freq_cur = 0, freq_first = 0, freq_last = 0,
				 freq_max = 10000, freq_remain = 0;
	char *optfreq = NULL, *optc;
	char * token = NULL;
	int * tmp_frequencies;

	// got a NULL pointer?
	if (optarg == NULL)
	{
        return -1;
    }

	freq_remain = freq_max;

	// create a writable string
	optc = optfreq = strdup(optarg);
	ALLEGE(optc != NULL);

    tmp_frequencies = calloc(freq_max + 1, sizeof(int));
	ALLEGE(tmp_frequencies != NULL);

	// split string in tokens, separated by ','
	while ((token = strsep(&optfreq, ",")) != NULL)
	{
		// range defined?
		if (strchr(token, '-') != NULL)
		{
			// only 1 '-' ?
			if (strchr(token, '-') == strrchr(token, '-'))
			{
				// are there any illegal characters?
				for (i = 0; i < strlen(token); i++)
				{
					if ((token[i] < '0' || token[i] > '9') && (token[i] != '-'))
					{
						free(tmp_frequencies);
						free(optc);
						return -1;
					}
				}

				if (sscanf(token, "%u-%u", &freq_first, &freq_last) != EOF)
				{
					if (freq_first > freq_last)
					{
						free(tmp_frequencies);
						free(optc);
						return -1;
					}
					for (i = freq_first; i <= freq_last; i++)
					{
                        if (!is_invalid_frequency(detected_frequencies, i)
                            && freq_remain > 0)
						{
							tmp_frequencies[freq_max - freq_remain] = i;
							freq_remain--;
						}
					}
				}
				else
				{
					free(tmp_frequencies);
					free(optc);
					return -1;
				}
			}
			else
			{
				free(tmp_frequencies);
				free(optc);
				return -1;
			}
		}
		else
		{
			// are there any illegal characters?
			for (i = 0; i < strlen(token); i++)
			{
				if ((token[i] < '0') || (token[i] > '9'))
				{
					free(tmp_frequencies);
					free(optc);
					return -1;
				}
			}

			if (sscanf(token, "%u", &freq_cur) != EOF)
			{
                if (!is_invalid_frequency(detected_frequencies, freq_cur)
                    && freq_remain > 0)
				{
					tmp_frequencies[freq_max - freq_remain] = freq_cur;
					freq_remain--;
				}

				/* special case "-C 0" means: scan all available frequencies */
				if (freq_cur == 0)
				{
					freq_first = 1;
					freq_last = 9999;
					for (i = freq_first; i <= freq_last; i++)
					{
                        if (!is_invalid_frequency(detected_frequencies, i)
                            && freq_remain > 0)
						{
							tmp_frequencies[freq_max - freq_remain] = i;
							freq_remain--;
						}
					}
				}
			}
			else
			{
				free(tmp_frequencies);
				free(optc);
				return -1;
			}
		}
	}

    *own_frequencies
        = malloc((freq_max - freq_remain + 1) * sizeof *own_frequencies);
    ALLEGE(*own_frequencies != NULL);

	if (freq_max > 0 && freq_max >= freq_remain) //-V560
	{
		for (i = 0; i < (freq_max - freq_remain); i++) //-V658
		{
            (*own_frequencies)[i] = tmp_frequencies[i];
		}
	}

    (*own_frequencies)[i] = frequency_sentinel;

	free(tmp_frequencies);
	free(optc);
	if (i == 1)
	{
        return (*own_frequencies)[0]; // exactly 1 frequency given
    }

	if (i == 0)
	{
        return -1; // error occurred
    }

	return 0; // frequency hopping
}

static bool name_already_specified(
	char const * const interface_name,
	char const * const * const iface,
	size_t if_count)
{
	bool already_specified;

	for (size_t i = 0; i < if_count; i++)
	{
		if (strcmp(iface[i], interface_name) == 0)
		{
			already_specified = true;
			goto done;
		}
	}

	already_specified = false;

done:
	return already_specified;
}

static int initialise_cards(
	const char * cardstr,
	struct wif * * wi)
{
	char * buffer;
	char * buf = NULL;
	int if_count = 0;
	char * interface_name;
	char const * iface[MAX_CARDS];

	// Check card string is valid
	if (cardstr == NULL || cardstr[0] == '0')
	{
		if_count = -1;
		goto done;
	}

	buf = buffer = strdup(cardstr);
	if (buf == NULL)
	{
		if_count = -1;
		goto done;
	}

	while ((if_count < MAX_CARDS)
		   && ((interface_name = strsep(&buffer, ",")) != NULL))
	{
		/* Ignore repeated interface names. */
		if (name_already_specified(interface_name, iface, if_count))
		{
			continue;
		}

		wi[if_count] = wi_open(interface_name);
		if (wi[if_count] == NULL)
		{
			if_count = -1;
			goto done;
		}

		iface[if_count] = interface_name;
		if_count++;
	}

done:
	free(buf);

	return if_count;
}

static unsigned int set_encryption_filter(char const * const input)
{
    unsigned int encryption_filter = 0;

    if (input == NULL)
    {
        encryption_filter = 0; 
        goto done;
    }

    if (strcasecmp(input, "opn") == 0)
    {
        encryption_filter |= STD_OPN;
    }

    if (strcasecmp(input, "wep") == 0)
    {
        encryption_filter |= STD_WEP;
    }

	if (strcasecmp(input, "wpa") == 0)
	{
        encryption_filter |= STD_WPA;
        encryption_filter |= STD_WPA2;
        encryption_filter |= AUTH_SAE;
	}

    if (strcasecmp(input, "wpa1") == 0)
    {
        encryption_filter |= STD_WPA;
    }

    if (strcasecmp(input, "wpa2") == 0)
    {
        encryption_filter |= STD_WPA2;
    }

	if (strcasecmp(input, "wpa3") == 0)
	{
		encryption_filter |= AUTH_SAE;
	}

	if (strcasecmp(input, "owe") == 0)
	{
		encryption_filter |= AUTH_OWE;
	}

done:
    return encryption_filter;
}

static struct wif * reopen_card(struct wif * const old)
{
    struct wif * new_wi;
	char ifnam[MAX_IFACE_NAME];

    /* The interface name needs to be saved because closing the
     * card frees all resources associated with the wi.
     */
    strlcpy(ifnam, wi_get_ifname(old), sizeof ifnam);

    wi_close(old);
    new_wi = wi_open(ifnam);

    if (new_wi == NULL)
    {
        printf("Can't reopen %s\n", ifnam);
    }

    return new_wi;
}

static bool reopen_cards(struct wif * * const wi, size_t const num_cards)
{
    bool success;

    for (size_t i = 0; i < num_cards; i++)
    {
        wi[i] = reopen_card(wi[i]);
        if (wi[i] == NULL)
        {
            success = false;
            goto done;
        }
    }

    success = true;

done:
    return success;
}

static void close_cards(struct wif * * const wi, size_t const num_cards)
{
	for (size_t i = 0; i < num_cards; i++)
	{
        if (wi[i] != NULL) /* May be NULL due to a reopen failure. */
        {
            wi_close(wi[i]);
        }
	}
}

static void write_monitor_mode_message(
    char * const msg_buffer,
    size_t const msg_buffer_size,
    char const * const interface_name)
{
    snprintf(msg_buffer,
             msg_buffer_size,
             "%s reset to monitor mode",
             interface_name);
}

static void write_fixed_channel_message(
    char * const msg_buffer,
    size_t const msg_buffer_size,
    char const * const interface_name,
    int const channel)
{
    snprintf(msg_buffer,
             msg_buffer_size,
             "fixed channel %s: %d ",
             interface_name,
             channel);
}

static void write_fixed_frequency_message(
    char * const msg_buffer,
    size_t const msg_buffer_size,
    char const * const interface_name,
    int const frequency)
{
    snprintf(msg_buffer,
             msg_buffer_size,
             "fixed frequency %s: %d ",
             interface_name,
             frequency);
}

static struct wif * check_for_monitor_mode_on_card(
    struct wif * const wi,
    char * const msg_buffer,
    size_t const msg_buffer_size)
{
    int const monitor = wi_get_monitor(wi);
    struct wif * new_wi;

    if (monitor == 0)
    {
        new_wi = wi;
        goto done;
    }

    // reopen in monitor mode
    new_wi = reopen_card(wi);

    write_monitor_mode_message(
        msg_buffer,
        msg_buffer_size,
        wi_get_ifname(wi));

done:
    return new_wi;
}

static bool check_for_monitor_mode_on_cards(
    struct wif * * const wi,
    size_t const num_cards,
    char * const msg_buffer,
    size_t const msg_buffer_size)
{
    bool success;

    for (size_t i = 0; i < num_cards; i++)
	{
        struct wif * new_wi = 
            check_for_monitor_mode_on_card(wi[i], msg_buffer, msg_buffer_size);

        if (new_wi != wi[i])
        {
            wi[i] = new_wi;
            if (wi[i] == NULL)
            {
                success = false;
                goto done;
            }
        }
	}

    success = true;

done:
	return success;
}

static bool check_channel_on_card(
    struct wif * const wi,
    int const desired_channel,
    char * const msg_buffer,
    size_t const msg_buffer_size,
    int const ignore_negative_one
#ifdef CONFIG_LIBNL
    , unsigned int htval
#endif
    )
{
    bool changed_channel;
    int const current_channel = wi_get_channel(wi);

    if (ignore_negative_one && current_channel == invalid_channel)
    {
        changed_channel = false;
        goto done;
    }

    if (desired_channel == current_channel)
    {
        changed_channel = false;
        goto done;
    }

#ifdef CONFIG_LIBNL
    wi_set_ht_channel(wi, desired_channel, htval);
#else
    wi_set_channel(wi, desired_channel);
#endif

    write_fixed_channel_message(msg_buffer,
                                msg_buffer_size,
                                wi_get_ifname(wi),
                                current_channel);

    changed_channel = true;

done:
    return changed_channel;
}

static void check_channel_on_cards(
    struct wif * * const wi,
    int const * const current_channels,
    size_t const num_cards,
    char * const msg_buffer,
    size_t const msg_buffer_size,
    int const ignore_negative_one
#ifdef CONFIG_LIBNL
    , unsigned int htval
#endif
    )
{
    for (size_t i = 0; i < num_cards; i++)
	{
        check_channel_on_card(wi[i], 
                              current_channels[i], 
                              msg_buffer, 
                              msg_buffer_size, 
                              ignore_negative_one
#ifdef CONFIG_LIBNL
                              , htval
#endif
                             );
	}
}

static bool check_frequency_on_card(
    struct wif * const wi,
    int const desired_frequency,
    char * const msg_buffer,
    size_t const msg_buffer_size)
{
    bool changed_frequency;
    int const current_frequency = wi_get_freq(wi);

    /* FIXME: replace 0 with invalid_frequency if only ever -1. */
    if (current_frequency < 0)
    {
        changed_frequency = false;
        goto done;
    }

    if (desired_frequency == current_frequency)
    {
        changed_frequency = false;
        goto done;
    }

    wi_set_freq(wi, desired_frequency);

    write_fixed_frequency_message(msg_buffer,
                                  msg_buffer_size,
                                  wi_get_ifname(wi),
                                  current_frequency);

    changed_frequency = true;

done:
    return changed_frequency;
}

static void check_frequency_on_cards(
    struct wif * * const wi,
    int const * const current_frequencies,
    size_t const num_cards,
    char * const msg_buffer,
    size_t const msg_buffer_size)
{
    for (size_t i = 0; i < num_cards; i++)
	{
        check_frequency_on_card(wi[i], current_frequencies[i], msg_buffer, msg_buffer_size);
	}
}

static bool update_interface_cards(
    struct wif * * const wi,
    size_t const num_cards,
    bool const single_channel,
    int const * const current_channels,
    bool const single_frequency,
    int const * const current_frequencies,
    char * const msg_buffer,
    size_t const msg_buffer_size,
    int const ignore_negative_one
#ifdef CONFIG_LIBNL
    , unsigned int htval
#endif
    )
{
    bool success;

    if (!check_for_monitor_mode_on_cards(wi, 
                                         num_cards, 
                                         msg_buffer, 
                                         msg_buffer_size))
    {
        success = false;
        goto done;
    }

    if (single_channel)
    {
        check_channel_on_cards(wi, 
                               current_channels, 
                               num_cards, 
                               msg_buffer, 
                               msg_buffer_size,
                               ignore_negative_one
#ifdef CONFIG_LIBNL
                               , htval
#endif
                               );
    }
    if (single_frequency)
    {
        check_frequency_on_cards(wi, 
                                 current_frequencies, 
                                 num_cards, 
                                 msg_buffer, 
                                 msg_buffer_size);
    }

    success = true;

done:
    return success;
}

static void detect_frequency_range(
    struct wif * wi,
    struct detected_frequencies_st * const detected_frequencies,
    int const start_freq,
    int const end_freq)
{
    for (int freq = start_freq;
          detected_frequencies->count < detected_frequencies->table_size && freq <= end_freq;
          freq += 5)
    {
        if (wi_set_freq(wi, freq) == 0)
        {
            detected_frequencies->frequencies[detected_frequencies->count] = freq;
            detected_frequencies->count++;
        }

        int const channel_13_freq = 2482;

        if (freq == channel_13_freq)
        {
            int const channel_14_freq = 2484;
            // special case for chan 14, as its 12MHz away from 13, not 5MHz
            if (wi_set_freq(wi, channel_14_freq) == 0)
            {
                detected_frequencies->frequencies[detected_frequencies->count] = channel_14_freq;
                detected_frequencies->count++;
            }
        }
    }
}

static void detected_frequencies_initialise(
    struct detected_frequencies_st * const detected_frequencies,
    size_t const max_frequencies)
{
    detected_frequencies->count = 0;
    detected_frequencies->table_size = max_frequencies;
    // field for frequencies supported
    detected_frequencies->frequencies =
        calloc(detected_frequencies->table_size, sizeof(int));

    ALLEGE(detected_frequencies->frequencies != NULL);
}

static void detected_frequencies_cleanup(
    struct detected_frequencies_st * const detected_frequencies)
{
    free(detected_frequencies->frequencies);
    detected_frequencies->frequencies = NULL;
}


static void detect_frequencies(
    struct wif * wi,
    struct detected_frequencies_st * const detected_frequencies)
{
	REQUIRE(wi != NULL);

    size_t const max_freq_num = 2048; // should be enough to keep all available channels

	printf("Checking available frequencies; this could take few seconds.\n");

    detected_frequencies_initialise(detected_frequencies, max_freq_num);

    int start_freq = 2192;
    int end_freq = 2732;
    detect_frequency_range(wi, detected_frequencies, start_freq, end_freq);

    // again for 5GHz channels
    start_freq = 4800;
    end_freq = 6000;
    detect_frequency_range(wi, detected_frequencies, start_freq, end_freq);

    printf("Done. Found %zu frequencies\n", detected_frequencies->count);
}

static bool array_contains(
    int const * const array, 
    size_t const length, 
    int const value)
{
    bool found;
	REQUIRE(array != NULL);

    for (size_t i = 0; i < length; i++)
    {
        if (array[i] == value)
        {
            found = true;
            goto done;
        }
    }

    found = false;

done:
    return found;
}

static void rearrange_frequencies(int * const own_frequencies)
{
	int * freqs;
    int left; 
    int pos;
	int last_used = 0;
    int cur_freq; 
    int round_done;
    static int const width = DEFAULT_CHANNEL_WIDTH_MHZ;

    size_t const count = get_frequency_count(own_frequencies, false);
	left = count;
	pos = 0;

	freqs = calloc(count + 1, sizeof *freqs);
	ALLEGE(freqs != NULL);
	round_done = 0;

	while (left > 0)
	{
		cur_freq = own_frequencies[pos % count];

        if (cur_freq == last_used)
        {
            round_done = 1;
        }

		if ((count - left) > 0 
            && !round_done
			&& ABS(last_used - cur_freq) < width)
		{
            /* Do nothing. */
            /* FIXME: fix that conditional to be opposite in sense. */
		}
        else if (!array_contains(freqs, count, cur_freq))
		{
			freqs[count - left] = cur_freq;
			last_used = cur_freq;
			left--;
			round_done = 0;
		}

		pos++;
	}

	memcpy(own_frequencies, freqs, count * sizeof *own_frequencies);
	free(freqs);
}

static void drop_privileges(void)
{
	if (setuid(getuid()) == -1)
	{
		perror("setuid");
	}
}

static bool start_child_process(int * const pipe_handles)
{
    int result; /* -1: error, 0: child process, > 0: parent_process */
    int const pipe_result = pipe(pipe_handles);

    IGNORE_NZ(pipe_result);

    result = fork();

    if (result == -1)
    {
        goto done;
    }

    if (result == 0)
    {
        /* This is the child process. */

        /* Close the read end of the communications pipe as the child
         * only writes data for the parent to read.
         */
        close(pipe_handles[0]);
    }
    else
    {
        /* This is the parent process. */
        /* Close the write end of the communications pipe as the parent
         * only reads data written by the child.
         */
        close(pipe_handles[1]);
    }

done:

    return result;
}

static bool start_frequency_hopper_process(
    struct local_options * const options,
    struct wif * * const wi,
    int const frequency_count)
{
    pid_t const main_pid = getpid();
    int const result = start_child_process(options->channel_hopper_pipe);

    if (result == 0)
    {
        /* reopen cards. This way parent & child don't share resources for
        * accessing the card (e.g. file descriptors) which may cause
        * problems.  -sorbo
        */

        if (!reopen_cards(wi, options->num_cards))
        {
            exit(EXIT_FAILURE);
        }

        drop_privileges();

        frequency_hopper(options->channel_hopper_pipe[1],
                         wi,
                         options->num_cards,
                         frequency_count,
                         options->channel_switching_method,
                         options->own_frequencies,
                         options->frequency,
                         options->frequency_hop_millisecs,
                         main_pid);

        exit(EXIT_FAILURE);
    }

    bool const child_started = result > 0;

    return child_started;
}

static bool start_channel_hopper_process(
    struct local_options * const options,
    struct wif * * const wi,
    int const channel_count)
{
    pid_t const main_pid = getpid();
    int const result = start_child_process(options->channel_hopper_pipe);

    if (result == 0)
    {
        /* reopen cards. This way parent & child don't share resources for
        * accessing the card (e.g. file descriptors) which may cause
        * problems.  -sorbo
        */

        if (!reopen_cards(wi, options->num_cards))
        {
            exit(EXIT_FAILURE);
        }

        drop_privileges();

        channel_hopper(options->channel_hopper_pipe[1],
                       wi,
                       options->num_cards,
                       channel_count,
                       options->channel_switching_method,
                       options->channels,
                       options->channel,
                       options->active_scan_sim > 0,
                       options->frequency_hop_millisecs,
                       main_pid
#ifdef CONFIG_LIBNL
                       , options->htval
#endif
                      );

        exit(EXIT_FAILURE);
    }

    bool const child_started = result > 0;

    return child_started;
}

static bool pipe_has_data_ready(int const fd)
{
    bool have_data_ready;
    fd_set rfds;

    if (fd == -1)
    {
        have_data_ready = false;
        goto done;
    }

    int pipe_ready;
    do
    {
        struct timeval tv =
        {
            .tv_sec = 0,
            .tv_usec = 0
        };
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds); // NOLINT(hicpp-signed-bitwise)
        pipe_ready = select(fd + 1, &rfds, NULL, NULL, &tv);
    }
    while (pipe_ready < 0 && errno == EINTR);

    if (pipe_ready <= 0 || !FD_ISSET(fd, &rfds))
    {
        have_data_ready = false;
        goto done;
    }

    have_data_ready = true;

done:
    return have_data_ready;
}

static bool pipe_read(int const fd, void * const data, size_t const data_size)
{
    /* data_size is the expected number of bytes to read. */
    bool have_read_data;

    if (!pipe_has_data_ready(fd))
    {
        have_read_data = false;
        goto done;
    }

    int read_result;
    do
    {
        read_result = read(fd, data, data_size);
    }
    while (read_result < 0 && errno == EINTR);

    size_t const bytes_read = (size_t)read_result;

    have_read_data = bytes_read == data_size;

done:
    return have_read_data;
}

static void check_for_channel_hopper_data(struct local_options * const options)
{
    struct channel_hopper_data_st hopper_data = { 0 };

    while (pipe_read(options->channel_hopper_pipe[0],
                     &hopper_data,
                     sizeof hopper_data))
    {
        channel_hopper_data_handler(options, &hopper_data);
    }
}

static void process_event(
    struct local_options * const options,
    signal_event_t const event)
{
    switch (event)
    {
        case signal_event_window_changed:
            handle_window_changed_event();
            break;
        case signal_event_terminate:
            handle_terminate_event(options);
            break;
        default:
            /* Unknown event. */
            break;
    }
}

static void check_for_signal_events(struct local_options * const options)
{
    int event = 0;

    while (pipe_read(options->signal_event_pipe[0],
                     &event,
                     sizeof event))
    {
        process_event(options, event);
    }
}

static void check_for_user_input(struct local_options * const options)
{
    int keycode = 0;

    while (pipe_read(options->input_thread_pipe[0],
                     &keycode,
                     sizeof keycode))
    {
        handle_input_key(options, keycode);
    }
}


#define AIRODUMP_NG_CSV_EXT "csv"
#define KISMET_CSV_EXT "kismet.csv"
#define KISMET_NETXML_EXT "kismet.netxml"
#define WIFI_EXT "wifi"
#define AIRODUMP_NG_GPS_EXT "gps"
#define AIRODUMP_NG_CAP_EXT "cap"
#define AIRODUMP_NG_LOG_CSV_EXT "log.csv"

static bool open_output_files(struct local_options * const options)
{
	bool success;
	char * ofn;
	size_t ofn_len;
	size_t const ADDED_LENGTH = 17; /* FIXME: Work out the required length from
									 *  the extensions etc
									 */
    if (options->no_filename_index)
    {
        options->f_index = -1;
    }
    else
    {
        options->f_index = find_first_free_file_index(options->dump_prefix);
    }

	/* Create a buffer of the length of the prefix + '-' + 2 numbers + '.'
	   + longest extension ("kismet.netxml") + terminating 0. */
    ofn_len = strlen(options->dump_prefix) + ADDED_LENGTH + 1;
	ofn = malloc(ofn_len);
	ALLEGE(ofn != NULL);

    /* TODO: Loop over the array rather than checking each index 
     * specifically. Note that the wifi dump might need to 
     * generate the output filename differently than the others (no 
     * index). 
     * Perhaps include a suffix and filename generator callback, or 
     * just have a filename type specifier. 
     */
    if (options->dump[dump_type_csv].needed)
	{
		char const * const filename = 
            create_output_filename(
                ofn, 
                ofn_len, 
                options->dump_prefix, 
                options->f_index, 
                AIRODUMP_NG_CSV_EXT);

        options->dump[dump_type_csv].context =
			dump_open(dump_type_csv,
                      filename,
					  options->sys_name,
					  options->loc_name,
					  options->filter_seconds,
                      options->file_reset_seconds,
                      options->airodump_start_time,
                      options->gpsd.required);

        if (options->dump[dump_type_csv].context == NULL)
		{
            fprintf(stderr, "Could not create \"%s\".\n", filename);

			success = false;
			goto done;
		}
	}

    if (options->dump[dump_type_kismet_csv].needed)
	{
        char const * const filename =
            create_output_filename(
                ofn,
                ofn_len,
                options->dump_prefix,
                options->f_index,
                KISMET_CSV_EXT);

        options->dump[dump_type_kismet_csv].context =
			dump_open(dump_type_kismet_csv,
                      filename,
                      options->sys_name,
                      options->loc_name,
                      options->filter_seconds,
                      options->file_reset_seconds,
                      options->airodump_start_time,
                      options->gpsd.required);

        if (options->dump[dump_type_kismet_csv].context == NULL)
		{
            fprintf(stderr, "Could not create \"%s\".\n", filename);

			success = false;
			goto done;
		}
	}

    if (options->dump[dump_type_kismet_netxml].needed)
	{
        char const * const filename =
            create_output_filename(
                ofn,
                ofn_len,
                options->dump_prefix,
                options->f_index,
                KISMET_NETXML_EXT);

        options->dump[dump_type_kismet_netxml].context =
			dump_open(dump_type_kismet_netxml,
                      filename,
                      options->sys_name,
                      options->loc_name,
                      options->filter_seconds,
                      options->file_reset_seconds,
                      options->airodump_start_time,
                      options->gpsd.required);

        if (options->dump[dump_type_kismet_netxml].context == NULL)
		{
            fprintf(stderr, "Could not create \"%s\".\n", filename);

			success = false;
			goto done;
		}
	}

    if (options->dump[dump_type_wifi_scanner].needed)
	{
        char const * const filename =
            create_output_filename(
                ofn,
                ofn_len,
                options->dump_prefix,
                options->f_index,
                WIFI_EXT);

        options->dump[dump_type_wifi_scanner].context =
			dump_open(dump_type_wifi_scanner,
                      filename,
                      options->sys_name,
                      options->loc_name,
                      options->filter_seconds,
                      options->file_reset_seconds,
                      options->airodump_start_time,
                      options->gpsd.required);

        if (options->dump[dump_type_wifi_scanner].context == NULL)
		{
            fprintf(stderr, "Could not create \"%s\".\n", filename);

			success = false;
			goto done;
		}
	}

    if (options->pcap_output.required)
    {
        char const * const filename =
            create_output_filename(
                ofn,
                ofn_len,
                options->dump_prefix,
                options->f_index,
                AIRODUMP_NG_CAP_EXT);

        options->pcap_output.writer =
            packet_writer_open(packet_writer_type_pcap, filename);

        if (options->pcap_output.writer == NULL)
        {
            fprintf(stderr, "Could not create \"%s\".\n", filename);

            success = false;
            goto done;
        }
    }
    else if (options->ivs.required)
    {
        char const * const filename =
            create_output_filename(
                ofn,
                ofn_len,
                options->dump_prefix,
                options->f_index,
                IVS2_EXTENSION);

        options->ivs.fp = ivs_log_open(filename);
        if (options->ivs.fp == NULL)
        {
            success = false;
            goto done;
        }
    }

    if (options->log_csv.required)
    {
        char const * const filename =
            create_output_filename(
                ofn,
                ofn_len,
                options->dump_prefix,
                options->f_index,
                AIRODUMP_NG_LOG_CSV_EXT);

        options->log_csv.fp = log_csv_file_open(filename);
        if (options->log_csv.fp == NULL)
        {
            success = false;
            goto done;
        }
    }

    if (options->gpsd.required)
    {
        char const * const filename =
            create_output_filename(
                ofn,
                ofn_len,
                options->dump_prefix,
                options->f_index,
                AIRODUMP_NG_GPS_EXT);

        options->gpsd.fp = fopen(filename, "wb+");
        if (options->gpsd.fp == NULL)
        {
            perror("fopen failed");
            fprintf(stderr, "Could not create \"%s\".\n", filename);

            success = false;
            goto done;
        }
        /* Store the filename as it is possibly used to delete the 
         * file at shutdown. There's possibly a better way to do this.
         */
        options->gpsd.filename = strdup(filename);
    }

    success = true;

done:
	free(ofn);

	return success;
}

static void update_dump_output_files(struct local_options * const options)
{
    for (dump_type_t dump_type = 0; dump_type < dump_type_COUNT; dump_type++)
    {
        dump_context_st * const dump_context = options->dump[dump_type].context;

        if (dump_context != NULL)
        {
            dump_write(dump_context,
                       &options->ap_list,
                       &options->sta_list,
                       options->encryption_filter,
                       &options->essid_filter);
        }
    }
}

static void close_dump_output_files(struct local_options * const options)
{
    for (dump_type_t dump_type = 0; dump_type < dump_type_COUNT; dump_type++)
    {
        dump_context_st * const dump_context = options->dump[dump_type].context;

        if (dump_context != NULL)
        {
            dump_close(dump_context);
            options->dump[dump_type].context = NULL;
        }
    }
}

static void close_output_files(struct local_options * const options)
{
    close_dump_output_files(options);

    if (options->gpsd.fp != NULL)
	{
        fclose(options->gpsd.fp);
        if (!options->gps_context.save_gps && options->gpsd.filename != NULL)
        {
            unlink(options->gpsd.filename);
        }
        free(options->gpsd.filename);
    }

    if (options->pcap_output.writer != NULL)
    {
        packet_writer_close(options->pcap_output.writer);
        options->pcap_output.writer = NULL;
    }

    if (options->ivs.fp != NULL)
	{
        fclose(options->ivs.fp);
	}

    if (options->log_csv.fp != NULL)
	{
        fclose(options->log_csv.fp);
	}
}

static void do_quit_request_timeout_check(
    struct local_options * const options)
{
    if (options->quitting > 0)
	{
        time_t const seconds_since_last_quit_event = time(NULL) - options->quitting_event_ts;
        time_t const maximum_quit_event_interval_seconds = 3;

		if (seconds_since_last_quit_event > maximum_quit_event_interval_seconds)
		{
            options->quitting_event_ts = 0;
            options->quitting = 0;
            options->message[0] = '\0';
		}
	}
}

static void pace_pcap_reader(
	struct timeval * const previous_timestamp,
	struct timeval * const packet_timestamp,
	int const read_pkts)
{
    /* Control the speed that the packets are read from the file
     * to simulate the rate they were captured at.
     */
	if (previous_timestamp->tv_sec != 0
		&& previous_timestamp->tv_usec != 0)
	{
		const useconds_t usec_diff
			= (useconds_t)time_diff(previous_timestamp, packet_timestamp);

		if (usec_diff > 0)
		{
			usleep(usec_diff);
		}
	}
	else if (read_pkts % 10 == 0)
	{
		usleep(1);
	}

    // track the packet's timestamp
	*previous_timestamp = *packet_timestamp;
}

static void airodump_shutdown(
    struct local_options * const options,
    struct wif * * const wi)
{
	/* TODO: Restore signal handlers. */
    signal_event_shutdown(options->signal_event_pipe);

    if (options->gpsd.required)
	{
        gps_tracker_stop(&options->gps_context);
	}

    free(options->elapsed_time);
    free(options->own_channels);

    pcap_reader_close(options->pcap_reader_context);

    essid_filter_context_cleanup(&lopt.essid_filter);

    close_cards(wi, options->num_cards);

    update_dump_output_files(&lopt);

    close_output_files(&lopt);

    free(options->airodump_start_time);
    options->airodump_start_time = NULL;

    if (options->interactive_mode > 0)
    {
        pthread_join(options->input_tid, NULL);
        close(options->input_thread_pipe[1]);
        close(options->input_thread_pipe[0]);
    }

    sta_list_free(&options->sta_list);

    ap_list_free(&lopt);

    na_info_list_free(&options->na_list);

    oui_context_free(options->manufacturer_list);

    /* TODO: Separate out shared key context init/cleanup. */
    if (options->shared_key.f_xor != NULL)
    {
        fclose(options->shared_key.f_xor);
    }

    ap_sort_context_free(options->sort.sort_context);
}

static bool handle_ready_wi_interface(
    struct local_options * const options, 
    struct wif * * const wi,
    size_t const interface_index,
    uint8_t * const packet_buffer,
    size_t packet_buffer_size)
{
    bool success;
    struct rx_info ri;

    ssize_t const packet_length =
        wi_read(*wi, NULL, NULL, packet_buffer, packet_buffer_size, &ri);

    if (packet_length == -1)
    {
        options->wi_consecutive_failed_reads[interface_index]++;
        if (options->wi_consecutive_failed_reads[interface_index]
            >= options->max_consecutive_failed_interface_reads)
        {
            success = false;
            goto done;
        }

        snprintf(options->message,
                 sizeof(options->message),
                 "interface %s down ",
                 wi_get_ifname(*wi));

        *wi = reopen_card(*wi);
        if (*wi == NULL)
        {
            success = false;
            goto done;
        }
    }
    else
    {
        options->wi_consecutive_failed_reads[interface_index] = 0;
        dump_add_packet(packet_buffer, packet_length, &ri, interface_index);
    }

    success = true;

done:
    return success;
}

static int capture_packet_from_cards(
    struct local_options * const options,
    struct wif * * wi,
    size_t num_cards,
    uint8_t * const packet_buffer,
    size_t packet_buffer_size)
{
    /* Capture one packet from each card. */
    int result;
    fd_set rfds;
    int max_fd = -1;

    FD_ZERO(&rfds);
    for (size_t i = 0; i < num_cards; i++)
    {
        int const interface_fd = wi_fd(wi[i]);

        FD_SET(interface_fd, &rfds); // NOLINT(hicpp-signed-bitwise)
        if (interface_fd > max_fd)
        {
            max_fd = interface_fd;
        }
    }
    struct timeval tv0 =
    {
        .tv_sec = options->update_interval_seconds,
        .tv_usec = (options->update_interval_seconds == 0) ? REFRESH_RATE : 0
    };

    if (select(max_fd + 1, &rfds, NULL, NULL, &tv0) < 0)
    {
        if (errno == EINTR)
        {
            result = 0;
            goto done;
        }
        perror("select failed");

        result = -1;
        goto done;
    }

    for (size_t i = 0; i < options->num_cards; i++)
    {
        if (FD_ISSET(wi_fd(wi[i]), &rfds)) // NOLINT(hicpp-signed-bitwise)
        {
            if (!handle_ready_wi_interface(
                    options, 
                    &wi[i], 
                    i, 
                    packet_buffer, 
                    packet_buffer_size))
            {
                result = -1;
                goto done;
            }
        }
    }

    result = 1;

done:
    return result;
}

static void update_console(struct local_options * const options)
{
    struct sort_context_st * const sort_context = &options->sort;

    if (sort_context->sort_required || sort_context->do_sort_always)
    {
        dump_sort(options, sort_context->sort_context);
        sort_context->sort_required = false;
    }

    dump_print(options->window_size.ws_row, 
               options->window_size.ws_col, 
               options->num_cards);
}

static void do_refresh(struct local_options * const options)
{
    purge_old_nodes(options, options->max_node_age);
    aps_purge_old_packets(&options->ap_list, BUFFER_TIME_MILLISECS);

    update_data_packets_per_second(options);

    if (options->interactive_mode > 0
        && (!options->console_output.paused || options->console_output.required))
    {
        options->console_output.required = false;

        update_window_size(&options->window_size);
        update_console(options);
    }
}

static void dump_contexts_initialise(
    struct local_options * const options, 
    bool const are_needed)
{
    for (dump_type_t dump_type = 0; dump_type < dump_type_COUNT; dump_type++)
    {
        options->dump[dump_type].needed = are_needed;
    }
}

static bool read_one_packet_from_file(
    struct local_options * const options,
    uint8_t * const packet_buffer,
    size_t packet_buffer_size,
    struct timeval * const packet_timestamp)
{
    bool read_a_packet;
    size_t packet_length;
    struct rx_info ri;

    pcap_reader_result_t const result =
        pcap_read(options->pcap_reader_context,
                  packet_buffer,
                  packet_buffer_size,
                  &packet_length,
                  &ri,
                  packet_timestamp);

    if (result == pcap_reader_result_ok)
    {
        static size_t const file_dummy_card_number = 0;
        dump_add_packet(packet_buffer, packet_length, &ri, file_dummy_card_number);

        read_a_packet = true;
        goto done;
    }

    read_a_packet = false;

    if (result == pcap_reader_result_done)
    {
        pcap_reader_close(lopt.pcap_reader_context);
        lopt.pcap_reader_context = NULL;

        snprintf(lopt.message,
                 sizeof(lopt.message),
                 "Finished reading input file %s.",
                 lopt.s_file);

    }

done:
    return read_a_packet;
}

int main(int argc, char * argv[])
{
    /* The user thread args must remain in scope as long as the 
     * thread is running. 
     */
    struct input_thread_args_st input_thread_args;
    int program_exit_code;
    bool had_error = false;
#define ONE_HOUR (60 * 60)
#define ONE_MIN (60)
	int read_pkts = 0;

	long time_slept;
	long cycle_time;
	char * output_format_string;
    int i;

	struct wif * wi[MAX_CARDS];

	int found;
	int freq[2];
	size_t num_opts = 0;
	int option = 0;
	int option_index = 0;
    int reset_val = 0;
    int output_format_first_time = 1;

	time_t tt1;
	time_t tt2;
	time_t start_time;

	uint8_t h80211[4096];

	struct timeval tv0;
	struct timeval current_time_timestamp;
	struct timeval tv2;
	struct timeval tv3;
	struct timeval last_active_scan_timestamp;
	struct timeval previous_timestamp = {.tv_sec = 0, .tv_usec = 0 };
	struct tm * lt;

	static const struct option long_options[]
		= {{"ht20", 0, 0, '2'},
		   {"ht40-", 0, 0, '3'},
		   {"ht40+", 0, 0, '5'},
		   {"band", 1, 0, 'b'},
		   {"beacon", 0, 0, 'e'},
		   {"beacons", 0, 0, 'e'},
		   {"cswitch", 1, 0, 's'},
		   {"netmask", 1, 0, 'm'},
		   {"bssid", 1, 0, 'd'},
		   {"essid", 1, 0, 'N'},
		   {"essid-regex", 1, 0, 'R'},
		   {"channel", 1, 0, 'c'},
		   {"gpsd", 0, 0, 'g'},
		   {"ivs", 0, 0, 'i'},
		   {"write", 1, 0, 'w'},
		   {"encrypt", 1, 0, 't'},
		   {"update", 1, 0, 'u'},
		   {"berlin", 1, 0, 'B'},
		   {"help", 0, 0, 'H'},
		   {"nodecloak", 0, 0, 'D'},
		   {"showack", 0, 0, 'A'},
		   {"detect-anomaly", 0, 0, 'E'},
		   {"output-format", 1, 0, 'o'},
           {"sys-name", 1, 0, 'X'},
           {"loc-name", 1, 0, 'y'},
           {"filter-seconds", 1, 0, 'F'},
           {"max-age", 1, 0, 'v'},
           {"file-reset-minutes", 1, 0, 'P'},
           {"ignore-negative-one", 0, &lopt.ignore_negative_one, 1},
           {"no-filename-index", 0, &lopt.no_filename_index, 1},
           {"no-shared-key", 0, &lopt.no_shared_key, 1},
           {"manufacturer", 0, 0, 'M' },
		   {"uptime", 0, 0, 'U'},
		   {"write-interval", 1, 0, 'I'},
		   {"wps", 0, 0, 'W'},
		   {"background", 1, 0, 'K'},
		   {"min-packets", 1, 0, 'n'},
		   {"real-time", 0, 0, 'T'},
		   {0, 0, 0, 0}};

	console_utf8_enable();
	ac_crypto_init();

	textstyle(TEXT_RESET); //(TEXT_RESET, TEXT_BLACK, TEXT_WHITE);

	/* initialize a bunch of variables */

	rand_init();
	memset(&lopt, 0, sizeof(lopt));

	lopt.chanoption = 0;
	lopt.freqoption = 0;
	lopt.num_cards = 0;
	time_slept = 0;
    lopt.max_consecutive_failed_interface_reads = 2;

    lopt.channel_switching_method = channel_switching_method_fifo;
	lopt.channels = bg_chans;
	lopt.one_beacon = 1;
	lopt.singlechan = 0;
	lopt.singlefreq = 0;
	lopt.dump_prefix = NULL;
	lopt.record_data = 0;
    lopt.pcap_output.writer = NULL;
    lopt.max_node_age = 0;

    lopt.ivs.required = false;
    lopt.ivs.fp = NULL;

    lopt.gpsd.required = false;
    lopt.gpsd.fp = NULL;

    lopt.shared_key.f_xor = NULL;
    lopt.shared_key.sk_len = 0;
    lopt.shared_key.sk_len2 = 0;
    lopt.shared_key.sk_start = 0;
    memset(lopt.shared_key.sharedkey, '\x00', sizeof(lopt.shared_key.sharedkey));

    lopt.encryption_filter = 0;
	lopt.asso_client = 0;

	lopt.active_scan_sim = 0;
	lopt.update_interval_seconds = 0;
	lopt.decloak = 1;
	lopt.is_berlin = 0;
	lopt.maxnumaps = 0;
	lopt.berlin = 120;
	lopt.show_ap = 1;
	lopt.show_sta = 1;
	lopt.show_ack = 0;
	lopt.hide_known = 0;
	lopt.maxsize_essid_seen = 5; // Initial value: length of "ESSID"
	lopt.show_manufacturer = 0;
	lopt.show_uptime = 0;
    lopt.frequency_hop_millisecs = DEFAULT_HOPFREQ;
	lopt.s_file = NULL;
	lopt.s_iface = NULL;
	lopt.pcap_reader_context = NULL;
	lopt.detect_anomaly = 0;
	lopt.airodump_start_time = NULL;
    lopt.manufacturer_list = NULL;

    lopt.channel_hopper_pipe[0] = -1;
    lopt.channel_hopper_pipe[1] = -1;

    lopt.signal_event_pipe[0] = -1;
    lopt.signal_event_pipe[1] = -1;

    lopt.input_thread_pipe[0] = -1;
    lopt.input_thread_pipe[1] = -1; 

    lopt.pcap_output.required = true;

    lopt.log_csv.fp = NULL;
    lopt.log_csv.required = true;

    dump_contexts_initialise(&lopt, true);

	lopt.file_write_interval = 5; // Write file every 5 seconds by default
	lopt.maxsize_wps_seen = 6;
	lopt.show_wps = 0;
    lopt.interactive_mode = -1;
    lopt.sys_name[0] = '\0';
    lopt.loc_name[0] = '\0';
    lopt.filter_seconds = ONE_HOUR;
    lopt.file_reset_seconds = ONE_MIN;
    lopt.do_exit = 0;
	lopt.min_pkts = 2;
	lopt.relative_time = false;
#ifdef CONFIG_LIBNL
	lopt.htval = CHANNEL_NO_HT;
#endif

    essid_filter_context_initialise(&lopt.essid_filter);

	TAILQ_INIT(&lopt.na_list);
	TAILQ_INIT(&lopt.ap_list);
	TAILQ_INIT(&lopt.sta_list);

    reset_selections(&lopt);

    lopt.message[0] = '\0';

	gettimeofday(&tv0, NULL);

	lt = localtime(&tv0.tv_sec);

    for (i = 0; i < MAX_CARDS; i++)
	{
        lopt.channel[i] = channel_sentinel;
        lopt.frequency[i] = frequency_sentinel;
        lopt.wi_consecutive_failed_reads[i] = 0;
	}

    MAC_ADDRESS_CLEAR(&lopt.f_bssid);
    MAC_ADDRESS_CLEAR(&lopt.f_netmask);

	/* check the arguments */

    for (i = 0; long_options[i].name != NULL; i++)
    {
		; /* Do nothing. */
    }
	num_opts = i;

	for (i = 0; i < argc; i++) // go through all arguments
	{
		found = 0;
		if (strlen(argv[i]) >= 3)
		{
			if (argv[i][0] == '-' && argv[i][1] != '-')
			{
				// we got a single dash followed by at least 2 chars
				// lets check that against our long options to find errors
				for (size_t j = 0; j < num_opts; j++)
				{
					if (strcmp(argv[i] + 1, long_options[j].name) == 0)
					{
						// found long option after single dash
						found = 1;
						if (i > 1 && strcmp(argv[i - 1], "-") == 0)
						{
							// separated dashes?
							printf("Notice: You specified \"%s %s\". Did you "
								   "mean \"%s%s\" instead?\n",
								   argv[i - 1],
								   argv[i],
								   argv[i - 1],
								   argv[i]);
						}
						else
						{
							// forgot second dash?
							printf("Notice: You specified \"%s\". Did you mean "
								   "\"-%s\" instead?\n",
								   argv[i],
								   argv[i]);
						}
						break;
					}
				}
				if (found)
				{
					sleep(3);
					break;
				}
			}
		}
	}

	do
	{
		option_index = 0;

		option
			= getopt_long(argc,
						  argv,
						  "b:c:egiw:s:t:u:m:d:N:R:aHDB:Ahf:r:EC:o:x:MUI:WK:n:T:F:P:v:",
                          long_options,
						  &option_index);

		if (option < 0) break;

		switch (option)
		{
			case 0:

				break;

			case ':':
			case '?':

				printf("\"%s --help\" for help.\n", argv[0]);
				program_exit_code = EXIT_FAILURE;
				goto done;

			case 'K':
			{
				char * invalid_str = NULL;
				long int bg_mode = strtol(optarg, &invalid_str, 10);

				if ((invalid_str && *invalid_str != 0)
					|| !(bg_mode == 0 || bg_mode == 1))
				{
					printf("Invalid background mode. Must be '0' or '1'\n");
					program_exit_code = EXIT_FAILURE;
					goto done;
				}
                lopt.interactive_mode = !bg_mode;
				break;
			}
			case 'I':

				if (!is_string_number(optarg))
				{
					printf("Error: Write interval is not a number (>0). "
						   "Aborting.\n");
    				program_exit_code = EXIT_FAILURE;
    				goto done;
    			}

				lopt.file_write_interval = (int) strtol(optarg, NULL, 10);

				if (lopt.file_write_interval <= 0)
				{
					printf("Error: Write interval must be greater than 0. "
						   "Aborting.\n");
					program_exit_code = EXIT_FAILURE;
					goto done;
				}
				break;

			case 'T':
				lopt.relative_time = true;
				break;

			case 'E':
				lopt.detect_anomaly = 1;
				break;

			case 'e':

				lopt.one_beacon = 0;
				break;

			case 'a':

				lopt.asso_client = 1;
				break;

			case 'A':

				lopt.show_ack = 1;
				break;

			case 'h':

				lopt.hide_known = 1;
				break;

			case 'D':

				lopt.decloak = 0;
				break;

			case 'M':

				lopt.show_manufacturer = 1;
				break;

			case 'U':
				lopt.show_uptime = 1;
				break;

			case 'W':

				lopt.show_wps = 1;
				break;

			case 'c':

				if (lopt.channel[0] > 0 || lopt.chanoption == 1)
				{
                    if (lopt.chanoption == 1)
                    {
						printf("Notice: Channel range already given\n");
                    }
                    else
                    {
						printf("Notice: Channel already given (%d)\n",
                               lopt.channel[0]);
                    }
					break;
				}

				lopt.channel[0] = get_channels(&lopt.own_channels, optarg);

				if (lopt.channel[0] < 0)
				{
					airodump_usage();
					program_exit_code = EXIT_FAILURE;
					goto done;
				}

				lopt.chanoption = 1;

				if (lopt.channel[0] == 0)
				{
					lopt.channels = lopt.own_channels;
				}
                else
                {
                    lopt.channels = bg_chans;
                }
				break;

			case 'C':

				if (lopt.channel[0] > 0 || lopt.chanoption == 1)
				{
                    if (lopt.chanoption == 1)
                    {
						printf("Notice: Channel range already given\n");
                    }
                    else
                    {
						printf("Notice: Channel already given (%d)\n",
                               lopt.channel[0]);
                    }
					break;
				}

				if (lopt.freqoption == 1)
				{
					printf("Notice: Frequency range already given\n");
					break;
				}

				lopt.freqstring = optarg;
				lopt.freqoption = 1;

				break;

			case 'b':

				if (lopt.chanoption == 1)
				{
					printf("Notice: Channel range already given\n");
					break;
				}
				freq[0] = freq[1] = 0;

				for (i = 0; i < (int) strlen(optarg); i++)
				{
                    if (optarg[i] == 'a')
                    {
						freq[1] = 1;
                    }
                    else if (optarg[i] == 'b' || optarg[i] == 'g')
                    {
						freq[0] = 1;
                    }
					else
					{
						printf("Error: invalid band (%c)\n", optarg[i]);
						printf("\"%s --help\" for help.\n", argv[0]);

                        program_exit_code = EXIT_FAILURE;
						goto done;
					}
				}

                if (freq[1] + freq[0] == 2)
                {
					lopt.channels = abg_chans;
                }
                else if (freq[1] == 1)
                {
                    lopt.channels = a_chans;
                }
                else
                {
                    lopt.channels = bg_chans;
                }

				break;

			case 'i':

				// Reset output format if it's the first time the option is
				// specified
				if (output_format_first_time)
				{
					output_format_first_time = 0;

                    lopt.pcap_output.required = false;
                    lopt.log_csv.required = false;

                    dump_contexts_initialise(&lopt, false);
				}

                if (lopt.pcap_output.required)
				{
					airodump_usage();
					fprintf(stderr,
							"Invalid output format: IVS and PCAP "
							"format cannot be used together.\n");

					program_exit_code = EXIT_FAILURE;
					goto done;
				}

				lopt.ivs.required = true;
				break;

			case 'g':

                lopt.gpsd.required = true;
				break;

			case 'w':

				if (lopt.dump_prefix != NULL)
				{
					printf("Notice: dump prefix already given\n");
					break;
				}
				/* Write prefix */
				lopt.dump_prefix = optarg;
				lopt.record_data = 1;
				break;

			case 'r':

				if (lopt.s_file != NULL)
				{
					printf("Packet source already specified.\n");
					printf("\"%s --help\" for help.\n", argv[0]);
    				program_exit_code = EXIT_FAILURE;
    				goto done;
    			}
				lopt.s_file = optarg;
				break;

			case 's':

                if (strtol(optarg, NULL, 10) >= channel_switching_method_COUNT
                    || errno == EINVAL)
				{
					airodump_usage();
    				program_exit_code = EXIT_FAILURE;
    				goto done;
    			}
                if (lopt.channel_switching_method != channel_switching_method_fifo)
				{
					printf("Notice: switching method already given\n");
					break;
				}
                lopt.channel_switching_method = (int)strtol(optarg, NULL, 10);
				break;

			case 'u':

			    lopt.update_interval_seconds = (int)strtol(optarg, NULL, 10);

				/* If failed to parse or value < 0, use default, 100ms */
				if (lopt.update_interval_seconds < 0)
				{
					lopt.update_interval_seconds = 0;
                }

				break;

			case 'f':

                lopt.frequency_hop_millisecs = (int)strtol(optarg, NULL, 10);

				/* If failed to parse or value <= 0, use default, 100ms */
                if (lopt.frequency_hop_millisecs <= 0)
                {
                    lopt.frequency_hop_millisecs = DEFAULT_HOPFREQ;
                }

				break;

			case 'B':

				lopt.is_berlin = 1;
				lopt.berlin = (int) strtol(optarg, NULL, 10);
				if (lopt.berlin <= 0)
				{
					lopt.berlin = 120;
				}

				break;

			case 'm':

				if (!MAC_ADDRESS_IS_EMPTY(&lopt.f_netmask))
				{
					printf("Notice: netmask already given\n");
					break;
				}
				if (getmac(optarg, 1, (uint8_t *)&lopt.f_netmask) != 0)
				{
					printf("Notice: invalid netmask\n");
					printf("\"%s --help\" for help.\n", argv[0]);
					program_exit_code = EXIT_FAILURE;
					goto done;
				}
				break;

			case 'd':

				if (!MAC_ADDRESS_IS_EMPTY(&lopt.f_bssid))
				{
					printf("Notice: bssid already given\n");
					break;
				}
				if (getmac(optarg, 1, (uint8_t *)&lopt.f_bssid) != 0)
				{
					printf("Notice: invalid bssid\n");
					printf("\"%s --help\" for help.\n", argv[0]);

					program_exit_code = EXIT_FAILURE;
					goto done;
				}
				break;

			case 'N':
                essid_filter_context_add_essid(&lopt.essid_filter, optarg);
				break;

			case 'R':

#ifdef HAVE_PCRE
                {
                    char const * pcreerror;
                    int pcreerroffset;

                    int const added =
                        essid_filter_context_add_regex(
                            &lopt.essid_filter, 
                            optarg, 
                            &pcreerror, 
                            &pcreerroffset);

                    if (added < 0)
                    {
                        printf("Error: ESSID regular expression already given. "
                               "Aborting\n");

                        program_exit_code = EXIT_FAILURE;
                        goto done;
                    }
                    else if (added == 0)
                    {
                        printf("Error: regular expression compilation failed at "
                               "offset %d: %s; aborting\n",
                               pcreerroffset,
                               pcreerror);

                        program_exit_code = EXIT_FAILURE;
                        goto done;
                    }
                }
#else
				printf("Error: Airodump-ng wasn't compiled with PCRE support; "
					   "aborting\n");
                program_exit_code = EXIT_FAILURE;
                goto done;
#endif

				break;

			case 't':

                lopt.encryption_filter = set_encryption_filter(optarg);
				break;

			case 'n':

				lopt.min_pkts = strtoul(optarg, NULL, 10);
				break;

            case 'X':
                strlcpy(lopt.sys_name, optarg, sizeof lopt.sys_name);
                break;

            case 'y':
                strlcpy(lopt.loc_name, optarg, sizeof lopt.loc_name);
                break;

            case 'F':
                lopt.filter_seconds = strtoul(optarg, NULL, 10);
                break;

            case 'P':
                reset_val = strtoul(optarg, NULL, 10);
                lopt.file_reset_seconds = reset_val * ONE_MIN;
                break;

            case 'v':
				lopt.max_node_age = strtoul(optarg, NULL, 10) * ONE_MIN;
                break;

            case 'o':

				// Reset output format if it's the first time the option is
				// specified
				if (output_format_first_time)
				{
					output_format_first_time = 0;

                    lopt.pcap_output.required = false;
                    lopt.log_csv.required = false;

                    dump_contexts_initialise(&lopt, false);
                }

				// Parse the value
				output_format_string = strtok(optarg, ",");
				while (output_format_string != NULL)
				{
					if (strlen(output_format_string) > 0)
					{
						if (strncasecmp(output_format_string, "csv", 3) == 0
							|| strncasecmp(output_format_string, "txt", 3) == 0)
						{
                            lopt.dump[dump_type_csv].needed = true;
						}
						else if (strncasecmp(output_format_string, "pcap", 4)
									 == 0
								 || strncasecmp(output_format_string, "cap", 3)
										== 0)
						{
                            if (lopt.ivs.required)
							{
								airodump_usage();
								fprintf(stderr,
										"Invalid output format: IVS "
										"and PCAP format cannot be "
										"used together.\n");
								program_exit_code = EXIT_FAILURE;
								goto done;
							}
                            lopt.pcap_output.required = true;
						}
						else if (strncasecmp(output_format_string, "ivs", 3)
								 == 0)
						{
                            if (lopt.pcap_output.required)
							{
								airodump_usage();
								fprintf(stderr,
										"Invalid output format: IVS "
										"and PCAP format cannot be "
										"used together.\n");
								program_exit_code = EXIT_FAILURE;
								goto done;
							}
                            lopt.ivs.required = true;
						}
						else if (strncasecmp(output_format_string, "kismet", 6)
								 == 0)
						{
                            lopt.dump[dump_type_kismet_csv].needed = true;
						}
						else if (strncasecmp(output_format_string, "gps", 3)
								 == 0)
						{
                            lopt.gpsd.required = true;
						}
						else if (strncasecmp(output_format_string, "netxml", 6)
									 == 0
								 || strncasecmp(
										output_format_string, "newcore", 7)
										== 0
								 || strncasecmp(
										output_format_string, "kismet-nc", 9)
										== 0
								 || strncasecmp(
										output_format_string, "kismet_nc", 9)
										== 0
								 || strncasecmp(output_format_string,
												"kismet-newcore",
												14)
										== 0
								 || strncasecmp(output_format_string,
												"kismet_newcore",
												14)
										== 0)
						{
                            lopt.dump[dump_type_kismet_netxml].needed = true;
						}
						else if (strncasecmp(output_format_string, "logcsv", 6)
								 == 0)
						{
                            lopt.log_csv.required = true;
						}
                        else if (strncasecmp(output_format_string, "wifi_scanner", 12) == 0)
                        {
                            lopt.dump[dump_type_wifi_scanner].needed = true;
                        }
                        else if (strncasecmp(output_format_string, "default", 7)
								 == 0)
						{
                            lopt.pcap_output.required = true;
                            lopt.log_csv.required = true;

                            dump_contexts_initialise(&lopt, true);
                        }
						else if (strncasecmp(output_format_string, "none", 4)
								 == 0)
						{
                            lopt.pcap_output.required = false;
                            lopt.log_csv.required = false;
                            lopt.gpsd.required = false;
                            lopt.ivs.required = false;

                            dump_contexts_initialise(&lopt, false);
                        }
						else
						{
							// Display an error if it does not match any value
							fprintf(stderr,
									"Invalid output format: <%s>\n",
									output_format_string);
							program_exit_code = EXIT_FAILURE;
							goto done;
						}
					}
					output_format_string = strtok(NULL, ",");
				}

				break;

			case 'H':
				airodump_usage();
				program_exit_code = EXIT_FAILURE;
				goto done;

			case 'x':

				lopt.active_scan_sim = (int) strtol(optarg, NULL, 10);

                if (lopt.active_scan_sim < 0)
                {
                    lopt.active_scan_sim = 0;
                }
				break;

			case '2':
#ifndef CONFIG_LIBNL
				printf("HT Channel unsupported\n");

				program_exit_code = EXIT_FAILURE;
				goto done;
#else
				lopt.htval = CHANNEL_HT20;
#endif
				break;
			case '3':
#ifndef CONFIG_LIBNL
				printf("HT Channel unsupported\n");

				program_exit_code = EXIT_FAILURE;
				goto done;
#else
				lopt.htval = CHANNEL_HT40_MINUS;
#endif
				break;
			case '5':
#ifndef CONFIG_LIBNL
				printf("HT Channel unsupported\n");

				program_exit_code = EXIT_FAILURE;
				goto done;
#else
				lopt.htval = CHANNEL_HT40_PLUS;
#endif
				break;

			default:
				airodump_usage();
				program_exit_code = EXIT_FAILURE;
				goto done;
		}
	} while (1);

	if ((argc - optind) != 1 && lopt.s_file == NULL)
	{
		if (argc == 1)
		{
			airodump_usage();
		}
		if (argc - optind == 0)
		{
			printf("No interface specified.\n");
		}
		if (argc > 1)
		{
			printf("\"%s --help\" for help.\n", argv[0]);
		}

		program_exit_code = EXIT_FAILURE;
		goto done;
	}

	if ((argc - optind) == 1)
	{
        lopt.s_iface = argv[argc - 1];
    }

	if (!MAC_ADDRESS_IS_EMPTY(&lopt.f_netmask)
		&& MAC_ADDRESS_IS_EMPTY(&lopt.f_bssid))
	{
		printf("Notice: specify bssid \"--bssid\" with \"--netmask\"\n");
		printf("\"%s --help\" for help.\n", argv[0]);
		program_exit_code = EXIT_FAILURE;
		goto done;
	}

	if (lopt.show_wps && lopt.show_manufacturer)
	{
		lopt.maxsize_essid_seen += lopt.maxsize_wps_seen;
    }

	if (lopt.s_iface != NULL)
	{
        lopt.num_cards = initialise_cards(lopt.s_iface, wi);

		if (lopt.num_cards <= 0 || lopt.num_cards >= MAX_CARDS)
		{
			printf("Failed initializing wireless card(s): %s\n", lopt.s_iface);
			program_exit_code = EXIT_FAILURE;
			goto done;
		}

		if (lopt.freqoption && lopt.freqstring != NULL) // use frequencies
		{
            struct detected_frequencies_st detected_frequencies;

            detect_frequencies(wi[0], &detected_frequencies);

            lopt.frequency[0] =
                getfrequencies(&lopt.own_frequencies, 
                               &detected_frequencies, 
                               lopt.freqstring);

            detected_frequencies_cleanup(&detected_frequencies);

			if (lopt.frequency[0] == invalid_frequency)
			{
				printf("No valid frequency given.\n");
				program_exit_code = EXIT_FAILURE;
				goto done;
			}

            rearrange_frequencies(lopt.own_frequencies);

            if (lopt.frequency[0] == frequency_sentinel)
			{
                /* Start a child process to hop between frequencies. */
                size_t const freq_count = 
                    get_frequency_count(lopt.own_frequencies, false);

                start_frequency_hopper_process(&lopt, wi, freq_count);
			}
			else
			{
				for (size_t i = 0; i < lopt.num_cards; i++)
				{
					wi_set_freq(wi[i], lopt.frequency[0]);
					lopt.frequency[i] = lopt.frequency[0];
				}
				lopt.singlefreq = 1;
			}
		}
		else // use channels
		{
            if (lopt.channel[0] == channel_sentinel)
			{
                /* Start a child process to hop between channels. */
                size_t const chan_count = 
                    get_channel_count(lopt.channels, false);

                start_channel_hopper_process(&lopt, wi, chan_count);
			}
			else
			{
				for (size_t i = 0; i < lopt.num_cards; i++)
				{
#ifdef CONFIG_LIBNL
					wi_set_ht_channel(wi[i], lopt.channel[0], lopt.htval);
#else
					wi_set_channel(wi[i], lopt.channel[0]);
#endif
					lopt.channel[i] = lopt.channel[0];
				}
				lopt.singlechan = 1;
			}
		}
	}

	drop_privileges();

    /* Check if an input file was specified. */
	if (lopt.s_file != NULL)
	{
		lopt.pcap_reader_context = pcap_reader_open(lopt.s_file);
		if (lopt.pcap_reader_context == NULL)
		{
			perror("open failed");
			program_exit_code = EXIT_FAILURE;
			goto done;
		}
	}

    start_time = time(NULL);
	/* Create start time string for kismet netxml file */
    lopt.airodump_start_time = time_as_string(start_time);

	/* open or create the output files */
    if (lopt.record_data)
    {
        if (!open_output_files(&lopt))
		{
			program_exit_code = EXIT_FAILURE;
			goto done;
		}
    }

	signal_event_initialise(lopt.signal_event_pipe);

	lopt.manufacturer_list = load_oui_file();

    /* Start the GPS tracker if requested. */
    if (lopt.gpsd.required)
	{
        if (!gps_tracker_start(&lopt.gps_context, lopt.gpsd.fp, &lopt.do_exit))
		{
			program_exit_code = EXIT_FAILURE;
			goto done;
		}
    }

	tt1 = time(NULL);
	tt2 = time(NULL);
	gettimeofday(&tv3, NULL);
    gettimeofday(&last_active_scan_timestamp, NULL);

    lopt.elapsed_time = strdup("0 s");
    ALLEGE(lopt.elapsed_time != NULL);

    if (lopt.interactive_mode == -1)
	{
        lopt.interactive_mode = !is_background();
	}

    if (lopt.interactive_mode > 0)
    {
        terminal_prepare();
        lopt.sort.sort_required = true;

        int const pipe_result = pipe(lopt.input_thread_pipe);
        IGNORE_NZ(pipe_result);

        /* Only start the user input thread if running in interactive 
         * mode. 
         */
        input_thread_args.do_exit = &lopt.do_exit;
        input_thread_args.update_fd = lopt.input_thread_pipe[1];

        if (pthread_create(&lopt.input_tid, NULL, &input_thread, &input_thread_args)
                   != 0)
        {
            perror("Could not create input thread");
            program_exit_code = EXIT_FAILURE;
            goto done;
        }
    }

    while (!lopt.do_exit)
	{
		time_t current_time;

        if (lopt.interactive_mode > 0)
        {
            check_for_user_input(&lopt);
            do_quit_request_timeout_check(&lopt);
        }

        check_for_signal_events(&lopt);

        if (lopt.do_exit)
        {
            /* This flag may have been set by a signal event or user 
             * input. 
             */
            continue;
        }

        check_for_channel_hopper_data(&lopt);

        current_time = time(NULL);
		time_t const seconds_since_last_output_write = current_time - tt1;
        bool const need_to_update_dump_outputs =
            seconds_since_last_output_write >= lopt.file_write_interval;

        if (need_to_update_dump_outputs)
		{
			/* update the output files */
			tt1 = current_time;
            update_dump_output_files(&lopt);
		}

		current_time = time(NULL);
		time_t const seconds_since_last_generic_update = current_time - tt2;
        time_t const generic_update_interval_seconds = 5;
        bool const generic_update_required =
            seconds_since_last_generic_update > generic_update_interval_seconds;

        if (generic_update_required)
		{
			tt2 = current_time;

            lopt.sort.sort_required = true;

            if (lopt.gpsd.required)
			{
				gps_tracker_update(&lopt.gps_context);
			}

			/* update elapsed time */
			free(lopt.elapsed_time);
			lopt.elapsed_time = getStringTimeFromSec(difftime(tt2, start_time));
		}

        gettimeofday(&current_time_timestamp, NULL);

		if (lopt.active_scan_sim > 0)
		{
            long const cycle_time2 = 1000000UL * (current_time_timestamp.tv_sec - last_active_scan_timestamp.tv_sec)
                + (current_time_timestamp.tv_usec - last_active_scan_timestamp.tv_usec);
            bool const probe_requests_required =
                cycle_time2 > lopt.active_scan_sim * 1000;

            if (probe_requests_required)
            {
                gettimeofday(&last_active_scan_timestamp, NULL);

                send_probe_requests(wi, lopt.num_cards);
            }
		}

        cycle_time = 1000000UL * (current_time_timestamp.tv_sec - tv3.tv_sec)
            + (current_time_timestamp.tv_usec - tv3.tv_usec);

        if (cycle_time > 500000)
		{
			gettimeofday(&tv3, NULL);

			update_rx_quality(&lopt);

			if (lopt.s_iface != NULL)
			{
                if (!update_interface_cards(wi,
                                            lopt.num_cards,
                                            lopt.singlechan,
                                            lopt.channel,
                                            lopt.singlefreq,
                                            lopt.frequency,
                                            lopt.message,
                                            sizeof lopt.message,
                                            lopt.ignore_negative_one
#ifdef CONFIG_LIBNL
                                            , lopt.htval
#endif
                                            ))
                {
                    had_error = true;
                    lopt.do_exit = true;
                    continue;
                }
			}
		}

		if (lopt.pcap_reader_context != NULL)
		{
            struct timeval packet_timestamp; 

            /* Read one packet from a file. */
            if (read_one_packet_from_file(&lopt, h80211, sizeof h80211, &packet_timestamp))
            {
                read_pkts++;
                if (lopt.relative_time)
                {
                    pace_pcap_reader(&previous_timestamp,
                                     &packet_timestamp,
                                     read_pkts);
                }
            }
		}
		else if (lopt.s_iface != NULL)
		{
            /* Read a packet from each interface/card. */
            int result =
                capture_packet_from_cards(
                    &lopt,
                    wi,
                    lopt.num_cards,
                    h80211,
                    sizeof h80211);

            if (result < 0)
            {
                had_error = true;
                lopt.do_exit = true;
                continue;
            }
		}
        else
        {
			usleep(1);
        }

        if (lopt.do_exit)
        {
            lopt.do_exit = true;
            continue;
        }

		gettimeofday(&tv2, NULL);

        time_slept += 1000000UL * (tv2.tv_sec - current_time_timestamp.tv_sec)
            + (tv2.tv_usec - current_time_timestamp.tv_usec);
        bool const refresh_required =
            time_slept > REFRESH_RATE
            && time_slept > lopt.update_interval_seconds * 1000000;

        if (refresh_required)
		{
            time_slept = 0;
            do_refresh(&lopt);
		}
	}

	airodump_shutdown(&lopt, wi);

	program_exit_code = had_error ? EXIT_FAILURE : EXIT_SUCCESS;

done:
    if (lopt.interactive_mode > 0)
    {
        terminal_restore();
    }

	return program_exit_code;
}

