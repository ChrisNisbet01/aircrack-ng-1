/*
 *  Airodump-ng text files output
 *
 *  Copyright (C) 2018-2020 Thomas d'Otreppe <tdotreppe@aircrack-ng.org>
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

#include <stdio.h>
#include <time.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h> // ftruncate
#include <sys/types.h> // ftruncate
#include <sys/time.h>
#ifdef HAVE_PCRE
#include <pcre.h>
#endif

#include "aircrack-ng/defs.h"
#include "airodump-ng.h"
#include "aircrack-ng/support/communications.h"
#include "dump_write.h"
#include "dump_write_private.h"
#include "dump_write_wifi_scanner.h"
#include "dump_csv.h"
#include "dump_kismet_csv.h"
#include "dump_kismet_netxml.h"
#include "gps.h"

extern struct communication_options opt;

extern int getFrequencyFromChannel(int channel); // "aircrack-osdep/common.h"

char * format_text_for_csv(uint8_t const * const input, size_t const len)
{
    // Unix style encoding
    char * ret;
    size_t pos;
    int contains_space_end;
    static char const hex_table[] = "0123456789ABCDEF";

    if (len == 0 || input == NULL)
    {
        ret = strdup("");
        ALLEGE(ret != NULL);

        goto done;
    }

    pos = 0;
    contains_space_end = input[0] == ' ' || input[len - 1] == ' ';

    // Make sure to have enough memory for all that stuff
    ret = malloc((len * 4) + 1 + 2);
    ALLEGE(ret != NULL);

    if (contains_space_end)
    {
        ret[pos++] = '"';
    }

    for (size_t i = 0; i < len; i++)
    {
        if (!isprint(input[i]) || input[i] == ',' || input[i] == '\\'
            || input[i] == '"')
        {
            ret[pos++] = '\\';
        }

        if (isprint(input[i]))
        {
            ret[pos++] = input[i];
        }
        else if (input[i] == '\n')
        {
            ret[pos++] = 'n';
        }
        else if (input[i] == '\r')
        {
            ret[pos++] = 'r';
        }
        else if (input[i] == '\t')
        {
            ret[pos++] = 't';
        }
        else
        {
            uint8_t const val = input[i];

            ret[pos++] = 'x';
            ret[pos++] = hex_table[(val >> 4) & 0x0f];
            ret[pos++] = hex_table[val & 0x0f];
        }
    }

    if (contains_space_end)
    {
        ret[pos++] = '"';
    }

    ret[pos++] = '\0';

    char * const rret = realloc(ret, pos);

    if (rret != NULL)
    {
        ret = rret;
    }

done:
    return ret;
}

FILE * log_csv_file_open(char const * const filename)
{
    bool success;
    FILE * fp = fopen(filename, "wb+");

    if (fp == NULL)
    {
        perror("fopen failed");
        fprintf(stderr, "Could not create \"%s\".\n", filename);

        success = false;
        goto done;
    }

    fprintf(fp,
            "LocalTime, GPSTime, ESSID, BSSID, Power, "
            "Security, Latitude, Longitude, Latitude Error, "
            "Longitude Error, Type\r\n");

    success = true;

done:
    if (!success && fp != NULL)
    {
        fclose(fp);
        fp = NULL;
    }

    return fp;
}

int dump_write_airodump_ng_logcsv_add_ap(
    FILE * fp,
    const struct AP_info * ap_cur,
    const int32_t ri_power,
    struct tm * tm_gpstime,
    float const * const gps_loc)
{
	if (ap_cur == NULL || fp == NULL)
	{
		return 0;
	}

	// Local computer time
	const struct tm * ltime = localtime(&ap_cur->tlast);
	fprintf(fp,
			"%04d-%02d-%02d %02d:%02d:%02d,",
			1900 + ltime->tm_year,
			1 + ltime->tm_mon,
			ltime->tm_mday,
			ltime->tm_hour,
			ltime->tm_min,
			ltime->tm_sec);

	// Gps time
    fprintf(fp,
			"%04d-%02d-%02d %02d:%02d:%02d,",
			1900 + tm_gpstime->tm_year,
			1 + tm_gpstime->tm_mon,
			tm_gpstime->tm_mday,
			tm_gpstime->tm_hour,
			tm_gpstime->tm_min,
			tm_gpstime->tm_sec);

	// ESSID
    fprintf(fp, "%s,", ap_cur->essid);

	// BSSID
    fprintf_mac_address(fp, &ap_cur->bssid);
    fprintf(fp, ",");


	// RSSI
    fprintf(fp, "%d,", ri_power);

	// Network Security
    if ((ap_cur->security & (STD_OPN | STD_WEP | STD_WPA | STD_WPA2)) == 0)
    {
        fputs(" ", fp);
    }
	else
	{
		if (ap_cur->security & STD_WPA2)
            fputs(" WPA2 ", fp);
        if (ap_cur->security & STD_WPA)
            fputs(" WPA ", fp);
        if (ap_cur->security & STD_WEP)
            fputs(" WEP ", fp);
        if (ap_cur->security & STD_OPN)
            fputs(" OPN", fp);
	}

    fputs(",", fp);

	// Lat, Lon, Lat Error, Lon Error
    fprintf(fp,
			"%.6f,%.6f,%.3f,%.3f,AP\r\n",
			gps_loc[gps_latitude],
			gps_loc[gps_longitude],
			gps_loc[gps_latitude_error],
			gps_loc[gps_longitude_error]);

	return (0);
}

int dump_write_airodump_ng_logcsv_add_client(
    FILE * const fp,
    const struct AP_info * ap_cur,
    const struct ST_info * st_cur,
    const int32_t ri_power,
    struct tm * tm_gpstime,
    float const * const gps_loc)
{
    if (st_cur == NULL || fp == NULL)
	{
		return 0;
	}

	// Local computer time
	struct tm * ltime = localtime(&ap_cur->tlast);
    fprintf(fp,
			"%04d-%02d-%02d %02d:%02d:%02d,",
			1900 + ltime->tm_year,
			1 + ltime->tm_mon,
			ltime->tm_mday,
			ltime->tm_hour,
			ltime->tm_min,
			ltime->tm_sec);

	// GPS time
    fprintf(fp,
			"%04d-%02d-%02d %02d:%02d:%02d,",
			1900 + tm_gpstime->tm_year,
			1 + tm_gpstime->tm_mon,
			tm_gpstime->tm_mday,
			tm_gpstime->tm_hour,
			tm_gpstime->tm_min,
			tm_gpstime->tm_sec);

	// Client => No ESSID
    fprintf(fp, ",");

	// BSSID
    fprintf_mac_address(fp, &st_cur->stmac);
    fprintf(fp, ",");


	// RSSI
    fprintf(fp, "%d,", ri_power);

	// Client => Network Security: none
    fprintf(fp, ",");

	// Lat, Lon, Lat Error, Lon Error
    fprintf(fp,
			"%.6f,%.6f,%.3f,%.3f,",
			gps_loc[gps_latitude],
			gps_loc[gps_longitude],
			gps_loc[gps_latitude_error],
			gps_loc[gps_longitude_error]);

	// Type
    fprintf(fp, "Client\r\n");

	return (0);
}

void dump_write(
    struct dump_context_st * const dump,
    struct ap_list_head * const ap_list,
    struct sta_list_head * const sta_list,
    unsigned int const f_encrypt,
    struct essid_filter_context_st const * const essid_filter)
{
    if (dump == NULL)
    {
        goto done;
    }

    dump->dump(dump->priv, ap_list, sta_list, f_encrypt, essid_filter);

done:
    return;
}

void dump_close(struct dump_context_st * const dump)
{
    if (dump == NULL)
    {
        goto done;
    }

    if (dump->close != NULL)
    {
        dump->close(dump->priv);
    }

    free(dump);

done:
    return;
}

struct dump_context_st * dump_open(
    dump_type_t const  dump_type,
    char const * const filename,
    char const * const sys_name,
    char const * const location_name,
    time_t const filter_seconds,
    int const file_reset_seconds,
    char const * const airodump_start_time,
    bool const use_gpsd)
{
    bool success;
    struct dump_context_st * dump = calloc(1, sizeof *dump);

    if (dump == NULL)
    {
        success = false;
        goto done;
    }

    switch (dump_type)
    {
        case dump_type_wifi_scanner:
            if (!wifi_scanner_dump_open(dump,
                                        filename,
                                        sys_name,
                                        location_name,
                                        filter_seconds,
                                        file_reset_seconds))
            {
                success = false;
                goto done;
            }
            break;
        case dump_type_csv:
            if (!csv_dump_open(dump,
                               filename))
            {
                success = false;
                goto done;
            }
            break;
        case dump_type_kismet_csv:
            if (!kismet_csv_dump_open(dump,
                                      filename))
            {
                success = false;
                goto done;
            }
            break;
        case dump_type_kismet_netxml:
            if (!kismet_netxml_dump_open(dump,
                                         filename,
                                         airodump_start_time,
                                         use_gpsd))
            {
                success = false;
                goto done;
            }
            break;
        default:
            success = false;
            goto done;
    }

    success = true;

done:
    if (!success)
    {
        dump_close(dump);
        dump = NULL;
    }

    return dump;
}


