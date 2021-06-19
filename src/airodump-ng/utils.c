#include "utils.h"
#include "aircrack-ng/defs.h"

#include <string.h>
#include <sys/wait.h>

char * time_as_string(time_t const time)
{
	char * const string = strdup(ctime(&time));

	ALLEGE(string != NULL);

	/* Remove the new line that is included by ctime(). 
     * Cripes why wouldn't it just provide the time, and not assume 
     * that it goes at the end of a line? 
     */
	if (strlen(string) > 0)
	{
		string[strlen(string) - 1] = '\0';
	}

	return string;
}

char const * create_output_filename(char * const buffer,
									size_t const buffer_size,
									char const * const prefix,
									int const index,
									char const * const suffix)
{
	if (index < 0)
	{
		snprintf(buffer, buffer_size, "%s.%s", prefix, suffix);
	}
	else
	{
		snprintf(buffer, buffer_size, "%s-%02d.%s", prefix, index, suffix);
	}

	return buffer;
}

int wait_proc(pid_t in, pid_t * out)
{
	int stat = 0;
	pid_t pid;

	do
	{
		pid = waitpid(in, &stat, WNOHANG);
	} while (pid < 0 && errno == EINTR);

	if (out != NULL)
	{
		*out = pid;
	}

	int status = -1;
	if (WIFEXITED(stat))
	{
		status = WEXITSTATUS(stat);
	}
	else if (WIFSIGNALED(stat))
	{
		status = WTERMSIG(stat);
	}

	return status;
}

void make_printable(uint8_t * const buf, size_t const buf_size)
{
	for (size_t i = 0; i < buf_size; i++)
	{
		if (buf[i] < (uint8_t) ' ')
		{
			buf[i] = '.';
		}
	}
}

static int is_filtered_netmask(mac_address const * const bssid,
							   mac_address const * const f_bssid,
							   mac_address const * const f_netmask)
{
	REQUIRE(bssid != NULL);

	mac_address mac1;
	mac_address mac2;

	for (size_t i = 0; i < sizeof mac1; i++)
	{
		/* FIXME - Do (a ^ b) & mask? */
		mac1.addr[i] = bssid->addr[i] & f_netmask->addr[i];
		mac2.addr[i] = f_bssid->addr[i] & f_netmask->addr[i];
	}

	bool const is_filtered = !MAC_ADDRESS_EQUAL(&mac1, &mac2);

	return is_filtered;
}

bool bssid_is_filtered(mac_address const * const bssid,
					   mac_address const * const f_bssid,
					   mac_address const * const f_netmask)
{
	bool is_filtered;

	if (MAC_ADDRESS_IS_EMPTY(f_bssid))
	{
		is_filtered = false;
		goto done;
	}

	if (!MAC_ADDRESS_IS_EMPTY(f_netmask))
	{
		if (is_filtered_netmask(bssid, f_bssid, f_netmask))
		{
			is_filtered = true;
			goto done;
		}
	}
	else if (!MAC_ADDRESS_EQUAL(f_bssid, bssid))
	{
		is_filtered = true;
		goto done;
	}

	is_filtered = false;

done:
	return is_filtered;
}

#define TSTP_SEC                                                               \
	1000000ULL /* It's a 1 MHz clock, so a million ticks per second! */
#define TSTP_MIN (TSTP_SEC * 60ULL)
#define TSTP_HOUR (TSTP_MIN * 60ULL)
#define TSTP_DAY (TSTP_HOUR * 24ULL)

char * parse_timestamp(unsigned long long timestamp)
{
#define TSTP_LEN 15
	static char s[TSTP_LEN];
	unsigned long long rem;
	unsigned char days, hours, mins, secs;

	// Calculate days, hours, mins and secs
	days = (uint8_t)(timestamp / TSTP_DAY);
	rem = timestamp % TSTP_DAY;
	hours = (unsigned char) (rem / TSTP_HOUR);
	rem %= TSTP_HOUR;
	mins = (unsigned char) (rem / TSTP_MIN);
	rem %= TSTP_MIN;
	secs = (unsigned char) (rem / TSTP_SEC);

	snprintf(s, sizeof s, "%3ud %02u:%02u:%02u", days, hours, mins, secs);

	return s;
}

bool essid_has_control_chars(uint8_t const * const essid,
							 size_t const essid_length)
{
	bool has_control_chars;

	for (size_t i = 0; i < essid_length; i++)
	{
		if (essid[i] > '\0' && essid[i] < ' ')
		{
			has_control_chars = true;
			goto done;
		}
	}

	has_control_chars = false;

done:
	return has_control_chars;
}

int moving_exponential_average(int const new_value,
							   int const old_value,
							   float const smoothing_factor)
{
	// Moving exponential average
	// ma_new = alpha * new_sample + (1-alpha) * ma_old;
	int smoothed_value = (int) (smoothing_factor * new_value
								+ (1.f - smoothing_factor) * old_value);

	return smoothed_value;
}
