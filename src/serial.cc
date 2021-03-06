#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/poll.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "globals.h"
#include "Packet.h"

static int fd;
static struct termios serialterm;

static int getbaudrate(int speed)
{
	switch (speed)
	{
		case 50:	return B50;
		case 75:	return B75;
		case 110:	return B110;
		case 134:	return B134;
		case 150:	return B150;
		case 200:	return B200;
		case 300:	return B300;
		case 600:	return B600;
		case 1200:	return B1200;
		case 1800:	return B1800;
		case 2400:	return B2400;
		case 4800:	return B4800;
		case 9600:	return B9600;
		case 19200:	return B19200;
		case 38400:	return B38400;
		case 57600:	return B57600;
		case 115200:	return B115200;
		case 230400:	return B230400;
	}

	error("%d baud is not supported", speed);
	return 0;
}

static bool pause(int milliseconds)
{
	struct pollfd p;
	p.fd = fd;
	p.events = POLLIN;
	return !!poll(&p, 1, milliseconds);
}

static void synchronise_e2()
{
	int i;

	verbose("Sync using e2 protocol\n");

	/* Set to the slow rate, and do the handshake. */

	cfsetspeed(&serialterm, getbaudrate(SlowBaudRate));
	i = tcsetattr(fd, TCSANOW, &serialterm);
	if (i == -1)
		error("Failed to set up serial port.");

	verbose("Flushing buffers...\n");

	unsigned char c;
	for (;;)
	{
		i = read(fd, &c, 1);
		if (i != 1)
			break;
	}

	printf("Waiting for device reset...\n");

	/* Keep sending 1B characters every tenth of a second until we get the
	 * 06 response. */

	do
	{
		c = 0x1B;
		i = write(fd, &c, 1);
		if (i == -1)
			error("Write error.");

		if (pause(100))
		{
			do
			{
				i = read(fd, &c, 1);
				if ((i == 1) && (c == 0x06))
					break;
			}
			while (i == 1);
		}
	}
	while (c != 0x06);

	verbose("Handshaking...\n");

	/* Keep reading 06 characters until we get a delay of more than 300ms.
	 * */

	for (;;)
	{
		if (!pause(300))
			break;

		do
		{
		again:
			i = read(fd, &c, 1);
#ifdef SPEW_TRACING
			printf(" <%02X/%d", c, i);
#endif
			if ((i == 1) && (c != 0x06))
			{
				printf("Handshake oddity --- device sent %02X "
					"when it should have sent 06!\n", c);
				goto again;
			}
		}
		while (i == 1);
	}
}

static void synchronise_e3()
{
	int i;
	struct pollfd p;

	verbose("Sync using e3 protocol\n");

	/* Set to the slow rate. */

	cfsetspeed(&serialterm, getbaudrate(SlowBaudRate));
	i = tcsetattr(fd, TCSANOW, &serialterm);
	if (i == -1)
		error("Failed to set up serial port.");


	verbose("Flushing buffers...\n");

	unsigned char c;
	for (;;)
	{
		i = read(fd, &c, 1);
		if (i != 1)
			break;
	}

	printf("Waiting for device reset...\n");

	/* The e3 doesn't seem to respond sanely the way the e2 does; it doesn't
	 * appear to tell us when the handshake is done. So instead we spam it
	 * with 0x1b bytes until it stops responding, then send it a GETVERSION
	 * packet and see whether it responds sensibly.
	 */

	for (;;)
	{
		for (;;)
		{
			/* Wait for data; max on 500ms. */

			p.fd = fd;
			p.events = POLLIN;
			if (poll(&p, 1, 500) == 0)
				break;

			/* We got some data. Read it all. */

			for (;;)
			{
				int i = read(fd, &c, 1);
				if (i == 1)
					continue;

				p.fd = fd;
				p.events = POLLIN;
				if (poll(&p, 1, 100) == 0)
					break;
			}

			/* Send a 0x1b. */

			c = 0x1b;
			i = write(fd, &c, 1);
			if (i == -1)
				error("Write error.");
		}

		/* Send the GETVERSION packet. */

		Packet pq;
		pq.request = PACKET_GETVERSION;
		pq.length = 0;
		pq.write();

		p.fd = fd;
		p.events = POLLIN;
		if (poll(&p, 1, 200) != 0)
		{
			read(fd, &c, 1);
			if (c == 0x02)
				break;
		}

		printf("Handshake failed, retrying\n");
	}

	/* Consume the rest of the packet. */

	for (;;)
	{
		struct pollfd p;
		p.fd = fd;
		p.events = POLLIN;
		if (poll(&p, 1, 100) == 0)
			break;

		read(fd, &c, 1);
	}
}

