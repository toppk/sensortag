/******************************************************************************

 @file  sensortag.c

 @brief This file is the SensorTag application's main body.

 Group: WCS, BTS
 Target Device: CC2650, CC2640

 ******************************************************************************
 
 Copyright (c) 2015-2024, Texas Instruments Incorporated
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions
 are met:

 *  Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

 *  Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

 *  Neither the name of Texas Instruments Incorporated nor the names of
    its contributors may be used to endorse or promote products derived
    from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 ******************************************************************************
 Release Name: ble_sdk_2_02_02_
 Release Date: 2024-08-05 19:10:12
 *****************************************************************************/

/*******************************************************************************
 * INCLUDES
 */
#include <string.h>
#include <xdc/std.h>

#include <xdc/runtime/System.h>
#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Queue.h>
#include <ti/sysbios/knl/Task.h>

#include <ICall.h>

#include "gatt.h"
#include "gatt_profile_uuid.h"
#include "hci.h"
#include "gapgattserver.h"
#include "gattservapp.h"
#include "gapbondmgr.h"
#include "osal_snv.h"
#include "util.h"
#include "icall_apimsg.h"

#include "SensorI2C.h"
#include <driverlib/aon_batmon.h>
#include <driverlib/aon_rtc.h>
#include <driverlib/sys_ctrl.h>
#include "SensorTagTest.h"
#include "sensortag_revision.h"
#include "Board.h"
#include "SensorUtil.h"
#include "devinfoservice.h"
#include "movementservice.h"
#include "displayservice.h"
#include "sensortag_register.h"
#ifdef FACTORY_IMAGE
#include "sensortag_factoryreset.h"
#endif

// SensorTag on-board devices
#include "st_util.h"

#include "sensortag_tmp.h"
#include "sensortag_hum.h"
#include "sensortag_bar.h"
#include "sensortag_mov.h"
#include "sensortag_opt.h"
#include "sensortag_keys.h"
#include "sensortag_io.h"
#include "sensortag_batt.h"
#include "sensortag_audio.h"
#include "sensortag_oad.h"
#include "sensortag_conn_ctrl.h"

// DevPack devices
#include "sensortag_display.h"
#include "sensortag_light.h"

/*******************************************************************************
 * CONSTANTS
 */

// How often to perform periodic event (in milliseconds)
#define ST_PERIODIC_EVT_PERIOD               1000

// Connect-mode advertising interval; readings change every 5 s, so once a second is plenty
// (units of 625us, 1600=1s)
#define DEFAULT_ADVERTISING_INTERVAL          1600

// General discoverable mode advertises indefinitely
#define DEFAULT_DISCOVERABLE_MODE             GAP_ADTYPE_FLAGS_LIMITED

// Beacon mode: each reading goes out as a short non-connectable burst, then the radio is quiet
// (units of 625us, 2400=1.5s)
#define BURST_ADVERTISING_INTERVAL            2400
#define BURST_PERIOD                          3750    // ms; adverts at 0, 1.5 and 3 s
// Bluetooth SIG company ID reserved for internal and test use
#define BEACON_COMPANY_ID                     0xFFFF
// A message's first byte: format in the low nibble, flags in the high nibble (unused ones 0)
#define BEACON_FORMAT                         0x01
#define INFO_FORMAT                           0x02
#define FLAG_CONNECT_MODE                     0x10
#define INFO_PERIOD                           15000   // ms the info packet replaces readings
#define INFO_ADVERTISING_INTERVAL             160     // 100 ms, so scanners hear the press at once
#ifndef BUILD_GIT
#define BUILD_GIT                             0
#define BUILD_DIRTY                           1
#define FW_VERSION_X100                       0
#define BLE_SDK_VERSION                       0
#endif

// Minimum connection interval (units of 1.25ms, 80=100ms) if automatic
// parameter update request is enabled
#define DEFAULT_DESIRED_MIN_CONN_INTERVAL     8

// Maximum connection interval (units of 1.25ms, 800=1000ms) if automatic
// parameter update request is enabled
#define DEFAULT_DESIRED_MAX_CONN_INTERVAL     800

// Slave latency to use if automatic parameter update request is enabled
#define DEFAULT_DESIRED_SLAVE_LATENCY         0

// Supervision timeout value (units of 10ms, 1000=10s) if automatic parameter
// update request is enabled
#define DEFAULT_DESIRED_CONN_TIMEOUT          100

// Whether to enable automatic parameter update request when a
// connection is formed
#define DEFAULT_ENABLE_UPDATE_REQUEST         GAPROLE_LINK_PARAM_UPDATE_WAIT_BOTH_PARAMS

// Connection Pause Peripheral time value (in seconds)
#define DEFAULT_CONN_PAUSE_PERIPHERAL         1

// Company Identifier: Texas Instruments Inc. (13)
#define TI_COMPANY_ID                         0x000D
#define TI_ST_DEVICE_ID                       0x03
#define TI_ST_KEY_DATA_ID                     0x00

// Length of board address
#define B_ADDR_STR_LEN                        15

#if defined (PLUS_BROADCASTER)
  #define ADV_IN_CONN_WAIT                    500 // delay 500 ms
#endif

// Task configuration
#define ST_TASK_PRIORITY                      1

#ifndef ST_TASK_STACK_SIZE
// Stack size may be overridden by project settings
#define ST_TASK_STACK_SIZE                    700
#endif

