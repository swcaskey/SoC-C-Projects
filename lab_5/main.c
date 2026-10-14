/******************************************************************************
 * AXI Timer PWM Lab - Zynq-7000 (classic xparameters.h/BSP flow)
 * Derived from Xilinx xtmrctr_pwm_example.c.
 * Copyright (C) 2018 - 2020 Xilinx, Inc. All rights reserved.
 * Copyright (C) 2022 - 2026 Advanced Micro Devices, Inc. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 *
 * Block design in the lab handout:
 *   axi_timer_0/interrupt -> xlconcat_0/In0 -> axi_intc_0/intr[0]
 *   axi_gpio_0/ip2intc_irpt -> xlconcat_0/In1 -> axi_intc_0/intr[1]
 *   axi_intc_0/irq -> processing_system7_0/IRQ_F2P[0]
 *
 * Set LAB_PART_B to 0 to demonstrate a 500 ms repeating brightness ramp.
 * Set LAB_PART_B to 1 for BTN0 dim / BTN1 brighten (5% per press).
 ******************************************************************************/

/***************************** Include Files *********************************/
#include <stdio.h>
#include "xtmrctr.h"
#include "xgpio.h"
#include "xintc.h"
#include "xscugic.h"
#include "xil_printf.h"
#include "xil_exception.h"
#include "xparameters.h"
#include "xstatus.h"
#include "xtime_l.h"
#include "xil_io.h"

/************************** Constant Definitions *****************************/
#ifndef LAB_PART_B
#define LAB_PART_B              0   /* 0: automatic ramp, 1: buttons */
#endif

#define TMRCTR_DEVICE_ID        XPAR_TMRCTR_0_DEVICE_ID
#define INTC_DEVICE_ID          XPAR_INTC_0_DEVICE_ID
#define GIC_DEVICE_ID           XPAR_SCUGIC_SINGLE_DEVICE_ID
#define GPIO_DEVICE_ID          XPAR_GPIO_0_DEVICE_ID

/* In the supplied block diagram, Timer is xlconcat In0 (AXI INTC bit 0). */
#define TMRCTR_INTERRUPT_ID     XPAR_INTC_0_TMRCTR_0_VEC_ID

/* GIC interrupt for axi_intc_0 connected to Zynq IRQ_F2P[0] */
#define GIC_INTC_INTERRUPT_ID XPS_FPGA0_INT_ID

#define TMRCTR_0                0U
#define TMRCTR_1                1U
#define PWM_PERIOD              1000000U  /* 1 ms, in nanoseconds (driver API) */
/* Handout PWM bit (PWMA0/PWMB0). XTmrCtr_PwmEnable() sets this internally. */
#define XTC_PWM_OPTION          0x00000200UL
#define CYCLE_PER_DUTYCYCLE     5U        /* 5 PWM periods per brightness step */
#define MAX_DUTYCYCLE           100U
#define RAMP_LEVELS             100U
#define INTERRUPT_TIMEOUT_MS    100U
#define PWM_CLOCK_NS            10U
#define PWM_PERIOD_COUNTS       100000U

#if XPAR_TMRCTR_0_CLOCK_FREQ_HZ != 100000000U
#error This application expects the supplied 100 MHz AXI Timer hardware.
#endif

/* For reference only (100 MHz down-counter): (100000 cycles - 2) = 0x1869E.
 * XTmrCtr_PwmConfigure() calculates and writes TLR0/TLR1 for us. */
#define PWM_TLR0_1MS            0x0001869EUL

#define GPIO_CHANNEL            1U
#define BUTTON0_MASK            0x01U     /* btns_4bits[0]: decrease */
#define BUTTON1_MASK            0x02U     /* btns_4bits[1]: increase */
#define BUTTON_STEP             5U        /* percent per press */
#define BUTTON_POLL_TICKS       1U        /* sample once per PWM period */
#define BUTTON_DEBOUNCE_TICKS   20U       /* 20 identical samples */

