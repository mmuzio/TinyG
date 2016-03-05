/*
 * switch.c - switch handling functions
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2016 Alden S. Hart, Jr.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software. If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* Switch Modes
 *
 *	The switches are considered to be homing switches when machine_state is
 *	MACHINE_HOMING. At all other times they are treated as limit switches:
 *	  - Hitting a homing switch puts the current move into feedhold
 *	  - Hitting a limit switch causes the machine to shut down and go into lockdown until reset
 *
 * 	The normally open switch modes (NO) trigger an interrupt on the falling edge
 *	and lockout subsequent interrupts for the defined lockout period. This approach
 *	beats doing debouncing as an integration as switches fire immediately.
 *
 * 	The normally closed switch modes (NC) trigger an interrupt on the rising edge
 *	and lockout subsequent interrupts for the defined lockout period. Ditto on the method.
 */

#include <avr/interrupt.h>

#include "tinyg.h"
#include "config.h"
#include "switch.h"
#include "hardware.h"
#include "controller.h"
#include "canonical_machine.h"
#include "text_parser.h"

//static void _switch_isr_helper(uint8_t sw_num);
//static bool _read_input_corrected(const uint8_t sw_num);
//static void _read_input_corrected(const uint8_t sw_num);
static bool _read_raw_switch(const uint8_t sw_num);
static void _dispatch_switch(const uint8_t sw_num);

/*
 * Interrupt levels and vectors - The vectors are hard-wired to xmega ports
 * If you change axis port assignments you need to change these, too.
 *
 *  #define GPIO1_INTLVL (PORT_INT0LVL_HI_gc|PORT_INT1LVL_HI_gc)	// can't be hi
 *  #define GPIO1_INTLVL (PORT_INT0LVL_MED_gc|PORT_INT1LVL_MED_gc)
 *  #define GPIO1_INTLVL (PORT_INT0LVL_LO_gc|PORT_INT1LVL_LO_gc)	// shouldn't be low
 */
#define GPIO1_INTLVL (PORT_INT0LVL_MED_gc|PORT_INT1LVL_MED_gc)

// port assignments for vectors
#define X_MIN_ISR_vect PORTA_INT0_vect	// these must line up with the SWITCH assignments in system.h
#define Y_MIN_ISR_vect PORTD_INT0_vect
#define Z_MIN_ISR_vect PORTE_INT0_vect
#define A_MIN_ISR_vect PORTF_INT0_vect
#define X_MAX_ISR_vect PORTA_INT1_vect
#define Y_MAX_ISR_vect PORTD_INT1_vect
#define Z_MAX_ISR_vect PORTE_INT1_vect
#define A_MAX_ISR_vect PORTF_INT1_vect

/* Note: v7 boards have external strong pullups on GPIO2 pins (2.7K ohm).
 *	v6 and earlier use internal pullups only. Internal pullups are set
 *	regardless of board type but are extraneous for v7 boards.
 */
#define PIN_MODE PORT_OPC_PULLUP_gc				// pin mode. see iox192a3.h for details
//#define PIN_MODE PORT_OPC_TOTEM_gc			// alternate pin mode for v7 boards

/*
 * switch_init()    - initialize homing/limit switches
 * reset_switches() - reset all switches
 *
 *	switch_init() assumes sys_init() and st_init() have been run previously to
 *	bind the ports and set bit IO directions, respectively
 */

void switch_init(void)
{
	for (uint8_t i=0; i<NUM_SWITCH_PAIRS; i++) {
		// old code from when switches fired on one edge or the other:
		//	uint8_t int_mode = (sw.switch_type == SW_TYPE_NORMALLY_OPEN) ? PORT_ISC_FALLING_gc : PORT_ISC_RISING_gc;

		// setup input bits and interrupts (previously set to inputs by st_init())
		if (sw.mode[MIN_SWITCH(i)] != SW_MODE_DISABLED) {
			hw.sw_port[i]->DIRCLR = SW_MIN_BIT_bm;		 	// set min input - see 13.14.14
			hw.sw_port[i]->PIN6CTRL = (PIN_MODE | PORT_ISC_BOTHEDGES_gc);
			hw.sw_port[i]->INT0MASK = SW_MIN_BIT_bm;	 	// interrupt on min switch
		} else {
			hw.sw_port[i]->INT0MASK = 0;	 				// disable interrupt
		}
		if (sw.mode[MAX_SWITCH(i)] != SW_MODE_DISABLED) {
			hw.sw_port[i]->DIRCLR = SW_MAX_BIT_bm;		 	// set max input - see 13.14.14
			hw.sw_port[i]->PIN7CTRL = (PIN_MODE | PORT_ISC_BOTHEDGES_gc);
			hw.sw_port[i]->INT1MASK = SW_MAX_BIT_bm;		// max on INT1
		} else {
			hw.sw_port[i]->INT1MASK = 0;
		}
		// set interrupt levels. Interrupts must be enabled in main()
		hw.sw_port[i]->INTCTRL = GPIO1_INTLVL;				// see gpio.h for setting
	}
	reset_switches();
}