// Internal Events for RTOS application
#define ST_STATE_CHANGE_EVT                   0x0001
#define ST_CHAR_CHANGE_EVT                    0x0002
#define ST_PERIODIC_EVT                       0x0004
#define SBP_OAD_WRITE_EVT                     0x0008
#define ST_BEACON_EVT                         0x0010
#define ST_INFO_END_EVT                       0x0020
#define ST_BURST_END_EVT                      0x0040

// Misc.
#define INVALID_CONNHANDLE                    0xFFFF
#define TEST_INDICATION_BLINKS                5  // Number of blinks
#define OAD_PACKET_SIZE                       18
#define FLAGS_OFFSET                          2  // Offsets in advertising data
#define FORMAT_OFFSET                         7
#define BEACON_SEQ_OFFSET                     8

// Which advertising runs when not connected
#define ADV_MODE_BEACON                       0
#define ADV_MODE_CONNECTABLE                  1

/*******************************************************************************
 * TYPEDEFS
 */

// App event passed from profiles.
typedef struct
{
  uint8_t event;  // Which profile's event
  uint8_t serviceID; // New status
  uint8_t paramID;
} stEvt_t;

/*******************************************************************************
 * GLOBAL VARIABLES
 */
// Profile state and parameters
gaprole_States_t gapProfileState = GAPROLE_INIT;

// Semaphore globally used to post events to the application thread
ICall_Semaphore sem;

// Entity ID globally used to check for source and/or destination of messages
ICall_EntityID selfEntityMain;

// Global pin resources
PIN_State pinGpioState;
PIN_Handle hGpioPin;

/*******************************************************************************
 * LOCAL VARIABLES
 */

// Task configuration
static Task_Struct sensorTagTask;
static Char sensorTagTaskStack[ST_TASK_STACK_SIZE];

// Entity ID globally used to check for source and/or destination of messages

// Clock instances for internal periodic events.
static Clock_Struct periodicClock;

// Queue object used for app messages
static Queue_Struct appMsg;
static Queue_Handle appMsgQueue;

// events flag for internal application events.
static uint16_t events;

// self-test result
static uint8_t selfTestMap;

// GAP - SCAN RSP data (max size = 31 bytes)
static uint8_t scanRspData[] =
{
  // complete name
  0x11,   // length of this data
  GAP_ADTYPE_LOCAL_NAME_COMPLETE,
#ifdef CC1350STK
  'C', 'C', '1', '3', '5', '0', ' ',
#else
  'C', 'C', '2', '6', '5', '0', ' ',
#endif
  'S', 'e', 'n',  's',  'o',  'r',  'T',  'a',  'g',

  // connection interval range
  0x05,   // length of this data
  GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE,
  LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL),
  HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL),
  LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL),
  HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL),

  // Tx power level
  0x02,   // length of this data
  GAP_ADTYPE_POWER_LEVEL,
  0       // 0dBm
};

// GAP - Advertisement data (max size = 31 bytes, though this is
// best kept short to conserve power while advertising)
static uint8_t advertData[] =
{
  // Flags; limited discoverable in the connectable window, none as a beacon
  0x02,   // length of this data
  GAP_ADTYPE_FLAGS,
  GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

  // Readings, little-endian; 0xFFFF where a sensor didn't answer
  0x0D,   // length of this data
  GAP_ADTYPE_MANUFACTURER_SPECIFIC,
  LO_UINT16(BEACON_COMPANY_ID),
  HI_UINT16(BEACON_COMPANY_ID),
  BEACON_FORMAT,
  0x00,                                   // sequence, bumped with each reading
  0xFF, 0xFF,                             // HDC1000 raw temperature
  0xFF, 0xFF,                             // HDC1000 raw humidity
  0xFF, 0xFF,                             // OPT3001 raw light
  0x00, 0x00                              // battery, volts in 8.8 fixed point
};

// Sent for INFO_PERIOD after a button 2 press, in place of advertData
static uint8_t infoData[] =
{
  0x02,   // length of this data
  GAP_ADTYPE_FLAGS,
  GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

  0x19,   // length of this data
  GAP_ADTYPE_MANUFACTURER_SPECIFIC,
  LO_UINT16(BEACON_COMPANY_ID),
  HI_UINT16(BEACON_COMPANY_ID),
  INFO_FORMAT,
  0x00,                                   // 8: button 2 presses since boot
  0x00, 0x00,                             // 9: app version, FW_VERSION * 100 (150 = 1.50)
  0x00, 0x00, 0x00, 0x00,                 // 11: git commit the app was built from
  0x00,                                   // 15: 1 if built with uncommitted changes
  0x00, 0x00, 0x00,                       // 16: BLE SDK (stack) version, major minor patch
  0x00, 0x00, 0x00, 0x00,                 // 19: stack start address it was linked against
  0x00,                                   // 23: power-on self-test map
  0x00,                                   // 24: reset source
  0x00, 0x00, 0x00, 0x00                  // 25: uptime, seconds
};
static bool infoActive = false;
static Clock_Struct infoClock;
static bool burstActive = false;
static Clock_Struct burstClock;
static uint16_t beaconInterval = 0;   // interval the running beacon advertising uses

static uint8_t advMode = ADV_MODE_BEACON;
static bool advSwitching = false;
static volatile uint16_t beaconRaw[3] = {0xFFFF, 0xFFFF, 0xFFFF};

// Device information parameters
#ifdef CC1350STK
static const uint8_t devInfoModelNumber[] = "CC1350 SensorTag";
#else
static const uint8_t devInfoModelNumber[] = "CC2650 SensorTag";
#endif
static const uint8_t devInfoNA[] =          "N.A.";
static const uint8_t devInfoFirmwareRev[] = FW_VERSION_STR;
static const uint8_t devInfoMfrName[] =     "Texas Instruments";
static const uint8_t *devInfoHardwareRev =  devInfoNA;

