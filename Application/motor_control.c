/*
 * motor_control.c
 *
 *  Created on: 7 May 2024
 *      Author: Chee
 */

/*********************************************************************
 * INCLUDES
 */
#include "Hardware/gGo_device_params.h"
#include "Hardware/STM32MCP.h"

#include "Application/motor_control.h"
#include "Application/periodic_communication.h"
#include "Application/data_analytics.h"
#include "Application/brake_and_throttle.h"
#include "Application/lights.h"
#include "Application/led_display.h"

/*********************************************************************
 * CONSTANTS
 */
#define MOTOR_CONNECT           1

#define TEMPERATUREOFFSET       50

/*********************************************************************
 * GLOBAL VARIABLES
 */
/*********************************************************************
 * LOCAL VARIABLES
 */
//static simplePeripheral_bleCBs_t *motorcontrol_bleCBs;
/** MCUDArray contains mcu retrieved data for data_analysis **/
MCUD_t MCUDArray = {LEVEL45,
                    0,//3000,
                    0,//32000,
                    0,//3000,
                    0,  // speed rpm
                    1,  // rpm_status. 1 = 0 or positive, 0 = negative
                    70,
                    70,
                    0
                    };

/** STM32MCDArray contains data for commanding / controlling the MCU and hence Motor **/
// initial values for {allowable_rpm, speed_mode_IQmax, IQ_value, ramp_rate, brake_percent, error_msg, brake_status, light_status, speed_mode}
STM32MCPD_t STM32MCDArray = {BRAKE_AND_THROTTLE_MAXSPEED_AMBLE,
                             14000,
                             0,
                             BRAKE_AND_THROTTLE_RAMPRATE_AMBLE,
                             0,
                             0,
                             0,
                             LIGHT_STATUS_OFF,
                             BRAKE_AND_THROTTLE_SPEED_MODE_AMBLE
                             };

/**********************************************************************
 *  Local functions
 */
static void motorcontrol_processGetRegisterFrameMsg(uint8_t *txPayload, uint8_t txPayloadLength, uint8_t *rxPayload, uint8_t rxPayloadLength);
static void motorcontrol_processGetMotorErrorFrameMsg(uint8_t *txPayload, uint8_t txPayloadLength, uint8_t *rxPayload, uint8_t rxPayloadLength);
static void motorcontrol_rxMsgCb(uint8_t *rxMsg, STM32MCP_txMsgNode_t *STM32MCP_txMsgNode);
static void motorcontrol_exMsgCb(uint8_t exceptionCode);
static void motorcontrol_erMsgCb(uint8_t errorCode);

/*********************************************************************
 * TYPEDEFS
 */
static STM32MCP_CBs_t motor_control_STM32MCP_CBs =
{
     motorcontrol_rxMsgCb,
     motorcontrol_exMsgCb,
     motorcontrol_erMsgCb
};

/*********************************************************************
* PUBLIC FUNCTIONS
*/

uint8_t wkUpPin = 0xFF;

/*
PIN_Config WakeUp[] = {
      CC2640R2_GENEV_5X5_ID_DIO5 | PIN_INPUT_EN | PIN_PULLUP | PINCC26XX_WAKEUP_NEGEDGE,
      PIN_TERMINATE
};

*/

/*********************************************************************
 * @fn      motor_control_init
 *
 * @brief   Initialization function for the Motor Control Task.
 *
 * @param   none
 *
 * @return  none
 */
uint8_t bootFlag = 0xFF;

void motor_control_init(void)
{
    data_analytics_MCUDArrayRegister(&MCUDArray);            // passes ptrMCUDArray to dataAnalysis.c
    periodic_communication_MCUArrayRegister(&MCUDArray);
    brake_and_throttle_MCUArrayRegister(&MCUDArray);

    brake_and_throttle_STM32MCDArrayRegister(&STM32MCDArray);
    lights_STM32MCPDArrayRegister(&STM32MCDArray);

    /*STM32 MCP Control Protocol*/
    STM32MCP_registerCBs(&motor_control_STM32MCP_CBs);      // pass pointer to motor_control_STM32MCP_CBs to STM32MCP.c
    STM32MCP_startCommunication();    //ACTIVATE UART Communication

}