void logon()
{
	Packet p;
	int i;

	fd = open(SerialPort, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd == -1)
		error("Failed to open serial port.");

	/* Set up the serial port. */

	tcgetattr(fd, &serialterm);
	serialterm.c_cflag = CS8 | CLOCAL | CREAD;
	serialterm.c_lflag = 0;
	serialterm.c_oflag = 0;
	serialterm.c_iflag = IGNPAR;

	/* If we're retrying the old connection, set up at the fast rate --- we're
	 * relying on a previous pblq execution having set the device up.
	 * If this is a new connection, resync. */

	if (RetryConnection)
	{
		cfsetspeed(&serialterm, getbaudrate(FastBaudRate));
		int i = tcsetattr(fd, TCSANOW, &serialterm);
		if (i == -1)
			error("Failed to set up serial port.");
	}
	else
	{
		if (Protocol == 2)
			synchronise_e2();
		else
			synchronise_e3();
	}

	/* Prod device until it starts responding. */

	verbose("Getting PBL status...\n");
	for (;;)
	{
		p.request = PACKET_GETVERSION;
		p.length = 0;
		p.write();

		if (!pause(300))
			continue;

		/* It replied! */

		p.read();
		if (MaximumPacketSize == 0)
			MaximumPacketSize = p.gets(8);

		verbose("PBL V%d.%d build %d; maximum packet size 0x%X bytes\n",
			p.getb(4), p.getb(5), p.gets(6),
			p.gets(8));
		break;
	}

	/* Change baud rate, if the user wants us to. */

	if (FastBaudRate != SlowBaudRate)
	{
		verbose("Switching to %d baud...\n", FastBaudRate);

		/* Ask for the new baud rate. */

		p.request = PACKET_SETBAUD;
		p.length = 4;
		p.setq(0, FastBaudRate);
		p.write();

		p.read();
		if (p.gets(0) != 0x89)
			error("failed to change remote baud rate "
				"(response code %04X).",
				p.gets(0));

		cfsetspeed(&serialterm, getbaudrate(FastBaudRate));
		i = tcsetattr(fd, TCSADRAIN, &serialterm);
		if (i == -1)
			error("failed to change local baud rate.");

		/* Keep sending NOP packets until we get a response. */

		p.request = PACKET_GETVERSION;
		p.length = 0;

		for (;;)
		{
			p.write();

			if (pause(300))
				break;
		}

		p.read();
	}
}

void sendbyte(byte c)
{
	int i;

#ifdef SPEW_TRACING
	printf("<%02X\n", c);
#endif

	do
	{
		struct pollfd p;
		p.fd = fd;
		p.events = POLLOUT;
		if (poll(&p, 1, INT_MAX) == 0)
			break;

		i = write(fd, &c, 1);
		if (i == -1)
			error("I/O error on write: %s",
				strerror(errno));
	}
	while (i != 1);
}

byte recvbyte()
{
	int i;
	byte b;

	do
	{
		struct pollfd p;
		p.fd = fd;
		p.events = POLLIN;
		if (poll(&p, 1, INT_MAX) == 0)
			break;

		i = read(fd, &b, 1);
		if (i == -1)
			error("I/O error on read: %s",
				strerror(errno));
	}
	while (i != 1);

#ifdef SPEW_TRACING
	printf(">%02X\n", b);
#endif
	return b;
}

void dodgyterm()
{
	printf("Serial terminal starting (CTRL+C to quit)\n");

	/* Set up the serial port in slow mode. */

	fd = open(SerialPort, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd == -1)
		error("Failed to open serial port.");

	tcgetattr(fd, &serialterm);
	serialterm.c_cflag = CS8 | CLOCAL | CREAD;
	serialterm.c_lflag = 0;
	serialterm.c_oflag = 0;
	serialterm.c_iflag = IGNPAR;

	cfsetspeed(&serialterm, getbaudrate(SlowBaudRate));
	int i = tcsetattr(fd, TCSADRAIN, &serialterm);
	if (i == -1)
		error("Failed to set up serial port.");

	/* Put the console into raw mode. */

	struct termios oldconsole;
	struct termios console;
	tcgetattr(0, &oldconsole);
	console = oldconsole;
	cfmakeraw(&console);
	i = tcsetattr(0, TCSANOW, &console);
	if (i == -1)
		error("Failed to put console into raw mode.");

	/* Wait for input on either device. */

	for (;;)
	{
		struct pollfd p[2];
		p[0].fd = fd;
		p[0].events = POLLIN;
		p[1].fd = 0;
		p[1].events = POLLIN;

		poll(p, 2, INT_MAX);

		if (p[0].revents)
		{
			byte b = recvbyte();
			write(0, &b, 1);
		}

		if (p[1].revents)
		{
			byte b;
			read(0, &b, 1);
			if (b == 3)
				break;
			sendbyte(b);
		}
	}

	/* Put the console back the way it was. */

	tcsetattr(0, TCSANOW, &oldconsole);
}