/************************** Function Prototypes ******************************/
static int TmrCtrPwmExample(XTmrCtr *InstancePtr);
static int TmrCtrSetupIntrSystem(XIntc *IntcInstancePtr,
                                 XScuGic *GicInstancePtr,
                                 XTmrCtr *TmrCtrInstancePtr);
static void TimerCounterHandler(void *CallBackRef, u8 TmrCtrNumber);
static int WaitForCycles(u32 Cycles);
static int SetPwmDuty(XTmrCtr *InstancePtr, u32 Level, u32 MaxLevel);

/************************** Variable Definitions *****************************/
static XIntc InterruptController;
static XScuGic GicController;
static XTmrCtr TimerCounterInst;
#if LAB_PART_B
static XGpio GpioInstance;
#endif

/* Incremented by the registered timer callback. */
static volatile u32 PeriodTimerHit = 0U;
static volatile u32 HighTimerHit = 0U;

/*****************************************************************************/
int main(void)
{
    int Status = TmrCtrPwmExample(&TimerCounterInst);

    if (Status != XST_SUCCESS) {
        xil_printf("Tmrctr PWM Example Failed\r\n");
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

/*****************************************************************************/
static int TmrCtrPwmExample(XTmrCtr *TmrCtrInstancePtr)
{
    int Status;
#if LAB_PART_B
    u8 DutyCycle;
#endif

    /* Start the PS global timer for a real elapsed-time IRQ timeout.
     * Preserve its count and other control bits. It does not drive PWM. */
    Xil_Out32(GLOBAL_TMR_BASEADDR + GTIMER_CONTROL_OFFSET,
              Xil_In32(GLOBAL_TMR_BASEADDR + GTIMER_CONTROL_OFFSET) | 1U);

    Status = XTmrCtr_Initialize(TmrCtrInstancePtr, TMRCTR_DEVICE_ID);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Status = XTmrCtr_SelfTest(TmrCtrInstancePtr, TMRCTR_0);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /* Required Xilinx callback function-pointer registration. */
    XTmrCtr_SetHandler(TmrCtrInstancePtr, TimerCounterHandler,
                       TmrCtrInstancePtr);

    Status = TmrCtrSetupIntrSystem(&InterruptController, &GicController,
                                   TmrCtrInstancePtr);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /* Enable interrupts for both of the PWM timers (as in Xilinx example). */
    XTmrCtr_SetOptions(TmrCtrInstancePtr, TMRCTR_0, XTC_INT_MODE_OPTION);
    XTmrCtr_SetOptions(TmrCtrInstancePtr, TMRCTR_1, XTC_INT_MODE_OPTION);

#if LAB_PART_B
    /* Part B: single AXI GPIO channel with four button bits. */
    Status = XGpio_Initialize(&GpioInstance, GPIO_DEVICE_ID);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }
    XGpio_SetDataDirection(&GpioInstance, GPIO_CHANNEL, 0x0FU);
    /* Buttons are sampled with the timer timebase; GPIO IRQ stays disabled. */
    XGpio_InterruptGlobalDisable(&GpioInstance);
    XGpio_InterruptDisable(&GpioInstance, XGPIO_IR_CH1_MASK);

    DutyCycle = 50U;
    Status = SetPwmDuty(TmrCtrInstancePtr, DutyCycle, MAX_DUTYCYCLE);
    if (Status != XST_SUCCESS) {
        return Status;
    }
    /* Fail visibly rather than waiting forever if the timer IRQ is absent. */
    if (WaitForCycles(BUTTON_POLL_TICKS) != XST_SUCCESS) {
        xil_printf("ERROR: PWM timer interrupts not received in Part B\r\n");
        return XST_FAILURE;
    }
    xil_printf("Part B: BTN0 decreases, BTN1 increases brightness\r\n");

    {
        u32 LastButtons = 0U;
        u32 CandidateButtons = 0U;
        u32 StableSamples = 0U;
        u32 Buttons;
        u32 Pressed;

        while (1) {
            if (WaitForCycles(BUTTON_POLL_TICKS) != XST_SUCCESS) {
                xil_printf("ERROR: PWM timer interrupts stopped\r\n");
                return XST_FAILURE;
            }
            Buttons = XGpio_DiscreteRead(&GpioInstance, GPIO_CHANNEL) &
                       (BUTTON0_MASK | BUTTON1_MASK);
            if (Buttons != CandidateButtons) {
                CandidateButtons = Buttons;
                StableSamples = 1U;
            } else if (StableSamples < BUTTON_DEBOUNCE_TICKS) {
                StableSamples++;
            }
            if (StableSamples < BUTTON_DEBOUNCE_TICKS ||
                Buttons == LastButtons) {
                continue;
            }

            Pressed = Buttons & ~LastButtons;
            LastButtons = Buttons;
            /* One change per debounced press; simultaneous presses ignored. */
            if (Buttons == BUTTON0_MASK && Pressed == BUTTON0_MASK) {
                DutyCycle = (DutyCycle <= BUTTON_STEP) ? 0U :
                            (u8)(DutyCycle - BUTTON_STEP);
                (void)SetPwmDuty(TmrCtrInstancePtr, DutyCycle, MAX_DUTYCYCLE);
                xil_printf("Brightness = %d%%\r\n", DutyCycle);
            } else if (Buttons == BUTTON1_MASK && Pressed == BUTTON1_MASK) {
                DutyCycle = (DutyCycle >= MAX_DUTYCYCLE - BUTTON_STEP) ?
                            MAX_DUTYCYCLE : (u8)(DutyCycle + BUTTON_STEP);
                (void)SetPwmDuty(TmrCtrInstancePtr, DutyCycle, MAX_DUTYCYCLE);
                xil_printf("Brightness = %d%%\r\n", DutyCycle);
            }
        }
    }
#else
    /* Part A: 100 levels including off and near-full intensity.
     * 100 levels x 5 interrupts/level x 1 ms/interrupt = 500 ms.
     * The callback updates brightness without restarting either counter. */
    Status = SetPwmDuty(TmrCtrInstancePtr, 0U, RAMP_LEVELS - 1U);
    if (Status != XST_SUCCESS) {
        return Status;
    }
    xil_printf("Part A: repeating 500 ms PWM brightness ramp\r\n");

    while (1) {
        Status = WaitForCycles(CYCLE_PER_DUTYCYCLE);
        if (Status != XST_SUCCESS) {
            xil_printf("ERROR: PWM timer interrupts not received\r\n");
            return XST_FAILURE;
        }
    }
#endif
}

/*****************************************************************************/
/* Use Xilinx PWM APIs at startup. Subsequent changes update only TLR1,
 * which auto-reloads in hardware; the 1 ms carrier keeps running.
 * Equal TLRs produce LOW, so Level=0 deliberately uses equal loads.
 * Exact 100% is not representable: the top level is 99.999% at 100 MHz. */
static int SetPwmDuty(XTmrCtr *InstancePtr, u32 Level, u32 MaxLevel)
{
    u32 HighCounts;
    u32 HighLoad;

    if (MaxLevel == 0U || Level > MaxLevel) {
        return XST_FAILURE;
    }
    if (Level == 0U) {
        HighCounts = PWM_PERIOD_COUNTS;
    } else if (Level == MaxLevel) {
        HighCounts = PWM_PERIOD_COUNTS - 1U;
    } else {
        HighCounts = (PWM_PERIOD_COUNTS * Level) / MaxLevel;
        if (HighCounts < 2U) {
            HighCounts = 2U;
        }
    }
    HighLoad = HighCounts - 2U;

    if (!InstancePtr->IsPwmEnabled) {
        XTmrCtr_PwmDisable(InstancePtr);
        /* The return value is an achieved-duty byte, not a status code.
         * Arguments are validated above; check actual register values. */
        (void)XTmrCtr_PwmConfigure(InstancePtr, PWM_PERIOD,
                                   HighCounts * PWM_CLOCK_NS);
        if (XTmrCtr_GetLoadReg(InstancePtr->BaseAddress, TMRCTR_0) !=
            PWM_TLR0_1MS ||
            XTmrCtr_GetLoadReg(InstancePtr->BaseAddress, TMRCTR_1) !=
            HighLoad) {
            xil_printf("ERROR: PWM load register configuration failed\r\n");
            return XST_FAILURE;
        }
        XTmrCtr_PwmEnable(InstancePtr);
    } else {
        XTmrCtr_SetResetValue(InstancePtr, TMRCTR_1, HighLoad);
    }
    return XST_SUCCESS;
}

/*****************************************************************************/
static int WaitForCycles(u32 Cycles)
{
    u32 LastTick = PeriodTimerHit;
    XTime StartTime;
    XTime Now;

    XTime_GetTime(&StartTime);

    while ((u32)(PeriodTimerHit - LastTick) < Cycles) {
        XTime_GetTime(&Now);
        if ((Now - StartTime) >=
            ((XTime)COUNTS_PER_SECOND * INTERRUPT_TIMEOUT_MS) / 1000U) {
            return XST_FAILURE;
        }
    }
    return XST_SUCCESS;
}

/*****************************************************************************/
/* Required callback: registered with XTmrCtr_SetHandler().                    */
static void TimerCounterHandler(void *CallBackRef, u8 TmrCtrNumber)
{
#if LAB_PART_B
    (void)CallBackRef;
#else
    XTmrCtr *InstancePtr = (XTmrCtr *)CallBackRef;
    static u32 NoOfCycles = 0U;
    static u32 DutyCycle = 0U;
#endif
    if (TmrCtrNumber == TMRCTR_0) {
        PeriodTimerHit++;
#if !LAB_PART_B
        if (++NoOfCycles == CYCLE_PER_DUTYCYCLE) {
            NoOfCycles = 0U;
            if (++DutyCycle == RAMP_LEVELS) {
                DutyCycle = 0U;
            }
            /* A single load-register write; no stop/restart or UART in ISR. */
            (void)SetPwmDuty(InstancePtr, DutyCycle, RAMP_LEVELS - 1U);
        }
#endif
    }
    if (TmrCtrNumber == TMRCTR_1) {
        HighTimerHit++;
    }
}

/*****************************************************************************/
/* Additional step vs. Xilinx's simple interrupt setup: the supplied hardware
 * cascades an AXI Interrupt Controller into the Zynq PS GIC. Both interrupt
 * controllers must be initialized and their handlers connected.             */
static int TmrCtrSetupIntrSystem(XIntc *IntcInstancePtr,
                                 XScuGic *GicInstancePtr,
                                 XTmrCtr *TmrCtrInstancePtr)
{
    XScuGic_Config *GicConfig;
    int Status;

    Status = XIntc_Initialize(IntcInstancePtr, INTC_DEVICE_ID);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Status = XIntc_Connect(IntcInstancePtr, TMRCTR_INTERRUPT_ID,
                           (XInterruptHandler)XTmrCtr_InterruptHandler,
                           (void *)TmrCtrInstancePtr);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Status = XIntc_Start(IntcInstancePtr, XIN_REAL_MODE);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }
    XIntc_Enable(IntcInstancePtr, TMRCTR_INTERRUPT_ID);

    GicConfig = XScuGic_LookupConfig(GIC_DEVICE_ID);
    if (GicConfig == NULL) {
        return XST_FAILURE;
    }
    Status = XScuGic_CfgInitialize(GicInstancePtr, GicConfig,
                                    GicConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /* AXI INTC's irq is held active while an enabled interrupt is pending. */
    XScuGic_SetPriorityTriggerType(GicInstancePtr, GIC_INTC_INTERRUPT_ID,
                                    0xA0U, 0x01U);  /* level-sensitive */

    Status = XScuGic_Connect(GicInstancePtr, GIC_INTC_INTERRUPT_ID,
                             (Xil_ExceptionHandler)XIntc_InterruptHandler,
                             (void *)IntcInstancePtr);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }
    XScuGic_Enable(GicInstancePtr, GIC_INTC_INTERRUPT_ID);

    Xil_ExceptionInit();
    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT,
                                 (Xil_ExceptionHandler)XScuGic_InterruptHandler,
                                 GicInstancePtr);
    Xil_ExceptionEnable();
    return XST_SUCCESS;
}