/*********************************************************************
 * @fn      motorcontrol_processGetRegisterFrameMsg
 *
 * @brief   When the motor controller sends the get register frame message
 *
 * @param   STM32MCP_rxMsg_t - The memory message to the received message, the size of the message is the second index rxMsg[1]
 *
 * @return  None.
 */
uint8_t controller_error_code;
uint8_t rpmStatus = 1;
uint8_t voltage_ack = 0;
uint8_t speed_ack = 0;
uint16_t payloadVoltage;
static void motorcontrol_processGetRegisterFrameMsg(uint8_t *txPayload, uint8_t txPayloadLength, uint8_t *rxPayload, uint8_t rxPayloadLength)
{
    uint8_t regID = txPayload[0];
    switch(regID)
    {
    case STM32MCP_BUS_VOLTAGE_REG_ID:
        {
            uint16_t voltage_mV = *((uint16_t*) rxPayload) * 1000; // rxPayload in V, voltage in mV
            payloadVoltage = *((uint16_t*) rxPayload);
        // **** store voltage_mV in MCUArray.bat_voltage_mV
            MCUDArray.bat_voltage_mV = voltage_mV;
            voltage_ack++;
            break;
        }
    // *********  current sensor to be added to MCU.  Reserved case for current measurement
    case STM32MCP_TORQUE_MEASURED_REG_ID:
        {
        //keep current in mV - do not convert it to A
            uint16_t current_mA = *((uint8_t*) rxPayload) * 1000;
        // **** store current_mA in MCUArray.bat_current_mV
            MCUDArray.bat_current_mA = current_mA;
            break;
        }
    case STM32MCP_HEATSINK_TEMPERATURE_REG_ID:
        {
            uint8_t heatSinkTemperature_Celcius = (*((uint8_t*) rxPayload) & 0xFF);     // temperature can be a negative value, unless it is offset by a value
        // **** store heatSinkTemperature_Celcius in MCUArray.heatSinkTemperature_Celcius
            MCUDArray.heatSinkTempOffset50_Celcius = heatSinkTemperature_Celcius + TEMPERATUREOFFSET;  // +50
            break;
        }
    case STM32MCP_SPEED_MEASURED_REG_ID:    // RPM
        {
            uint16_t rpm;   // but payload length is 0x05
            int32_t rawRPM = *((int32_t*) rxPayload);
            if(rawRPM >= 0)
            {
                rpm = (uint16_t) (rawRPM & 0xFFFF);
                rpmStatus = 1;  // when rpm is >= 0
            }
            else  // **** if rawRPM is negative, e.g. pushing the E-scooter in reverse, No power shall be delivered to motor.
            {
                rpm = (uint16_t) (-rawRPM & 0xFFFF);
                rpmStatus = 0;  // when rpm < 0
            }
            // **** store rpm in MCUArray.speed_rpm
            MCUDArray.speed_rpm = rpm;
            MCUDArray.rpm_status = rpmStatus;
            speed_ack++;
            break;
        }
// ********************    Need to create new REG_IDs
    case STM32MCP_MOTOR_TEMPERATURE_REG_ID:
        {
            uint8_t motorTemperature_Celcius = (*((uint8_t*) rxPayload) & 0xFF);     // temperature can be a negative value, unless it is offset by a value
            MCUDArray.motorTempOffset50_Celcius = motorTemperature_Celcius + TEMPERATUREOFFSET;    // +50

            break;
        }
    case STM32MCP_CONTROLLER_ERRORCODE_REG_ID:
        {
            controller_error_code = (*((uint8_t*) rxPayload) & 0xFF);     //

            //
            break;
        }
    case STM32MCP_CONTROLLER_PHASEVOLTAGE_REG_ID:
        {
//            uint16_t voltage_mV = *((uint16_t*) rxPayload) * 1000; // rxPayload in V, voltage in mV
        /**** store voltage_mV in MCUArray.phase_voltage_mV ***/
//            MCUDArray.phase_voltage_mV = voltage_mV;
            //
            break;
        }
    case STM32MCP_CONTROLLER_PHASECURRENT_REG_ID:
        {
//            uint16_t current_mA = *((uint8_t*) rxPayload) * 1000;
        /**** store current_mA in MCUArray.phase_current_mV   ***/
//            MCUDArray.phase_current_mA = 3000;//current_mA;
            //
            break;
        }
    default:
            break;
    }

}

