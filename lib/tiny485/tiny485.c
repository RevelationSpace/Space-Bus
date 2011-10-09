/** \file tiny485.c
 * \brief An SBLP hardware-side implementation for the ATTiny series of microcontrollers.
 *
 * This file implements the hardware-dependent parts of the SBLP protocol.
 * It is specific to the Atmel ATTiny85 microcontroller.
 *
 * Responsibilities of this layer:
 *	\li Interfacing to the bus via a MAX485-style transceiver
 *	\li Byte-level framing with start & stop bits (as per Atmel application note AVR307)
 *	\li Synchronising to the bus on start-up
 *	\li Escaping the synchronisation and escape bytes
 *
 * \todo the decision for uart-mode transmission has not been
 * finalised. Make sure this code is correct when it has.
 *
 * \todo there must be a nicer way to do bus synchronisation...
 */

#include <avr/interrupt.h>
#include <avr/io.h>

#include "../interop.h"
#include "tiny485_pin.h"
#include "tiny485.h"

/* timing info */
#define T485_BIT_TIMER 104	/**< length of one bit in timer cycles */

/* USI seeds */
#define T485_RECV_SEED	0x07	/**< receive seed:  shift in 16-7=9 bits (start + data) */
#define T485_XMIT_SEED	0x0B	/**< transmit seed: shift out 16-11=5 bits (half a byte + start/stop) */

/* bit constants */
#define T485_INIT1	0b01111111	/**< first part of init sequence */
#define T485_INIT2	0b11000000	/**< second part of init sequence */

/* data bytes with special meanings */
#define T485_SYNC_BYTE		0xFF		/**< The synchronisation byte */
#define T485_ESCAPE_BYTE	0x55		/**< Escape byte for syncs in messages */

#define T485_ESCAPED_SYNC	0x00		/**< A synchronisation byte when escaped */
#define T485_ESCAPED_ESCAPE	0x01		/**< An escape byte when escaped */

/* flags for field below */
#define T485_FLAG_ESCAPE	0x01

static struct {
	enum {
		T485_STATE_INIT1,	/**< look for first half of init sequence */
		T485_STATE_INIT2,	/**< look for second half of init sequence */

		T485_STATE_IDLE,	/**< idle: ready to start transmitting or receiving */
		T485_STATE_RECV,	/**< receive: currently receiving a byte */

		T485_STATE_XMIT1,	/**< transmit: currently transmitting first half of a byte */
		T485_STATE_XMIT2	/**< transmit: currently transmitting second half of a byte */
	} state;

	uint8_t flags;			/**< internal flags. Mostly used for keeping track of escape states. */
	uint8_t buf;			/**< buffer for second half of byte */
} t485_data;

/** reverse bits in a byte. necessary for host/wire bit order switching.  */
inline uint8_t bit_reverse(uint8_t b) {
	uint8_t i, buf = 0x0;

	for(i=0; i<=7; i++)
		if(b & (1 << i)) buf |= (1 << (7-i));

	return buf;
}

/* direct hardware interfacing convenience functions
 * do what they say on the tin - see relevant AVR datasheets for details
 */

/* timer0 is switched by connecting/disconnecting the prescaler
 * (clk_io / 8) to/from the timer0 clock.
 */
#define TIM0_ON()	TCCR0B |= 0b00000010	/**< Turn timer 0 on. */
#define TIM0_OFF()	TCCR0B &= 0b11111000	/**< Turn timer 0 off. */

/* the USI is switched by turning the entire USI and its clock on/off */
#define USI_ON()	USICR |= 0b00010100	/**< Turn the USI on. */
#define USI_OFF()	USICR &= 0b11000011	/**< Turn the USI off. */


/* PCINT0 is switched by switching the entire pin-change interrupt. */
#define PCINT0_ON()	GIMSK |= 0b00100000	/**< Turn pin-change interrupt 0 on */
#define PCINT0_OFF()	GIMSK &= 0b11011111	/**< Turn pin-change interrupt 0 off */

/* The timer interrupt is switched by setting its bit in the timer interrupt mask. */
#define TIM0INT_ON()	TIMSK |= 0b00000001	/**< Turn the timer interrupt on. */
#define TIM0INT_OFF()	TIMSK &= 0b11111110	/** turn the timer interrupt off. */


/* load a counter value into the USI counter. */
#define USICOUNTER(n)	USISR = (USISR & 0b11110000) | n

/* interface functions */
/** Start a transmission.
 * This will tell the line driver to take the bus off high-impedance
 * mode so other nodes can see our transmission. It also turns the
 * pin-change interrupt off so we don't get distracted by seeing our own
 * data.
 */
void begin_transmission() {
	DEN_PORT |= _BV(DEN);
	PCINT0_OFF();
}

/** End a transmission.
 * This will return the bus to high-impedance mode and turn the pin-
 * change interrupt back on.
 */
void end_transmission() {
	DEN_PORT &= ~_BV(DEN);
	PCINT0_ON();
}

/** Send a single byte.
 * This will start pushing out a byte through the USI into the line
 * driver. Make sure to call the begin_transmission function before
 * this so the data appears on the bus as well.
 */
