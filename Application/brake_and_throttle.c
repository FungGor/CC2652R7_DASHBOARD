/*
 * brake_and_throttle.c
 *
 *  Created on: 7 May 2024
 *      Author: Chee
 */
/*********************************************************************
 * INCLUDES
 */
#include <stdint.h>
#include <math.h>

#include "Hardware/gGo_device_params.h"
#include "Hardware/STM32MCP.h"

#include "Application/brake_and_throttle.h"

#include "Profiles/dashboard_profile.h"
#include "Profiles/controller_profile.h"
#include "Profiles/battery_profile.h"
#include "Profiles/profile_charVal.h"

#include "Application/led_display.h"
#include "Application/data_analytics.h"
#include "Application/snv_internal.h"
#include "Application/periodic_communication.h"

#include "UDHAL/UDHAL_ADC.h"

/*********************************************************************
 * CONSTANTS
 */
#undef  ESCOOTER_RUN
#define ESCOOTER_DEBUG 1
/*********************************************************************
 * GLOBAL VARIABLES
 *********************************************************************/
/* ControlLaw Options: NormalLaw (1) or DirectLaw (0).
 *          Normal law algorithm modulates the speed to not exceed the defined limit
 *          Direct law algorithm does not modulate the speed in any way
 */
uint8_t     ControlLaw = BRAKE_AND_THROTTLE_NORMALLAW; // BRAKE_AND_THROTTLE_NORMALLAW or BRAKE_AND_THROTTLE_DIRECTLAW; //

uint8_t     speedMode = BRAKE_AND_THROTTLE_SPEED_MODE_LEISURE;

uint16_t    adcValues[2];
uint16_t    adc2Result;             // adc2Result is a holder of the throttle ADC reading
uint16_t    throttlePercent;        // Actual throttle applied in percentage
uint16_t    throttlePercent0 = 0;
uint16_t    IQ_input;               // Iq value input by rider
uint16_t    IQ_applied = 0;             // Iq value command sent to STM32 / motor Controller
uint16_t    rpm_limit = REG_MAXP_RPM;
uint16_t    brakePercent;           // brake applied in percentage
uint16_t    brakeStatus = 0;

uint8_t     dashboardErrorCodeStatus = 0xFF;

/********************************************************************
 *  when brakeAndThrottle_errorMsg is true (=1),
 *  it generally means either (1) brake signal is not connected and/or (2) throttle signal is not connect
 *  IQ_input is set to 0 = zero throttle by default
 */
uint8_t brakeAndThrottle_errorMsg = BRAKE_AND_THROTTLE_NORMAL;
uint8_t throttle_errorStatus = 0;   // 0 indicates normal (no error)
uint8_t brake_errorStatus = 0;      // 0 indicates normal (no error)

/****** Safety Protections  **************************************/
// Safety feature 1:  Speed mode cannot be changed while throttle is pressed
// Safety feature 2:  Abnormal brake or throttle signals will automatically change speed mode to amble mode
//                    0E indicates brake error.  0C indicates throttle error
// Safety feature 3:  Power to motor is cut when brake is engaged
// Safety feature 4:  Speed and Power protections are activated under Normal Control Law
// Safety feature 5:  Power is only applied to motor at speed greater than 3 km/hr (or 80 rpm)
// Safety feature 6:  No Power is applied at negative speeds
/*********************************************************************
 *
 * LOCAL VARIABLES
 */
profileCharVal_t *ptr_bat_profileCharVal;
static uint8_t   *ptr_charVal;

//static uint8_t  state = 0;
static uint8_t  brakeAndThrottleIndex = 0;
static uint16_t brakeADCSamples[BRAKE_AND_THROTTLE_SAMPLES];
uint16_t        throttleADCSamples[BRAKE_AND_THROTTLE_SAMPLES];
uint16_t        RPM_array[BRAKE_AND_THROTTLE_SAMPLES];

/** Speed limiter variables **/
uint8_t         exponent  = 3;
float           speedModFactor = 1;
uint32_t        limit_exceedance_flag = 0;    // a flag for indicating rpm limit was exceeded