/*********************************************************************
 * @fn      motorcontrol_processGetMotorErrorFrameMsg
 *
 * @brief   Shows all the errors on LED Display
 *
 * @param
 *
 * @return  None.
 */
uint8_t why_fail = 0xFF;
static void motorcontrol_processGetMotorErrorFrameMsg(uint8_t *txPayload, uint8_t txPayloadLength, uint8_t *rxPayload, uint8_t rxPayloadLength)
{
    uint8_t regID = txPayload[0];
    if(regID == ESCOOTER_ERROR_REPORT)
    {
        uint8_t fault = rxPayload[0];
        switch(fault)
        {
           case SYS_NORMAL_CODE:
              break;

           case HALL_SENSOR_ERROR_CODE:
               why_fail = HALL_SENSOR_ERROR_CODE;
               led_display_ErrorPriority(HALL_SENSOR_ERROR_PRIORITY);
               break;

           case PHASE_I_ERROR_CODE:
               why_fail = PHASE_I_ERROR_CODE;
               led_display_ErrorPriority(PHASE_I_ERROR_PRIORITY);
               break;

           case MOSFET_ERROR_CODE:
               why_fail = MOSFET_ERROR_CODE;
               led_display_ErrorPriority(MOSFET_ERROR_PRIORITY);
               break;

           case GATE_DRIVER_ERROR_CODE:
               why_fail = GATE_DRIVER_ERROR_CODE;
               led_display_ErrorPriority(GATE_DRIVER_ERROR_PRIORITY);
               break;

           case BMS_COMM_ERROR_CODE:
               why_fail = BMS_COMM_ERROR_CODE;
               led_display_ErrorPriority(BMS_COMM_ERROR_PRIORITY);
               break;

           case MOTOR_TEMP_ERROR_CODE:
               why_fail = MOTOR_TEMP_ERROR_CODE;
               led_display_ErrorPriority(MOTOR_TEMP_ERROR_PRIORITY);
               break;

           case BATTERY_TEMP_ERROR_CODE:
               why_fail = BATTERY_TEMP_ERROR_CODE;
               led_display_ErrorPriority(BATTERY_TEMP_ERROR_PRIORITY);
               break;

        }

    }

}

/*********************************************************************
 * @fn      motorcontrol_rxMsgCb
 *
 * @brief   When the motor controller sends the feedback message back, it reaches here
 *
 * @param   rxMsg - The memory message to the received message, the size of the message is the second index rxMsg[1]
 *          motorID - The motor ID described in STM32MCP
 *          frameID - The frame ID described in STM32MCP
 *
 * @return  None.
 */
