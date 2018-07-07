#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <termios.h>
#include <usb.h>
#include <linux/input.h>
#include <time.h>
#include <string.h>
#include <signal.h>
#include <sys/prctl.h>
#include <libconfig.h>
#include <syslog.h>
#include <json-c/json.h>
#include <curl/curl.h>

#define UNPRIVILEGED_USER  65534 // nobody
#define UNPRIVILEGED_GROUP 65534 // nobody

static int keep_going = 1;
static int upload_to_cloud = 1;
static int run_as_daemon = 0;

static char *config_file = NULL;
static const char *wixel_connection_type;

static const char *api_endpoint;
static const char *user_email;
static const char *api_token;

typedef struct sensor_data {
	char capture_timestamp[26];
	int relative_time;
	int transmitter_id;
	int raw_value;
	int filtered_value;
	int battery_life;
	int received_signal_strength;
	int transmission_id;
	double bg_value;
} sensor_data_t;

/* Convert raw value from sensor to bloog glucose level in mmol/l */
double raw_to_bg(int raw_value, int filtered_value) 
{
	// This is just a simple calculation with not much scientific backround.
	// It was originally written just for testing. However, it seems to 
	// work quite nicely so it was kept this way.
	
	int raw = (raw_value + filtered_value) / 2;
	double bg = (double)raw / (double)(1250*18);
	
	return bg;
}

struct curl_fetch_st {
	char *payload;
	size_t size;
};


/* Standard daemonizing */
void daemonize()
{
	pid_t pid;
	
	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}
	else if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	if (setsid() < 0) {
		exit(EXIT_FAILURE);
	}

	pid = fork();
	if (pid < 0) {
		exit(EXIT_FAILURE);
	}
	else if (pid > 0) {
		exit(EXIT_SUCCESS);
	}
	
	umask(0);
	chdir("/");

	/* Close all open file descriptors */
	int x;
	for (x = sysconf(_SC_OPEN_MAX); x>=0; x--) {
		close(x);
	}

 	openlog("sswixelbridge", LOG_PID, LOG_DAEMON);
}


/* Basic signal handling */
void handle_signal(int signal)
{
	syslog(LOG_INFO, "Handle signal, %d", signal);
	
	switch (signal)
	{
		// "Hard" exit. Stop immediately, don't wait for main loop
		// to finish.
		case SIGTERM:
		case SIGINT:
			syslog(LOG_INFO, "Sokeriseuranta Wixel Bridge shutting down...");
			keep_going = 0;
			
			// Close all open file descriptors, including Wixel.
			int x;
			for (x = sysconf(_SC_OPEN_MAX); x>=0; x--) {
				close(x);
			}
			
			closelog();
			exit(EXIT_SUCCESS);
			
			break;
			
		// "Soft" exit. Just inform the main loop not to continue,
		// and let the program exit by itself.
		case SIGHUP:
			syslog(LOG_INFO, "Sokeriseuranta Wixel Bridge shutting down...");
			keep_going = 0;
			break;
	}
}

/* Standard command line argument handling */
void read_params(int argc, char **argv)
{
	int option;
	
	while ((option = getopt (argc, argv, "c::d::l")) != -1)
	{
		switch (option)
		{
			case 'c':
				config_file = malloc(strlen(optarg) + 1);
				break;
			case 'd':
				run_as_daemon = 1;
				break;
			case 'l':
				upload_to_cloud = 0;
				break;
			default:
				break;
		}
	}
}

/* Read config file using libconfig */
int read_config(char *config_filename) 
{
	config_t cfg;
	config_init(&cfg);
	
	syslog(LOG_INFO, "Reading config file %s", config_filename);
	
	if (!config_read_file(&cfg, config_filename)) {
		syslog(LOG_ERR, "Unable to read config file %s", config_filename);
		config_destroy(&cfg);
		
		return EXIT_FAILURE;
	}
	
	config_lookup_string(&cfg, "api_endpoint", &api_endpoint);
	config_lookup_string(&cfg, "api_token", &api_token);
	config_lookup_string(&cfg, "user_email", &user_email);
	config_lookup_string(&cfg, "wixel_connection_type", &wixel_connection_type);
}