// GAP GATT Attributes
static const uint8_t *attDeviceName = devInfoModelNumber;

// Pins that are actively used by the application
static PIN_Config SensortagAppPinTable[] =
{
    Board_LED1       | PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MAX,     /* LED initially off             */
    Board_LED2       | PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MAX,     /* LED initially off             */
    Board_KEY_LEFT   | PIN_INPUT_EN | PIN_PULLUP | PIN_IRQ_BOTHEDGES | PIN_HYSTERESIS,        /* Button is active low          */
    Board_KEY_RIGHT  | PIN_INPUT_EN | PIN_PULLUP | PIN_IRQ_BOTHEDGES | PIN_HYSTERESIS,        /* Button is active low          */
    Board_RELAY      | PIN_INPUT_EN | PIN_PULLDOWN | PIN_IRQ_BOTHEDGES | PIN_HYSTERESIS,      /* Relay is active high          */
    Board_BUZZER     | PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MAX,     /* Buzzer initially off          */

    PIN_TERMINATE
};

/*******************************************************************************
 * LOCAL FUNCTIONS
 */

static void SensorTag_init(void);
static void SensorTag_taskFxn(UArg a0, UArg a1);
static void SensorTag_processStackMsg(ICall_Hdr *pMsg);
static void SensorTag_processGATTMsg(gattMsgEvent_t *pMsg);
static void SensorTag_processAppMsg(stEvt_t *pMsg);
static void SensorTag_processStateChangeEvt(gaprole_States_t newState) ;
static void SensorTag_processCharValueChangeEvt(uint8_t serviceID, uint8_t paramID) ;
static void SensorTag_stateChangeCB(gaprole_States_t newState);
static void SensorTag_resetAllModules(void);
static void SensorTag_clockHandler(UArg arg);
static void SensorTag_enqueueMsg(uint8_t event, uint8_t serviceID, uint8_t paramID);
static void SensorTag_callback(PIN_Handle handle, PIN_Id pinId);
static void SensorTag_setDeviceInfo(void);
static void SensorTag_updateBeacon(void);
static void SensorTag_pushAdvertData(void);
static void put32(uint8_t *p, uint32_t v);
static void SensorTag_setAdvInterval(uint16_t advInt);
static void SensorTag_startAdvertising(void);
static void SensorTag_refreshBeacon(void);

/*******************************************************************************
 * PROFILE CALLBACKS
 */

// GAP Role Callbacks
static gapRolesCBs_t sensorTag_gapRoleCBs =
{
  SensorTag_stateChangeCB     // Profile State Change Callbacks
};

// GAP Bond Manager Callbacks
static gapBondCBs_t sensorTag_bondMgrCBs =
{
  NULL, // Passcode callback (not used by application)
  NULL  // Pairing / Bonding state Callback (not used by application)
};

/*******************************************************************************
 * PUBLIC FUNCTIONS
 */

/*******************************************************************************
 * @fn      SensorTag_createTask
 *
 * @brief   Task creation function for the SensorTag.
 *
 * @param   none
 *
 * @return  none
 */
void SensorTag_createTask(void)
{
  Task_Params taskParams;

  // Configure task
  Task_Params_init(&taskParams);
  taskParams.stack = sensorTagTaskStack;
  taskParams.stackSize = ST_TASK_STACK_SIZE;
  taskParams.priority = ST_TASK_PRIORITY;

  Task_construct(&sensorTagTask, SensorTag_taskFxn, &taskParams, NULL);
}

/*******************************************************************************
 * @fn      SensorTag_testResult
 *
 * @brief   Get result of self-test
 *
 * @param   none
 *
 * @return  bitmap with result of self-test
 */
uint8_t SensorTag_testResult(void)
{
    return selfTestMap;
}

/*******************************************************************************
 * @fn      SensorTag_init
 *
 * @brief   Called during initialization and contains application
 *          specific initialization (i.e. hardware initialization/setup,
 *          table initialization, power up notification, etc), and
 *          profile initialization/setup.
 *
 * @param   none
 *
 * @return  none
 */
