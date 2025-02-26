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

#include "ButtonArray.hh"
#include "CommandParser.hh"
#include "Configuration.hh"
#include "LiquidCrystalSerial.hh"
#include "Pin.hh"
#include "SoftI2cManager.hh"
#include "Motion.hh"
#include "UART.hh"
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/atomic.h>
#include <util/delay.h>
#include <stdio.h>

void reset(bool hard_reset) {
    ATOMIC_BLOCK(ATOMIC_FORCEON) {
        // TODO investigate below
        // uint8_t resetFlags = MCUSR & 0x0f;
        // check for brown out reset flag and report if true

        // clear watch dog timer and re-enable
        if (hard_reset) {
            // ATODO: remove disable
            wdt_disable();
            MCUSR = 0x0;
            wdt_enable(WDTO_8S); // 8 seconds is max timeout
        }
    }
}

void init_timers() {
    /// TIMER 5: stepper interupts. Currently running at 7.8KHz.
    /// CS = 010   -- prescaler is CLK/8
    /// WGM = 0100 -- CTC mode, count up from 0 to OCR5A and then reset to 0
    TCCR5A = 0x00;         // Output compare pins disabled, WGM1:0 = 00
    TCCR5B = 0x0A;         // 00001010 WGM3:2 = 01, CS = 010
    OCR5A = 0x0c8;         // 16MHz / 8(prescale) / 200(OCR5A) == 10000Hz
    //OCR5A = 0x080;         // 16MHz / 8(prescale) / 128(OCR5A) == 15625Hz ** This is the best for stability
    //OCR5A = 0x064;         // 16MHz / 8(prescale) / 200(OCR5A) == 20000Hz
    //OCR5A = 0x100;         // 16MHz / 8(prescale) / 256(OCR5A) == 7812.5Hz
    TIMSK5 |= 1 << OCIE5A; // turn on OCR5A match interrupt
}

int intdbg = 0;
LiquidCrystalSerial lcd(LCD_STROBE, LCD_DATA, LCD_CLK);

typedef enum {
    RC_OK,
    RC_ERR,
    RC_FULL,
} ResultCode;

ResultCode handle_mcode() {
    const uint8_t v = cmd().code().value;
    switch (v) {
    case 3: // Plotter pen down
    case 4: // Plotter pen up
        if (!motion::queue_ready()) return RC_FULL;
        motion::enqueue_pen(v == 4); // M4 is up
        return RC_OK;
    case 230:  // Enable character echo
    case 231:  // Disable character echo
        set_echo(v == 230);
        return RC_OK;

    case 17:  // Enable selected steppers
    case 18:  // Disable selected steppers
        {
            bool en = v == 17;
            bool all = true;
            for (int i = 0; i < 3; i++)
                if (cmd()[i] != 0) {
                    motion::enable(i, en);
                    all = false;
                }
            if (all) // none specified, apply to all
                for (int i = 0; i < 3; i++) motion::enable(i, en);
        }
        return RC_OK;
    case 114: // Report current position
        {
            char buf[40];
            sprintf(buf,"X: %.2f Y: %.2f",
                    motion::get_axis_position(motion::X),
                    motion::get_axis_position(motion::Y));
            UART::write_string(buf);
        }
        return RC_OK;
    case 115: // Report if queue is empty
        if (motion::queue_done()) return RC_OK;
        return RC_FULL;
    default:
        return RC_ERR;
    }
}

ResultCode handle_gcode() {
    const uint8_t v = cmd().code().value;
    switch (v) {
    case 1:   // Linear move
    case 0:   // Rapid move
        {
            if (!motion::queue_ready()) return RC_FULL;
            float feedrate = cmd().has_param(F)?cmd()[F]:(v==0?DEFAULT_G0_FEEDRATE:DEFAULT_G1_FEEDRATE);
            motion::enqueue_move(cmd()[X], cmd()[Y], feedrate);
            return RC_OK;
        }
    case 4:   // Dwell
        if (!motion::queue_ready()) return RC_FULL;
        motion::enqueue_dwell(cmd()[P]);
        return RC_OK;
    case 92:
        motion::reset_axes();
        return RC_OK;
    case 100:
        {
            float feedrate = cmd().has_param(F)?cmd()[F]:(v==0?DEFAULT_G0_FEEDRATE:DEFAULT_G1_FEEDRATE);
            motion::set_jog(cmd()[X], cmd()[Y], feedrate);
            return RC_OK;
        }
    case 101:
        motion::set_jog(0,0,0);
        return RC_OK;
    default:
        return RC_ERR;
    }
}

int main() {
    INTERFACE_POWER.setDirection(true);
    INTERFACE_POWER.setValue(false);

    INTERFACE_LED_ONE.setDirection(true);
    INTERFACE_LED_TWO.setDirection(true);
    INTERFACE_LED_ONE.setValue(true);
    INTERFACE_LED_TWO.setValue(true);

    SoftI2cManager::getI2cManager().init();

    reset(true);
    motion::init();
    motion::setPotValue(X_POT_PIN, 40);
    motion::setPotValue(Y_POT_PIN, 80);
    init_timers();
    sei();

    ButtonArray::init();

    lcd.begin(LCD_SCREEN_WIDTH, LCD_SCREEN_HEIGHT);
    lcd.clear();
    lcd.home();
    lcd.setCursor(0, 0);

    UART::initialize();
    UART::write_string("Ready.");
    while (1) {
        wdt_reset();
        if (check_for_command()) {
            if (!cmd().is_ok()) {
                UART::write_string("err [parse]");
            } else {
                ResultCode result = RC_ERR;
                switch (cmd().code().code) {
                case 'M': result = handle_mcode(); break;
                case 'G': result = handle_gcode(); break;
                default: break; 
                }
                switch (result) {
                case RC_OK: UART::write_string("ok"); break;
                case RC_FULL: UART::write_string("full"); break;
                case RC_ERR: UART::write_string("err"); break;
                }
            }
            cmd().reset();
        }
        /*
          example of button scan call, we're not using the UI right now
        ButtonArray::scan();
        if (ButtonArray::pressed() & CENTER)
        */
    }
    return 0;
}

ISR(TIMER5_COMPA_vect) {
    // Handle stepper interrupt.
    motion::do_interrupt();
    // intdbg++;
}