/* Keep reading the file descriptor fd until expected character is encountered */
void read_until(int fd, char *buffer, char expected, size_t buf_len)
{
	int pos = 0;
	int fail_count = 0;
	time_t t1 = time(NULL);
	double timeout = 370;
	
	while(pos < buf_len) {
		// Read one character at a time to the correct buffer posision.
		// Check status after that.
		ssize_t bytes_read = read(fd, buffer+pos, 1);
		
		// Found the expected character - all done.
		if( buffer[pos] == expected ) {
			break;
		}
		
		
		// As a failsafe measure, check also the time that has been used.
		// If more than timeout seconds have passed, reset the buffer to
		// zero so that it is recognized as invalid, and quit.
		time_t t2 = time(NULL);
		double diff = difftime(t2, t1);
		if (diff >= timeout) {
			memset(buffer, 0, buf_len);
			syslog(LOG_DEBUG, "Timeout on reading Wixel");
			break;
		}
		
		if (bytes_read > 0) {
			// Everything's OK; reset fail count and advance buffer position.
			pos++;
			fail_count = 0;
		}
		else {
			// Reasons to quit:
			// 1. Wixel file descriptor has become invalid. Sometimes this happend, and
			//    it may appear as a different device file to the system.
			// 2. Too many failed attempts to read.
			
			fail_count++;
			
			if (fcntl(fd, F_GETFD) == -1 || errno == EBADF || errno == ENOENT) {
				syslog(LOG_DEBUG, "Wixel file has changed or file descriptor is not valid");
				memset(buffer, 0, buf_len);
				break;
			}
			
			if (fail_count > 5) {
				syslog(LOG_WARNING, "Too many read failures from Wixel");
				memset(buffer, 0, buf_len);
				break;
			}
			
			sleep(5);
		}
	}
}

/* Read data from Wixel (serial port) and create a sensor data struct */
sensor_data_t* read_wixel(int fd)
{
	syslog(LOG_DEBUG, "Reading Wixel");
	char buf[100] = {0};
	read_until(fd, buf, '\n', 100);
	syslog(LOG_DEBUG, "Wixel data buffer: %s", buf);
	
	// TODO check string start, should be "sokeriseuranta:", required correct Wixel software.
	
	
	// Input data is a string with space-delimited values. Use token extraction
	// to split the data to individual values. Also convert them to integer, at lest
	// for now everything is numeric values.
	int values[8] = {0};
	char *token = strtok(buf, " ");
	int n = 0;
	while (token != NULL) {
		values[n++] = atoi(token);
		token = strtok(NULL, " ");
	}
	
	sensor_data_t *data = malloc(sizeof(sensor_data_t));
	
	time_t now;
	time(&now);
	struct tm* tm_info = localtime(&now);
	strftime(data->capture_timestamp, 26, "%Y-%m-%d %H:%M:%S", tm_info);
	
	data->relative_time = 0;
	data->transmitter_id = values[0];
	data->raw_value = values[1];
	data->filtered_value = values[2];
	data->battery_life = values[3];
	data->received_signal_strength = values[4];
	data->transmission_id = values[5];
	data->bg_value = raw_to_bg(data->raw_value, data->filtered_value);
	
	// If, for any reasong, the bg value is zero, it is considered as invalid.
	// Return null to inform rest of the code not to handle this anymore.
	if (data->bg_value == 0) {
		free(data);
		data = NULL;
	}
	
	return data;
}