/**  Speed mode parameters  **/
static uint16_t speedModeIQmax;
static uint8_t  reductionRatio;
static uint16_t rampRate;
static uint16_t allowableSpeed;

MCUD_t          *ptr_bat_MCUDArray;


/*********************************************************************
* FUNCTIONS
*/
static void brake_and_throttle_normalLawControl();


/**** obtain/register the pointer to MCUDArray   ****/
extern void brake_and_throttle_MCUArrayRegister(MCUD_t *ptrMCUDArray)
{
    ptr_bat_MCUDArray = ptrMCUDArray;
}

/**** obtain/register the pointer to STM32MCPDArray   ****/
STM32MCPD_t *ptr_bat_STM32MCPDArray;
extern void brake_and_throttle_STM32MCDArrayRegister(STM32MCPD_t *ptrSTM32MCPDArray)
{
    ptr_bat_STM32MCPDArray = ptrSTM32MCPDArray;
}

/**********************************************************************
 *  Local functions
 */
static void brake_and_throttle_getSpeedModeParams();

/*********************************************************************
 * @fn      brake_and_throttle_init
 *
 * @brief   Start the brake ADC and timer
 *
 * @param   none
 *
 * @return  none
 *********************************************************************/
void brake_and_throttle_init()
{

    ptr_bat_profileCharVal = profile_charVal_profileCharValRegister();

    UDHAL_ADC_ptrADCValues(&adcValues);

    data_analytics_dashErrorCodeStatusRegister(&dashboardErrorCodeStatus);

    /* Get initial speed mode */
    uint8_t speedModeinit;
    speedModeinit = data_analytics_getSpeedmodeInit();
    speedMode = speedModeinit;
    /* Initiate and Get speed mode parameters */
    brake_and_throttle_getSpeedModeParams();

    ptr_bat_STM32MCPDArray->speed_mode = speedMode;
    ptr_bat_STM32MCPDArray->speed_mode_IQmax = speedModeIQmax;
    ptr_bat_STM32MCPDArray->ramp_rate = rampRate;
    ptr_bat_STM32MCPDArray->allowable_speed = allowableSpeed;

    for (uint8_t ii = 0; ii < BRAKE_AND_THROTTLE_SAMPLES; ii++)
    {
        brakeADCSamples[ii] = BRAKE_ADC_CALIBRATE_L;
        throttleADCSamples[ii] = THROTTLE_ADC_CALIBRATE_L;
        RPM_array[ii] = 0;
    }

    /* led_display_init() must be before brake_and_throttle_init() */
    led_display_setSpeedMode(speedMode);  // update speed mode on dashboard led display
    led_control_setControlLaw(ControlLaw);

}


/*********************************************************************
 * @fn      brake_and_throttle_ADC_conversion
 *
 * @brief   This function perform ADC conversion
 *          This function is called when timer6 overflows
 *
 * @param
 *********************************************************************/
uint8_t     brake_errorFlag = 0;
uint16_t    RPM_temp;
uint8_t     brakeAndThrottleIndex_minus_1;
uint8_t     brakeAndThrottleIndex_minus_2;
uint8_t     throttle_error_count = 0;
uint8_t     brake_error_count = 0;
uint16_t    throttleADCsample = 0;  //throttleADCsample
uint16_t    brakeADCsample = 0;     //brakeADCsample

uint16_t    bat_count = 0;
uint16_t    RPM_prev;
uint16_t    IQapp_prev = 0;
float       drpmdIQ;
uint16_t    IQ_maxPout = 0xFFFF;