uint8_t msg_rx = 0;
static void motorcontrol_rxMsgCb(uint8_t *rxMsg, STM32MCP_txMsgNode_t *STM32MCP_txMsgNode)
{
    uint8_t frameID = STM32MCP_txMsgNode->txMsg[0] & 0x1F;

    uint8_t rxPayloadLength = rxMsg[1];
    uint8_t *rxPayload = (uint8_t*)malloc(sizeof(uint8_t)*rxPayloadLength);
    memcpy(rxPayload, rxMsg + 2, rxPayloadLength);

    uint8_t txPayloadLength = STM32MCP_txMsgNode->size;
    uint8_t *txPayload = (uint8_t*)malloc(sizeof(uint8_t)*txPayloadLength);
    memcpy(txPayload, STM32MCP_txMsgNode->txMsg + 2, txPayloadLength);

    switch(frameID)
    {
    case STM32MCP_SET_REGISTER_FRAME_ID:
        break;
    case STM32MCP_GET_REGISTER_FRAME_ID:
        motorcontrol_processGetRegisterFrameMsg(txPayload, txPayloadLength, rxPayload, rxPayloadLength);
        break;
    case STM32MCP_EXECUTE_COMMAND_FRAME_ID:
        break;
    case STM32MCP_GET_BOARD_INFO_FRAME_ID:
        break;
    case STM32MCP_EXEC_RAMP_FRAME_ID:
        break;
    case STM32MCP_GET_REVUP_DATA_FRAME_ID:
        break;
    case STM32MCP_SET_REVUP_DATA_FRAME_ID:
        break;
    case STM32MCP_SET_CURRENT_REFERENCES_FRAME_ID:
        break;
    case DEFINE_ESCOOTER_BEHAVIOR_ID:
        msg_rx++;
        motorcontrol_processGetMotorErrorFrameMsg(txPayload, txPayloadLength, rxPayload, rxPayloadLength);
        break;
    case STM32MCP_SET_DRIVE_MODE_CONFIG_FRAME_ID:
        break;
    case STM32MCP_SET_DYNAMIC_TORQUE_FRAME_ID:
        break;
    default:
        break;
    }
    free(rxPayload);
    free(txPayload);
}

/*********************************************************************
 * @fn      motorcontrol_exMsgCb
 *
 * @brief   When the motor controller sends the exception back, it reaches here
 *
 * @param   exceptionCode - The exception code described in STM32MCP
 *
 * @return  None.
 */
static void motorcontrol_exMsgCb(uint8_t exceptionCode)
{
    switch(exceptionCode)
        {
        case STM32MCP_QUEUE_OVERLOAD:
            break;
        case STM32MCP_EXCEED_MAXIMUM_RETRANSMISSION_ALLOWANCE:
            break;
        default:
            break;
        }
}

/*********************************************************************
 * @fn      motorcontrol_erMsgCb
 *
 * @brief   When the motor controller sends the error back, it reaches here
 *
 * @param   exceptionCode - The error code described in STM32MCP
 *
 * @return  None.
 */
uint8_t uart_err = 0x00;
static void motorcontrol_erMsgCb(uint8_t errorCode)
{
    switch(errorCode)
    {
    case STM32MCP_BAD_FRAME_ID:
        break;
    case STM32MCP_WRITE_ON_READ_ONLY:
        break;
    case STM32MCP_READ_NOT_ALLOWED:
        break;
    case STM32MCP_BAD_TARGET_DRIVE:
        break;
    case STM32MCP_OUT_OF_RANGE:
        break;
    case STM32MCP_BAD_COMMAND_ID:
        break;
    case STM32MCP_OVERRUN_ERROR:
        break;
    case STM32MCP_TIMEOUT_ERROR:
        break;
    case STM32MCP_BAD_CRC:
        break;
    default:
        break;
    }
}

/*********************************************************************
 * @fn      motor_control_minSpeed
 *
 * @brief   Determines whether the E-Scooter reaches mini speed
 *
 * @param   None
 *
 * @return  Returns 0x01 if the E-Scooter is > 3km/hr (80 RPM)
 *          Returns 0x00 if the E-Scooter is < 3km/hr (80 RPM)
 */
uint8_t motor_control_minSpeed()
{
    if((MCUDArray.speed_rpm >= REG_MINP_RPM) && (MCUDArray.rpm_status))
    {
        return ABOVE_MIN_SPEED;
    }
    else
    {
        return BELOW_MIN_SPEED;
    }
}