/* Try to locate the Wixel device. */
int find_wixel(const char *datatype) 
{
	// On Raspberry Pi, Wixel appears as ttyAMA0 when it is connected
	// via GPIO pins, or ttyACM* if it is connected via USB.
	
	int wixel = 0;
	int failcount = 3;
	
	while (failcount >= 0 && wixel == 0) {
		if (strcmp(datatype, "serial") == 0) {
			wixel = open("/dev/ttyAMA0", O_RDONLY | O_NOCTTY);
		}
		else if (strcmp(datatype, "usb") == 0) {
			if (access("/dev/ttyACM0", F_OK) != -1) {
				syslog(LOG_DEBUG, "Opening ACM0");
				wixel = open("/dev/ttyACM0", O_RDONLY | O_NOCTTY);
			}
			else if (access("/dev/ttyACM1", F_OK) != -1) {
				syslog(LOG_DEBUG, "Opening ACM1");
				wixel = open("/dev/ttyACM1", O_RDONLY | O_NOCTTY);
			}
			else if (access("/dev/ttyACM2", F_OK) != -1) {
				syslog(LOG_DEBUG, "Opening ACM2");
				wixel = open("/dev/ttyACM2", O_RDONLY | O_NOCTTY);
			}
			else if (access("/dev/ttyACM3", F_OK) != -1) {
				syslog(LOG_DEBUG, "Opening ACM3");
				wixel = open("/dev/ttyACM3", O_RDONLY | O_NOCTTY);
			}
		}
		
		failcount--;
	}
	
	
	// If Wixel is found, set standard 8N1 serial mode.
	if (wixel != -1) {
		struct termios serial_port_settings;
		
		tcgetattr(wixel, &serial_port_settings);
		cfsetispeed(&serial_port_settings,B9600);
		cfsetospeed(&serial_port_settings,B9600);
		
		/* 8N1 Mode */
		serial_port_settings.c_cflag &= ~PARENB;   /* Disables the Parity Enable bit(PARENB),So No Parity   */
		serial_port_settings.c_cflag &= ~CSTOPB;   /* CSTOPB = 2 Stop bits,here it is cleared so 1 Stop bit */
		serial_port_settings.c_cflag &= ~CSIZE;    /* Clears the mask for setting the data size             */
		serial_port_settings.c_cflag |=  CS8;      /* Set the data bits = 8                                 */
		serial_port_settings.c_cflag &= ~CRTSCTS;       /* No Hardware flow Control                         */
		serial_port_settings.c_cflag |= CREAD | CLOCAL; /* Enable receiver,Ignore Modem Control lines       */ 
		
		
		serial_port_settings.c_iflag &= ~(IXON | IXOFF | IXANY); /* Disable XON/XOFF flow control both i/p and o/p */
		serial_port_settings.c_iflag &= ~(ICANON | ECHO | ECHOE | ISIG);
		serial_port_settings.c_cc[VTIME] = 100;
        
		if((tcsetattr(wixel, TCSANOW, &serial_port_settings)) != 0) {
			syslog(LOG_ERR, "Cannot write serial port settings");
		}
		
		tcflush(wixel, TCIFLUSH);
	}
	
	return wixel;
}

/* Basic cURL callback function, which writes received data to a buffer */
size_t curl_callback (void *contents, size_t size, size_t nmemb, void *userp) {
	// Calculate buffer size
	size_t actual_size = size * nmemb;
	
	// Cast pointer to fetch struct 
	struct curl_fetch_st *p = (struct curl_fetch_st *)userp;

	// Expand buffer to fit actual data size
	p->payload = (char *)realloc(p->payload, p->size + actual_size + 1);

	if (p->payload == NULL) {
		syslog(LOG_ERR, "Failed to expand buffer in curl_callback");
		free(p->payload);

		return -1;
	}

	// Copy contents to buffer
	memcpy(&(p->payload[p->size]), contents, actual_size);

	// set new buffer size
	p->size += actual_size;

	// Ensure null termination
	p->payload[p->size] = 0;

	return actual_size;
}

/* Basic cURL function to read or post data */
CURLcode curl_fetch_url(CURL *ch, const char *url, struct curl_fetch_st *fetch) {
	CURLcode result_code;

	// Init payload
	fetch->payload = (char *)calloc(1, sizeof(fetch->payload));

	// Check payload
	if (fetch->payload == NULL) {
		syslog(LOG_ERR, "Failed to allocate payload in curl_fetch_url");
		return CURLE_FAILED_INIT;
	}

	fetch->size = 0;

	// Set url to fetch
	curl_easy_setopt(ch, CURLOPT_URL, url);

	// set calback function
	curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, curl_callback);

	// Pass fetch struct pointer */
	curl_easy_setopt(ch, CURLOPT_WRITEDATA, (void *)fetch);

	// Set default user agent
	curl_easy_setopt(ch, CURLOPT_USERAGENT, "libcurl-agent/1.0");

	// Set timeout
	curl_easy_setopt(ch, CURLOPT_TIMEOUT, 5);

	// Enable location redirects
	curl_easy_setopt(ch, CURLOPT_FOLLOWLOCATION, 1);

	// Set maximum allowed redirects
	curl_easy_setopt(ch, CURLOPT_MAXREDIRS, 1);

	// Fetch the url
	result_code = curl_easy_perform(ch);

	return result_code;
}

