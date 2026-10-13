#include "xparameters.h"
#include "xtmrctr.h"
#include "xgpio.h"
#include "xscugic.h"
#include "xil_exception.h"

// ============================================================================
// LAB MODE SELECTOR: Set to 1 for Part A (Auto-Pulse), Set to 2 for Part B (Buttons)
// ============================================================================
#define LAB_PART             1

// Hardware Configuration IDs automatically grabbed from xparameters.h
#define TIMER_DEVICE_ID      XPAR_TMRCTR_0_DEVICE_ID
#define GPIO_DEVICE_ID       XPAR_GPIO_0_DEVICE_ID
#define INTC_DEVICE_ID       XPAR_SCUGIC_SINGLE_DEVICE_ID
#define INTC_IRQ_ID          XPAR_FABRIC_AXI_TIMER_0_INTERRUPT_INTR

// 1ms total PWM period @ 100MHz (10ns clock period) in Countdown mode:
// PWM_PERIOD = (TLR0 + 2) * 10ns -> 1,000,000ns = (TLR0 + 2) * 10ns -> TLR0 = 99,998
// 99,998 in hexadecimal form is exactly 0x0001869E
#define TIMER_PERIOD_HEX     0x0001869E

// Custom Lab Option Definitions (Injected directly into hardware registers)
#define XTC_PWM_OPTION       0x00000200UL // Bit 9: Enables PWM Mode
#define XTC_GENT_OPTION      0x00000004UL // Bit 2: Enables GenerateOut Signal
#define XTC_UDT_OPTION       0x00000002UL // Bit 1: Enables Countdown Mode

#define FADE_STEP_SIZE       200    // Used in Part A (Every 1ms interrupt)
#define BUTTON_STEP_SIZE     5000   // Used in Part B (Coarser steps for push buttons)

// Driver Instances
XTmrCtr TimerInstance;
XGpio ButtonGpio;
XScuGic InterruptController;

// Global State Tracking
static int current_duty_cycle = 0;

// Function Prototypes
void TMR_Intr_Handler(void *CallBackRef, u8 TmrCtrNumber);
int SetupInterruptSystem(XScuGic *IntcInstancePtr, XTmrCtr *InstancePtr, u16 IntrId);