/*********************************************************************
 * @fn      motor_control_setIQvalue
 *
 * @brief   When the brake and throttle completed the adc conversion, it sends message here to communicate with STM32
 *
 * @param   throttlePercent - how much speed or torque in percentage the escooter should reach
 *          errorMsg - The error Msg
 *
 * @return  None.
 */
//static void motor_control_setIQvalue(uint16_t allowableSpeed, uint16_t IQValue, uint8_t errorMsg)
extern void motor_control_setIQvalue()
{
        if(STM32MCDArray.error_msg == BRAKE_AND_THROTTLE_NORMAL)
        {
            /*When driver accelerates / decelerates by twisting the throttle, the IQ signal with max. speed will be sent to the motor controller.
             * */
            STM32MCP_setDynamicCurrent(brake_and_throttle_getThrottlePercent(), STM32MCDArray.IQ_value);
        }
        else
        {
            /*In case the brake and throttle are in malfunction, for safety, the E-Scooter stops operation. User
             *has to check the wire connections.
             *You have to ensure the wires are connected properly!
             * */
            //STM32MCP_executeCommandFrame(STM32MCP_MOTOR_1_ID, STM32MCP_STOP_MOTOR_COMMAND_ID);
            STM32MCP_setDynamicCurrent(brake_and_throttle_getThrottlePercent(), 0);
            /*Sends Error Report to STM32 Motor Controller --> Transition to EMERGENCY STOP state*/
        }
}

/*********************************************************************
 * @fn      motor_control_speedModeParamsChg
 *
 * @brief   When there is a speed mode change, it sends message here to communicate with STM32
 *
 * @param   speed mode parameters
 *
 *
 * @return  None.
 */
//static void motor_control_speedModeParamsChg(uint16_t torqueIQ, uint16_t allowableSpeed, uint16_t rampRate)
extern void motor_control_speedModeParamsChg()
{
    //STM32MCP_setSpeedModeConfiguration(STM32MCDArray.speed_mode_IQmax, STM32MCDArray.allowable_rpm, STM32MCDArray.ramp_rate);
#ifdef MOTOR_CONNECT

    /*** send speed mode change parameters to motor control   ***/
    STM32MCP_setSpeedModeConfiguration(STM32MCDArray.speed_mode_IQmax, STM32MCDArray.allowable_rpm, STM32MCDArray.ramp_rate);

#endif //MOTOR_CONNECT
}

extern void motor_control_changeSpeedMode()
{
    STM32MCP_setSpeedModeConfiguration(STM32MCDArray.speed_mode_IQmax, STM32MCDArray.allowable_rpm, STM32MCDArray.ramp_rate);
}
/*********************************************************************
 * @fn      motor_control_brakeStatusChg
 *
 * @brief   When the brake status is changed, it sends command to STM32 to cut power to motor
 *
 * @param   None.
 *
 * @return  None.
 */
extern void motor_control_brakeStatusChg()
{
    uint8_t brake_debugID;
    if (STM32MCDArray.brake_status)
    {
        brake_debugID = ESCOOTER_BRAKE_PRESS;
    }
    else if (!(STM32MCDArray.brake_status))
    {
        brake_debugID = ESCOOTER_BRAKE_RELEASE;
    }
}

/*********************************************************************
 * @fn      motor_control_taillightStatusChg
 *
 * @brief   When the brake status is changed, it sends command to STM32 to cut power to motor
 *
 * @param   None.
 *
 * @return  None.
 */
extern void motor_control_taillightStatusChg()
{
    uint8_t light_sysCmdId = STM32MCDArray.tail_light_status;
#ifdef MOTOR_CONNECT

    STM32MCP_controlEscooterBehavior(ESCOOTER_TOGGLE_TAIL_LIGHT);

#endif //MOTOR_CONNECT

}