static void SensorTag_init(void)
{
  // Setup I2C for sensors
  SensorI2C_open();

  // Handling of buttons, LED, relay
  hGpioPin = PIN_open(&pinGpioState, SensortagAppPinTable);
  PIN_registerIntCb(hGpioPin, SensorTag_callback);

  // ***************************************************************************
  // N0 STACK API CALLS CAN OCCUR BEFORE THIS CALL TO ICall_registerApp
  // ***************************************************************************
  // Register the current thread as an ICall dispatcher application
  // so that the application can send and receive messages.
  ICall_registerApp(&selfEntityMain, &sem);

  // Create an RTOS queue for message from profile to be sent to app.
  appMsgQueue = Util_constructQueue(&appMsg);

  // Create one-shot clocks for internal periodic events.
  Util_constructClock(&periodicClock, SensorTag_clockHandler,
                      ST_PERIODIC_EVT_PERIOD, 0, false, ST_PERIODIC_EVT);
  Util_constructClock(&infoClock, SensorTag_clockHandler,
                      INFO_PERIOD, 0, false, ST_INFO_END_EVT);
  Util_constructClock(&burstClock, SensorTag_clockHandler,
                      BURST_PERIOD, 0, false, ST_BURST_END_EVT);

  // Setup the GAP
  GAP_SetParamValue(TGAP_CONN_PAUSE_PERIPHERAL, DEFAULT_CONN_PAUSE_PERIPHERAL);

  // Setup the GAP Peripheral Role Profile
  {
    // Beacon advertising starts once the role is up (GAPROLE_STARTED)
    uint8_t initialAdvertEnable = FALSE;

    // By setting this to zero, the device will go into the waiting state after
    // being discoverable for 30.72 second, and will not being advertising again
    // until the enabler is set back to TRUE
    uint16_t advertOffTime = 0;

    uint8_t enableUpdateRequest = DEFAULT_ENABLE_UPDATE_REQUEST;
    uint16_t desiredMinInterval = DEFAULT_DESIRED_MIN_CONN_INTERVAL;
    uint16_t desiredMaxInterval = DEFAULT_DESIRED_MAX_CONN_INTERVAL;
    uint16_t desiredSlaveLatency = DEFAULT_DESIRED_SLAVE_LATENCY;
    uint16_t desiredConnTimeout = DEFAULT_DESIRED_CONN_TIMEOUT;

    // Set the GAP Role Parameters
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t),
                         &initialAdvertEnable);
    GAPRole_SetParameter(GAPROLE_ADVERT_OFF_TIME, sizeof(uint16_t),
                         &advertOffTime);

    GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, sizeof(scanRspData),
                         scanRspData);
    GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);

    GAPRole_SetParameter(GAPROLE_PARAM_UPDATE_ENABLE, sizeof(uint8_t),
                         &enableUpdateRequest);
    GAPRole_SetParameter(GAPROLE_MIN_CONN_INTERVAL, sizeof(uint16_t),
                         &desiredMinInterval);
    GAPRole_SetParameter(GAPROLE_MAX_CONN_INTERVAL, sizeof(uint16_t),
                         &desiredMaxInterval);
    GAPRole_SetParameter(GAPROLE_SLAVE_LATENCY, sizeof(uint16_t),
                         &desiredSlaveLatency);
    GAPRole_SetParameter(GAPROLE_TIMEOUT_MULTIPLIER, sizeof(uint16_t),
                         &desiredConnTimeout);
  }

  // Set the GAP Characteristics
  GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN,
                   (void*)attDeviceName);

  // Set advertising interval
  {
    uint16_t advInt = DEFAULT_ADVERTISING_INTERVAL;

    GAP_SetParamValue(TGAP_LIM_DISC_ADV_INT_MIN, advInt);
    GAP_SetParamValue(TGAP_LIM_DISC_ADV_INT_MAX, advInt);
    GAP_SetParamValue(TGAP_GEN_DISC_ADV_INT_MIN, advInt);
    GAP_SetParamValue(TGAP_GEN_DISC_ADV_INT_MAX, advInt);
  }

  // Setup the GAP Bond Manager
  {
    uint32_t passkey = 0; // passkey "000000"
    uint8_t pairMode = GAPBOND_PAIRING_MODE_WAIT_FOR_REQ;
    uint8_t mitm = FALSE;
    uint8_t ioCap = GAPBOND_IO_CAP_NO_INPUT_NO_OUTPUT;
    uint8_t bonding = TRUE;

    GAPBondMgr_SetParameter(GAPBOND_DEFAULT_PASSCODE, sizeof(uint32_t),
                            &passkey);
    GAPBondMgr_SetParameter(GAPBOND_PAIRING_MODE, sizeof(uint8_t), &pairMode);
    GAPBondMgr_SetParameter(GAPBOND_MITM_PROTECTION, sizeof(uint8_t), &mitm);
    GAPBondMgr_SetParameter(GAPBOND_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
    GAPBondMgr_SetParameter(GAPBOND_BONDING_ENABLED, sizeof(uint8_t), &bonding);
  }

  // Initialize GATT attributes
  GGS_AddService(GATT_ALL_SERVICES);           // GAP
  GATTServApp_AddService(GATT_ALL_SERVICES);   // GATT attributes

  // Add device info service.
  DevInfo_AddService();

  // Add application specific device information
  SensorTag_setDeviceInfo();

  // Add battery monitor
  SensorTagBatt_init();

  // Power on self-test for sensors, flash and DevPack
  selfTestMap = SensorTag_testExecute(ST_TEST_MAP);

  if (selfTestMap == ST_TEST_MAP)
  {
#ifdef IOID_GREEN_LED
    // Blink green LED
    SensorTagIO_blinkLed(IOID_GREEN_LED, TEST_INDICATION_BLINKS);
#else
    // Blink red LED
    SensorTagIO_blinkLed(IOID_RED_LED, TEST_INDICATION_BLINKS);
#endif
  }
  else
  {
    // Blink red LED
    SensorTagIO_blinkLed(IOID_RED_LED, TEST_INDICATION_BLINKS);
  }

#ifdef FACTORY_IMAGE
  // Check if a factory image exists and apply current image if necessary
  if (!SensorTagFactoryReset_hasImage())
  {
    if (!SensorTagFactoryReset_storeCurrentImage())
    {
        // Failure to store factory image should be reported as flash failure
        selfTestMap &= ~SENSOR_FLASH_TEST_BM;
    }
  }
#endif

  // Sensor modules
  SensorTagTmp_init();                            // IR temperature sensor
  SensorTagHum_init();                            // Humidity sensor
  SensorTagBar_init();                            // Pressure sensor
  SensorTagMov_init();                            // Movement processor
  SensorTagOpt_init();                            // Optical sensor

  // Auxiliary modules
  SensorTagKeys_init();                           // Key and relay handling
  SensorTagIO_init();                             // IO (LED+buzzer+self test)
  SensorTagRegister_init();                       // Register Service
  SensorTagAudio_init();                          // Voice Control
  SensorTagOad_init();                            // Over the Air Download

  // DevPack devices
  SensorTagDisplay_init();                        // Display DevPack
  SensorTagLight_init();                          // LED Light Devpack

  // Start the Device
  GAPRole_StartDevice(&sensorTag_gapRoleCBs);

  // Start Bond Manager
  VOID GAPBondMgr_Register(&sensorTag_bondMgrCBs);

  // Enable interrupt handling for keys and relay
  PIN_registerIntCb(hGpioPin, SensorTag_callback);
}

