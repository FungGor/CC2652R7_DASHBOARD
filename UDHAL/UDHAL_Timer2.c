/******************************************************************************

 @file  UDHAL_TIM2.c

 @brief This library is used for STM32MCP/STM32MCP.h to counter the heartbeat duration


 *****************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ti/sysbios/knl/Clock.h>
#include <UDHAL/UDHAL_Timer2.h>
#include <xdc/runtime/Error.h>

/*********************************************************************
 * LOCAL VARIABLES
 */
static Clock_Handle ClockHandle;
static Clock_Params clkParams;
static uint32_t clockTicks;
static Error_Block eb;
/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void UDHAL_TIMER2_start();
static void UDHAL_TIMER2_timerResetCounter();
static void UDHAL_TIMER2_stop();
static void UDHAL_TIMER2_OVClockFxn();
/*********************************************************************
 * Marco
 */
static STM32MCP_timerManager_t timerManager =
{
    UDHAL_TIMER2_start,
    UDHAL_TIMER2_timerResetCounter,
    UDHAL_TIMER2_stop
};
/*********************************************************************
 *
 * @fn      UDHAL_TIM2_init
 *
 * @brief   To initialize the timer and uart tx function
 *
 * @param   None.
 *
 * @return  None.
 */
void UDHAL_TIMER2_init()
{
    Error_init(&eb);
    clockTicks = STM32MCP_TIMEOUT_PERIOD * (1000 / Clock_tickPeriod) - 1; // -1 to ensure overflow occurs at STM32MCP_HEARTBEAT_PERIOD - not at 1 tick after STM32MCP_HEARTBEAT_PERIOD
    ClockHandle = Clock_create (UDHAL_TIMER2_OVClockFxn, clockTicks, &clkParams, &eb);
    STM32MCP_registerTimer(&timerManager);
    Clock_setTimeout(ClockHandle, clockTicks);
//    STM32MCP_registerHeartbeat(&timer);  // Warning: send pointer to mpb_timer to button.c // UDHAL_TIMER2_init() is called by STM32MCP.c,
                                         // but here, UDHAL_TIMER2_init() also calls a function in STM32MCP.c.
                                         // Such coding practice (recursive/circular calling) must not be allowed.
}
/*********************************************************************
 *
 * @fn      UDHAL_TIM2_params_init
 *
 * @brief   To initialize the timer and uart tx function
 *
 * @param   None.
 *
 * @return  None.
 */
void UDHAL_TIMER2_params_init()
{
    Clock_Params_init(&clkParams);
    clkParams.period = clockTicks;
    clkParams.startFlag = FALSE;
    clkParams.arg = (UArg)0x0000;
    Clock_setTimeout(ClockHandle, clockTicks);
    Clock_setPeriod(ClockHandle, clockTicks);
}


/*********************************************************************
 * @fn      UDHAL_TIMER2_timerRegister
 *
 * @brief   sends the pointer to timer to the calling function.
 *          It is used to register the pointer to the clock functions
 *          of UDHAL_TIMER2.
 *
 * @param   Nil
 *
 * @return  A set of function pointer contain the timer function
 */
extern STM32MCP_timerManager_t* UDHAL_TIMER2_timerRegister()
{
    return (&timerManager);
}


/*********************************************************************
 * @fn      UDHAL_TIM2_start
 *
 * @brief   To start the timer for timeout.
 *          This function will be used by STM32MCP flow retransmission.
 *          Put it into the STM32MCP_params_t structure to register as callback function
 *
 * @param   None.
 *
 * @return  None.
 */
static void UDHAL_TIMER2_start()
{
   // Set the initial timeout
    Clock_start(ClockHandle);
}

static void UDHAL_TIMER2_timerResetCounter()
{
    Clock_setPeriod(ClockHandle, clockTicks);
    Clock_setTimeout(ClockHandle, clockTicks);
}
/*********************************************************************
 * @fn      UDHAL_TIM2_stop
 *
 * @brief   To stop the timer for flow control timeout.
 *          This function will be used by STM32MCP flow retransmission.
 *          Put it into the STM32MCP_params_t structure to register as callback function.
 *
 * @param   None.
 *
 * @return  None.
 */
static void UDHAL_TIMER2_stop()
{
    Clock_stop(ClockHandle);
}
/*********************************************************************
 * @fn      UDHAL_TIM2_OVClockFxn
 *
 * @brief   After timeout, retransmission will be sent
 *          You must add STM32MCP_retransmission to this function
 *
 * @param   none
 *
 * @return  none
 */

/****
 * flag & 2 --> heartbeat
 */
static void UDHAL_TIMER2_OVClockFxn()
{
    STM32MCP_retransmission();
}
