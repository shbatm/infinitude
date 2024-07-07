#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <mosquitto.h>

#include "buffy.h"
#include "crc.h"
#include "config.h"

/* raw tty code mostly from:
	http://www.cs.uleth.ca/~holzmann/C/system/ttyraw.c */

void tty_atexit(void);
int tty_reset(void);
void tty_raw(void);
int screenio(struct mosquitto *mosq);
void fatal(char *mess);
void on_connect(struct mosquitto *mosq, void *obj, int reason_code);
void on_publish(struct mosquitto *mosq, void *obj, int mid);

static struct termios orig_termios; /* TERMinal I/O Structure */
static int ttyfd = STDIN_FILENO;	/* STDIN_FILENO is 0 by default */

int main(int argc, char **argv)
{
	/* check that input is from a tty */
	if (isatty(ttyfd))
	{
		/* store current tty settings in orig_termios */
		if (tcgetattr(ttyfd, &orig_termios) < 0)
			fatal("can't get tty settings");
		/* register the tty reset with the exit handler */
		if (atexit(tty_atexit) != 0)
			fatal("atexit: can't register tty reset");
		tty_raw(); /* put tty in raw mode */
	}
	else
	{
		fprintf(stderr, "Not a tty. Reading from file...\n");
	}
	struct mosquitto *mosq;
	int rc;

	/* Required before calling other mosquitto functions */
	mosquitto_lib_init();

	/* Create a new client instance.
	 * id = NULL -> ask the broker to generate a client id for us
	 * clean session = true -> the broker should remove old sessions when we connect
	 * obj = NULL -> we aren't passing any of our private data for callbacks
	 */
	mosq = mosquitto_new(NULL, true, NULL);
	if (mosq == NULL)
	{
		fprintf(stderr, "Error: Out of memory.\n");
		return 1;
	}

	/* Configure MQTT Username and Password */
	mosquitto_username_pw_set(mosq, MQTT_USERNAME, MQTT_PASSWORD);

	/* Configure callbacks. This should be done before connecting ideally. */
	mosquitto_connect_callback_set(mosq, on_connect);
	mosquitto_publish_callback_set(mosq, on_publish);

	/* Connect to mqtt broker with a keepalive of 60 seconds.
	 * This call makes the socket connection only, it does not complete the MQTT
	 * CONNECT/CONNACK flow, you should use mosquitto_loop_start() or
	 * mosquitto_loop_forever() for processing net traffic. */
	rc = mosquitto_connect(mosq, MQTT_HOST, MQTT_PORT, 60);
	if (rc != MOSQ_ERR_SUCCESS)
	{
		mosquitto_destroy(mosq);
		fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
		return 1;
	}

	/* Run the network loop in a background thread, this call returns quickly. */
	rc = mosquitto_loop_start(mosq);
	if (rc != MOSQ_ERR_SUCCESS)
	{
		mosquitto_destroy(mosq);
		fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
		return 1;
	}

	screenio(mosq); /* run application code */
	mosquitto_lib_cleanup();
	return 0; /* tty_atexit will restore terminal */
}

#define READ_REQ 0x0b
#define WRITE_REQ 0x0c
#define REPLY 0x06
#define ERROR 0x15

#pragma pack(push, 1)

typedef struct
{
	uint8_t table;
	uint8_t row;
} careg;

typedef struct
{
	uint8_t type;
	uint8_t idx;
} cardev;

typedef struct
{
	cardev dst;
	cardev src;
	uint8_t len;
	uint16_t reserved;
	uint8_t type;
	char payload[256];
	uint16_t crc;
} carframe;

typedef struct
{
	char pad;
	careg reg;
} caread;

typedef struct
{
	char pad;
	careg reg;
	char payload[256];
} carwrite;

typedef struct
{
	char ack;
	careg reg;
} careply;

#pragma pack(pop)

/* Callback called when the client receives a CONNACK message from the broker. */
void on_connect(struct mosquitto *mosq, void *obj, int reason_code)
{
	/* Print out the connection result. mosquitto_connack_string() produces an
	 * appropriate string for MQTT v3.x clients, the equivalent for MQTT v5.0
	 * clients is mosquitto_reason_string().
	 */
	printf("on_connect: %s\n", mosquitto_connack_string(reason_code));
	if (reason_code != 0)
	{
		/* If the connection fails for any reason, we don't want to keep on
		 * retrying in this example, so disconnect. Without this, the client
		 * will attempt to reconnect. */
		mosquitto_disconnect(mosq);
	}

	/* You may wish to set a flag here to indicate to your application that the
	 * client is now connected. */
}