void brake_and_throttle_ADC_conversion()
{
    /************************************************************************************
     *      get brake ADC measurement
     *      get throttle ADC measurement
     *      Stores ADC measurement in arrays brakeADCSamples & throttleADCSamples
     ************************************************************************************/
    UDHAL_ADC_Convert();
    throttleADCsample = adcValues[0];
    brakeADCsample = adcValues[1];

    /*******************************************************************************************************************************
     *      Error Checking
     *      Check whether throttle ADC reading is logical, if illogical, brakeAndThrottle_errorMsg = error (!=0)
     *      These Conditions occur when throttle or brake signals/power are not connected, or incorrect supply voltage
     *      Once this condition occurs (brakeAndThrottle_errorMsg != 0), check throttle connections, hall sensor fault,
     *      Reset (Power off and power on again) is require to reset brakeAndThrottle_errorMsg.
     *******************************************************************************************************************************/
    if ( (throttleADCsample >= THROTTLE_ADC_THRESHOLD_L) && (throttleADCsample <= THROTTLE_ADC_THRESHOLD_H) && (throttle_error_count != 0) )
    {
        throttle_error_count = 0;   // Reset throttle_error_count
    }

    if ( (throttleADCsample < THROTTLE_ADC_THRESHOLD_L) || (throttleADCsample > THROTTLE_ADC_THRESHOLD_H) )
    {
        throttle_error_count++;
        if ((throttle_error_count >= 5) && (!throttle_errorStatus) )
        {
            throttle_errorStatus = 1;
            brakeAndThrottle_errorMsg = THROTTLE_ERROR;

        /* if throttle errorStatus = 1 -> disable throttle input and zero Iq command to Motor Controller Unit */
            led_display_ErrorPriority(THROTTLE_ERROR_PRIORITY);   /* throttle error priority = 11 */

            /******  Dashboard services *************************************/
            /* updates error code Characteristic Value -> Mobile App */
            if (dashboardErrorCodeStatus > THROTTLE_ERROR_PRIORITY)
            {
                dashboardErrorCodeStatus = THROTTLE_ERROR_PRIORITY;
                ptr_charVal = (ptr_bat_profileCharVal->ptr_dash_charVal->ptr_dashErrorCode);
                profile_setCharVal(ptr_charVal, DASHBOARD_ERROR_CODE_LEN, THROTTLE_ERROR_CODE);
            }
        }
    }   // -> This set of codes determines if throttle error is present

    /*******************************************************************************************************************************
     *      Throttle Signal Calibration
     *      Truncates the average throttle ADC signals to within THROTTLE_ADC_CALIBRATE_L and THROTTLE_ADC_CALIBRATE_H
     *******************************************************************************************************************************/
    if ((throttleADCsample > THROTTLE_ADC_CALIBRATE_H))
    {
        throttleADCsample = THROTTLE_ADC_CALIBRATE_H;
    }

    if ((throttleADCsample < THROTTLE_ADC_CALIBRATE_L))
    {
        throttleADCsample = THROTTLE_ADC_CALIBRATE_L;
    }

    throttleADCSamples[ brakeAndThrottleIndex ] = throttleADCsample;

    /*******************************************************************************************************************************
     *      Error Checking
     *      Check whether brake ADC reading is logical, if illogical, brakeAndThrottle_errorMsg = error (!=0)
     *      These Conditions occur when brake signals/power are not connected, or incorrect supply voltage
     *      Once this condition occurs (brakeAndThrottle_errorMsg != 0), check brake connections, hall sensor fault,
     *      Reset (Power off and power on again) is require to reset brakeAndThrottle_errorMsg.
     *******************************************************************************************************************************/
    if ( (brakeADCsample >= BRAKE_ADC_THRESHOLD_L) && (brakeADCsample <= BRAKE_ADC_THRESHOLD_H) && (brake_error_count != 0) )
    {
        brake_error_count = 0;   // Reset brake_error_count
    }

    if ((brakeADCsample < BRAKE_ADC_THRESHOLD_L) || (brake_errorFlag != 0) || (brakeADCsample > BRAKE_ADC_THRESHOLD_H))
    {
        /* Adjust Sensitivity */
        brake_error_count++;
        if ((brake_error_count >= 5) && (brake_errorStatus == 0))
        {
            brake_errorFlag = 1; /* set and flag that brake_errorFlag = 1 */
        }

      /* For safety reason, speed mode will not immediately change, but flagged to do so only when throttle value is less than THROTTLE_ADC_CALIBRATE_L */
      /* if brake_errorStatus was originally == 0 and throttle is not pressed, then update (this routine prevents repetitive executions) */
        if ((brake_errorStatus == 0) && (throttleADCsample <= THROTTLE_ADC_CALIBRATE_L))
        {
            brakeAndThrottle_errorMsg = BRAKE_ERROR;
            brake_errorStatus = 1;
           /* When brake error occurs, i.e. brake errorStatus = 1 -> limited to Amble mode Only */
            speedMode = BRAKE_AND_THROTTLE_SPEED_MODE_AMBLE;

           /* Note: speed mode is changed! Update Led Display, update App data */
            brake_and_throttle_toggleSpeedMode();   // execute speed mode change
            led_display_setSpeedMode(speedMode);   // set speed mode on led display

            /* Send error code to the Motor Controller */
            led_display_ErrorPriority(BRAKE_ERROR_PRIORITY);   // send brake error priority = 10
            brake_errorFlag = 0; /* reset brake_errorFlag to 0 */

            /******  Dashboard services *************************************/
            /* updates error code Characteristic Value -> Mobile App */
            if (dashboardErrorCodeStatus > BRAKE_ERROR_PRIORITY)
            {
                dashboardErrorCodeStatus = BRAKE_ERROR_PRIORITY;
                ptr_charVal = (ptr_bat_profileCharVal->ptr_dash_charVal->ptr_dashErrorCode);
                profile_setCharVal(ptr_charVal, DASHBOARD_ERROR_CODE_LEN, BRAKE_ERROR_CODE);
            }
        }
    }   // this set of codes checks for brake error

    /*******************************************************************************************************************************
     *      Brake Signal Calibration
     *      Truncates the average brake ADC signals to within BRAKE_ADC_CALIBRATE_L and BRAKE_ADC_CALIBRATE_H
     *******************************************************************************************************************************/
    if((brakeADCsample > BRAKE_ADC_CALIBRATE_H))
    {
        brakeADCsample = BRAKE_ADC_CALIBRATE_H;
    }

    if((brakeADCsample < BRAKE_ADC_CALIBRATE_L))
    {
        brakeADCsample = BRAKE_ADC_CALIBRATE_L;
    }

    brakeADCSamples[ brakeAndThrottleIndex ] = brakeADCsample;

    /*******************************************************************************************************************************
     *      the sampling interval is defined by "GPT_TIME"
     *      the number of samples is defined by "BRAKE_AND_THROTTLE_SAMPLES"
     *      Sum the most recent "BRAKE_AND_THROTTLE_SAMPLES" number of data points, and
     *      calculate weighted moving average brake and throttle ADC values
     *      Weight factor of 2 is applied to the newest throttle sample, all other samples have weight factor of 1
     *******************************************************************************************************************************/
    uint16_t    brakeADCsum = 0;
    uint16_t    throttleADCsum = 0;
    uint8_t     jj;

    for (jj = 0; jj < BRAKE_AND_THROTTLE_SAMPLES; jj++)
    {
        brakeADCsum += brakeADCSamples[jj];
        throttleADCsum += throttleADCSamples[jj];
    }

    /***** Calculate moving average values ******/
    uint16_t    brakeADCAvg = brakeADCsum / BRAKE_AND_THROTTLE_SAMPLES;             // declared as global variable for debugging only
    uint16_t    throttleADCAvg = throttleADCsum / BRAKE_AND_THROTTLE_SAMPLES;

    /********************************************************************************************************************************
     *  brakePercent is in percentage - has value between 0 - 100 %
     ********************************************************************************************************************************/
    brakePercent = (brakeADCAvg - BRAKE_ADC_CALIBRATE_L) * 100 / (BRAKE_ADC_CALIBRATE_H - BRAKE_ADC_CALIBRATE_L);
    /********************************************************************************************************************************
     *  throttlePercent is in percentage - has value between 0 - 100 %
     ********************************************************************************************************************************/
    throttlePercent = (throttleADCAvg - THROTTLE_ADC_CALIBRATE_L) * 100 / (THROTTLE_ADC_CALIBRATE_H - THROTTLE_ADC_CALIBRATE_L);

    /*****  update brakeAndThrottle_errorMsg on STM32MCPDArray.error_msg  *****/
    ptr_bat_STM32MCPDArray->error_msg = brakeAndThrottle_errorMsg;

    /********************** Brake Power Off Protect State Machine  *******************************************************************************
     *              if brake is engaged, defined as brakePercent being greater than say 50%,
     *              dashboard will instruct motor controller to cut power to motor for safety precautions.
     *              Once power to motor is cut, both the brake & throttle must be fully released before power delivery can be resumed
    **********************************************************************************************************************************************/
    if (brake_errorStatus == 0)
    {/* if there is no brake error .... */
     /* condition where brake is engaged (1) and throttle is greater than 30% */
        if ((brakeStatus == 1) && (brakePercent <= BRAKEPERCENTTHRESHOLD) && (throttlePercent <= THROTTLEPERCENTTHRESHOLD))
        { // This condition resets brakeStatus to zero
            brakeStatus = 0;
        }
        else if ((brakeStatus == 0) && (brakePercent > BRAKEPERCENTTHRESHOLD))
        {// condition when brake is not initially pressed and rider pulls on the brake
            brakeStatus = 1;    // if brakeStatus == 1, cut power to motor
        }
    }
    else
    {  // when brake_errorStatus == 1
        brakeStatus = 0;    // if we set brakeStatus = 1 when brake error exists, the motor will be disabled completely.
                            // if we set brakeStatus = 0 when brake error exists, we could still operate the motor with throttle
    }

    /**************************************************************************************
     *       if Brake is engaged -> cut power to motor and activate brake light
     **************************************************************************************/
    ptr_bat_STM32MCPDArray->brake_percent = brakePercent;
    ptr_bat_STM32MCPDArray->brake_status = brakeStatus;

    /******** Get RPM from mcu  ******************/
    RPM_prev = RPM_temp;                                // this is the previous RPM_temp value.  For studying purposes only

    RPM_temp = ptr_bat_MCUDArray->speed_rpm;            //

    /******** compute drpm / dIQ  -  For studying purposes only   **********/
    if (((IQ_applied - IQapp_prev) == 0) || ((RPM_temp - RPM_prev)/(IQ_applied - IQapp_prev) >= 0xFFFF) ||
            ((RPM_temp - RPM_prev)/(IQ_applied - IQapp_prev) <= -0xFFFF))
    {
        drpmdIQ = 0xFFFF;
    }
    else
    {
        drpmdIQ = (RPM_temp - RPM_prev)/(IQ_applied - IQapp_prev);
    }

    IQapp_prev = IQ_applied;                            // for studying purposes only

    /******** End compute drpm / dIQ  -  For investigation only   **********/

    /******** Throttle Error Safety Protocol -> when throttle error detected or speed is negative, IQ_input is set to zero
     *  Calculating the IQ Value
     *  Notes: Power is delivered to the motor if:
     *   (1) brake is not engaged
     *   (2) RPM is above the REG_MINP_RPM
     *  These features are for safety reasons
     **********************************************************************************/
    if ((!throttle_errorStatus) || (ptr_bat_MCUDArray->rpm_status))
    {
        if ((brakeStatus)||(RPM_temp < REG_MINP_RPM))
        {
        /*The E-Scooter Stops*/
        /*DRIVE_START = 0 --> Then we don't need to send dynamic Iq messages to the motor controller in order to relieve the UART loads*/
#ifdef ESCOOTER_RUN
            IQ_input = 0;
#endif

#ifdef ESCOOTER_DEBUG
            /*The E-Scooter Starts*/
            /*DRIVE_START = 1 --> The We could send dynamic Iq messages to the motor controller */
            IQ_input = speedModeIQmax * throttlePercent / 100;
#endif
        }
        else
        {
        /*The E-Scooter Starts*/
        /*DRIVE_START = 1 --> The We could send dynamic Iq messages to the motor controller */
            IQ_input = speedModeIQmax * throttlePercent / 100;
        }

    /***** Normal Law: modulated IQ_applied depending on rpm limit and output power limit *********************/
        if (ControlLaw == BRAKE_AND_THROTTLE_NORMALLAW)
        {
            brake_and_throttle_normalLawControl();
        }
        else
        {
            IQ_applied = IQ_input;
        }
    /***** End Normal Law *************************************/

    }
    else
    {
        IQ_input = 0;
        IQ_applied = IQ_input;
    }


    /********************************************************************************************************************************
     * Data required by STM32MCP to command Motor Controller
     ********************************************************************************************************************************/
    ptr_bat_STM32MCPDArray->IQ_value = IQ_applied;

    // in "brakeAndThrottle_CB(allowableSpeed, IQ_input, brakeAndThrottle_errorMsg)", brakeAndThrottle_errorMsg is sent to the motor control unit for error handling if necessary.
    // Add one more conditions in order to fed the power into the motor controller
    // if DRIVE_START == 1 -> then run the command for dynamic Iq, otherwise: ignore it!
//#ifdef CC2640R2_GENEV_5X5_ID

//    motor_control_setIQvalue();         //  this is moved to GPT main loop under N = 1.

//#endif    //CC2640R2_GENEV_5X5_ID

    /***** Increments brakeAndThrottleIndex by 1  ***/
    brakeAndThrottleIndex++;
    if (brakeAndThrottleIndex >= BRAKE_AND_THROTTLE_SAMPLES)
    {
        brakeAndThrottleIndex = 0;
    }

}