void reset_switches()
{
    for (uint8_t i=0; i < NUM_SWITCHES; i++) {
        Timeout_clear(&sw.timeout[i]);          // clear all debounce timers
        sw.state[i] = _read_raw_switch(i);      // set initial conditions
	}
}

/*
 * Switch closure processing routines
 */
/*
ISR(X_MIN_ISR_vect)	{ _switch_isr_helper(SW_MIN_X);}
ISR(Y_MIN_ISR_vect)	{ _switch_isr_helper(SW_MIN_Y);}
ISR(Z_MIN_ISR_vect)	{ _switch_isr_helper(SW_MIN_Z);}
ISR(A_MIN_ISR_vect)	{ _switch_isr_helper(SW_MIN_A);}
ISR(X_MAX_ISR_vect)	{ _switch_isr_helper(SW_MAX_X);}
ISR(Y_MAX_ISR_vect)	{ _switch_isr_helper(SW_MAX_Y);}
ISR(Z_MAX_ISR_vect)	{ _switch_isr_helper(SW_MAX_Z);}
ISR(A_MAX_ISR_vect)	{ _switch_isr_helper(SW_MAX_A);}

static void _switch_isr_helper(uint8_t sw_num)
{
//	if (sw.mode[sw_num] == SW_MODE_DISABLED) { return; } // this is never supposed to happen
//	if (sw.debounce[sw_num] == SW_LOCKOUT) { return; }   // exit if switch is in lockout
//	sw.debounce[sw_num] = SW_DEGLITCHING;                // either transitions state from IDLE or overwrites it
//	sw.count[sw_num] = -SW_DEGLITCH_TICKS;               // reset deglitch count regardless of entry state
    _dispatch_switch(sw_num);
//	_read_input_corrected(sw_num);                       // sets the state value in the struct
}
*/

ISR(X_MIN_ISR_vect)	{ _dispatch_switch(SW_MIN_X);}
ISR(Y_MIN_ISR_vect)	{ _dispatch_switch(SW_MIN_Y);}
ISR(Z_MIN_ISR_vect)	{ _dispatch_switch(SW_MIN_Z);}
ISR(A_MIN_ISR_vect)	{ _dispatch_switch(SW_MIN_A);}
ISR(X_MAX_ISR_vect)	{ _dispatch_switch(SW_MAX_X);}
ISR(Y_MAX_ISR_vect)	{ _dispatch_switch(SW_MAX_Y);}
ISR(Z_MAX_ISR_vect)	{ _dispatch_switch(SW_MAX_Z);}
ISR(A_MAX_ISR_vect)	{ _dispatch_switch(SW_MAX_A);}

/*
 * _read_raw_switch() - primitive to read input
 *
 * Read raw switch input and sense-correct for ACTIVE_LO / HI
 */

static bool _read_raw_switch(const uint8_t sw_num)
{
	uint8_t raw;    // raw is a naked bit, like 0b01000000 or 0b00000000
	switch (sw_num) {
		case SW_MIN_X: { raw = hw.sw_port[AXIS_X]->IN & SW_MIN_BIT_bm; break;}
		case SW_MAX_X: { raw = hw.sw_port[AXIS_X]->IN & SW_MAX_BIT_bm; break;}
		case SW_MIN_Y: { raw = hw.sw_port[AXIS_Y]->IN & SW_MIN_BIT_bm; break;}
		case SW_MAX_Y: { raw = hw.sw_port[AXIS_Y]->IN & SW_MAX_BIT_bm; break;}
		case SW_MIN_Z: { raw = hw.sw_port[AXIS_Z]->IN & SW_MIN_BIT_bm; break;}
		case SW_MAX_Z: { raw = hw.sw_port[AXIS_Z]->IN & SW_MAX_BIT_bm; break;}
		case SW_MIN_A: { raw = hw.sw_port[AXIS_A]->IN & SW_MIN_BIT_bm; break;}
		case SW_MAX_A: { raw = hw.sw_port[AXIS_A]->IN & SW_MAX_BIT_bm; break;}
        default: { return (false); }    // ERROR
	}
    return (raw ^ sw.switch_type);	// XOR to correct for ACTIVE mode. Casts to bool.
//    return (read ^ (sw.switch_type ^ 1));	// correct for ACTIVE mode
}