/* Callback called when the client knows to the best of its abilities that a
 * PUBLISH has been successfully sent. For QoS 0 this means the message has
 * been completely written to the operating system. For QoS 1 this means we
 * have received a PUBACK from the broker. For QoS 2 this means we have
 * received a PUBCOMP from the broker. */
void on_publish(struct mosquitto *mosq, void *obj, int mid)
{
	printf("Message with mid %d has been published.\n", mid);
}

/* This function pretends to read some data from a sensor and publish it.*/
void publish_sensor_data(struct mosquitto *mosq, char subtopic[10], char *payload)
{
	char topic[20];
	int rc;

	snprintf(topic, sizeof(topic), "cardump/%s", subtopic);

	/* Publish the message
	 * mosq - our client instance
	 * *mid = NULL - we don't want to know what the message id for this message is
	 * topic = "example/temperature" - the topic on which this message will be published
	 * payloadlen = strlen(payload) - the length of our payload in bytes
	 * payload - the actual payload
	 * qos = 2 - publish with QoS 2 for this example
	 * retain = false - do not use the retained message feature for this message
	 */
	rc = mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, 2, false);
	if (rc != MOSQ_ERR_SUCCESS)
	{
		fprintf(stderr, "Error publishing: %s\n", mosquitto_strerror(rc));
	}
}

int screenio(struct mosquitto *mosq)
{
	int bytes;

	buffy framebuf;
	framebuf.len = 0;
	carframe frame;
	crcInit();

	char buffer[32]; // serial reads tend to have less than 32 bytes

	uint8_t tries = 0;
	printf("Time\tFrom\tTo\tType\tLength\tHex Content\n");
	int shifts = 0;
	int syncs = 0;

	for (;;)
	{
		bytes = read(ttyfd, buffer, 32);
		if (bytes < 0)
			fatal("Read error");
		if (bytes == 0)
		{
			tries += 1;
		}
		else
		{
			tries = 0;
		}
		if (tries > 9)
			fatal("Not trying again");

		bufadd(&framebuf, buffer, bytes);

		if (framebuf.len < 4)
			continue;
		int datalen = framebuf.data[4];
		if (datalen == 0)
		{
			shifts++;
			bufshift(&framebuf, (int)(framebuf.len >> 1));
			continue;
		}

		int framelen = 10 + datalen;
		if (framebuf.len < framelen)
			continue;

		if (shifts > 0)
			fprintf(stderr, "Looking for %d byte frame in %d byte buffer\n", datalen, framebuf.len);

		if (crcFast(framebuf.data, framelen) == 0)
		{
			if (shifts > 0)
			{
				fprintf(stderr, "*** Synced stream after %d shifts ***\n", shifts);
				shifts = 0;
				syncs++;
				if (syncs > 100)
					fatal("Stream too noisy");
			}
			memcpy(&frame, framebuf.data, framelen);
			frame.crc = framebuf.data[framelen - 2] << 8 | framebuf.data[framelen - 1];

			if (READ_REQ == frame.type)
			{
				fprintf(stderr, "--------------READ from %x ------------\n", frame.dst.type);
				caread req;
				memcpy(&req, frame.payload, 3);
				fprintf(stderr, "Request for table %d, row %d\n", req.reg.table, req.reg.row);
			}

			if (WRITE_REQ == frame.type)
			{
				fprintf(stderr, "--------------WRITE to %x ------------\n", frame.dst.type);
				carwrite req;
				memcpy(&req, frame.payload, 256);
				fprintf(stderr, "Write to table %d, row %d\n", req.reg.table, req.reg.row);
				for (int i = 0; i < frame.len; i++)
					fprintf(stderr, "%02x ", req.payload[i]);
				fprintf(stderr, "\n");
			}

			if (REPLY == frame.type)
			{
				// Temperatures from Outdoor Unit
				if (frame.src.type == 0x52 && frame.payload[1] == 0x03 && frame.payload[2] == 0x02)
				{
					fprintf(stderr, "Received temperatures from outdoor unit\n");
					// for (int i = 0; i < frame.len; i++)
					// 	printf("%02x ", frame.payload[i]);
					// printf("\n");
					int16_t oat = (frame.payload[5] << 8) | frame.payload[6];	// Outdoor Air Temp
					int16_t oct = (frame.payload[9] << 8) | frame.payload[10];	// Outdoor Coil Temp
					int16_t ost = (frame.payload[13] << 8) | frame.payload[14]; // Outdoor Supply Temp
					int16_t ssh = (frame.payload[17] << 8) | frame.payload[18]; // Superheat Temp
					int16_t unk = (frame.payload[21] << 8) | frame.payload[22]; // Unknown ID 0x14B
					int16_t odt = (frame.payload[25] << 8) | frame.payload[26]; // Discharge Temp

					char mqtt_payload[256];
					snprintf(mqtt_payload, sizeof(mqtt_payload),
							 "{\"oat\": %d,\"oct\": %d,\"ost\": %d,\"ssh\": %d,\"unk\": %d,\"odt\": %d}",
							 oat / 16, oct / 16, ost / 16, ssh / 16, unk / 16, odt / 16);
					publish_sensor_data(mosq, "odu_temps", mqtt_payload);
				}
			}

			if (frame.payload[1] == 0x02)
			{
				if (frame.payload[2] == 0x02)
				{ // Time Frame
					fprintf(stderr, "Time is %d:%d\n", frame.payload[3], frame.payload[4]);
				}
				if (frame.payload[2] == 0x03)
				{ // Date Frame
					fprintf(stderr, "Date is %d-%d-%d\n", frame.payload[5] + 2000, frame.payload[4], frame.payload[3]);
				}
			}

			printf("%d\t%x\t%x\t%02x\t%d\t", (int)time(NULL), frame.src.type, frame.dst.type, frame.type, frame.len);
			for (int i = 0; i < frame.len; i++)
				printf("%02x ", frame.payload[i]);
			printf("\n");

			bufshift(&framebuf, framelen);
		}
		else
		{
			shifts++;
			bufshift(&framebuf, 1);
		}
	}
}