/*********************************************************************
 * @fun    brake_and_throttle_normalLawControl
 *
 * @brief   Normal Law Algorithm
 *
 * @param   Nil
 *
 * @return  Nil
 *********************************************************************/
uint32_t    normalLaw_count = 0;

static void brake_and_throttle_normalLawControl()
{
    uint16_t RPM_0;
    if (RPM_temp < 1)
    {
        RPM_0 = 1;
    }
    else
    {
        RPM_0 = RPM_temp;
    }
    speedModFactor = pow((rpm_limit/(float)RPM_0), exponent);

    /***   Speed limiter  ****/
    if (IQ_input == 0)
    {
        IQ_applied = IQ_input;
    }
    else // (IQ_input > 0)    // i.e. (IQ_input != 0)
    {
        if (( RPM_temp < rpm_limit ) && (!limit_exceedance_flag))
        {
            IQ_applied = IQ_input;
        }
        else if (( RPM_temp >= rpm_limit ) && (!limit_exceedance_flag))
        {
            IQ_applied = (float)speedModFactor * IQ_input;
            limit_exceedance_flag = 1;
        }
        else if (( RPM_temp >= rpm_limit ) && (limit_exceedance_flag))
        {
            IQ_applied = (float)speedModFactor * IQ_applied;
        }
        else if ((RPM_temp < rpm_limit) && (limit_exceedance_flag))    // when (RPM_temp >= rpm_limit), speedModFactor is less than or equal to 1.
        {
            IQ_applied = IQ_input * 0.5;
            limit_exceedance_flag = 0;
        }

        /***  Power Output Limiter  *****/
        IQ_maxPout = REG_MAXPOUT / (RPM_temp * 2 * PI_CONSTANT / 60) / KT_CONSTANT / KIQ_CONSTANT;
        if (IQ_applied > IQ_maxPout)
        {
            IQ_applied = IQ_maxPout;
        }

        if (IQ_applied >= IQ_input)
        {
            IQ_applied = IQ_input;
        }

    }
    normalLaw_count++;  // for debugging

}


