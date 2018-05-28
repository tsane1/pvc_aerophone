/**
 * osc_client.h
 * Definition of a class for connecting to the central controller using OSC over UDP
 *
 * @author  Tanuj Sane, Andrew Walter
 * @since   4/20/2018
 * @version 1.1
 *
 * Changelog:
 * - 1.0 Initial commit
 * - 1.1 
 */

#ifndef OSC_CLIENT_H_
#define OSC_CLIENT_H_

#include "mbed.h"
#include "UDPSocket.h"
#include "SocketAddress.h"

#define BROADCAST_IP			"192.168.2.255"
#define OSC_PORT				8000
#define INSTRUMENT_NAME			"pvc_aerophone"
#define OSC_MSG_SIZE			256

typedef char byte;

// An ugly hack from a hero at https://os.mbed.com/questions/75208/Broadcast-in-mbedos-5/ to get
// around the fact that mbed-os v5 doesn't have a method for setting a UDPSocket as a broadcasting
// socket (v2 had this functionality)
// -----------------------------------------------
#include "lwip/ip.h"
#include "lwip/api.h"
 
//to do this is terribly wrong :D because this struct is not visible to the api
struct lwip_socket {
  bool in_use;
  struct netconn *conn;
  struct netbuf *buf;
  u16_t offset;
 
  void(*cb)(void *);
  void *data;
};
 
class UDPBroadcastSocket: public UDPSocket
{
public: //or you can make it private/protected and call it in the constructor
	template<typename S >
		UDPBroadcastSocket(S* stack) : UDPSocket(stack) { }
 void set_broadcast(bool broadcast) {
  //extreme violence, because we dont have yet the broadcast feature exposed in the new mbed-os lwip impl
  struct lwip_socket *s = (struct lwip_socket *)_socket;
  if (broadcast)
    s->conn->pcb.ip->so_options |= SOF_BROADCAST;
  else
    s->conn->pcb.ip->so_options &= ~SOF_BROADCAST;
 }
};
// -----------------------------------------------

enum {
	OSC_SIZE_ADDRESS = 64,
	OSC_SIZE_FORMAT = 16,
	OSC_SIZE_DATA = 128
};

typedef struct {
	char address[OSC_SIZE_ADDRESS];			// The OSC address indicating where to dispatch
	char format[OSC_SIZE_FORMAT];			// Format specifier for the contained byte array
	byte data[OSC_SIZE_DATA]; 				// The byte array containing the data
	int data_size;							// The size of the data array
} OSCMessage;

/**
 * Calculate the OSC size (next multiple of 4 to the length) of a string
 *
 * @param str	The string
 *
 * @return The padded length to be used for this string in OSC handling
 */
static int OSC_SIZE(char* str) {
	int len = strlen(str) + 1;
	if(len % 4 != 0) len += 4 - (len % 4);

	return len;
}

/**
 * Create a new OSCMessage with the given values
 *
 * @param address	The dispatch address
 * @param format	The format of the data
 * @param ...		Variadic parameters to be stored in the data array; informed by "format"
 */
static OSCMessage* build_osc_message(char* address, char* format, ...) {
	OSCMessage* msg = (OSCMessage*) malloc(sizeof(OSCMessage));

	// Copy in the strings that can be placed immediately
	strcpy(msg->address, address);
	strcpy(msg->format, format);

	// Create two variadic parameters arrays, one for calculating size and the other to populate
	va_list args; va_start(args, format);

	// Populate the data buffer with the variadic arguments
	int i = 0, j = 1; unsigned int k = 0;
	uint32_t int_container; float float_container; char* string_container;

	while(format[j] != '\0') {
		switch(format[j++]) {
		case 'i':
			int_container = (uint32_t) va_arg(args, int);
			memcpy(msg->data + i, &int_container, sizeof(uint32_t));
			i += sizeof(uint32_t);
			break;

		case 'f':
			float_container = (float) va_arg(args, double);
			memcpy(msg->data + i, &float_container, sizeof(float));
			i += sizeof(float);
			break;

		case 's':
			string_container = (char*) va_arg(args, void*);
			int m = strlen(string_container) + 1, n = OSC_SIZE(string_container);
			memcpy(msg->data + i, string_container, m);
			for(k = m; k < n; k++) {
				msg->data[i + k] = '\0';
			}
			i += n;
			break;
		}
	}
	msg->data_size = i;

	// Free the variadic arguments
	va_end(args);

	return msg;
}

/**
 * Flatten an OSCMessage into a buffer of bytes.
 * 
 * @param msg 		The message to flatten
 * @param len_ptr (out)	The length of the generated buffer is stored here. Always a multiple of 4.
 * @return A buffer filled with the flattened contents of the given OSCMessage.
 */
byte* flatten_oscmessage(OSCMessage* msg, int* len_ptr) {
	// Calculate the length of the buffer to send.
	// Note the use of OSC_SIZE instead of strlen here - it's important!
	// The length is guaranteed to be a multiple of 4 because OSC_SIZE will always return a
	// multiple of 4, and data_size is guaranteed to be a multiple of 4.
	int padded_address_length = OSC_SIZE(msg->address), padded_format_length = OSC_SIZE(msg->format);
	int length = padded_address_length + padded_format_length + msg->data_size;

	// Allocate the buffer and a position pointer
	byte* stream = (byte*) malloc(length);
	// Need to zero out the buffer in case the address or format string need padding bytes
	for(int i = 0; i < length; i++) {
		stream[i] = 0;
	}
	byte* posn = stream;

	// Flatten the OSCMessage into the allocated stream buffer
	strcpy(posn, msg->address);
	posn += padded_address_length;

	strcpy(posn, msg->format);
	posn += padded_format_length;

	for(int i = 0; i < msg->data_size; i++) {
		*(posn++) = msg->data[i];
	}
	*len_ptr = length;
	return stream;
}