void fatal(char *message)
{
	fprintf(stderr, "fatal error: %s\n", message);
	exit(1);
}

/* exit handler for tty reset */
void tty_atexit(void) /* NOTE: If the program terminates due to a signal   */
{					  /* this code will not run.  This is for exit()'s     */
	printf("Exit. Reset tty to initial settings.\n");
	tty_reset(); /* only.  For resetting the terminal after a signal, */
} /* a signal handler which calls tty_reset is needed. */

/* reset tty - useful also for restoring the terminal when this process
   wishes to temporarily relinquish the tty
*/
int tty_reset(void)
{
	/* flush and reset */
	if (tcsetattr(ttyfd, TCSAFLUSH, &orig_termios) < 0)
		return -1;
	return 0;
}

void tty_raw()
{
	struct termios raw;

	raw = orig_termios; /* copy original and then modify below */

	/* input modes - clear indicated ones giving: no break, no CR to NL,
		 no parity check, no strip char, no start/stop output (sic) control */
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

	/* output modes - clear giving: no post processing such as NL to CR+NL */
	raw.c_oflag &= ~(OPOST);

	/* control modes - set 8 bit chars */
	raw.c_cflag |= (CS8);

	/* local modes - clear giving: echoing off, canonical off (no erase with
		 backspace, ^U,...),  no extended functions, no signal chars (^Z,^C) */
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

	/* control chars - set return condition: min number of bytes and timer */
	raw.c_cc[VMIN] = 5;
	raw.c_cc[VTIME] = 8; /* after 5 bytes or .8 seconds
																		after first byte seen      */
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0; /* immediate - anything       */

	raw.c_cc[VMIN] = 2;
	raw.c_cc[VTIME] = 0; /* after two bytes, no timer  */
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 8; /* after a byte or .8 seconds */

	/* put terminal in raw mode after flushing */
	if (tcsetattr(ttyfd, TCSAFLUSH, &raw) < 0)
		fatal("can't set raw mode");

	// printf("input speed was %d\n",cfgetispeed(&orig_termios));
	cfsetispeed(&orig_termios, B38400); // Set 38.4k
	cfsetospeed(&orig_termios, B38400); // Set 38.4k
										// printf("input speed set to %d\n",cfgetispeed(&orig_termios));
}