/*******************************************************************************
 * @fn      SensorTag_taskFxn
 *
 * @brief   Application task entry point for the SensorTag
 *
 * @param   a0, a1 (not used)
 *
 * @return  none
 */
static void SensorTag_taskFxn(UArg a0, UArg a1)
{
  // Initialize application
  SensorTag_init();

  // Application main loop
  for (;;)
  {
    // Waits for a signal to the semaphore associated with the calling thread.
    // Note that the semaphore associated with a thread is signalled when a
    // message is queued to the message receive queue of the thread or when
    // ICall_signal() function is called onto the semaphore.
    ICall_Errno errno = ICall_wait(ICALL_TIMEOUT_FOREVER);

    if (errno == ICALL_ERRNO_SUCCESS)
    {
      ICall_EntityID dest;
      ICall_ServiceEnum src;
      ICall_HciExtEvt *pMsg = NULL;

      if (ICall_fetchServiceMsg(&src, &dest,
                                (void **)&pMsg) == ICALL_ERRNO_SUCCESS)
      {
        if ((src == ICALL_SERVICE_CLASS_BLE) && (dest == selfEntityMain))
        {
          // Process inter-task message
          SensorTag_processStackMsg((ICall_Hdr *)pMsg);
        }

        if (pMsg)
        {
          ICall_freeMsg(pMsg);
        }
      }

      // If RTOS queue is not empty, process app message.
      while (!Queue_empty(appMsgQueue))
      {
        stEvt_t *pMsg = (stEvt_t *)Util_dequeueMsg(appMsgQueue);
        if (pMsg)
        {
          // Process message.
          SensorTag_processAppMsg(pMsg);

          // Free the space from the message.
          ICall_free(pMsg);
        }
      }

      // Process new data if available
      SensorTagKeys_processEvent();
#ifndef EXCLUDE_AUDIO
      if (gapProfileState == GAPROLE_CONNECTED)
      {
        SensorTagAudio_processEvent();
      }
#endif
      SensorTagOpt_processSensorEvent();
      SensorTagMov_processSensorEvent();
      SensorTagBatt_processSensorEvent();
    }

    if (!!(events & ST_BEACON_EVT))
    {
      events &= ~ST_BEACON_EVT;
      SensorTag_updateBeacon();
    }

    if (!!(events & ST_INFO_END_EVT))
    {
      events &= ~ST_INFO_END_EVT;
      infoActive = false;
      SensorTag_pushAdvertData();
      SensorTag_refreshBeacon();
    }

    if (!!(events & ST_BURST_END_EVT))
    {
      events &= ~ST_BURST_END_EVT;
      burstActive = false;
      SensorTag_refreshBeacon();
    }

    if (!!(events & ST_PERIODIC_EVT))
    {
      events &= ~ST_PERIODIC_EVT;

      if (gapProfileState == GAPROLE_ADVERTISING)
      {
        Util_startClock(&periodicClock);
      }

      // Blink green LED when advertising
      if (gapProfileState == GAPROLE_ADVERTISING)
      {
#ifdef IOID_GREEN_LED
        SensorTagIO_blinkLed(IOID_GREEN_LED, 1);
#else
        SensorTagIO_blinkLed(IOID_RED_LED, 1);
#endif
        #ifdef INCLUDE_DISPLAY
        SensorTagDisplay_showBatteryLevel();
        #endif
      }
    }

    // OAD event queue
    SensorTagOad_processEvent();
  } // task loop
}


/*******************************************************************************
 * @fn      SensorTag_setDeviceInfo
 *
 * @brief   Set application specific Device Information
 *
 * @param   none
 *
 * @return  none
 */
static void SensorTag_setDeviceInfo(void)
{
  DevInfo_SetParameter(DEVINFO_MODEL_NUMBER, sizeof(devInfoModelNumber),
                       (void*)devInfoModelNumber);
  DevInfo_SetParameter(DEVINFO_SERIAL_NUMBER, sizeof(devInfoNA),
                       (void*)devInfoNA);
  DevInfo_SetParameter(DEVINFO_SOFTWARE_REV, sizeof(devInfoNA),
                       (void*)devInfoNA);
  DevInfo_SetParameter(DEVINFO_FIRMWARE_REV, sizeof(devInfoFirmwareRev),
                       (void*)devInfoFirmwareRev);
  DevInfo_SetParameter(DEVINFO_HARDWARE_REV, sizeof(devInfoHardwareRev),
                       (void*)devInfoHardwareRev);
  DevInfo_SetParameter(DEVINFO_MANUFACTURER_NAME, sizeof(devInfoMfrName),
                       (void*)devInfoMfrName);
}

