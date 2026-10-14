/******************************************************************************
 * AXI Timer PWM Lab - Zynq-7000 (classic xparameters.h/BSP flow)
 * Based on Xilinx xtmrctr_pwm_example.c (MIT License, Xilinx/AMD).
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

/************************** Constant Definitions *****************************/
#define LAB_PART_B              0   /* 0: automatic ramp, 1: buttons */

#define TMRCTR_DEVICE_ID        XPAR_TMRCTR_0_DEVICE_ID
#define INTC_DEVICE_ID          XPAR_INTC_0_DEVICE_ID
#define GIC_DEVICE_ID           XPAR_SCUGIC_SINGLE_DEVICE_ID
#define GPIO_DEVICE_ID          XPAR_GPIO_0_DEVICE_ID

/* In the supplied block diagram, Timer is xlconcat In0 (AXI INTC bit 0). */
#define TMRCTR_INTERRUPT_ID     0U

/* GIC interrupt for axi_intc_0 connected to Zynq IRQ_F2P[0] */
#define GIC_INTC_INTERRUPT_ID XPS_FPGA0_INT_ID

#define TMRCTR_0                0U
#define TMRCTR_1                1U
#define PWM_PERIOD              1000000U  /* 1 ms, in nanoseconds (driver API) */
/* Handout PWM bit (PWMA0/PWMB0). XTmrCtr_PwmEnable() sets this internally. */
#define XTC_PWM_OPTION          0x00000200UL
#define CYCLE_PER_DUTYCYCLE     5U        /* 5 PWM periods per brightness step */
#define MAX_DUTYCYCLE           100U
#define DUTYCYCLE_DIVISOR       100U
#define WAIT_COUNT              30000000U

/* For reference only (100 MHz down-counter): (100000 cycles - 2) = 0x1869E.
 * XTmrCtr_PwmConfigure() calculates and writes TLR0/TLR1 for us. */
#define PWM_TLR0_1MS            0x0001869EUL

#define GPIO_CHANNEL            1U
#define BUTTON0_MASK            0x01U     /* btns_4bits[0]: decrease */
#define BUTTON1_MASK            0x02U     /* btns_4bits[1]: increase */
#define BUTTON_STEP             5U        /* percent per press */
#define BUTTON_POLL_TICKS       20U       /* 20 ms between reads */

/************************** Function Prototypes ******************************/
static int TmrCtrPwmExample(XTmrCtr *InstancePtr);
static int TmrCtrSetupIntrSystem(XIntc *IntcInstancePtr,
                                 XScuGic *GicInstancePtr,
                                 XTmrCtr *TmrCtrInstancePtr);
