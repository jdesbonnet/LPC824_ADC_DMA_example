/*
===============================================================================
 Name        : LPC824_ADC_DMA_example.c
 Author      : Joe Desbonnet, jdesbonnet@gmail.com
 Version     : 1.0 (16 July 2015)
 Copyright   : None.
 Description : Example of how to use the LPC82x (eg LPC824) to read data from the
 ADC data registers directly to SRAM using Direct Memory Access (DMA). This allows
 capturing data at the max data rate (1.2Msps for LPC824) without any CPU intervention.

 DMA transfers are limited to 1024 words (which can be 8,16 or 32 bit words). However
 transfers can be chained to facilitate longer data capture. This is illustrated in
 this example by chaining 3 DMA transfers of 1024 words (samples) back-to-back. ADC
 samples are triggered using the SCT (SCT0_OUT3 is programmed as the ADC sample trigger).
 Since the DMA supports transfers of 8, 16, 32 bit words, I am transferring the lower
 16 bits of the ADC data register. Note that bits 15:4 of the ADC data register hold
 the ADC value. So a shift right of 4 bits (>>4) needs to be performed on each
 16 bit word by the CPU after capture to yield ADC values 0 - 4095.

 This example is built upon examples provided by NXP for the LPC824. I recommend having
 chapters 21 (ADC), 11,12 (DMA), 16 (SCT) of UM10800 LPC82x User Manual for reference.
===============================================================================
*/

#if defined (__USE_LPCOPEN)
#if defined(NO_BOARD_LIB)
#include "chip.h"
#else
#include "board.h"
#endif
#endif

#include <cr_section_macros.h>

#include <stdio.h>

//
// Hardware configuration
//
#define UART_BAUD_RATE 115200
#define ADC_CHANNEL 3
#define ADC_SAMPLE_RATE 500000

// Define function to pin mapping. Pin numbers here refer to PIO0_n
// and is not the same as a package pin number.
// Use PIO0_0 and PIO0_4 for UART RXD, TXD (same as ISP)
#define PIN_UART_RXD 0
#define PIN_UART_TXD 4
// General purpose debug pin
#define PIN_DEBUG 14
// Assign SCT0_OUT3 to external pin for debugging
#define PIN_SCT_DEBUG 15


#define DMA_BUFFER_SIZE 1024

static DMA_CHDESC_T dmaDescA;
static DMA_CHDESC_T dmaDescB;
static DMA_CHDESC_T dmaDescC;

// This is where we put ADC results
static uint16_t adc_buffer[DMA_BUFFER_SIZE*3];

static volatile int dmaBlockCount = 0;


/**
 * Send printf() to UART
 */
/*
int __sys_write(int fileh, char *buf, int len) {
	Chip_UART_SendBlocking(LPC_USART0, buf,len);
	return len;
}
*/

/**
 * @brief Pulse debugging pin to indicate an event on a oscilloscope trace.
 * @param n Number of times to pulse the pin.
 * @return None
 */
static void debug_pin_pulse (int n)
{
	int i;
	for (i = 0; i < n; i++) {
		Chip_GPIO_SetPinState(LPC_GPIO_PORT, 0, PIN_DEBUG, true);
		Chip_GPIO_SetPinState(LPC_GPIO_PORT, 0, PIN_DEBUG, false);
	}
}

/**
 * @brief Send one byte to UART. Block if UART busy.
 * @param n Byte to send to UART.
 * @return None.
 */
static void print_byte (uint8_t n) {
	//Chip_UART_SendBlocking(LPC_USART0, &n, 1);

	// Wait until data can be written to FIFO (TXRDY==1)
	while ( (Chip_UART_GetStatus(LPC_USART0) & UART_STAT_TXRDY) == 0) {}

	Chip_UART_SendByte(LPC_USART0, n);
}

/**
 * @brief Print a signed integer in decimal radix.
 * @param n Number to print.
 * @return None.
 */