/*******************************************************************************
 * @fn      SensorTag_processAppMsg
 *
 * @brief   Process an incoming callback from a profile.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void SensorTag_processAppMsg(stEvt_t *pMsg)
{
  switch (pMsg->event)
  {
    case ST_STATE_CHANGE_EVT:
      SensorTag_processStateChangeEvt((gaprole_States_t)pMsg->serviceID);
      break;

    case ST_CHAR_CHANGE_EVT:
      SensorTag_processCharValueChangeEvt(pMsg->serviceID, pMsg->paramID);
      break;

    default:
      // Do nothing.
      break;
  }
}

/*******************************************************************************
 * @fn      SensorTag_stateChangeCB
 *
 * @brief   Callback from GAP Role indicating a role state change.
 *
 * @param   newState - new state
 *
 * @return  none
 */
static void SensorTag_stateChangeCB(gaprole_States_t newState)
{
  SensorTag_enqueueMsg(ST_STATE_CHANGE_EVT, newState, NULL);
}

/*******************************************************************************
 * @fn      SensorTag_processStateChangeEvt
 *
 * @brief   Process a pending GAP Role state change event.
 *
 * @param   newState - new state
 *
 * @return  none
 */
static void SensorTag_processStateChangeEvt(gaprole_States_t newState)
{
#ifdef PLUS_BROADCASTER
  static bool firstConnFlag = false;
#endif // PLUS_BROADCASTER

  switch (newState)
  {
  case GAPROLE_STARTED:
    {
      uint8_t ownAddress[B_ADDR_LEN];
      uint8_t systemId[DEVINFO_SYSTEM_ID_LEN];
#ifdef IOID_GREEN_LED
      SensorTagIO_blinkLed(IOID_GREEN_LED, 5);
#endif
      GAPRole_GetParameter(GAPROLE_BD_ADDR, ownAddress);

      // use 6 bytes of device address for 8 bytes of system ID value
      systemId[0] = ownAddress[0];
      systemId[1] = ownAddress[1];
      systemId[2] = ownAddress[2];

      // set middle bytes to zero
      systemId[4] = 0x00;
      systemId[3] = 0x00;

      // shift three bytes up
      systemId[7] = ownAddress[5];
      systemId[6] = ownAddress[4];
      systemId[5] = ownAddress[3];

      DevInfo_SetParameter(DEVINFO_SYSTEM_ID, DEVINFO_SYSTEM_ID_LEN, systemId);

      AONBatMonEnable();
      SensorTag_refreshBeacon();
    }
    break;

  case GAPROLE_ADVERTISING:
    // Start the clock
    if (!Util_isActive(&periodicClock))
    {
      Util_startClock(&periodicClock);
    }

    // Make sure key presses are not stuck
    SensorTag_updateAdvertisingData(0);
    break;

  case GAPROLE_CONNECTED:
    {
      // Start the clock
      if (!Util_isActive(&periodicClock))
      {
        Util_startClock(&periodicClock);
      }

      // Turn of LEDs and buzzer
      PIN_setOutputValue(hGpioPin, Board_LED1, Board_LED_OFF);
      PIN_setOutputValue(hGpioPin, Board_LED2, Board_LED_OFF);
      PIN_setOutputValue(hGpioPin, Board_BUZZER, Board_BUZZER_OFF);

      // Update connection parameters
      SensorTagConnectionControl_update();

#ifdef PLUS_BROADCASTER
      // Only turn advertising on for this state when we first connect
      // otherwise, when we go from connected_advertising back to this state
      // we will be turning advertising back on.
      if (firstConnFlag == false)
      {
        uint8_t advertEnabled = TRUE; // Turn on Advertising

        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t),
                             &advertEnabled);
        firstConnFlag = true;
      }
#endif // PLUS_BROADCASTER
    }
    break;

  case GAPROLE_CONNECTED_ADV:
    break;

  case GAPROLE_WAITING:
  case GAPROLE_WAITING_AFTER_TIMEOUT:
    if (!advSwitching)
    {
      SensorTag_resetAllModules();
    }
    {
      uint8_t connAdv;

      // After a disconnect the role resumes connectable advertising itself
      GAPRole_GetParameter(GAPROLE_ADVERT_ENABLED, &connAdv);
      gapProfileState = newState;   // SensorTag_refreshBeacon reads it
      if (!connAdv)
      {
        if (!advSwitching)
        {
          advMode = ADV_MODE_BEACON;
        }
        advSwitching = false;
        if (advMode == ADV_MODE_CONNECTABLE)
        {
          SensorTag_startAdvertising();
        }
        else
        {
          SensorTag_refreshBeacon();
        }
      }
    }
    break;

  case GAPROLE_ERROR:
    SensorTag_resetAllModules();
    PIN_setOutputValue(hGpioPin,Board_LED1, Board_LED_ON);
    break;

  default:
    break;
  }

  gapProfileState = newState;

#ifdef INCLUDE_DISPLAY
  SensorTagDisplay_showStatus();
#endif // INCLUDE_DISPLAY
}

/*******************************************************************************
 * @fn      SensorTag_charValueChangeCB
 *
 * @brief   Callback from Sensor Profile indicating a characteristic
 *          value change.
 *
 * @param   paramID - parameter ID of the value that was changed.
 *
 * @return  none
 */
void SensorTag_charValueChangeCB(uint8_t serviceID, uint8_t paramID)
{
  SensorTag_enqueueMsg(ST_CHAR_CHANGE_EVT, serviceID, paramID);
}

/*******************************************************************************
 * @fn      SensorTag_processCharValueChangeEvt
 *
 * @brief   Process pending Profile characteristic value change
 *          events. The events are generated by the network task (BLE)
 *
 * @param   serviceID - ID of the affected service
 * @param   paramID - ID of the affected parameter
 *
 * @return  none
 */
