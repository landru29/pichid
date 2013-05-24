// Demonstration code of USB I/O on PIC 18F2455 (and siblings) -
// turn LED on/off and echo a buffer back to host.
//
// Copyright (C) 2005 Alexander Enzmann
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
//

#include <pic18fregs.h>
#include "usb.h"
#include "servo.h"
#include "motor.h"
#include "solenoid.h"

// Define configuration registers (fuses)
#if defined(pic18f2550) || defined(pic18f2455) || defined(pic18f4550) || defined(pic18f4455)
code char at 0x300000 CONFIG1L = 0x24; // USB, /2 post (48MHz), /5 pre (20 MHz)
code char at 0x300001 CONFIG1H = 0x0e; // IESO=0, FCMEN=0, HS-PLL (40MHz)
code char at 0x300002 CONFIG2L = 0x20; // Brown out off, PWRT On
code char at 0x300003 CONFIG2H = 0x00; // WDT off
code char at 0x300004 CONFIG3L = 0xff; // Unused configuration bits
code char at 0x300005 CONFIG3H = 0x01; // No MCLR, PORTB digital, CCP2 - RC1
code char at 0x300006 CONFIG4L = 0x80; // ICD off, ext off, LVP off, stk ovr off
code char at 0x300007 CONFIG4H = 0xff; // Unused configuration bits
code char at 0x300008 CONFIG5L = 0xff; // No code read protection
code char at 0x300009 CONFIG5H = 0xff; // No data/boot read protection
code char at 0x30000A CONFIG6L = 0xff; // No code write protection
code char at 0x30000B CONFIG6H = 0xff; // No data/boot/table protection
code char at 0x30000C CONFIG7L = 0xff; // No table read protection
code char at 0x30000D CONFIG7H = 0xff; // No boot table protection
#endif

// HID feature buffer
volatile unsigned char HIDFeatureBuffer[HID_FEATURE_REPORT_BYTES];

void high_isr(void) shadowregs interrupt 1
{
    ;
}

void low_isr(void) shadowregs interrupt 2
{
    // If the timer associated with the servo timed out, then
    // ask the servo code to handle it.
    if (PIR2bits.TMR3IF)
    {
        servo1ISR();
    }
}

// Allocate buffers in RAM for storage of bytes that have either just
// come in from the SIE or are waiting to go out to the SIE.
char txBuffer[HID_INPUT_REPORT_BYTES];
char rxBuffer[HID_OUTPUT_REPORT_BYTES];

// Entry point for user initialization
void UserInit(void)
{
    // Configure analog
    ADCON0 = 0;    // A/D off, AN0 is selected channel
    ADCON1 = 0x0e; // Volt ref Vdd/Vss, only AN0 analog
    ADCON2 = 0x22; // Left justified, Fosc/32, 8 Tad
    ADCON0bits.ADON = 1; // enable the A/D module

    // Set RA4 (LED1) as an output
    TRISAbits.TRISA4 = 0;

    // Configure PWM.  Both motor and solonoid use it.
	//    Duty cycle = (PR2 + 1) * 4 * Tosc * (TMR2 Prescale)
	//    PWM Freq   = 1 / (Duty cycle)
	// Assuming the system clock is 48 MHz (Tosc = 1/48 uSec):
	// For PR2 = 0x7f, 4 * Tosc = 0.083 uSec;
	//    Duty cycle = 128 * 0.083 uSec = 10.67 uSec
	//    Frequency  = 93.750 KHz
    PR2   = 0x7f;        // Set PWM freq to 93.75 KHz
    PIR1bits.TMR2IF = 0; // Clear Timer 2 interrupt flag
    T2CON = 0x04;        // 1:1 prescale, 1:1 postscale, T2 on

    // Enable PWM outputs after a new PWM cycle has started
    while (!PIR1bits.TMR2IF)
        ;

    // Configure devices
    configServos();
    motorInit();
    configSolenoid();

    // Check RE3 to see if the device is self-powered.
    // TBD: check self power every USB GET_STATUS
    selfPowered = PORTEbits.RE3;

	// Enable interrupts.
	//RCONbits.IPEN   = 1; // Enable priority levels on interrupts
	//INTCONbits.GIEL = 1; // Enable low-priority interrupts
	//INTCONbits.GIEH = 1; // Enable high-priority interrupts
}

// Central processing loop.  Whenever the firmware isn't busy servicing
// the USB, we will get control here to do other processing.
void ApplicationTasks(void)
{
	// Note: put tasks here that have to be performed when
	// the circuit is powered.

	// Check to see if we have external power.
    selfPowered = PORTEbits.RE3;

	// TBD: If not on external power (self powered), then disable
	// motor and servo.  They draw too much current to be powered
	// by a USB port.

	// Need to periodically check the voltage generated by
	// the boost circuit to ensure it doesn't go too high
	// (or stay too low after discharge through solenoid).
    checkSolenoidVoltage();

    // User Application USB tasks
    if ((deviceState < CONFIGURED) || (UCONbits.SUSPND==1))
        return;

	// Note: Put tasks here that only make sense if we are attached
	// to the host.

	// Enable interrupts.
	RCONbits.IPEN   = 1; // Enable priority levels on interrupts
	INTCONbits.GIEL = 1; // Enable low-priority interrupts
	INTCONbits.GIEH = 1; // Enable high-priority interrupts
}