/*********************************************************************
 * @fn      brakeAndThrottle_getSpeedModeParams
 *
 * @brief   Get speed Mode parameters
 *
 * @param   speedMode
 *
 * @return  none
 *********************************************************************/
static void brake_and_throttle_getSpeedModeParams()
{
    switch(speedMode)
    {
    case BRAKE_AND_THROTTLE_SPEED_MODE_AMBLE:                   // Amble mode
        {
            reductionRatio = BRAKE_AND_THROTTLE_SPEED_MODE_REDUCTION_RATIO_AMBLE;
            speedModeIQmax = reductionRatio * STM32MCP_TORQUEIQ_MAX / 100;
            rampRate = BRAKE_AND_THROTTLE_RAMPRATE_AMBLE;
            allowableSpeed = BRAKE_AND_THROTTLE_MAXSPEED_AMBLE;
            break;
        }
    case BRAKE_AND_THROTTLE_SPEED_MODE_LEISURE:                 // Leisure mode
        {
            reductionRatio = BRAKE_AND_THROTTLE_SPEED_MODE_REDUCTION_RATIO_LEISURE;
            speedModeIQmax = reductionRatio * STM32MCP_TORQUEIQ_MAX / 100;
            rampRate = BRAKE_AND_THROTTLE_RAMPRATE_LEISURE;
            allowableSpeed = BRAKE_AND_THROTTLE_MAXSPEED_LEISURE;
            break;
        }
    case BRAKE_AND_THROTTLE_SPEED_MODE_SPORTS:                  // Sports mode
        {
            reductionRatio = BRAKE_AND_THROTTLE_SPEED_MODE_REDUCTION_RATIO_SPORTS;
            speedModeIQmax = reductionRatio * STM32MCP_TORQUEIQ_MAX / 100;
            rampRate = BRAKE_AND_THROTTLE_RAMPRATE_SPORTS;
            allowableSpeed = BRAKE_AND_THROTTLE_MAXSPEED_SPORTS;
            break;
        }
    default:
        break;
    }
}

