/*
 * Copyright 2010 by Adam Mayer	 <adam@makerbot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include "Command.hh"
#include "Steppers.hh"
#include "Commands.hh"
#include "Configuration.hh"
#include "Timeout.hh"
#include "CircularBuffer.hh"
#include <util/atomic.h>
#include <avr/eeprom.h>
#include "Eeprom.hh"
#include "EepromMap.hh"
#include "SDCard.hh"
#include "Pin.hh"
#include <util/delay.h>
#include "Piezo.hh"
#include "RGB_LED.hh"
#include "Interface.hh"
#include "UtilityScripts.hh"
#include "stdio.h"
#include "Menu_locales.hh"
#include "Version.hh"
//#include "StepperAxis.hh"


namespace command {

#define COMMAND_BUFFER_SIZE 512
uint8_t buffer_data[COMMAND_BUFFER_SIZE];
CircularBuffer command_buffer(COMMAND_BUFFER_SIZE, buffer_data);
uint8_t currentToolIndex = 0;

uint32_t line_number;

bool outstanding_tool_command = false;
bool check_temp_state = false;
bool paused = false;
bool active_paused = false;
bool heat_shutdown = false;
bool cold_pause = false;
uint32_t sd_count = 0;
uint8_t sd_fail_count = 0;

uint16_t getRemainingCapacity() {
	uint16_t sz;
	ATOMIC_BLOCK(ATOMIC_FORCEON) {
		sz = command_buffer.getRemainingCapacity();
	}
	return sz;
}

void pause(bool pause) {
	paused = pause;
}
void heatShutdown(){
	heat_shutdown = true;
}

bool isPaused() {
	return paused;
}

bool isActivePaused(){
	return active_paused;
}

bool isEmpty() {
	return command_buffer.isEmpty();
}

void push(uint8_t byte) {
	command_buffer.push(byte);
}

uint8_t pop8() {
//	sd_count ++;
	return command_buffer.pop();
}

int16_t pop16() {
	union {
		// AVR is little-endian
		int16_t a;
		struct { 
			uint8_t data[2];} b;
	} shared;
	shared.b.data[0] = command_buffer.pop();
	shared.b.data[1] = command_buffer.pop();
//	sd_count+=2;
	return shared.a;
}

int32_t pop32() {
	union {
		// AVR is little-endian
		int32_t a;
		struct {
			uint8_t data[4];
		} b;
	} shared;
	shared.b.data[0] = command_buffer.pop();
	shared.b.data[1] = command_buffer.pop();
	shared.b.data[2] = command_buffer.pop();
	shared.b.data[3] = command_buffer.pop();
//	sd_count+=4;
	return shared.a;
}

enum CommandState{
	READY,
	MOVING,
	DELAY,
	HOMING,
	WAIT_ON_TOOL,
	WAIT_ON_PLATFORM,
	WAIT_ON_BUTTON
} mode = READY;

Timeout command_buffer_timeout;

/// Bitmap of button pushes to wait for
uint8_t button_mask;
enum {
	BUTTON_TIMEOUT_CONTINUE = 0,
	BUTTON_TIMEOUT_ABORT = 1,
	BUTTON_CLEAR_SCREEN = 2
};
/// Action to take when button times out
uint8_t button_timeout_behavior;


bool sdcard_reset = false;

void reset() {
	command_buffer.reset();
	line_number = 0;
	check_temp_state = false;
	paused = false;
	active_paused = false;
	sd_count = 0;
	sdcard_reset = false;
	sd_fail_count = 0;
	mode = READY;
}

bool isWaiting(){
	return (mode == WAIT_ON_BUTTON);
}
    
bool isReady() {
	return (mode == READY);
}

uint32_t getLineNumber() {
	return line_number;	
}

void clearLineNumber() {
	line_number = 0;
}

enum SleepStates{
	SLEEP_NONE,
	SLEEP_START_WAIT,
	SLEEP_MOVING,
	SLEEP_ACTIVE,
	SLEEP_MOVING_WAIT,
	SLEEP_RESTART,
	SLEEP_HEATING_P,
	SLEEP_HEATING_A,
	SLEEP_RETURN,
	SLEEP_FINISHED
} sleep_mode = SLEEP_NONE;

const static int16_t z_mm_per_second_18 = 140;
const static int16_t xy_mm_per_second_80 = 130;
const static int16_t ab_mm_per_second_20 = 520;

uint16_t extruder_temp[2];
uint16_t platform_temp;
Point sleep_position;
bool fan_state = false;

void startSleep(){

	// record current position
	sleep_position = steppers::getStepperPosition();
	fan_state = EX_FAN.getValue();
	Motherboard &board = Motherboard::getBoard();
	
	// retract
	Point retract = Point(sleep_position[0], sleep_position[1], sleep_position[2], sleep_position[3] + (uint32_t)stepperAxisStepsPerMM(A_AXIS), 
        sleep_position[4] + stepperAxisStepsPerMM(B_AXIS));
	steppers::setTarget(retract, ab_mm_per_second_20);
	
	// record heater state
	extruder_temp[0] = board.getExtruderBoard(0).getExtruderHeater().get_set_temperature();
	extruder_temp[1] = board.getExtruderBoard(1).getExtruderHeater().get_set_temperature();
	platform_temp = board.getPlatformHeater().get_set_temperature();

	if(cold_pause){
		// cool heaters
		board.getExtruderBoard(0).getExtruderHeater().set_target_temperature(0);
		board.getExtruderBoard(1).getExtruderHeater().set_target_temperature(0);
		board.getPlatformHeater().set_target_temperature(0);
	}
	
	// move to wait position
	Point z_pos = Point(retract);
	z_pos[Z_AXIS] = 150L*(int32_t)stepperAxisStepsPerMM(Z_AXIS); 
	Point wait_pos = Point(z_pos);
	wait_pos[X_AXIS] = -110.5*stepperAxisStepsPerMM(X_AXIS);
	wait_pos[Y_AXIS] = -74*stepperAxisStepsPerMM(Y_AXIS);
	
	steppers::setTarget(z_pos, z_mm_per_second_18);
	steppers::setTarget(wait_pos, xy_mm_per_second_80);
	board.setExtra(false);
}

void stopSleep(){
	// move to build position
	Point z_pos = Point(steppers::getStepperPosition());
	/// set filament position to sleep_position
	z_pos[A_AXIS] = sleep_position[A_AXIS];
	z_pos[B_AXIS] = sleep_position[B_AXIS];
	steppers::definePosition(z_pos);
	/// move z_axis first
	z_pos[Z_AXIS] = sleep_position[Z_AXIS];
	steppers::setTarget(z_pos, z_mm_per_second_18);
	/// move back to paused position
	steppers::setTarget(sleep_position, xy_mm_per_second_80);

	Motherboard::getBoard().setExtra(fan_state);	
}

void sleepReheat(){
	
	Motherboard &board = Motherboard::getBoard();

	// heat heaters
	board.getExtruderBoard(0).getExtruderHeater().set_target_temperature(extruder_temp[0]);
	board.getExtruderBoard(1).getExtruderHeater().set_target_temperature(extruder_temp[1]);
	board.getPlatformHeater().set_target_temperature(platform_temp);
	
	/// if platform is actively heating and extruder is not cooling down, pause extruder
	if(board.getPlatformHeater().isHeating() && !board.getPlatformHeater().isCooling()){
		if(!board.getExtruderBoard(0).getExtruderHeater().isCooling()){
			board.getExtruderBoard(0).getExtruderHeater().Pause(true);
			check_temp_state = true;
		}
		if(!board.getExtruderBoard(1).getExtruderHeater().isCooling()){
			board.getExtruderBoard(1).getExtruderHeater().Pause(true);
			check_temp_state = true;
		}
	}
				
}

SleepType sleep_type = SLEEP_TYPE_NONE;

void ActivePause(bool on, SleepType type){

	sleep_type = type;
	if(active_paused != on){
		if(on){
			if(sleep_type == SLEEP_TYPE_COLD){
				cold_pause = true;
				Motherboard::getBoard().getInterfaceBoard().errorMessage(SLEEP_WAIT_MSG);
				sleep_mode = SLEEP_START_WAIT;
			}else if(sleep_type == SLEEP_TYPE_FILAMENT){
				cold_pause = false;
				Motherboard::getBoard().getInterfaceBoard().errorMessage(CHANGE_FILAMENT_WAIT_MSG);
				sleep_mode = SLEEP_START_WAIT;
			}
			active_paused = on;
		}else{
			if(sleep_mode == SLEEP_START_WAIT){
				sleep_mode = SLEEP_NONE;
				active_paused = on;
			}else if(sleep_mode == SLEEP_MOVING){
				sleepReheat();
				sleep_mode = SLEEP_MOVING_WAIT;
			}else if(sleep_mode == SLEEP_ACTIVE){
				sleepReheat();
				sleep_mode = SLEEP_RESTART;
			}else if (sleep_type == SLEEP_TYPE_NONE){
				active_paused = on;
			}
		}
	}	
}

static void handleMovementCommand(const uint8_t &command) {

	if (command == HOST_CMD_QUEUE_POINT_EXT) {
		// check for completion
		if (command_buffer.getLength() >= 25) {
			Motherboard::getBoard().resetUserInputTimeout();
			pop8(); // remove the command code
			mode = MOVING;

			int32_t x = pop32();
			int32_t y = pop32();
			int32_t z = pop32();
			int32_t a = pop32();
			int32_t b = pop32();
			int32_t dda = pop32();

			line_number++;
		
			steppers::setTarget(Point(x,y,z,a,b), dda);
		}
	}
	else if (command == HOST_CMD_QUEUE_POINT_NEW) {
		// check for completion
		if (command_buffer.getLength() >= 26) {
			Motherboard::getBoard().resetUserInputTimeout();
			pop8(); // remove the command code
			mode = MOVING;
			
			int32_t x = pop32();
			int32_t y = pop32();
			int32_t z = pop32();
			int32_t a = pop32();
			int32_t b = pop32();
			int32_t us = pop32();
			uint8_t relative = pop8();

			line_number++;
			
			steppers::setTargetNew(Point(x,y,z,a,b), us, relative);
		}
	}else if (command == HOST_CMD_QUEUE_POINT_NEW_EXT ) {
	        // check for completion
	        if (command_buffer.getLength() >= 32) {
			Motherboard::getBoard().resetUserInputTimeout();
			pop8(); // remove the command code
			mode = MOVING;
	 
			int32_t x = pop32();
			int32_t y = pop32();
			int32_t z = pop32();
			int32_t a = pop32();
			int32_t b = pop32();
			int32_t dda_rate = pop32();
			uint8_t relative = pop8();
			int32_t distanceInt32 = pop32();
			float *distance = (float *)&distanceInt32;
			int16_t feedrateMult64 = pop16();
	 
			line_number++;
	            
			steppers::setTargetNewExt(Point(x,y,z,a,b), dda_rate, relative, *distance, feedrateMult64);
		}  
	}
}

bool start_build_flag = false;
bool platform_on_flag = false;

bool processExtruderCommandPacket() {
	Motherboard& board = Motherboard::getBoard();
	uint8_t	id = pop8();
	uint8_t command = pop8();
	pop8(); //uint8_t length = pop8();

	switch (command) {
		case SLAVE_CMD_SET_TEMP:
			/// we are clearing temps here for the beginning of a print instead of in reset because we want them to be set to zero temperature for as short a time as possible.
			if(start_build_flag){
				board.getExtruderBoard(0).getExtruderHeater().abort();
				board.getExtruderBoard(1).getExtruderHeater().abort();
				// don't reset the platform if we have received a platform temp command
				if (!platform_on_flag){
					board.getPlatformHeater().abort();
				}
				platform_on_flag = false;
				start_build_flag = false;
			}
			board.getExtruderBoard(id).getExtruderHeater().set_target_temperature(pop16());

			/// if platform is actively heating and extruder is not cooling down, pause extruder
			if(board.getPlatformHeater().isHeating() && !board.getPlatformHeater().isCooling() && !board.getExtruderBoard(id).getExtruderHeater().isCooling()){
				check_temp_state = true;
				board.getExtruderBoard(id).getExtruderHeater().Pause(true);
			}  /// else ensure extruder is not paused  
			else {
				board.getExtruderBoard(id).getExtruderHeater().Pause(false);
			}
			board.setBoardStatus(Motherboard::STATUS_PREHEATING, false);
			// warn the user if an invalid tool command is received
			if(id == 1 && eeprom::isSingleTool()){
				board.errorResponse(ERROR_INVALID_TOOL);
				board.getExtruderBoard(id).getExtruderHeater().set_target_temperature(0);
			}
			return true;
		// can be removed in process via host query works OK
 		case SLAVE_CMD_PAUSE_UNPAUSE:
			host::pauseBuild(!command::isPaused());
			return true;
		case SLAVE_CMD_TOGGLE_FAN:
			board.getExtruderBoard(id).setFan((pop8() & 0x01) != 0);
			return true;
		case SLAVE_CMD_TOGGLE_VALVE:
			board.setExtra((pop8() & 0x01) != 0);
			return true;
		case SLAVE_CMD_SET_PLATFORM_TEMP:
			board.setUsingPlatform(true);
			if(start_build_flag){ platform_on_flag = true;}
			board.getPlatformHeater().set_target_temperature(pop16());
			// pause extruder heaters platform is heating up
			bool pause_state; /// avr-gcc doesn't allow cross-initializtion of variables within a switch statement
			pause_state = false;
			if(!board.getPlatformHeater().isCooling()){
				pause_state = true;
			}
			check_temp_state = pause_state;
			board.getExtruderBoard(0).getExtruderHeater().Pause(pause_state);
			board.getExtruderBoard(1).getExtruderHeater().Pause(pause_state);
			board.setBoardStatus(Motherboard::STATUS_PREHEATING, false);
			if(!eeprom::hasHBP()){
				if(host::getHostState() != host::HOST_STATE_BUILDING_ONBOARD){
					board.errorResponse(ERROR_INVALID_PLATFORM);
				}
				board.getPlatformHeater().set_target_temperature(0);      
				board.setUsingPlatform(false);
			}
			return true;
		// not being used with 5D
		case SLAVE_CMD_TOGGLE_MOTOR_1:
			pop8();
			return true;
		// not being used with 5D
		case SLAVE_CMD_TOGGLE_MOTOR_2: 
			pop8();
			return true;
		case SLAVE_CMD_SET_MOTOR_1_PWM:
			pop8();
			return true;
		case SLAVE_CMD_SET_MOTOR_2_PWM:
			pop8();
			return true;
		case SLAVE_CMD_SET_MOTOR_1_DIR:
			pop8();
			return true;
		case SLAVE_CMD_SET_MOTOR_2_DIR:
			pop8();
			return true;
		case SLAVE_CMD_SET_MOTOR_1_RPM:
			pop32();
			return true;
		case SLAVE_CMD_SET_MOTOR_2_RPM:
			pop32();
			return true;
		case SLAVE_CMD_SET_SERVO_1_POS:
			pop8();
			return true;
		case SLAVE_CMD_SET_SERVO_2_POS:
			pop8();
			return true;
	}
	return false;
}

// A fast slice for processing commands and refilling the stepper queue, etc.
void runCommandSlice() {
	// get command from SD card if building from SD
	if (sdcard::isPlaying()) {
		while (command_buffer.getRemainingCapacity() > 0 && sdcard::playbackHasNext()) {
			sd_count++;
			command_buffer.push(sdcard::playbackNext());
		}
		if(!sdcard::playbackHasNext() && (sd_count < sdcard::getFileSize()) && !sdcard_reset){
			
		sd_fail_count++;
		if(sd_fail_count > 5){
			Motherboard::getBoard().getInterfaceBoard().resetLCD();
			Motherboard::getBoard().errorResponse(STATICFAIL_MSG);
			sdcard_reset = true;
			/// temporary behavior until we get a method to restart the build
			steppers::abort();
			command_buffer.reset();

			// cool heaters
			Motherboard &board = Motherboard::getBoard();
			board.getExtruderBoard(0).getExtruderHeater().set_target_temperature(0);
			board.getExtruderBoard(1).getExtruderHeater().set_target_temperature(0);
			board.getPlatformHeater().set_target_temperature(0);
    
			Point target = steppers::getPlannerPosition();
			target[2] = 150L*stepperAxisStepsPerMM(Z_AXIS);
			command::pause(false);
			steppers::setTarget(target, 150);
			sdcard::finishPlayback();
			sd_fail_count = 0;
			
			/// do the sd card initialization files
			//command_buffer.reset();
			//sdcard::startPlayback(host::getBuildName());
			//uint32_t count;
			//while(count < sd_count){
			//	sdcard::playbackNext();
			//}
			}
		}else if(!sdcard::playbackHasNext() && command_buffer.isEmpty() && isReady()){
			sdcard::finishPlayback();
		}
	}
	// get command from onboard script if building from onboard
	if(utility::isPlaying()){		
		while (command_buffer.getRemainingCapacity() > 0 && utility::playbackHasNext()){
			command_buffer.push(utility::playbackNext());
		}
		if(!utility::playbackHasNext() && command_buffer.isEmpty() && isReady()){
			utility::finishPlayback();
		}
	}
	
	// if printer is not waiting for tool or platform to heat, we need to make
	// sure the extruders are not in a paused state.  this is relevant when 
	// heating using the control panel in desktop software
	if(check_temp_state){
		if (Motherboard::getBoard().getPlatformHeater().has_reached_target_temperature()){
			// unpause extruder heaters in case they are paused
    		Motherboard::getBoard().getExtruderBoard(0).getExtruderHeater().Pause(false);
	  		Motherboard::getBoard().getExtruderBoard(1).getExtruderHeater().Pause(false);
	  		check_temp_state = false;
	 	}
	}
	// don't execute commands if paused or shutdown because of heater failure
	if (paused || heat_shutdown) {	return; }
    
	if (mode == HOMING) {
		if (!steppers::isRunning()) {
			mode = READY;
		} else if (command_buffer_timeout.hasElapsed()) {
			steppers::abort();
			mode = READY;
		}
	}
	if (mode == MOVING) {
		if (!steppers::isRunning()) {
			mode = READY;
		} 
	}
	if (mode == DELAY) {
		// check timers
		if (command_buffer_timeout.hasElapsed()) {
			mode = READY;
		}
	}
	if (mode == WAIT_ON_TOOL) {
		if(command_buffer_timeout.hasElapsed()){
			Motherboard::getBoard().errorResponse(ERROR_HEATING_TIMEOUT); 
			mode = READY;		
		}else if( Motherboard::getBoard().getExtruderBoard(currentToolIndex).getExtruderHeater().has_reached_target_temperature() && 
			!Motherboard::getBoard().getExtruderBoard(currentToolIndex).getExtruderHeater().isPaused()){
			Piezo::playTune(TUNE_PRINT_START);
			mode = READY;
		}else if(!Motherboard::getBoard().getExtruderBoard(currentToolIndex).getExtruderHeater().isHeating() && 
			!Motherboard::getBoard().getExtruderBoard(currentToolIndex).getExtruderHeater().isPaused()){
			mode = READY;
		}
	}
	if (mode == WAIT_ON_PLATFORM) {
		if(command_buffer_timeout.hasElapsed()){
			Motherboard::getBoard().errorResponse(ERROR_PLATFORM_HEATING_TIMEOUT); 
			mode = READY;		
		} else if (!Motherboard::getBoard().getPlatformHeater().isHeating()){
			mode = READY;
		}
		else if(Motherboard::getBoard().getPlatformHeater().has_reached_target_temperature()){
			mode = READY;
		}
	}
	if (mode == WAIT_ON_BUTTON) {
		if (command_buffer_timeout.hasElapsed()) {
			if (button_timeout_behavior & (1 << BUTTON_TIMEOUT_ABORT)) {
				// Abort build!
				// We'll interpret this as a catastrophic situation
				// and do a full reset of the machine.
				Motherboard::getBoard().reset(false);

			} else {
				mode = READY;
				Motherboard::getBoard().setBoardStatus(Motherboard::STATUS_WAITING_FOR_BUTTON, false);
				//	Motherboard::getBoard().interfaceBlink(0,0);
			}
		} else {
			// Check buttons
			InterfaceBoard& ib = Motherboard::getBoard().getInterfaceBoard();
			if (ib.buttonPushed()) {	
				if(button_timeout_behavior & (1 << BUTTON_CLEAR_SCREEN)) {
					ib.popScreen();
				}
				Motherboard::getBoard().interfaceBlink(0,0);
				Motherboard::getBoard().setBoardStatus(Motherboard::STATUS_WAITING_FOR_BUTTON, false);
				RGB_LED::setDefaultColor();
				mode = READY;
			}
		}
	}
	

	if (mode == READY) {
		
		/// motion capable pause
		/// this loop executes cold heat pausing and restart
		if(active_paused){
			// sleep called, waiting for current stepper move to finish
			if((sleep_mode == SLEEP_START_WAIT) && st_empty()){
				if(sleep_type == SLEEP_TYPE_COLD){
					Motherboard::getBoard().getInterfaceBoard().errorMessage(SLEEP_PREP_MSG);
				}else if(sleep_type == SLEEP_TYPE_FILAMENT){
					Motherboard::getBoard().getInterfaceBoard().errorMessage(CHANGE_FILAMENT_PREP_MSG);
				}
				startSleep();
				sleep_mode = SLEEP_MOVING;
			// moving to sleep waiting position
			}else if((sleep_mode == SLEEP_MOVING) && st_empty()){
				interface::popScreen();
				sleep_mode = SLEEP_ACTIVE;
				// If we are in change filament type sleep, play tune and start the timeout counter
				if (sleep_type == SLEEP_TYPE_FILAMENT) {
					command_buffer_timeout.start(USER_FILAMENT_INPUT_TIMEOUT);
					Piezo::playTune(TUNE_FILAMENT_START);
				}
				// If we are cold sleeping, lower current/torque of the stepper motors
				uint8_t pot_value = 20;
				if(sleep_type == SLEEP_TYPE_COLD){
					for(uint8_t i = 0; i < 2; ++i)
					{
						steppers::setAxisPotValue(i, pot_value);
					}
					for(uint8_t i = 3; i < 5; ++i)
					{
						steppers::setAxisPotValue(i, pot_value);
					}
				}
			// sleep_active state, this is where we actually sleep
			// Transitioning to previous state with type COLD if a timeout is reaching
			}else if(sleep_mode == SLEEP_ACTIVE) {
				// Check to see if we are changing filament and have timed out
				if ((sleep_type == SLEEP_TYPE_FILAMENT) && command_buffer_timeout.hasElapsed()) {
					// Reset timer
					command_buffer_timeout = Timeout();
					// Change sleep to cold sleep and cool down motors
					sleep_type = SLEEP_TYPE_COLD;
					// Pop the change filament screen
					interface::popScreen();
					// Display the warning indicating we time out
					bool reset_request = false;
					Motherboard::getBoard().errorResponse(TIMED_OUT_OF_CHANGE_FILAMENT, reset_request);
					// Lower current to the stepper motors except for the Z motor effectively lowering hold torque
					uint8_t pot_value = 20;
					for(uint8_t i = 0; i < 2; ++i)
					{
						steppers::setAxisPotValue(i, pot_value);
					}
					for(uint8_t i = 3; i < 5; ++i)
					{
						steppers::setAxisPotValue(i, pot_value);
					}
					// cool heaters
					Motherboard &board = Motherboard::getBoard();
					board.getExtruderBoard(0).getExtruderHeater().set_target_temperature(0);
					board.getExtruderBoard(1).getExtruderHeater().set_target_temperature(0);
					board.getPlatformHeater().set_target_temperature(0);
				}
			// restart called or
			// restart called while still moving to waiting position
			// wait for move to wait position to finish before restarting
			}else if(((sleep_mode == SLEEP_MOVING_WAIT) && st_empty()) ||
						(sleep_mode == SLEEP_RESTART)){
				uint8_t pot_value = 127;
				if (!(sleep_type == SLEEP_TYPE_FILAMENT)) {
					for(uint8_t i = 0; i < 2; ++i)
					{
						steppers::setAxisPotValue(i, pot_value);
					}
				}
				if(sleep_type == SLEEP_TYPE_COLD){
					for(uint8_t i = 3; i < 5; ++i)
					{
						steppers::setAxisPotValue(i, pot_value);
					}
				}
				Motherboard::getBoard().getInterfaceBoard().errorMessage(RESTARTING_MSG);
				// wait for platform to heat
				currentToolIndex = 0;
				mode = WAIT_ON_PLATFORM;
				/// set timeout to 30 minutes
				command_buffer_timeout.start(USER_INPUT_TIMEOUT);
				sleep_mode = SLEEP_HEATING_P;
				Motherboard::getBoard().StartProgressBar(3,0,20);
			// when platform is hot, wait for tool A
			}else if(sleep_mode == SLEEP_HEATING_P){
				currentToolIndex = 0;
				mode = WAIT_ON_TOOL;
				/// set timeout to 30 minutes
				command_buffer_timeout.start(USER_INPUT_TIMEOUT);
				sleep_mode = SLEEP_HEATING_A;
			// when tool A is hot, wait for tool B
			}else if (sleep_mode == SLEEP_HEATING_A){
				currentToolIndex = 1;
				mode = WAIT_ON_TOOL;
				/// set timeout to 30 minutes
				command_buffer_timeout.start(USER_INPUT_TIMEOUT);
				sleep_mode = SLEEP_RETURN;
			// when heaters are hot, return to print
			}else if (sleep_mode == SLEEP_RETURN){
				Motherboard::getBoard().StopProgressBar();
				stopSleep();
				sleep_mode = SLEEP_FINISHED;
			// when position is reached, restart print
			}else if((sleep_mode == SLEEP_FINISHED) && st_empty()){
				Motherboard::getBoard().getInterfaceBoard().popToOnboardStart();
				sleep_mode = SLEEP_NONE;
				active_paused = false;
			}
			return;

		}
		
		// process next command on the queue.
		if ((command_buffer.getLength() > 0)){
			Motherboard::getBoard().resetUserInputTimeout();
			
			uint8_t command = command_buffer[0];

			//If we're running acceleration, we want to populate the pipeline buffer,
			//but we also need to sync (wait for the pipeline buffer to clear) on certain
			//commands, we do that here
			//If we're not pipeline'able command, then we sync here,
			//by waiting for the pipeline buffer to empty before continuing
			if ((command != HOST_CMD_QUEUE_POINT_EXT) &&
					(command != HOST_CMD_QUEUE_POINT_NEW) &&
					(command != HOST_CMD_QUEUE_POINT_NEW_EXT) &&
					(command != HOST_CMD_ENABLE_AXES ) &&
					(command != HOST_CMD_SET_BUILD_PERCENT ) &&
					(command != HOST_CMD_CHANGE_TOOL ) &&
					(command != HOST_CMD_SET_POSITION_EXT) &&
					(command != HOST_CMD_SET_ACCELERATION_TOGGLE) &&
					(command != HOST_CMD_RECALL_HOME_POSITION) &&
					(command != HOST_CMD_FIND_AXES_MINIMUM) &&
					(command != HOST_CMD_FIND_AXES_MAXIMUM) &&
					(command != HOST_CMD_TOOL_COMMAND)){
				if ( ! st_empty() )     return;
			}

			if (command == HOST_CMD_QUEUE_POINT_EXT || command == HOST_CMD_QUEUE_POINT_NEW ||
					command == HOST_CMD_QUEUE_POINT_NEW_EXT) {
				handleMovementCommand(command);
			}  else if (command == HOST_CMD_CHANGE_TOOL) {
				if (command_buffer.getLength() >= 2) {
					pop8(); // remove the command code
					currentToolIndex = pop8();
					line_number++;
          
					steppers::changeToolIndex(currentToolIndex);
				}
			} else if (command == HOST_CMD_ENABLE_AXES) {
				if (command_buffer.getLength() >= 2) {
					pop8(); // remove the command code
					uint8_t axes = pop8();
					line_number++;
					
					// only execute this command if our buffer is empty
					// this is because skeinforge sends a zillion spurious enable commands that cause
					// clicking in the motors.
					if(st_empty()){
						bool enable = (axes & 0x80) != 0;
						for (int i = 0; i < STEPPER_COUNT; i++) {
							if ((axes & _BV(i)) != 0) {
								steppers::enableAxis(i, enable);
							}
						}
					}
				}
			} else if (command == HOST_CMD_STREAM_VERSION){
				if(command_buffer.getLength() >= 11){
      
					pop8();// remove the command code
					// stream number
					uint8_t version_high = pop8();
					uint8_t version_low = pop8();

					if((version_high *100 + version_low) != stream_version){
						Motherboard::getBoard().errorResponse(ERROR_STREAM_VERSION);
					}
					// extra version
					pop8();
					// checksum (currently not implemented)
					pop32();
					uint16_t bot_type = pop16();
					// extra bytes
					#ifdef MODEL_REPLICATOR
					if(bot_type != 0xD314){
						Motherboard::getBoard().errorResponse(ERROR_BOT_TYPE, false, false);
					} 
					#elif MODEL_REPLICATOR2
					if(bot_type != 0xB015){
						Motherboard::getBoard().errorResponse(ERROR_BOT_TYPE, false, false);
					} 
					#endif
					// eleven extra bytes
					pop16();
					pop32();
					pop32();
					pop8();
					line_number++;    
				}
			} else if (command == HOST_CMD_SET_POSITION_EXT) {
				// check for completion
				if (command_buffer.getLength() >= 21) {
					pop8(); // remove the command code
					int32_t x = pop32();
					int32_t y = pop32();
					int32_t z = pop32();
					int32_t a = pop32();
					int32_t b = pop32();
					line_number++;
					
					steppers::definePosition(Point(x,y,z,a,b));
				}
			} else if (command == HOST_CMD_DELAY) {
				if (command_buffer.getLength() >= 5) {
					mode = DELAY;
					pop8(); // remove the command code
					// parameter is in milliseconds; timeouts need microseconds
					uint32_t microseconds = pop32() * 1000L;
					line_number++;
					
					command_buffer_timeout.start(microseconds);
				}
			} else if (command == HOST_CMD_PAUSE_FOR_BUTTON) {
				if (command_buffer.getLength() >= 5) {
					pop8(); // remove the command code
					button_mask = pop8();
					uint16_t timeout_seconds = pop16();
					button_timeout_behavior = pop8();
					line_number++;
					
					if (timeout_seconds != 0) {
						command_buffer_timeout.start(timeout_seconds * 1000L * 1000L);
					} else {
						command_buffer_timeout = Timeout();
					}
					// set button wait via interface board
					Motherboard::getBoard().interfaceBlink(25,15);
					InterfaceBoard& ib = Motherboard::getBoard().getInterfaceBoard();
					ib.waitForButton(button_mask);
					Motherboard::getBoard().setBoardStatus(Motherboard::STATUS_WAITING_FOR_BUTTON, true);
					mode = WAIT_ON_BUTTON;
				}
			} else if (command == HOST_CMD_DISPLAY_MESSAGE) {
        
				InterfaceBoard& ib = Motherboard::getBoard().getInterfaceBoard();
				MessageScreen* scr = ib.GetMessageScreen();
				if (command_buffer.getLength() >= 6) {
					pop8(); // remove the command code
					uint8_t options = pop8();
					uint8_t xpos = pop8();
					uint8_t ypos = pop8();
					uint8_t timeout_seconds = pop8();
					line_number++;
					
					// check message clear bit
					if ( (options & (1 << 0)) == 0 ) { scr->clearMessage(); }
					// set position and add message
					scr->setXY(xpos,ypos);
					scr->addMessage(command_buffer); 
					
					// push message screen if the full message has been recieved
					if((options & (1 << 1))){          
						if (ib.getCurrentScreen() != scr) {
							ib.pushScreen(scr);
						} else {
							scr->refreshScreen();
						}
						// set message timeout if not a buttonWait call
						if ((timeout_seconds != 0) && (!(options & (1 <<2)))) {
								scr->setTimeout(timeout_seconds);//, true);
						}
						
						if (options & (1 << 2)) { // button wait bit --> start button wait
							if (timeout_seconds != 0) {
								command_buffer_timeout.start(timeout_seconds * 1000L * 1000L);
							} else {
								command_buffer_timeout = Timeout();
							}
							button_mask = (1 << ButtonArray::CENTER);  // center button
							button_timeout_behavior &= (1 << BUTTON_CLEAR_SCREEN);
							Motherboard::getBoard().interfaceBlink(25,15);
							InterfaceBoard& ib = Motherboard::getBoard().getInterfaceBoard();
							ib.waitForButton(button_mask);
							Motherboard::getBoard().setBoardStatus(Motherboard::STATUS_WAITING_FOR_BUTTON, true);
							mode = WAIT_ON_BUTTON;
						}
					}
				}
					
			} else if (command == HOST_CMD_FIND_AXES_MINIMUM ||
					command == HOST_CMD_FIND_AXES_MAXIMUM) {
				if (command_buffer.getLength() >= 8) {
					pop8(); // remove the command
					uint8_t flags = pop8();
					uint32_t feedrate = pop32(); // feedrate in us per step
					uint16_t timeout_s = pop16();
					line_number++;
					
					mode = HOMING;
					command_buffer_timeout.start(timeout_s * 1000L * 1000L);
					steppers::startHoming(command==HOST_CMD_FIND_AXES_MAXIMUM,
							flags,
							feedrate);
				}
			} else if (command == HOST_CMD_WAIT_FOR_TOOL) {
				if (command_buffer.getLength() >= 6) {
					mode = WAIT_ON_TOOL;
					pop8();
					currentToolIndex = pop8();
					pop16(); //uint16_t toolPingDelay = (uint16_t)pop16();
					uint16_t toolTimeout = (uint16_t)pop16();
					line_number++;
					
					// if we re-add handling of toolTimeout, we need to make sure
					// that values that overflow our counter will not be passed)
					command_buffer_timeout.start(toolTimeout*1000000L);
				}
			} else if (command == HOST_CMD_WAIT_FOR_PLATFORM) {
				// FIXME: Almost equivalent to WAIT_FOR_TOOL
				if (command_buffer.getLength() >= 6) {
					mode = WAIT_ON_PLATFORM;
					pop8();
					currentToolIndex = pop8();
					pop16(); //uint16_t toolPingDelay = (uint16_t)pop16();
					uint16_t toolTimeout = (uint16_t)pop16();
					line_number++;
					
					// if we re-add handling of toolTimeout, we need to make sure
					// that values that overflow our counter will not be passed)
					command_buffer_timeout.start(toolTimeout*1000000L);
				}
			} else if (command == HOST_CMD_STORE_HOME_POSITION) {

				// check for completion
				if (command_buffer.getLength() >= 2) {
					pop8();
					uint8_t axes = pop8();
					line_number++;
					
					// Go through each axis, and if that axis is specified, read it's value,
					// then record it to the eeprom.
					for (uint8_t i = 0; i < STEPPER_COUNT; i++) {
						if ( axes & (1 << i) ) {
							uint16_t offset = eeprom_offsets::AXIS_HOME_POSITIONS_MM + 4*i;
							uint32_t position = steppers::getStepperPosition()[i];
							// convert position to mmm
							position = (int32_t)(stepperAxisStepsToMM(position, i) * 1000.0) / 1000;
							cli();
							eeprom_write_block(&position, (void*) offset, 4);
							sei();
						}
					}
				}
			} else if (command == HOST_CMD_RECALL_HOME_POSITION) {
				// check for completion
				if (command_buffer.getLength() >= 2) {
					pop8();
					uint8_t axes = pop8();
					line_number++;

					Point newPoint = steppers::getStepperPosition();

					for (uint8_t i = 0; i < STEPPER_COUNT; i++) {
						if ( axes & (1 << i) ) {
							uint16_t offset = eeprom_offsets::AXIS_HOME_POSITIONS_MM + 4*i;
							cli();
							eeprom_read_block(&(newPoint[i]), (void*) offset, 4);
							sei();
							// convert new point to steps
							newPoint[i]  = stepperAxisMMToSteps(newPoint[i], i);
						}
					}

					steppers::defineHomePosition(newPoint);
				}

			} else if (command == HOST_CMD_SET_POT_VALUE){
				if (command_buffer.getLength() >= 3) {
					pop8(); // remove the command code
					uint8_t axis = pop8();
					uint8_t value = pop8();
					line_number++;
					steppers::setAxisPotValue(axis, value);
				}
			} else if (command == HOST_CMD_SET_RGB_LED){
				if (command_buffer.getLength() >= 6) {
					pop8(); // remove the command code

					uint8_t red = pop8();
					uint8_t green = pop8();
					uint8_t blue = pop8();
					uint8_t blink_rate = pop8();

					pop8(); //uint8_t effect = pop8();
					line_number++;
					RGB_LED::setLEDBlink(blink_rate);
					RGB_LED::setCustomColor(red, green, blue);

				}
			} else if (command == HOST_CMD_SET_BEEP){
				if (command_buffer.getLength() >= 6) {
					pop8(); // remove the command code
					uint16_t frequency= pop16();
					uint16_t beep_length = pop16();
					pop8(); //uint8_t effect = pop8();
					line_number++;
					Piezo::setTone(frequency, beep_length);

				}
			} else if (command == HOST_CMD_TOOL_COMMAND) {
				if (command_buffer.getLength() >= 4) { // needs a payload
					uint8_t payload_length = command_buffer[3];
					if (command_buffer.getLength() >= 4U+payload_length) {
						pop8(); // remove the command code
						line_number++;
						processExtruderCommandPacket();
					}
				}
			} else if (command == HOST_CMD_SET_BUILD_PERCENT){
				if (command_buffer.getLength() >= 3){
					pop8(); // remove the command code
					uint8_t percent = pop8();
					pop8(); // remove the reserved byte
					line_number++;
					interface::setBuildPercentage(percent);
				}
			} else if (command == HOST_CMD_QUEUE_SONG ) //queue a song for playing
 			{
				/// Error tone is 0,
				/// End tone is 1,
				/// all other tones user-defined (defaults to end-tone)
				if (command_buffer.getLength() >= 2){
					pop8(); // remove the command code
					uint8_t songId = pop8();
					line_number++;
					Piezo::playTune(songId);
				}

			} else if ( command == HOST_CMD_RESET_TO_FACTORY) {
				/// reset EEPROM settings to the factory value. Reboot bot.
				if (command_buffer.getLength() >= 2){
					pop8(); // remove the command code
					pop8(); //uint8_t options = pop8();
					line_number++;
					eeprom::factoryResetEEPROM();
					Motherboard::getBoard().reset(false);
				}
			} else if ( command == HOST_CMD_BUILD_START_NOTIFICATION) {
				if (command_buffer.getLength() >= 5){
					pop8(); // remove the command code
					pop32(); //int buildSteps = pop32();
					line_number++;
					host::handleBuildStartNotification(command_buffer);		
					start_build_flag = true;
					//Make sure heat hold is not on (for safety)
					Motherboard::getBoard().abortHeatHoldTimeout();
				}
			} else if ( command == HOST_CMD_BUILD_END_NOTIFICATION) {
				if (command_buffer.getLength() >= 2){
					pop8(); // remove the command code
					uint8_t flags = pop8();
					line_number++;
					host::handleBuildStopNotification(flags);
				}
			} else if ( command == HOST_CMD_SET_ACCELERATION_TOGGLE) {
				if (command_buffer.getLength() >= 2){
					pop8(); // remove the command code
					uint8_t status = pop8();
					line_number++;
					steppers::setSegmentAccelState(status == 1);
				} 
			} else {
			}
		}
	}
	
	/// we're not handling overflows in the line counter.  Possibly implement this later.
	if(line_number > MAX_LINE_COUNT){
		line_number = MAX_LINE_COUNT + 1;
	}
}
}