static void SensorTag_processCharValueChangeEvt(uint8_t serviceID,
                                                uint8_t paramID)
{
  switch (serviceID)
  {
  case SERVICE_ID_TMP:
    SensorTagTmp_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_HUM:
    SensorTagHum_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_BAR:
    SensorTagBar_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_MOV:
    SensorTagMov_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_OPT:
    SensorTagOpt_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_IO:
    SensorTagIO_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_BATT:
    SensorTagBatt_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_REGISTER:
    SensorTagRegister_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_CC:
    SensorTagConnControl_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_DISPLAY:
    SensorTagDisplay_processCharChangeEvt(paramID);
    break;

  case SERVICE_ID_LIGHT:
      SensorTagLight_processCharChangeEvt(paramID);
      break;
  default:
    break;
  }
}

/*******************************************************************************
 * @fn      SensorTag_processStackMsg
 *
 * @brief   Process an incoming stack message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static void SensorTag_processStackMsg(ICall_Hdr *pMsg)
{
  switch (pMsg->event)
  {
    case GATT_MSG_EVENT:
      // Process GATT message
      SensorTag_processGATTMsg((gattMsgEvent_t *)pMsg);
      break;

    default:
      // do nothing
      break;
  }
}

/*******************************************************************************
 * @fn      SensorTag_processGATTMsg
 *
 * @brief   Process GATT messages
 *
 * @return  none
 */
static void SensorTag_processGATTMsg(gattMsgEvent_t *pMsg)
{
  GATT_bm_free(&pMsg->msg, pMsg->method);
}

/*******************************************************************************
 * @fn      SensorTag_clockHandler
 *
 * @brief   Handler function for clock time-outs.
 *
 * @param   arg - event type
 *
 * @return  none
 */
static void SensorTag_clockHandler(UArg arg)
{
  // Store the event.
  events |= arg;

  // Wake up the application.
  Semaphore_post(sem);
}

/*******************************************************************************
 * @fn      SensorTag_enqueueMsg
 *
 * @brief   Creates a message and puts the message in RTOS queue.
 *
 * @param   event - message event.
 * @param   serviceID - service identifier
 * @param   paramID - parameter identifier
 *
 * @return  none
 */
static void SensorTag_enqueueMsg(uint8_t event, uint8_t serviceID, uint8_t paramID)
{
  stEvt_t *pMsg;

  // Create dynamic pointer to message.
  if ((pMsg = ICall_malloc(sizeof(stEvt_t))))
  {
    pMsg->event = event;
    pMsg->serviceID = serviceID;
    pMsg->paramID = paramID;

    // Enqueue the message.
    Util_enqueueMsg(appMsgQueue, sem, (uint8_t*)pMsg);
  }
}


/*********************************************************************
 * @fn      SensorTag_resetAllModules
 *
 * @brief   Reset all modules, typically when a connection is terminated.
 *
 * @param   none
 *
 * @return  none
 */
static void SensorTag_resetAllModules(void)
{
  SensorTagTmp_reset();
  SensorTagHum_reset();
  SensorTagBar_reset();
  SensorTagMov_reset();
  SensorTagOpt_reset();
  SensorTagBatt_reset();
  SensorTagIO_reset();
  SensorTagRegister_reset();
  SensorTagKeys_reset();
  SensorTagLight_reset();
}

/*!*****************************************************************************
 *  @fn         SensorTag_callback
 *
 *  Interrupt service routine for buttons, relay and MPU
 *
 *  @param      handle PIN_Handle connected to the callback
 *
 *  @param      pinId  PIN_Id of the DIO triggering the callback
 *
 *  @return     none
 ******************************************************************************/
static void SensorTag_callback(PIN_Handle handle, PIN_Id pinId)
{
  switch (pinId)
  {
  case Board_KEY_LEFT:
    SensorTagKeys_processKeyLeft();
    break;

  case Board_KEY_RIGHT:
    SensorTagKeys_processKeyRight();
    break;

  case Board_RELAY:
    SensorTagKeys_processRelay();
    break;

  default:
    /* Do nothing */
    break;
  }
}


/*******************************************************************************
 * @fn      SensorTag_updateAdvertisingData
 *
 * @brief   Update the advertising data with the latest key press status
 *
 * @return  none
 */
void SensorTag_updateAdvertisingData(uint8_t keyStatus)
{
  // Key state is no longer advertised; the beacon carries readings instead
  SensorTag_pushAdvertData();
}

static void SensorTag_pushAdvertData(void)
{
  uint8_t flags = (advMode == ADV_MODE_CONNECTABLE) ? FLAG_CONNECT_MODE : 0;

  advertData[FORMAT_OFFSET] = BEACON_FORMAT | flags;
  infoData[FORMAT_OFFSET] = INFO_FORMAT | flags;
  if (infoActive)
  {
    infoData[FLAGS_OFFSET] = advertData[FLAGS_OFFSET];
    GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(infoData), infoData);
  }
  else
  {
    GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);
  }
}

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

/*******************************************************************************
 * @fn      SensorTag_leftKeyPressed
 *
 * @brief   Left button: blink red and announce the press in the beacon at once.
 */