// Initialization for a SET_FEATURE request.  This routine will be
// invoked during the setup stage and is used to set up the buffer
// for receiving data from the host
void SetupFeatureReport(byte reportID)
{
    if (reportID == 0)
    {
        // When the report arrives in the data stage, the data will be  
        // stored in HIDFeatureBuffer.
        inPtr = (byte*)&HIDFeatureBuffer;
    }
}

// Post processing for a SET_FEATURE request.  After all the data has
// been delivered from host to device, this will be invoked to perform
// application specific processing.
void SetFeatureReport(byte reportID)
{
#if DEBUG_PRINT
    //printf("SetFeatureReport(0x%hx)\r\n", reportID);
#endif
    // Currently only handling report 0, ignore any others.
    if (reportID == 0)
    {
        byte solenoidFlag = HIDFeatureBuffer[3];

        // Set the state of the LED based on bit 0 of the first byte
        // of the feature report.
        PORTAbits.RA4 = (HIDFeatureBuffer[0] & 0x01);

        // Set the speed of the motor based on the second byte
        motorSetSpeed((char)(HIDFeatureBuffer[1] - 128));

        // Set the servo position based on the third byte
        setServo1(HIDFeatureBuffer[2]);

        // Fire the solenoid if bit 0 of the fourth byte is set
        // solenoidFlag = HIDFeatureBuffer[3]; // Workaround - bad code if HIDFeatureBuffer[3] & 0x01 in if statement
        if (solenoidFlag & 0x01)
        {
            // Fire the solenoid
            actuateSolenoid();
        }
        else
        {
            // Turn off power to the solenoid
            clearSolenoid();
        }
    }
}

// Handle a feature report request on the control pipe
void GetFeatureReport(byte reportID)
{
#if DEBUG_PRINT
    //printf("GetFeatureReport(0x%uhx): 0x%hx, 0x%hx\r\n",
    //    (byte)reportID, (byte)HIDFeatureBuffer[0],
    //    (byte)HIDFeatureBuffer[1]);
#endif
    if (reportID == 0)
    {
        // Handle report #0
        outPtr = (byte *)&HIDFeatureBuffer;
        HIDFeatureBuffer[0] = PORTA;
        HIDFeatureBuffer[1] = PORTB;
        HIDFeatureBuffer[2] = PORTC;
        HIDFeatureBuffer[3] = getSolenoidVoltage();
        HIDFeatureBuffer[4] = motorGetSpeed();
        HIDFeatureBuffer[5] = getServo1();
		HIDFeatureBuffer[6] = 0;
		if (selfPowered) HIDFeatureBuffer[6] |= 0x01;
		if (remoteWakeup) HIDFeatureBuffer[6] |= 0x02;
        wCount = HID_FEATURE_REPORT_BYTES;
    }
}

// Handle control out.  This might be an alternate way of processing
// an output report, so all that's needed is to point the output
// pointer to the output buffer
// Initialization for a SET_REPORT request.  This routine will be
// invoked during the setup stage and is used to set up the buffer
// for receiving data from the host
void SetupOutputReport(byte reportID)
{
    if (reportID == 0)
    {
        // When the report arrives in the data stage, the data will be  
        // stored in HIDFeatureBuffer
        inPtr = (byte*)&HIDRxBuffer;
    }
}

// Post processing for a SET_REPORT request.  After all the data has
// been delivered from host to device, this will be invoked to perform
// application specific processing.
void SetOutputReport(byte reportID)
{
#if DEBUG_PRINT
    //printf("SetOutputReport(0x%hx)\r\n", reportID);
#endif
    // Currently only handling report 0, ignore any others.
    if (reportID != 0)
        return;

    // TBD: do something.  Not currently implemented because the output
    // report is being handled by an interrupt endpoint.
}

// Handle a control input report
void GetInputReport(byte reportID)
{
#if DEBUG_PRINT
    printf("GetInputReport: 0x%uhx\r\n", reportID);
#endif
    if (reportID == 0)
    {
        // Send back the contents of the HID report
        // TBD: provide useful information...
        outPtr = (byte *)&HIDTxBuffer;

        // The number of bytes in the report (from usb.h).
        wCount = HID_INPUT_REPORT_BYTES;
    }
}

// Entry point of the firmware
void main(void)
{
    // Set all I/O pins to digital
    ADCON1 |= 0x0F;
    
    // Initialize USB
    UCFG = 0x14; // Enable pullup resistors; full speed mode

    deviceState = DETACHED;
    remoteWakeup = 0x00;
    selfPowered = 0x00;
    currentConfiguration = 0x00;

    // Call user initialization function
    UserInit();

    while(1)
    {
        // Ensure USB module is available
        EnableUSBModule();

        // As long as we aren't in test mode (UTEYE), process
        // USB transactions.
        if(UCFGbits.UTEYE != 1)
            ProcessUSBTransactions();

        // Application specific tasks
        ApplicationTasks();
    }
}