static void TimerCounterHandler(void *CallBackRef, u8 TmrCtrNumber);
static int WaitForCycles(u32 Cycles);
static void SetPwmDuty(XTmrCtr *InstancePtr, u32 DutyPercent);

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
    u8 DutyCycle;

    Status = XTmrCtr_Initialize(TmrCtrInstancePtr, TMRCTR_DEVICE_ID);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Status = XTmrCtr_SelfTest(TmrCtrInstancePtr, TMRCTR_0);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    Status = TmrCtrSetupIntrSystem(&InterruptController, &GicController,
                                   TmrCtrInstancePtr);
    if (Status != XST_SUCCESS) {
        return XST_FAILURE;
    }

    /* Required Xilinx callback function-pointer registration. */
    XTmrCtr_SetHandler(TmrCtrInstancePtr, TimerCounterHandler,
                       TmrCtrInstancePtr);

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

    DutyCycle = 50U;
    SetPwmDuty(TmrCtrInstancePtr, DutyCycle);
    /* Fail visibly rather than waiting forever if the timer IRQ is absent. */
    if (WaitForCycles(BUTTON_POLL_TICKS) != XST_SUCCESS) {
        xil_printf("ERROR: PWM timer interrupts not received in Part B\r\n");
        return XST_FAILURE;
    }
    xil_printf("Part B: BTN0 decreases, BTN1 increases brightness\r\n");

    {
        u32 LastButtons = 0U;
        u32 LastSampleTick = PeriodTimerHit;
        u32 Buttons;

        while (1) {
            /* Timer 0 interrupt supplies the 1 ms timebase for button sampling. */
            if ((u32)(PeriodTimerHit - LastSampleTick) < BUTTON_POLL_TICKS) {
                continue;
            }
            LastSampleTick = PeriodTimerHit;
            Buttons = XGpio_DiscreteRead(&GpioInstance, GPIO_CHANNEL);

            if ((Buttons & BUTTON0_MASK) && !(LastButtons & BUTTON0_MASK)) {
                DutyCycle = (DutyCycle <= BUTTON_STEP) ? 1U :
                            (u8)(DutyCycle - BUTTON_STEP);
                SetPwmDuty(TmrCtrInstancePtr, DutyCycle);
                xil_printf("Brightness = %d%%\r\n", DutyCycle);
            }
            if ((Buttons & BUTTON1_MASK) && !(LastButtons & BUTTON1_MASK)) {
                DutyCycle = (DutyCycle >= MAX_DUTYCYCLE - BUTTON_STEP) ?
                            MAX_DUTYCYCLE : (u8)(DutyCycle + BUTTON_STEP);
                SetPwmDuty(TmrCtrInstancePtr, DutyCycle);
                xil_printf("Brightness = %d%%\r\n", DutyCycle);
            }
            LastButtons = Buttons;
        }
    }
#else
    /* Part A: 100 brightness levels x 5 ms/level = 500 ms per ramp.
     * Driver requires HIGH_TIME >= 2 timer clocks; 1% looks nearly off. */
    DutyCycle = 1U;
    SetPwmDuty(TmrCtrInstancePtr, DutyCycle);
    xil_printf("Part A: repeating 500 ms PWM brightness ramp\r\n");

    while (1) {
        Status = WaitForCycles(CYCLE_PER_DUTYCYCLE);
        if (Status != XST_SUCCESS) {
            xil_printf("ERROR: PWM timer interrupts not received\r\n");
            return XST_FAILURE;
        }

        DutyCycle++;
        if (DutyCycle > MAX_DUTYCYCLE) {
            DutyCycle = 1U;
        }
        SetPwmDuty(TmrCtrInstancePtr, DutyCycle);
    }
#endif
}

/*****************************************************************************/
/* This follows the Xilinx example's disable -> configure -> enable pattern.  */
static void SetPwmDuty(XTmrCtr *InstancePtr, u32 DutyPercent)
{
    u32 HighTime = (PWM_PERIOD / DUTYCYCLE_DIVISOR) * DutyPercent;

    /* AXI Timer can drive PWM0 LOW if TLR1 == TLR0 (exact 100%).
     * At 100 MHz, 99.99% leaves a 100 ns LOW interval and appears fully ON.
     * Keep TLR1 strictly below TLR0, including at the 100% endpoint. */
    if (DutyPercent >= MAX_DUTYCYCLE) {
        HighTime = PWM_PERIOD - (PWM_PERIOD / 10000U);
    }

    XTmrCtr_PwmDisable(InstancePtr);
    (void)XTmrCtr_PwmConfigure(InstancePtr, PWM_PERIOD, HighTime);
    XTmrCtr_PwmEnable(InstancePtr);
}

/*****************************************************************************/
static int WaitForCycles(u32 Cycles)
{
    u32 LastTick = PeriodTimerHit;
    u32 WaitCount = WAIT_COUNT;

    while ((u32)(PeriodTimerHit - LastTick) < Cycles) {
        if (--WaitCount == 0U) {
            return XST_FAILURE;
        }
    }
    return XST_SUCCESS;
}

/*****************************************************************************/
/* Required callback: registered with XTmrCtr_SetHandler().                    */
static void TimerCounterHandler(void *CallBackRef, u8 TmrCtrNumber)
{
    (void)CallBackRef;
    if (TmrCtrNumber == TMRCTR_0) {
        PeriodTimerHit++;
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