void SensorTag_leftKeyPressed(void)
{
  uint16_t ver = FW_VERSION_X100;

  infoData[8]++;
  infoData[9] = LO_UINT16(ver);
  infoData[10] = HI_UINT16(ver);
  put32(infoData + 11, BUILD_GIT);
  infoData[15] = BUILD_DIRTY;
  infoData[16] = (BLE_SDK_VERSION >> 16) & 0xFF;
  infoData[17] = (BLE_SDK_VERSION >> 8) & 0xFF;
  infoData[18] = BLE_SDK_VERSION & 0xFF;
  put32(infoData + 19, ICALL_STACK0_START);
  infoData[23] = selfTestMap;
  infoData[24] = SysCtrlResetSourceGet();
  put32(infoData + 25, AONRTCSecGet());

  SensorTagIO_blinkLed(IOID_RED_LED, 3);
  infoActive = true;
  SensorTag_pushAdvertData();
  SensorTag_refreshBeacon();
  Util_restartClock(&infoClock, INFO_PERIOD);
}

/*******************************************************************************
 * @fn      SensorTag_beaconReading
 *
 * @brief   Called from the humidity task with fresh raw readings.
 */
void SensorTag_beaconReading(uint16_t rawTemp, uint16_t rawHum, uint16_t rawLux)
{
  beaconRaw[0] = rawTemp;
  beaconRaw[1] = rawHum;
  beaconRaw[2] = rawLux;
  events |= ST_BEACON_EVT;
  Semaphore_post(sem);
}

static void SensorTag_updateBeacon(void)
{
  uint8_t i;
  uint8_t *p = advertData + BEACON_SEQ_OFFSET;
  uint16_t batt = AONBatMonBatteryVoltageGet() & 0x7FF;

  *p++ += 1;
  for (i = 0; i < 3; i++)
  {
    *p++ = LO_UINT16(beaconRaw[i]);
    *p++ = HI_UINT16(beaconRaw[i]);
  }
  *p++ = LO_UINT16(batt);
  *p = HI_UINT16(batt);
  SensorTag_pushAdvertData();
  if (advMode == ADV_MODE_BEACON)
  {
    burstActive = true;
    Util_restartClock(&burstClock, BURST_PERIOD);
    SensorTag_refreshBeacon();
  }
}

/*******************************************************************************
 * @fn      SensorTag_connectMode
 *
 * @brief   True while the connect-mode window is open, for the sensor task's read period.
 */
bool SensorTag_connectMode(void)
{
  return advMode == ADV_MODE_CONNECTABLE;
}

static void SensorTag_setAdvInterval(uint16_t advInt)
{
  GAP_SetParamValue(TGAP_LIM_DISC_ADV_INT_MIN, advInt);
  GAP_SetParamValue(TGAP_LIM_DISC_ADV_INT_MAX, advInt);
  GAP_SetParamValue(TGAP_GEN_DISC_ADV_INT_MIN, advInt);
  GAP_SetParamValue(TGAP_GEN_DISC_ADV_INT_MAX, advInt);
  GAP_SetParamValue(TGAP_CONN_ADV_INT_MIN, advInt);
  GAP_SetParamValue(TGAP_CONN_ADV_INT_MAX, advInt);
}

static void SensorTag_startAdvertising(void)
{
  uint8_t on = TRUE;

  if (advMode == ADV_MODE_CONNECTABLE)
  {
    advertData[FLAGS_OFFSET] = DEFAULT_DISCOVERABLE_MODE
                               | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED;
    SensorTag_setAdvInterval(DEFAULT_ADVERTISING_INTERVAL);
    SensorTag_pushAdvertData();
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &on);
  }
  else
  {
    advertData[FLAGS_OFFSET] = GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED;
    beaconInterval = infoActive ? INFO_ADVERTISING_INTERVAL : BURST_ADVERTISING_INTERVAL;
    SensorTag_setAdvInterval(beaconInterval);
    SensorTag_pushAdvertData();
    GAPRole_SetParameter(GAPROLE_ADV_NONCONN_ENABLED, sizeof(uint8_t), &on);
  }
}

// Beacon mode advertises only during a burst or the info packet, at the interval that needs;
// the interval only changes on a restart, which comes back through GAPROLE_WAITING
static void SensorTag_refreshBeacon(void)
{
  uint8_t off = FALSE;
  uint16_t want = infoActive ? INFO_ADVERTISING_INTERVAL
                  : burstActive ? BURST_ADVERTISING_INTERVAL : 0;

  if (advMode != ADV_MODE_BEACON || advSwitching)
  {
    return;
  }
  if (gapProfileState == GAPROLE_ADVERTISING_NONCONN)
  {
    if (want != beaconInterval)
    {
      advSwitching = true;
      GAPRole_SetParameter(GAPROLE_ADV_NONCONN_ENABLED, sizeof(uint8_t), &off);
    }
  }
  else if (want != 0)
  {
    SensorTag_startAdvertising();
  }
}

/*******************************************************************************
 * @fn      SensorTag_toggleConnectable
 *
 * @brief   Power button: open the connectable window (OAD, TI app) or close it.
 *          The new mode starts from GAPROLE_WAITING once the old one has ended.
 */
void SensorTag_toggleConnectable(void)
{
  uint8_t off = FALSE;

  advMode = (advMode == ADV_MODE_BEACON) ? ADV_MODE_CONNECTABLE : ADV_MODE_BEACON;
  if (gapProfileState == GAPROLE_ADVERTISING)
  {
    advSwitching = true;
    GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &off);
  }
  else if (gapProfileState == GAPROLE_ADVERTISING_NONCONN)
  {
    advSwitching = true;
    GAPRole_SetParameter(GAPROLE_ADV_NONCONN_ENABLED, sizeof(uint8_t), &off);
  }
  else
  {
    SensorTag_startAdvertising();
  }
}

/*******************************************************************************
*******************************************************************************/