static void print_decimal (int n) {
	char buf[10];
	int i = 0;

	// Special case of n==0
	if (n == 0) {
		print_byte('0');
		return;
	}

	// Handle negative numbers
	if (n < 0) {
		print_byte('-');
		n = -n;
	}

	// Use modulo 10 to get least significant digit.
	// Then /10 to shift digits right and get next least significant digit.
	while (n > 0) {
		buf[i++] = '0' + n%10;
		n /= 10;
	}

	// Output digits in reverse order
	do {
		print_byte (buf[--i]);
	} while (i>0);

}

/**
 * @brief	DMA Interrupt Handler
 * @return	None
 */
void DMA_IRQHandler(void)
{
	// Pulse debug pin so can see when each DMA block ends on scope trace.
	debug_pin_pulse (8);

	// Clear DMA interrupt for the channel
	Chip_DMA_ClearActiveIntAChannel(LPC_DMA, DMA_CH0);

	// Increment the DMA counter. When 3 the main loop knows we're done.
	dmaBlockCount++;
}


int main(void) {

	int i;

	//
	// Initialize GPIO
	//
	Chip_GPIO_Init(LPC_GPIO_PORT);
	Chip_GPIO_SetPinDIROutput(LPC_GPIO_PORT, 0, PIN_DEBUG);
	Chip_GPIO_SetPinState(LPC_GPIO_PORT, 0, 15, false);

	//
	// Initialize UART
	//

	// Assign pins: use same assignment as serial bootloader
	Chip_Clock_EnablePeriphClock(SYSCTL_CLOCK_SWM);
	Chip_SWM_MovablePinAssign(SWM_U0_TXD_O, PIN_UART_TXD);
	Chip_SWM_MovablePinAssign(SWM_U0_RXD_I, PIN_UART_RXD);
	Chip_Clock_DisablePeriphClock(SYSCTL_CLOCK_SWM);

	Chip_UART_Init(LPC_USART0);
	Chip_UART_ConfigData(LPC_USART0,
			UART_CFG_DATALEN_8
			| UART_CFG_PARITY_NONE
			| UART_CFG_STOPLEN_1);

	Chip_Clock_SetUSARTNBaseClockRate((UART_BAUD_RATE * 16), true);
	Chip_UART_SetBaud(LPC_USART0, UART_BAUD_RATE);
	Chip_UART_TXEnable(LPC_USART0);
	Chip_UART_Enable(LPC_USART0);



	//
	// Setup ADC
	//

	Chip_ADC_Init(LPC_ADC, 0);

	// Need to do a calibration after initialization
	Chip_ADC_StartCalibration(LPC_ADC);
	while (!(Chip_ADC_IsCalibrationDone(LPC_ADC))) {}

	// Sampling clock rate (not conversion rate). A fully accurate conversion
	// requires 25 ADC clock cycles.
	//Chip_ADC_SetClockRate(LPC_ADC, 500000 * 25);
	Chip_ADC_SetClockRate(LPC_ADC, ADC_MAX_SAMPLE_RATE);
	Chip_ADC_SetDivider(LPC_ADC,0);

	// Setup a sequencer A to do the following: (ref UM10800, 21.6.2, Table (undefined).)
	// * Do conversions on channel ADC_CHANNEL only
	// * Trigger sampling on SCT0_OUT3  See UM10800 §21.3.3, Table 276.
	// Note, as far as I can tell, ADC_SEQ_CTRL_HWTRIG_* defines are incorrect for LPC824.
	// * Mode END_OF_SEQ : trigger DMA/interrupt when sequence is complete.
	Chip_ADC_SetupSequencer(LPC_ADC,
							ADC_SEQA_IDX,
							(ADC_SEQ_CTRL_CHANSEL(ADC_CHANNEL)
							//| ADC_SEQ_CTRL_HWTRIG_SCT_OUT1
							| (3<<12) // trig on SCT0_OUT3.
							| ADC_SEQ_CTRL_MODE_EOS
							)
									);

	// Enable fixed pin ADC3, ADC9 with SwitchMatrix. Cannot move ADC pins.
	Chip_Clock_EnablePeriphClock(SYSCTL_CLOCK_SWM);
	Chip_SWM_EnableFixedPin(SWM_FIXED_ADC3);
	Chip_Clock_DisablePeriphClock(SYSCTL_CLOCK_SWM);

	/* Clear all pending interrupts */
	Chip_ADC_ClearFlags(LPC_ADC, Chip_ADC_GetFlags(LPC_ADC));


	// This has impact on DMA operation. Why?
	Chip_ADC_EnableInt(LPC_ADC, (ADC_INTEN_SEQA_ENABLE
								//| ADC_INTEN_OVRRUN_ENABLE
								));


	/* Enable sequencer */
	Chip_ADC_EnableSequencer(LPC_ADC, ADC_SEQA_IDX);




	// Setup DMA for ADC

	/* DMA initialization - enable DMA clocking and reset DMA if needed */
	Chip_DMA_Init(LPC_DMA);
	/* Enable DMA controller and use driver provided DMA table for current descriptors */
	Chip_DMA_Enable(LPC_DMA);
	Chip_DMA_SetSRAMBase(LPC_DMA, DMA_ADDR(Chip_DMA_Table));

	/* Setup channel 0 for the following configuration:
	   - High channel priority
	   - Interrupt A fires on descriptor completion */
	Chip_DMA_EnableChannel(LPC_DMA, DMA_CH0);
	Chip_DMA_EnableIntChannel(LPC_DMA, DMA_CH0);
	Chip_DMA_SetupChannelConfig(LPC_DMA, DMA_CH0,
			(DMA_CFG_HWTRIGEN
					//| DMA_CFG_PERIPHREQEN  //?? what's this for???
					| DMA_CFG_TRIGTYPE_EDGE
					| DMA_CFG_TRIGPOL_HIGH
					| DMA_CFG_TRIGBURST_BURST
					| DMA_CFG_BURSTPOWER_1
					 | DMA_CFG_CHPRIORITY(0)
					 ));

	// Attempt to use ADC SEQA to trigger DMA xfer
	Chip_DMATRIGMUX_SetInputTrig(LPC_DMATRIGMUX, DMA_CH0, DMATRIG_ADC_SEQA_IRQ);

	// DMA is performed in 3 separate chunks (as max allowed in one transfer
	// is 1024 words). First to go is dmaDescA followed by B and C.

	// DMA descriptor for ADC to memory - note that addresses must
	// be the END address for source and destination, not the starting address.
	// DMA operations moves from end to start. [Ref ].
	dmaDescC.xfercfg = 			 (
			DMA_XFERCFG_CFGVALID  // Channel descriptor is considered valid
			| DMA_XFERCFG_SETINTA // DMA Interrupt A (A vs B can be read in ISR)
			//| DMA_XFERCFG_SWTRIG  // When written by software, the trigger for this channel is set immediately.
			| DMA_XFERCFG_WIDTH_16 // 8,16,32 bits allowed
			| DMA_XFERCFG_SRCINC_0 // do not increment source
			| DMA_XFERCFG_DSTINC_1 // increment dst by widthx1
			| DMA_XFERCFG_XFERCOUNT(DMA_BUFFER_SIZE)
			);
	dmaDescC.source = DMA_ADDR ( (&LPC_ADC->DR[ADC_CHANNEL]) );
	dmaDescC.dest = DMA_ADDR(&adc_buffer[DMA_BUFFER_SIZE*3 - 1]) ;
	dmaDescC.next = DMA_ADDR(0); // no more descriptors

	dmaDescB.xfercfg = 			 (
			DMA_XFERCFG_CFGVALID  // Channel descriptor is considered valid
			| DMA_XFERCFG_RELOAD  // Causes DMA to move to next descriptor when complete
			| DMA_XFERCFG_SETINTA // DMA Interrupt A (A vs B can be read in ISR)
			//| DMA_XFERCFG_SWTRIG  // When written by software, the trigger for this channel is set immediately.
			| DMA_XFERCFG_WIDTH_16 // 8,16,32 bits allowed
			| DMA_XFERCFG_SRCINC_0 // do not increment source
			| DMA_XFERCFG_DSTINC_1 // increment dst by widthx1
			| DMA_XFERCFG_XFERCOUNT(DMA_BUFFER_SIZE)
			);
	dmaDescB.source = DMA_ADDR ( (&LPC_ADC->DR[ADC_CHANNEL]) );
	dmaDescB.dest = DMA_ADDR(&adc_buffer[DMA_BUFFER_SIZE*2 - 1]) ;
	dmaDescB.next = (uint32_t)&dmaDescC;

	// ADC data register is source of DMA
	dmaDescA.source = DMA_ADDR ( (&LPC_ADC->DR[ADC_CHANNEL]) );
	dmaDescA.dest = DMA_ADDR(&adc_buffer[DMA_BUFFER_SIZE - 1]) ;
	//dmaDesc.next = DMA_ADDR(0);
	dmaDescA.next = (uint32_t)&dmaDescB;



	// Enable DMA interrupt. Will be invoked at end of DMA transfer.
	NVIC_EnableIRQ(DMA_IRQn);

	/* Setup transfer descriptor and validate it */
	Chip_DMA_SetupTranChannel(LPC_DMA, DMA_CH0, &dmaDescA);
	Chip_DMA_SetValidChannel(LPC_DMA, DMA_CH0);

	// Setup data transfer and hardware trigger
	// See "Transfer Configuration registers" UM10800, §12.6.18, Table 173, page 179
	Chip_DMA_SetupChannelTransfer(LPC_DMA, DMA_CH0,
			 (
				DMA_XFERCFG_CFGVALID  // Channel descriptor is considered valid
				| DMA_XFERCFG_RELOAD  // Causes DMA to move to next descriptor when complete
				| DMA_XFERCFG_SETINTA // DMA Interrupt A (A vs B can be read in ISR)
				//| DMA_XFERCFG_SWTRIG  // When written by software, the trigger for this channel is set immediately.
				| DMA_XFERCFG_WIDTH_16 // 8,16,32 bits allowed
				| DMA_XFERCFG_SRCINC_0 // do not increment source
				| DMA_XFERCFG_DSTINC_1 // increment dst by widthx1
				| DMA_XFERCFG_XFERCOUNT(DMA_BUFFER_SIZE)
				)
				);


	//
	// Setup SCT to trigger ADC sampling
	//


	Chip_SCT_Init(LPC_SCT);

	/* Stop the SCT before configuration */
	Chip_SCTPWM_Stop(LPC_SCT);

	// Match/capture mode register. (ref UM10800 section 16.6.11, Table 232, page 273)
	// Determines if match/capture operate as match or capture. Want all match.
	LPC_SCT->REGMODE_U = 0;


	// Event 0 control: (ref UM10800 section 16.6.25, Table 247, page 282).
	// set MATCHSEL (bits 3:0) = MATCH0 register(0)
	// set COMBMODE (bits 13:12)= MATCH only(1)
	// So Event0 is triggered on match of MATCH0
	LPC_SCT->EV[0].CTRL =   (0 << 0 )
							| 1 << 12;
	// Event enable register (ref UM10800 section 16.6.24, Table 246, page 281)
	// Enable Event0 in State0 (default state). We are not using states,
	// so this enables Event0 in the default State0.
	// Set STATEMSK0=1
	LPC_SCT->EV[0].STATE = 1<<0;


	// Configure Event2 to be triggered on Match2
	LPC_SCT->EV[2].CTRL =
						(2 << 0) // The match register (MATCH2) associated with this event
						| (1 << 12); // COMBMODE=1 (MATCH only)
	LPC_SCT->EV[2].STATE = 1; // Enable Event2 in State0 (default state)


	/* Clear the output in-case of conflict */
	int pin = 0;
	LPC_SCT->RES = (LPC_SCT->RES & ~(3 << (pin << 1))) | (0x01 << (pin << 1));

	/* Set and Clear do not depend on direction */
	LPC_SCT->OUTPUTDIRCTRL = (LPC_SCT->OUTPUTDIRCTRL & ~((3 << (pin << 1))|SCT_OUTPUTDIRCTRL_RESERVED));


	// Set SCT Counter to count 32-bits and reset to 0 after reaching MATCH0
	Chip_SCT_Config(LPC_SCT, SCT_CONFIG_32BIT_COUNTER | SCT_CONFIG_AUTOLIMIT_L);





	// Setup SCT for ADC/DMA sample timing.
	uint32_t clock_hz =  Chip_Clock_GetSystemClockRate();
	Chip_SCT_SetMatchReload(LPC_SCT, SCT_MATCH_2, (clock_hz/ADC_SAMPLE_RATE)/2 );
	Chip_SCT_SetMatchReload(LPC_SCT, SCT_MATCH_0,  clock_hz/ADC_SAMPLE_RATE);

	// Using SCT0_OUT3 to trigger ADC sampling
	// Set SCT0_OUT3 on Event0 (Event0 configured to occur on Match0)
	LPC_SCT->OUT[3].SET = 1 << 0;
	// Clear SCT0_OUT3 on Event2 (Event2 configured to occur on Match2)
	LPC_SCT->OUT[3].CLR = 1 << 2;

	// SwitchMatrix: Assign SCT_OUT3 to external pin for debugging
	Chip_Clock_EnablePeriphClock(SYSCTL_CLOCK_SWM);
	Chip_SWM_MovablePinAssign(SWM_SCT_OUT3_O, PIN_SCT_DEBUG);
	Chip_Clock_DisablePeriphClock(SYSCTL_CLOCK_SWM);

	// Start SCT
	Chip_SCT_ClearControl(LPC_SCT, SCT_CTRL_HALT_L | SCT_CTRL_HALT_H);



	// Loop until dmaBlockCount==3 (this is updated in the DMA interrupt service routine)
	dmaBlockCount = 0;
	while (dmaBlockCount < 3) {
		// Save power by sleeping as much as possible while waiting for DMAs to complete.
		__WFI();
	}

	// Done with ADC sampling, stop and switch off SCT, ADC
	Chip_SCT_DeInit(LPC_SCT);
	Chip_ADC_DeInit(LPC_ADC);
	NVIC_DisableIRQ(DMA_IRQn);

	// DMA complete. Now shift ADC data register values 4 bits right to yield
	// 12 bit ADC data in range 0 - 4095
	for (i = 0; i < DMA_BUFFER_SIZE * 3; i++) {
		adc_buffer[i] >>= 4;
	}

	// Output ADC values to UART.
	// Format: record-number adc-value.
	// One record per line.  Suggest using GnuPlot to plot them.
	for (i = 0; i < DMA_BUFFER_SIZE * 3; i++) {

		// It would be nice to use libc, but complicates packing up for others to use.
		//printf ("%d %d\n", i, adc_buffer[i]);

		// Use simple UART printing functions embedded in C file instead of libc.
		print_decimal(i);
		print_byte(' ');
		print_decimal(adc_buffer[i]);
		//print_byte('\r');
		print_byte('\n');

		int j;
		for (j = 0; j < 1000; j++) {
			__NOP();
		}

	}

	// Done. Sleep forever.
	while (1) {
		__WFI();
	}

}