class OSCClient {
private:
	SocketAddress controller;		// The address (IP, port) of the central controller
	SocketAddress address;			// The address (IP, port) of this instrument
	UDPBroadcastSocket udp_broadcast;	// The socket used to broadcast to the controller during discovery.
	UDPSocket udp;				// The socket used to communicate with the controller.

public:
	/** Constructor */
	template <typename S>
	OSCClient(S* stack):
		controller(BROADCAST_IP, OSC_PORT), address(stack->get_ip_address(), OSC_PORT), udp_broadcast(stack), udp(stack)
	{/* Empty */}

	/**
	 * Get the IP address of the controller
	 */
	const char* get_controller_ip() {
		return this->controller.get_ip_address();
	}

	/**
	 * Send an OSC Message over UDP
	 *
	 * @param msg	The OSCMessage to send out
	 *
	 * @return The number of bytes sent, or an error code
	 */
	nsapi_size_or_error_t send(OSCMessage* msg) {
		int length = 0;
		byte* stream = flatten_oscmessage(msg, &length);
		printf("Sending UDP data\n");
		// Send out the stream and then free it
		nsapi_size_or_error_t out = this->udp.sendto(this->controller, stream, length);
		free(stream);
		printf("Done sending UDP data\n");

		return out;
	}

	/**
	 * Receive an OSC message over UDP
	 *
	 * @param msg	The OSCMessage to populate with incoming data
	 *
	 * @return The number of bytes received, or an error code
	 */
	nsapi_size_or_error_t receive(OSCMessage* msg) {
		char* buffer = (char*) malloc(OSC_MSG_SIZE);
		char* start = buffer;
		unsigned int offset;
		int padding;

		nsapi_size_or_error_t recv = this->udp.recvfrom(&this->controller, buffer, OSC_MSG_SIZE);
		//printf("recieved %d bytes\n", recv);
		if(recv <= 0) return recv;

		// Clear the struct
		memset(msg, 0, sizeof(OSCMessage));

		// Copy the buffer directly into address, which will capture everything up to the first null terminator
		int address_length = strlen(buffer) + 1;
		strcpy(msg->address, buffer);
		buffer = buffer + address_length;

		offset = buffer - start;
		padding = 4 - (offset % 4);
		if (offset % 4 != 0) {
			printf("Dealing with padding %d\n", padding);
			buffer += padding;
		}

		// After advancing to that point, the next string extracted by strcpy will be the type tag
		int format_length = strlen(buffer) + 1;
		strcpy(msg->format, buffer);
		buffer = buffer + format_length;

		offset = buffer - start;
		padding = 4 - (offset % 4);
		if (offset % 4 != 0) {
			printf("Dealing with padding %d\n", padding);
			buffer += padding;
		}

		// Blindly copy everything else up to the end of the received byte stream
		memcpy(msg->data, buffer, recv - address_length - format_length);
		msg->data_size = recv - address_length - format_length;

		// What I wouldn't give for C to have automatic garbage collection
		free(start);

		return recv;
	}

	/** Register name and supported functions with the central controller */
	void connect() {
		// TODO: udp_broadcast socket can probably be a local variable in this function, 
		// instead of being a field of OSCClient
		
		// For the setup phase, allow the socket to block
		this->udp_broadcast.set_blocking(true);

		// Create and send the OSC message for registering the name of the instrument
		OSCMessage* msg = build_osc_message(
				"/NoticeMe",
				",ss",
				INSTRUMENT_NAME,
				this->address.get_ip_address()
		);
	
		// Enable broadcasting for the socket	
		this->udp_broadcast.set_broadcast(true);
		SocketAddress broadcast(BROADCAST_IP, OSC_PORT);
		
		int length = 0;
		byte* msg_buf = flatten_oscmessage(msg, &length);
		
		nsapi_size_or_error_t size_or_error = this->udp_broadcast.sendto(broadcast, msg_buf, length);
		if(size_or_error < 0) {
			printf("Error sending the registration message! (%d)\r\n", size_or_error);
			exit(1);
		}

		free(msg_buf);
		free(msg);

		// Reset broadcast and blocking for this socket
		this->udp_broadcast.set_broadcast(false);
		this->udp_broadcast.set_blocking(false);

		printf("Waiting to recieve a message\n");
		
		char* buffer = (char*) malloc(OSC_MSG_SIZE);

		// For the setup phase, allow the socket to block
		this->udp.set_blocking(true);
		
		// TODO: this section should keep trying to recieve messages until it gets the correct kind
		// of "acknowledgement" message (just in case)
		int status = this->udp.bind(OSC_PORT+1);
		printf("bind status: %d\n", status);
	
		// Get back a request for functions from the controller
		// TODO: use a new SocketAddress here instead of broadcast	
		size_or_error = this->udp.recvfrom(&broadcast, buffer, OSC_MSG_SIZE);	
		if(size_or_error < 0) {
			printf("Error finding controller! (%d)\r\n", size_or_error);
			exit(1);
		}

		printf("Controller ip addr:%s\n", broadcast.get_ip_address());
		// TODO: need to confirm that this moves the broadcast SocketAddress object into
		// this OSCClient object
		this->controller = broadcast;

		// For normal operation, socket should be polled
		this->udp.set_blocking(false);

		// FREEDOM!
		free(buffer);
	}
};

#endif /* OSC_CLIENT_H_ */