/* Upload bloog glucose data to Sokeriseuranta */
int upload_data(sensor_data_t *data)
{
	CURL *ch;                                               /* curl handle */
	CURLcode result_code;                                   /* curl result code */

	enum json_tokener_error jerr = json_tokener_success;    /* json parse error */

	struct curl_fetch_st curl_fetch;                        /* curl fetch struct */
	struct curl_fetch_st *cf = &curl_fetch;                 /* pointer to fetch struct */
	struct curl_slist *headers = NULL;                      /* http headers to send with request */

	if ((ch = curl_easy_init()) == NULL) {
		syslog(LOG_ERR, "Failed to create curl handle in fetch_session");
		return 1;
	}

	headers = curl_slist_append(headers, "Accept: application/json");
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept-Charset: UTF-8");
	
	// Sokeriseuranta does API authentication by email and token, which are transferred
	// in request headers.
	
	char email_header[strlen(user_email)+15];
	strcpy(email_header, "X-User-Email: ");
	strcat(email_header, user_email);
	headers = curl_slist_append(headers, email_header);
	
	char token_header[strlen(api_token)+17];
	strcpy(token_header, "X-Access-Token: ");
	strcat(token_header, api_token);
	headers = curl_slist_append(headers, token_header);

	// Create JSON payload to match Sokeriseuranta API requirements.
	
	json_object *bgdata = json_object_new_object();
	json_object_object_add(bgdata, "date", json_object_new_string(data->capture_timestamp));
	
	char bg_as_string[9];
	snprintf(bg_as_string, 8, "%f", data->bg_value);
	json_object_object_add(bgdata, "value", json_object_new_string(bg_as_string));
	json_object_object_add(bgdata, "entry_type", json_object_new_string("sensor_bg"));
	
	json_object *log_entry = json_object_new_object();
	json_object_object_add(log_entry, "log_entry", bgdata);
	
	json_object *log_entries = json_object_new_array();
	json_object_array_add(log_entries, log_entry);
	
	json_object *json = json_object_new_object();
	json_object_object_add(json, "log_entries", log_entries);
	
	syslog(LOG_DEBUG, "JSON data to be uploaded: %s\n", json_object_to_json_string(json));

	// Set curl options
	curl_easy_setopt(ch, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(ch, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(ch, CURLOPT_POSTFIELDS, json_object_to_json_string(json));

	// Actual posting of the request
	result_code = curl_fetch_url(ch, api_endpoint, cf);

	// Cleanup
	curl_easy_cleanup(ch);
	curl_slist_free_all(headers);
	json_object_put(json);

	// Check return code
	if (result_code != CURLE_OK || cf->size < 1) {
		syslog(LOG_ERR, "Failed to fetch url (%s) - curl said: %s", api_endpoint, curl_easy_strerror(result_code));
		return 2;
	}

	// Check payload
	if (cf->payload != NULL) {
		// Success
		syslog(LOG_DEBUG, "CURL Returned: \n%s", cf->payload);
		json = json_tokener_parse_verbose(cf->payload, &jerr);
		free(cf->payload);
	} 
	else {
		syslog(LOG_ERR, "Failed to populate payload");
		free(cf->payload);
		return 3;
	}

	// Check JSON error
	if (jerr != json_tokener_success) {
		syslog(LOG_ERR, "Failed to parse json string");
		json_object_put(json);
		return 4;
	}
	
	return 0;
}


int main(int argc, char **argv)
{
	read_params(argc, argv);
	
	if (config_file == NULL) {
		config_file = "sswixelbridge.cfg";
	}
	
	read_config(config_file);
	
	if (run_as_daemon == 1) {
		daemonize();
	}
	
	syslog(LOG_INFO, "Sokeriseuranta Wixel Bridge starting...");
	
	setuid(UNPRIVILEGED_USER);
	setgid(UNPRIVILEGED_GROUP);
		
	signal(SIGTERM, handle_signal);
	signal(SIGINT, handle_signal);
	signal(SIGHUP, handle_signal);
	
	int wixel = find_wixel(wixel_connection_type);
		
	while (keep_going) {
		if (access("/dev/ttyACM0", F_OK) == -1 &&
			access("/dev/ttyACM1", F_OK) == -1 &&
			access("/dev/ttyACM2", F_OK) == -1 &&
			access("/dev/ttyACM3", F_OK) == -1) {
			
			syslog(LOG_WARNING, "No device files found for Wixel");
			sleep(15);
		}

		// Wixel is not accessible. Try to re-find it.
		if (fcntl(wixel, F_GETFD) == -1 || errno == EBADF) {
			wixel = find_wixel(wixel_connection_type);
		}
			
		sensor_data_t *data = read_wixel(wixel);
		
		if (data != NULL) {
			float value = raw_to_bg(data->raw_value, data->filtered_value);
			syslog(LOG_INFO, "Got bg value %.2f", value);
		}
		
		if (data != NULL && upload_to_cloud == 1 && data->bg_value > 0) {
			syslog(LOG_INFO, "Uploading bg value %.2f", data->bg_value);
			upload_data(data);
		}
		else {
			syslog(LOG_WARNING, "There's something wrong with received bg value. Not uploading it.");
		}
		
		close(wixel);
		free(data);
		sleep(6);
	}
	
	close(wixel);
	closelog();
	
	return EXIT_SUCCESS;
}
