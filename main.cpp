/**
 * main.cpp
 * Controller for PVC Aerophone, vIII on Nucleo-144, F767ZI
 *
 * @author  Tanuj Sane
 * @since   4/26/2018
 * @version 1.0
 *
 * Changelog:
 * - 1.0 Initial commit
 */

#include "mbed.h"
#include "EthernetInterface.h"

#include "driver_board.h"
#include "osc_client.h"

#define EVER		;;
#define T_SYNC		7		/* ms */
#define T_NOTE		500		/* ms */

DriverBoard left(PC_8, PC_9, PC_10, PC_11);
DriverBoard right(PC_6, PB_15, PB_13, PB_12);

uint32_t swap_endian ( uint32_t number )
{
   uint32_t byte0, byte1, byte2, byte3;
   byte0 = (number & 0x000000FF) >> 0;
   byte1 = (number & 0x0000FF00) >> 8;
   byte2 = (number & 0x00FF0000) >> 16;
   byte3 = (number & 0xFF000000) >> 24;
   return ((byte0<<24) | (byte1 << 16) | (byte2 << 8) | (byte3 << 0));
}

/**
 * Dispatcher function for OSCMessages that calls the proper routine immediately
 */
static void osc_dispatch(OSCMessage* msg) {
	// Ensure that this message is addressed to this instrument
	char* token = strtok(msg->address, "/");
	if(strcmp(token, INSTRUMENT_NAME) != 0) {
		printf("Unrecognized address %s\r\n", token);
		return;
	}

	// Get the desired function call and dispatch the data to it
	token = strtok(NULL, "/");
	if(strcmp(token, "play") == 0) {
		// Cheat a bit and just check that the format is correct without actually using it to extract the data
		if(strcmp(msg->format, ",ii") != 0) {
			printf("Incorrect arguments (%s) for %s()\r\n", msg->format, token);
			return;
		}

		uint32_t pitch, velocity;
		memcpy(&pitch, msg->data, sizeof(uint32_t));
		memcpy(&velocity, msg->data + sizeof(uint32_t), sizeof(uint32_t));

		pitch = swap_endian(pitch);
		velocity = swap_endian(velocity);

		if(pitch < 48) left.play(pitch, velocity);
		else if(pitch < 60) right.play(pitch, velocity);
		else /* SKIP */ ;	// TODO: Handle C4 separately
	}
	else {
		printf("Unrecognized address %s\r\n", token);
		return;
	}
}

int main() {
	// Enable the DriverBoards (drive RST to high)
	left.init(); right.init();
	left.sync(-2);
	right.sync(-2);

	EthernetInterface eth; eth.connect();
	printf("Connected at %s\r\n", eth.get_ip_address());

	OSCClient osc(&eth); osc.connect();
	printf("Controller found at %s! Registered as %s\r\n", osc.get_controller_ip(), INSTRUMENT_NAME);

	OSCMessage* msg = (OSCMessage*) malloc(sizeof(OSCMessage));
	nsapi_size_or_error_t size_or_error;

	Timer sync_timer; sync_timer.start();
	Timer note_timer; note_timer.start();
	int elapsed = 0; int pitch = 36;

	for(EVER) {		// I'm hilarious
		// Poll for an incoming OSCMessage and dispatch it
		size_or_error = osc.receive(msg);
		if(size_or_error == NSAPI_ERROR_WOULD_BLOCK) /* Skip */;
		else if(size_or_error <= 0) {
			printf("ERROR! %d\r\n", size_or_error);
		}
		else osc_dispatch(msg);

		elapsed = note_timer.read_ms();
		/*
		if(elapsed >= T_NOTE) {
			if(pitch < 48) left.play(pitch++, 127);
			else right.play(pitch++, 127);

			if(pitch > 60) pitch = 36;
			note_timer.reset();
		}
		*/

		// Synchronize the internal state out to the DriverBoard pins at the desired frequency
		elapsed = sync_timer.read_ms();
		if(elapsed >= T_SYNC) {
			left.sync(elapsed);
			right.sync(elapsed);
			sync_timer.reset();
		}
	}
}