/*
 * _dispatch_switch() - process a switch interrupt
 *
 * Read raw switch input and sense-correct for ACTIVE_LO / HI
 */

static void _dispatch_switch(const uint8_t sw_num)
{
    // process conditions for return with no action
	if (sw.mode[sw_num] == SW_MODE_DISABLED) {      // input is disabled (not supposed to happen)
        return; 
    }
    if (Timeout_isSet(&sw.timeout[sw_num])) {       // input is in lockout period (take no action)
        return;
    }
	bool raw_switch = _read_raw_switch(sw_num);     // no change in state (not supposed to happen)
    if (sw.state[sw_num] == raw_switch) {
        return;
    }

    // read the switch, set edges and start lockout timer
    sw.state[sw_num] = raw_switch;                  // 1 = switch hit, 0 = switch unhit
    sw.edge[sw_num] = raw_switch;                   // 1 = leading edge, 0 = trailing edge
    Timeout_set(&sw.timeout[sw_num], SW_LOCKOUT_MS);

    // execute the functions
	if ((cm.cycle_state == CYCLE_HOMING) || (cm.cycle_state == CYCLE_PROBE)) { // regardless of switch type
    	cm_request_feedhold();
	} else if (sw.mode[sw_num] & SW_LIMIT_BIT) {     // should be a limit switch, so fire it.
    	controller_assert_limit_condition(sw_num+1);
	}
}
/*
void switch_rtc_callback(void)
{
	for (uint8_t i=0; i < NUM_SWITCHES; i++) {
		if (sw.mode[i] == SW_MODE_DISABLED || sw.debounce[i] == SW_IDLE)
            continue;

		if (++sw.count[i] == SW_LOCKOUT_TICKS) {        // state is either lockout or deglitching
			sw.debounce[i] = SW_IDLE;
            // check if the state has changed while we were in lockout...
            uint8_t old_state = sw.state[i];
            if(old_state != _read_input_corrected(i)) {
                sw.debounce[i] = SW_DEGLITCHING;
                sw.count[i] = -SW_DEGLITCH_TICKS;
            }
            continue;
		}
		if (sw.count[i] == 0) {                         // trigger point
			sw.sw_num_thrown = i;                       // record number of thrown switch
			sw.debounce[i] = SW_LOCKOUT;

			if ((cm.cycle_state == CYCLE_HOMING) || (cm.cycle_state == CYCLE_PROBE)) {		// regardless of switch type
				cm_request_feedhold();
			} else if (sw.mode[i] & SW_LIMIT_BIT) {     // should be a limit switch, so fire it.
                controller_assert_limit_condition(i+1); // This is supposed to work fromn the main loop
			}
		}
	}
}
*/

/*
 * set_switch_type()    - set global switch type
 * get_switch_type()    - return glgobal switch type
 * get_switch_mode()    - return switch mode setting
 * get_switch_thrown()  - return switch number most recently thrown
 */

void set_switch_type(uint8_t switch_type) { sw.switch_type = switch_type; }
uint8_t get_switch_type() { return sw.switch_type; }
uint8_t get_switch_mode(uint8_t sw_num) { return (sw.mode[sw_num]);}
uint8_t get_switch_thrown(void) { return(sw.sw_num_thrown);}


/***********************************************************************************
 * CONFIGURATION AND INTERFACE FUNCTIONS
 * Functions to get and set variables from the cfgArray table
 * These functions are not part of the NIST defined functions
 ***********************************************************************************/

stat_t sw_set_st(nvObj_t *nv)			// switch type (global)
{
	set_01(nv);
	switch_init();
	return (STAT_OK);
}

stat_t sw_set_sw(nvObj_t *nv)			// switch setting
{
	if (nv->value_int > SW_MODE_MAX_VALUE) {
        return (STAT_INPUT_VALUE_RANGE_ERROR);
    }
	set_ui8(nv);
	switch_init();
	return (STAT_OK);
}

/***********************************************************************************
 * TEXT MODE SUPPORT
 * Functions to print variables from the cfgArray table
 ***********************************************************************************/

#ifdef __TEXT_MODE

static const char fmt_st[] PROGMEM = "[st]  switch type%18d [0=NO,1=NC]\n";
void sw_print_st(nvObj_t *nv) { text_print(nv, fmt_st);}

#endif