/*********************************************************************
 * @fn      brake_and_throttle_toggleSpeedMode
 *
 * @brief   To change / toggle the speed Mode of the e-scooter
 *
 * @param   none
 *
 * @return  none
 */
uint8_t brake_and_throttle_toggleSpeedMode()
{
    if (brake_errorStatus == 0)
    {
        if (throttleADCsample <= THROTTLE_ADC_CALIBRATE_L)                                    // Only allow speed mode change when no throttle is applied - will by-pass if throttle is applied
        {
            if(speedMode == BRAKE_AND_THROTTLE_SPEED_MODE_AMBLE)                       // Amble mode to Leisure mode
            {
                speedMode = BRAKE_AND_THROTTLE_SPEED_MODE_LEISURE;
                reductionRatio = BRAKE_AND_THROTTLE_SPEED_MODE_REDUCTION_RATIO_LEISURE;
                speedModeIQmax = reductionRatio * STM32MCP_TORQUEIQ_MAX / 100;
                rampRate = BRAKE_AND_THROTTLE_RAMPRATE_LEISURE;
                allowableSpeed = BRAKE_AND_THROTTLE_MAXSPEED_LEISURE;
            }
            else if(speedMode == BRAKE_AND_THROTTLE_SPEED_MODE_LEISURE)                 // Leisure mode to Sports mode
            {
                speedMode = BRAKE_AND_THROTTLE_SPEED_MODE_SPORTS;
                reductionRatio = BRAKE_AND_THROTTLE_SPEED_MODE_REDUCTION_RATIO_SPORTS;
                speedModeIQmax = reductionRatio * STM32MCP_TORQUEIQ_MAX / 100;
                rampRate = BRAKE_AND_THROTTLE_RAMPRATE_SPORTS;
                allowableSpeed = BRAKE_AND_THROTTLE_MAXSPEED_SPORTS;
            }
            else if(speedMode == BRAKE_AND_THROTTLE_SPEED_MODE_SPORTS)                  // Sports mode back to Amble mode
            {
                speedMode = BRAKE_AND_THROTTLE_SPEED_MODE_AMBLE;
                reductionRatio = BRAKE_AND_THROTTLE_SPEED_MODE_REDUCTION_RATIO_AMBLE;
                speedModeIQmax = reductionRatio * STM32MCP_TORQUEIQ_MAX / 100;
                rampRate = BRAKE_AND_THROTTLE_RAMPRATE_AMBLE;
                allowableSpeed = BRAKE_AND_THROTTLE_MAXSPEED_AMBLE;
            }
        }
    }
    else
    {
        if(speedMode != BRAKE_AND_THROTTLE_SPEED_MODE_AMBLE)        // This condition prevents unnecessary repetitive changes that does nothing
        {
            speedMode = BRAKE_AND_THROTTLE_SPEED_MODE_AMBLE;
            reductionRatio = BRAKE_AND_THROTTLE_SPEED_MODE_REDUCTION_RATIO_AMBLE;
            speedModeIQmax = reductionRatio * STM32MCP_TORQUEIQ_MAX / 100;
            rampRate = BRAKE_AND_THROTTLE_RAMPRATE_AMBLE;
            allowableSpeed = BRAKE_AND_THROTTLE_MAXSPEED_AMBLE;
        }
    }

    /* Send updated speed mode parameters to motor control unit */
    ptr_bat_STM32MCPDArray->speed_mode = speedMode;
    ptr_bat_STM32MCPDArray->speed_mode_IQmax = speedModeIQmax;
    ptr_bat_STM32MCPDArray->ramp_rate = rampRate;
    ptr_bat_STM32MCPDArray->allowable_speed = allowableSpeed;

    /* call and execute change to MCU */
    motor_control_speedModeChg();   // Note STM32MCP function commented out for debugging purposes

    /* updates led display */
    led_display_setSpeedMode(speedMode);    // update led display

    /******  Dashboard services *************************************/
    /* updates speed mode Characteristic Value -> Mobile App */
    ptr_charVal = (ptr_bat_profileCharVal->ptr_dash_charVal->ptr_speedMode);
    profile_setCharVal(ptr_charVal, DASHBOARD_SPEED_MODE_LEN, speedMode);

    return (speedMode);
}