int main() {
    int status;

    // 1. Initialize Driver Components
    status = XTmrCtr_Initialize(&TimerInstance, TIMER_DEVICE_ID);
    if (status != XST_SUCCESS) return XST_FAILURE;

    status = XGpio_Initialize(&ButtonGpio, GPIO_DEVICE_ID);
    if (status != XST_SUCCESS) return XST_FAILURE;
    XGpio_SetDataDirection(&ButtonGpio, 1, 0xFF); // Buttons configured as input lines

    // 2. Link the Interrupt Handler
    XTmrCtr_SetHandler(&TimerInstance, (XTmrCtr_Handler)TMR_Intr_Handler, &TimerInstance);

    // 3. Set the standard basic options (Auto-Reload and Interrupts enabled)
    XTmrCtr_SetOptions(&TimerInstance, 0, XTC_AUTO_RELOAD_OPTION | XTC_INT_MODE_OPTION);
    XTmrCtr_SetOptions(&TimerInstance, 1, XTC_AUTO_RELOAD_OPTION | XTC_INT_MODE_OPTION);

    // 4. DIRECT REGISTER INJECTION: Read, modify, and write control registers (TCSR0 & TCSR1)
    // This perfectly fulfills your lab sequence guidelines for Generate Mode + PWM + Active High Out
    u32 tcsr0 = XTmrCtr_GetControlStatusReg(TimerInstance.BaseAddress, 0);
    u32 tcsr1 = XTmrCtr_GetControlStatusReg(TimerInstance.BaseAddress, 1);

    // Merge Countdown (UDT), GenerateOut (GENT), and your custom PWM Option bits
    XTmrCtr_SetControlStatusReg(TimerInstance.BaseAddress, 0, tcsr0 | XTC_PWM_OPTION | XTC_GENT_OPTION | XTC_UDT_OPTION);
    XTmrCtr_SetControlStatusReg(TimerInstance.BaseAddress, 1, tcsr1 | XTC_PWM_OPTION | XTC_GENT_OPTION | XTC_UDT_OPTION);

    // 5. Load the values into the target registers (Timer 0 = Period, Timer 1 = Duty Cycle Width)
    XTmrCtr_SetLoadReg(&TimerInstance, 0, TIMER_PERIOD_HEX);
    XTmrCtr_SetLoadReg(&TimerInstance, 1, current_duty_cycle);

    // 6. Connect the interrupt controller infrastructure
    status = SetupInterruptSystem(&InterruptController, &TimerInstance, INTC_IRQ_ID);
    if (status != XST_SUCCESS) return XST_FAILURE;

    // 7. Ignite the hardware counters
    XTmrCtr_Start(&TimerInstance, 0);
    XTmrCtr_Start(&TimerInstance, 1);

    // Main polling execution frame
    while(1) {
        #if (LAB_PART == 2)
            // ================================================================
            // PART B: DIRECT MANUAL BUTTON POLLING
            // ================================================================
            u32 btn_state = XGpio_DiscreteRead(&ButtonGpio, 1);

            if (btn_state & 0x01) { // Button 0 pressed -> Decrease brightness
                current_duty_cycle -= BUTTON_STEP_SIZE;
                if (current_duty_cycle < 0) current_duty_cycle = 0;
                XTmrCtr_SetLoadReg(&TimerInstance, 1, current_duty_cycle);
                while(XGpio_DiscreteRead(&ButtonGpio, 1) & 0x01); // Simple button debounce lock
            }
            else if (btn_state & 0x02) { // Button 1 pressed -> Increase brightness
                current_duty_cycle += BUTTON_STEP_SIZE;
                if (current_duty_cycle > TIMER_PERIOD_HEX) current_duty_cycle = TIMER_PERIOD_HEX;
                XTmrCtr_SetLoadReg(&TimerInstance, 1, current_duty_cycle);
                while(XGpio_DiscreteRead(&ButtonGpio, 1) & 0x02); // Simple button debounce lock
            }
        #endif
    }
    return 0;
}

// ============================================================================
// INTERRUPT HANDLER (Triggers automatically every 1ms when Timer 0 counts to 0)
// ============================================================================
void TMR_Intr_Handler(void *CallBackRef, u8 TmrCtrNumber) {
    XTmrCtr *InstancePtr = (XTmrCtr *)CallBackRef;

    #if (LAB_PART == 1)
        // ====================================================================
        // PART A: AUTOMATIC 500ms SAWTOOTH GENERATION
        // ====================================================================
        // Complete cycle lasts 500 ms. Handler executes every 1 ms.
        // Step size math: 99,998 max cycles / 500 total steps = ~200 units per shift interval
        current_duty_cycle += FADE_STEP_SIZE;

        // Once full intensity is achieved at 500 ms, clip instantly back to zero (Sawtooth rule)
        if (current_duty_cycle >= TIMER_PERIOD_HEX) {
            current_duty_cycle = 0;
        }

        // Apply dynamic modulation bounds down to the pulse width comparison load register (TLR1)
        XTmrCtr_SetLoadReg(InstancePtr, 1, current_duty_cycle);
    #endif
}

// GIC Hardware Initialization Logic Pipeline
int SetupInterruptSystem(XScuGic *IntcInstancePtr, XTmrCtr *InstancePtr, u16 IntrId) {
    int status;
    XScuGic_Config *IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
    status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig, IntcConfig->CpuBaseAddress);
    if (status != XST_SUCCESS) return XST_FAILURE;

    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, IntcInstancePtr);
    Xil_ExceptionEnable();

    status = XScuGic_Connect(IntcInstancePtr, IntrId, (Xil_InterruptHandler)XTmrCtr_InterruptHandler, (void *)InstancePtr);
    if (status != XST_SUCCESS) return XST_FAILURE;

    XScuGic_Enable(IntcInstancePtr, IntrId);
    return XST_SUCCESS;
}