void send_byte(uint8_t b) {
	USIDR = (b >> 1);
	USICOUNTER(T485_XMIT_SEED);

	t485_data.buf = (b << 4) | 0x0F;
	t485_data.state = T485_STATE_XMIT1;

	USI_ON();
	TIM0_ON();
}

void tiny485_init() {
	/* set state */
#ifdef REQUIRE_SYNC
	t485_data.state = T485_STATE_INIT1;	/* start hunting for init sequence */
#else
	t485_data.state = T485_STATE_IDLE;	/* start hunting for init sequence */
#endif

	/* initialise the pins we use */
	USI_DDR |=  _BV(DO);	/* DO  = output */
	USI_DDR &= ~_BV(DI);	/* DI  = input */
	DEN_DDR |=  _BV(DEN);	/* DEN = output */

	/* initialise timer */
	TCCR0A = 0b00000010;		/* CTC mode */
	TCCR0B = 0b00000010;		/* prescaler = 8 */
	OCR0A  = T485_BIT_TIMER;	/* compare match every bit length - this clocks the USI */

	/* initialise USI */
	USICR = 0b01010000;

	/* initialise pin-change interrupt */
	PCMSK = 0b00000001;		/* enable for PCINT0 (DI) pin */
	GIMSK = 0b00100000;		/* enable in general */
	sei();				/* turn on interrupts */
}


/* interrupt vectors */
/** Pin change ISR. Synchronise the receive timer to the node transmitting. */
ISR(PCINT0_vect) {
	switch(t485_data.state) {
		case T485_STATE_INIT1:	/* fallthrough */
		case T485_STATE_INIT2:
			/* synchronise the bit timer on transitions */
			TCNT0 = T485_BIT_TIMER / 2;
			break;

		case T485_STATE_IDLE:
			if((USI_PORT & _BV(DI)) == 0) {
				/* start bit detected! sync the timer - sample 1/2 bit length later */
				TCNT0 = T485_BIT_TIMER / 2;
				t485_data.state = T485_STATE_RECV;
				USICOUNTER(T485_RECV_SEED);	/* wait for 16-7 = 9 bits (start bit + a byte) */

				PCINT0_OFF();
				TIM0_ON();
				USI_ON();
			}
			break;

		default:
			/* do nothing */
			break;
	}
}

/** Bit timer interrupt - this interrupt is only enabled when sync-hunting. */
ISR(TIM0_COMPA_vect) {
	switch(t485_data.state) {
		case T485_STATE_INIT1:
			/* detect first half of sequence */
			if(USIDR == T485_INIT1) {
				t485_data.state = T485_STATE_INIT2;	/* hunt for second half */
				USIDR = 0;				/* clear USI data register so we don't get false hits for the second half */
			}
			break;

		case T485_STATE_INIT2:
			if(USIDR == T485_INIT2) {
				/* sync fully detected! we are now synchronised to the bus */
				t485_data.state = T485_STATE_IDLE;

				/* notify the layer above */
				sync_received();
			}
			break;

		default:
			/* do nothing */
			break;
	}
}

/** USI overflow vector - process the transmitted or received byte
 * This ISR gets run whenever the USI overflows - for us, this means a
 * byte has been transmitted or received. As we're abusing the USI as a
 * hacky UART, we may need to send another byte. If not, notify the
 * layer above via the appropriate struct hw_interface members.
 */
ISR(USI_OVF_vect) {
	switch(t485_data.state) {
		case T485_STATE_XMIT1:
			USISR |= _BV(USIOIF);	/* clear overflow flag */
			USIDR  = t485_data.buf;	/* ...or send another byte */
			USICOUNTER(T485_XMIT_SEED);
			t485_data.state = T485_STATE_XMIT2;
			break;

		case T485_STATE_XMIT2:
				USISR |= _BV(USIOIF);	/* clear overflow flag */
				USI_OFF();
				TIM0_OFF();		/* ...stop transmitting... */
				t485_data.state = T485_STATE_IDLE;

				/* ...and notify the layer above... */
				sei();
				byte_sent();
				break;

		case T485_STATE_RECV:
			/* if we're receiving, we need to handle the received byte */
			USI_OFF();
			TIM0_OFF();
			t485_data.state = T485_STATE_IDLE;

			/* load USI buffer into buf */
			t485_data.buf = USIBR;
			PCINT0_OFF();

			/* notify higher layer */
			sei();

			switch(USIBR) {
				case T485_SYNC_BYTE:
					/* it's a sync, so pass it on as one */
					sync_received();
					break;

				case T485_ESCAPE_BYTE:
					/* escape byte - set escape flag */
					t485_data.flags |= T485_FLAG_ESCAPE;
					break;

				default:
					if(t485_data.flags & T485_FLAG_ESCAPE) {
						/* we're in escape mode, determine which byte to send up */
						switch(USIBR) {
							case T485_ESCAPED_SYNC:
								byte_received(T485_SYNC_BYTE);
								break;

							case T485_ESCAPED_ESCAPE:
								byte_received(T485_ESCAPE_BYTE);
								break;

							default:
								/* regular data: this shouldn't happen here, but notify higher layer anyway */
								byte_received(USIBR);
								break;
						}
					} else {
						/* no escape, regular data - notify higher layer */
						byte_received(USIBR);
					}
					break;
			}
			break;

		default:
			/* do nothing */
			break;
	}
}