/*********************************************************************
 * @fn      brake_and_throttle_setSpeedMode
 *
 * @brief   To set the speed mode of the escooter
 *
 * @param   speedMode - the speed mode of the escooter
 *
 * @return  none
 */
void brake_and_throttle_setSpeedMode(uint8_t speed_mode)
{
    speedMode = speed_mode;
}

/*********************************************************************
 * @fn      brake_and_throttle_getSpeedMode
 *
 * @brief   To get the speed mode of the escooter
 *
 * @param   none
 *
 * @return  the speedmode of the escooter
 */
uint8_t brake_and_throttle_getSpeedMode()
{
    return (speedMode);
}

/*********************************************************************
 * @fn      brake_and_throttle_getControlLaw
 *
 * @brief   call this function to retrieve the current control law
 *
 * @param   None
 *
 * @return  ControlLaw
 *********************************************************************/
extern uint8_t brake_and_throttle_getControlLaw()
{
    return (ControlLaw);
}

/*********************************************************************
 * @fn      brake_and_throttle_setControlLaw
 *
 * @brief   call this function to set Control Law
 *
 * @param   UnitSelectDash
 *
 * @return  none
 *********************************************************************/
extern void brake_and_throttle_setControlLaw(uint8_t control_law)
{
    ControlLaw = control_law;
}

/*********************************************************************
 * @fn      brake_and_throttle_getThrottlePercent
 *
 * @brief   To get the throttle percentage of the escooter
 *
 * @param   none
 *
 * @return  the throttle percentage of the escooter
 */
uint16_t brake_and_throttle_getThrottlePercent()
{
    return (throttlePercent);
}

/*********************************************************************
 * @fn      brake_and_throttle_getBrakePercent
 *
 * @brief   To get the brake percentage of the escooter
 *
 * @param   none
 *
 * @return  the brake percentage of the escooter
 */
uint16_t brake_and_throttle_getBrakePercent()
{
    return (brakePercent);
}

/*********************************************************************
 * @fn      bat_dashboardErrorCodeStatusRegister
 *
 * @brief   Return the point to dashboardErrorCodeStatus to the calling function
 *
 * @param   none
 *
 * @return  &dashboardErrorCodeStatus
 */
extern uint8_t* bat_dashboardErrorCodeStatusRegister()
{
    return (&dashboardErrorCodeStatus);
}

