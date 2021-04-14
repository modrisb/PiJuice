/*
 * power_management.c
 *
 *  Created on: 04.04.2017.
 *      Author: milan
 */

#include "main.h"
#include "system_conf.h"

#include "nv.h"

#include "charger_bq2416x.h"
#include "fuel_gauge_lc709203f.h"
#include "time_count.h"
#include "power_source.h"
#include "button.h"
#include "io_control.h"
#include "led.h"
#include "iodrv.h"
#include "hostcomms.h"
#include "execution.h"
#include "rtc_ds1339_emu.h"
#include "taskman.h"
#include "util.h"

#include "power_management.h"

static bool POWERMAN_ResetHost(void);

#define WAKEUPONCHARGE_NV_INITIALISED	(0u != (m_wakeupOnChargeConfig & 0x80u))

static uint32_t m_powerMngmtTaskMsCounter;
static RunPinInstallationStatus_T m_runPinInstalled = RUN_PIN_NOT_INSTALLED;

static uint8_t m_wakeupOnChargeConfig __attribute__((section("no_init")));
static uint16_t m_wakeUpOnCharge __attribute__((section("no_init"))); // 0 - 1000, 0xFFFF - disabled
static bool m_delayedTurnOnFlag __attribute__((section("no_init")));
static uint32_t m_delayedTurnOnTimer __attribute__((section("no_init")));
static uint32_t m_delayedPowerOffTimeMs __attribute__((section("no_init")));
static uint16_t m_watchdogConfig __attribute__((section("no_init")));
static uint32_t m_watchdogExpirePeriod __attribute__((section("no_init"))); // 0 - disabled, 1-255 expiration time minutes
static uint32_t m_watchdogTimer __attribute__((section("no_init")));
static bool m_watchdogExpired __attribute__((section("no_init")));
static bool m_powerButtonPressedEvent __attribute__((section("no_init")));
static uint32_t m_lastWakeupTimer __attribute__((section("no_init")));


void POWERMAN_Init(void)
{
	const uint32_t sysTime = HAL_GetTick();
	const bool noBatteryTurnOn = CHARGER_GetNoBatteryTurnOnEnable();
	const bool chargerHasInput = CHARGER_IsChargeSourceAvailable();
	uint8_t tempU8;
	bool nvOk;

	// If not programmed will default to not installed
	NV_ReadVariable_U8(NV_RUN_PIN_CONFIG, (uint8_t*)&m_runPinInstalled);

	if (EXECUTION_STATE_NORMAL != executionState)
	{
		// on mcu power up
		m_wakeUpOnCharge = WAKEUP_ONCHARGE_DISABLED_VAL;
		m_wakeupOnChargeConfig = 0x7Fu;

		if (NV_ReadVariable_U8(WAKEUPONCHARGE_CONFIG_NV_ADDR, (uint8_t*)&m_wakeupOnChargeConfig))
		{
			if (m_wakeupOnChargeConfig <= 100u)
			{
				/* Set last bit in config value to show EE value is valid. */
				m_wakeupOnChargeConfig |= 0x80u;
			}
		}

		if (true == chargerHasInput)
		{	/* Charger is connected */
			m_delayedTurnOnFlag = noBatteryTurnOn;
		}
		else
		{	/* Charger is not connected */
			m_delayedTurnOnFlag = false;

			if (WAKEUPONCHARGE_NV_INITIALISED)
			{
				m_wakeUpOnCharge = (m_wakeupOnChargeConfig & 0x7Fu) <= 100u ?
										(m_wakeupOnChargeConfig & 0x7Fu) * 10u :
										WAKEUP_ONCHARGE_DISABLED_VAL;
			}
		}

		nvOk = true;

		nvOk &= NV_ReadVariable_U8(WATCHDOG_CONFIGL_NV_ADDR, &tempU8);
		m_watchdogConfig = tempU8;

		nvOk &= NV_ReadVariable_U8(WATCHDOG_CONFIGH_NV_ADDR, &tempU8);

		if (true == nvOk)
		{
			m_watchdogConfig |= (uint16_t)tempU8 << 8u;
		}
		else
		{
			m_watchdogConfig = 0u;
		}


		m_delayedPowerOffTimeMs = 0u;
		m_watchdogExpirePeriod = 0u;
		m_watchdogTimer = 0u;
		m_watchdogExpired = false;

		RTC_ClearWakeEvent();
		TASKMAN_ClearIOWakeEvent();
		m_powerButtonPressedEvent = false;
	}

	MS_TIMEREF_INIT(m_powerMngmtTaskMsCounter, sysTime);
}


void BUTTON_PowerOnEventCb(const Button_T * const p_button)
{
	const uint32_t sysTime = HAL_GetTick();

	const bool boostConverterEnabled = POWERSOURCE_IsBoostConverterEnabled();
	const PowerSourceStatus_T power5vIoStatus = POWERSOURCE_Get5VRailStatus();
	const uint32_t lastHostCommandAgeMs = HOSTCOMMS_GetLastCommandAgeMs(sysTime);

	// TODO - Check brackets are in the right places
	if ( ( (false == boostConverterEnabled) && (POW_SOURCE_NOT_PRESENT == power5vIoStatus) ) ||
			( MS_TIMEREF_TIMEOUT(m_lastWakeupTimer, sysTime, 12000u) && (lastHostCommandAgeMs > 11000) )
			)
	{

		if (true == POWERMAN_ResetHost())
		{
			m_wakeUpOnCharge = WAKEUP_ONCHARGE_DISABLED_VAL;
			RTC_ClearWakeEvent();
			TASKMAN_ClearIOWakeEvent();
			m_delayedPowerOffTimeMs = 0u;
		}
	}

	BUTTON_ClearEvent(p_button->index);

}


void BUTTON_PowerOffEventCb(const Button_T * const p_button)
{
	const bool boostConverterEnabled = POWERSOURCE_IsBoostConverterEnabled();
	const PowerSourceStatus_T power5vIoStatus = POWERSOURCE_Get5VRailStatus();

	if ( (true == boostConverterEnabled) && (POW_SOURCE_NOT_PRESENT == power5vIoStatus) )
	{
		POWERSOURCE_Set5vBoostEnable(false);
		m_powerButtonPressedEvent = true;
	}

	BUTTON_ClearEvent(p_button->index); // remove event
}


void BUTTON_PowerResetEventCb(const Button_T * const p_button)
{
	if (true == POWERMAN_ResetHost())
	{
		m_wakeUpOnCharge = WAKEUP_ONCHARGE_DISABLED_VAL;

		RTC_ClearWakeEvent();
		TASKMAN_ClearIOWakeEvent();

		m_delayedPowerOffTimeMs = 0u;
	}

	BUTTON_ClearEvent(p_button->index);
}


void POWERMAN_Task(void)
{
	const uint32_t sysTime = HAL_GetTick();
	const bool chargerHasInput = CHARGER_IsChargeSourceAvailable();
	const bool chargerHasBattery = (CHARGER_BATTERY_NOT_PRESENT != CHARGER_GetBatteryStatus());
	const uint16_t batteryRsoc = FUELGAUGE_GetSocPt1();
	const POWERSOURCE_RPi5VStatus_t pow5vInDetStatus = POWERSOURCE_GetRPi5VPowerStatus();
	const uint32_t lastHostCommandAgeMs = HOSTCOMMS_GetLastCommandAgeMs(sysTime);
	const bool rtcWakeEvent = RTC_GetWakeEvent();
	const bool ioWakeEvent = TASKMAN_GetIOWakeEvent();

	bool boostConverterEnabled;
	bool isWakeupOnCharge;

	if (MS_TIMEREF_TIMEOUT(m_powerMngmtTaskMsCounter, sysTime, POWERMANAGE_TASK_PERIOD_MS))
	{
		MS_TIMEREF_INIT(m_powerMngmtTaskMsCounter, sysTime);

		isWakeupOnCharge = (batteryRsoc >= m_wakeUpOnCharge) && (true == chargerHasInput) && (true == chargerHasBattery);

		if ( ( isWakeupOnCharge || rtcWakeEvent || ioWakeEvent ) // there is wake-up trigger
				&& 	(0u == m_delayedPowerOffTimeMs) // deny wake-up during shutdown
				&& 	(false == m_delayedTurnOnFlag)
				&& 	( (lastHostCommandAgeMs >= 15000u) && MS_TIMEREF_TIMEOUT(m_lastWakeupTimer, sysTime, 30000u) )
						//|| (!POW_5V_BOOST_EN_STATUS() && power5vIoStatus == POW_SOURCE_NOT_PRESENT) //  Host is non powered
		   	   	)
		{
			if (true == POWERMAN_ResetHost())
			{
				m_wakeUpOnCharge = WAKEUP_ONCHARGE_DISABLED_VAL;
				RTC_ClearWakeEvent();
				TASKMAN_ClearIOWakeEvent();
				m_delayedPowerOffTimeMs = 0u;

				if (0u != m_watchdogConfig)
				{
					// activate watchdog after wake-up if watchdog config has restore flag
					m_watchdogExpirePeriod = (uint32_t)m_watchdogConfig * 60000u;
					m_watchdogTimer = m_watchdogExpirePeriod;
				}
			}
		}


		if ( (0u !=  m_watchdogExpirePeriod) && (lastHostCommandAgeMs > m_watchdogTimer) )
		{
			if (true == POWERMAN_ResetHost())
			{
				m_wakeUpOnCharge = WAKEUP_ONCHARGE_DISABLED_VAL;
				m_watchdogExpired = true;
				RTC_ClearWakeEvent();
				TASKMAN_ClearIOWakeEvent();
				m_delayedPowerOffTimeMs = 0u;
			}

			m_watchdogTimer += m_watchdogExpirePeriod;
		}
	}

	if ( m_delayedTurnOnFlag && (MS_TIMEREF_TIMEOUT(m_delayedTurnOnTimer, sysTime, 100u)) )
	{
		POWERSOURCE_Set5vBoostEnable(true);
		m_delayedTurnOnFlag = 0u;
		MS_TIME_COUNTER_INIT(m_lastWakeupTimer);
	}

	boostConverterEnabled = POWERSOURCE_IsBoostConverterEnabled();

	// Time is set in the future, so need to work in int32 or it'll roll over
	if ( (0u != m_delayedPowerOffTimeMs) && (int32_t)MS_TIMEREF_DIFF(m_delayedPowerOffTimeMs, sysTime) > 0)
	{
		// TODO - Check what happens with unknown status,
		// maybe don't even bother checking as the thing will just be powered or not.
		if ((true == boostConverterEnabled) && (RPI5V_DETECTION_STATUS_POWERED != pow5vInDetStatus))
		{
			POWERSOURCE_Set5vBoostEnable(false);
		}

		// Disable timer
		m_delayedPowerOffTimeMs = 0u;

		/* Turn off led as it keeps flashing! */
		LED_SetRGB(LED_LED2_IDX, 0u, 0u, 0u);
	}

	if ( (false == chargerHasInput) &&
			(WAKEUP_ONCHARGE_DISABLED_VAL == m_wakeUpOnCharge) &&
			(WAKEUPONCHARGE_NV_INITIALISED)
			)
	{
		// setup wake-up on charge if charging stopped, power source removed
		m_wakeUpOnCharge = (m_wakeupOnChargeConfig & 0x7F) <= 100u ? (m_wakeupOnChargeConfig & 0x7Fu) * 10u :
																	WAKEUP_ONCHARGE_DISABLED_VAL;
	}
}


void InputSourcePresenceChangeCb(uint8_t event)
{

}


void POWERMAN_SchedulePowerOff(const uint8_t delayCode)
{
	if (delayCode <= 250u)
	{
		MS_TIMEREF_INIT(m_delayedPowerOffTimeMs, HAL_GetTick() + (delayCode * 1024u));

		if (m_delayedPowerOffTimeMs == 0u)
		{
			m_delayedPowerOffTimeMs = 1u; // 0 is used to indicate non active counter, so avoid that value.
		}
	}
	else if (delayCode == 0xFFu)
	{
		m_delayedPowerOffTimeMs = 0u; // deactivate scheduled power off
	}
}


uint8_t POWERMAN_GetPowerOffTime(void)
{
	const uint32_t sysTime = HAL_GetTick();
	const uint32_t timeLeft = MS_TIMEREF_DIFF(m_delayedPowerOffTimeMs, sysTime);

	if (0u != m_delayedPowerOffTimeMs)
	{
		if (timeLeft < (UINT8_MAX * 1024u))
		{
			return (uint8_t)(timeLeft / 1024u);
		}
		else
		{
			return 0u;
		}
	}
	else
	{
		return 0xFFu;
	}
}


bool POWERMAN_GetPowerButtonPressedStatus(void)
{
	return m_powerButtonPressedEvent;
}


bool POWERMAN_GetWatchdogExpired(void)
{
	return m_watchdogExpired;
}


void POWERMAN_ClearWatchdog(void)
{
	m_watchdogExpired = false;
}


void POWERMAN_SetRunPinConfigData(const uint8_t * const p_data, const uint8_t len)
{
	if (p_data[0u] > 1u)
	{
		return;
	}

	NV_WriteVariable_U8(NV_RUN_PIN_CONFIG, p_data[0u]);

	if (false == NV_ReadVariable_U8(NV_RUN_PIN_CONFIG, (uint8_t*)&m_runPinInstalled))
	{
		m_runPinInstalled = RUN_PIN_NOT_INSTALLED;
	}
}


void POWERMAN_GetRunPinConfigData(uint8_t * const p_data, uint16_t * const p_len)
{
	p_data[0] = m_runPinInstalled;
	*p_len = 1u;
}


void POWERMAN_SetWatchdogConfigData(const uint8_t * const p_data, const uint16_t len)
{
	uint32_t cfg;
	uint8_t tempU8;
	bool nvOk;

	if (len < 2u)
	{
		return;
	}

	cfg = UTIL_FromBytes_U16(&p_data[0u]) & 0x3FFFu;

	// Drop resolution if time is >= 16384 seconds
	if (0u != (p_data[1u] & 0x40u))
	{
		cfg >>= 2u;
		cfg |= 0x4000u;
	}

	if (p_data[1u] & 0x80)
	{
		m_watchdogConfig = cfg;
		NV_WriteVariable_U8(WATCHDOG_CONFIGL_NV_ADDR, (uint8_t)(m_watchdogConfig & 0xFFu));
		NV_WriteVariable_U8(WATCHDOG_CONFIGH_NV_ADDR, (uint8_t)((m_watchdogConfig >> 8u) & 0xFFu));

		nvOk = true;

		nvOk &= NV_ReadVariable_U8(WATCHDOG_CONFIGL_NV_ADDR, &tempU8);
		m_watchdogConfig = tempU8;

		nvOk &= NV_ReadVariable_U8(WATCHDOG_CONFIGH_NV_ADDR, &tempU8);

		if (false == nvOk)
		{
			m_watchdogConfig = 0u;
		}

		if (0u == m_watchdogConfig)
		{
			m_watchdogExpirePeriod = 0u;
			m_watchdogTimer = 0u;
		}
	}
	else
	{
		m_watchdogExpirePeriod = cfg * 60000u;
		m_watchdogTimer = m_watchdogExpirePeriod;
	}

}

void POWERMAN_GetWatchdogConfigData(uint8_t * const p_data, uint16_t * const p_len)
{
	uint16_t d;

	if (0u != m_watchdogConfig)
	{
		d = m_watchdogConfig;

		if (0u != (d & 0x4000u))
		{
			d = (d >> 2u) | 0x4000u;
		}

		d |= 0x8000u;
	}
	else
	{
		d = m_watchdogExpirePeriod / 60000u;

		if (0u != (d & 0x4000u))
		{
			 d = (d >> 2u) | 0x4000u;
		}
	}

	UTIL_ToBytes_U16(d, &p_data[0u]);

	*p_len = 2;
}


void POWERMAN_SetWakeupOnChargeData(const uint8_t * const p_data, const uint16_t len)
{
	uint16_t newWakeupVal = (p_data[0u] & 0x7Fu) * 10u;

	if (newWakeupVal > 1000u)
	{
		newWakeupVal = WAKEUP_ONCHARGE_DISABLED_VAL;
	}

	if (p_data[0u] & 0x80u)
	{
		NV_WriteVariable_U8(WAKEUPONCHARGE_CONFIG_NV_ADDR, (p_data[0u] & 0x7Fu) <= 100u ? p_data[0u] : 0x7Fu);

		if (NV_WriteVariable_U8(WAKEUPONCHARGE_CONFIG_NV_ADDR, m_wakeupOnChargeConfig) != NV_READ_VARIABLE_SUCCESS )
		{
			/* If writing to NV failed then disable wakeup on charge */
			m_wakeupOnChargeConfig = 0x7Fu;
			newWakeupVal = WAKEUP_ONCHARGE_DISABLED_VAL;
		}
		else
		{
			m_wakeupOnChargeConfig |= 0x80u;
		}
	}

	m_wakeUpOnCharge = newWakeupVal;
}


void POWERMAN_GetWakeupOnChargeData(uint8_t * const p_data, uint16_t * const p_len)
{
	if (WAKEUPONCHARGE_NV_INITIALISED)
	{
		p_data[0u] = m_wakeupOnChargeConfig;
	}
	else
	{
		p_data[0u] = (m_wakeUpOnCharge <= 1000) ? m_wakeUpOnCharge / 10u : 100u;
	}

	*p_len = 1u;
}


bool POWERMAN_CanShutDown(void)
{
	const uint32_t sysTime = HAL_GetTick();

	// Make sure pijuice does not sleep if starting up or just about to shutdown
	return (MS_TIMEREF_TIMEOUT(m_lastWakeupTimer, sysTime, 5000u)) && (m_delayedPowerOffTimeMs == 0u);
}


void POWERMAN_ClearPowerButtonPressed(void)
{
	m_powerButtonPressedEvent = false;
}


void POWERMAN_SetWakeupOnChargePt1(const uint16_t newValue)
{
	m_wakeUpOnCharge = newValue;
}


static bool POWERMAN_ResetHost(void)
{
	const bool boostConverterEnabled = POWERSOURCE_IsBoostConverterEnabled();
	const PowerSourceStatus_T power5vIoStatus = POWERSOURCE_Get5VRailStatus();

	if ( ((true == boostConverterEnabled) || (POW_SOURCE_NOT_PRESENT != power5vIoStatus))
			&& (RUN_PIN_INSTALLED == m_runPinInstalled)
			)
	{
		POWERSOURCE_Set5vBoostEnable(true);
		// activate RUN signal
		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET);
		DelayUs(100);
		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET);
		MS_TIME_COUNTER_INIT(m_lastWakeupTimer);
		return true;
	}
	else if (power5vIoStatus == POW_SOURCE_NOT_PRESENT)
	{
		if (true == boostConverterEnabled)
		{
			// do power circle, first turn power off
			POWERSOURCE_Set5vBoostEnable(false);
			// schedule turn on after delay
			m_delayedTurnOnFlag = 1;
			MS_TIME_COUNTER_INIT(m_delayedTurnOnTimer);
			MS_TIME_COUNTER_INIT(m_lastWakeupTimer);
			return true;
		}
		else
		{
			POWERSOURCE_Set5vBoostEnable(true);
			MS_TIME_COUNTER_INIT(m_lastWakeupTimer);
			return true;
		}
	}
	else if ( (true == boostConverterEnabled) || (POW_SOURCE_NOT_PRESENT != power5vIoStatus) )
	{
		// wakeup via RPI GPIO3
	    GPIO_InitTypeDef i2c_GPIO_InitStruct;
		i2c_GPIO_InitStruct.Pin = GPIO_PIN_6;
		i2c_GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_OD;
		i2c_GPIO_InitStruct.Pull = GPIO_NOPULL;
	    HAL_GPIO_Init(GPIOB, &i2c_GPIO_InitStruct);
	    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_RESET);
		DelayUs(100);
		HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_SET);
		i2c_GPIO_InitStruct.Pin       = GPIO_PIN_6;
		i2c_GPIO_InitStruct.Mode      = GPIO_MODE_AF_OD;
		i2c_GPIO_InitStruct.Pull      = GPIO_NOPULL;
		i2c_GPIO_InitStruct.Speed     = GPIO_SPEED_FREQ_HIGH;
		i2c_GPIO_InitStruct.Alternate = GPIO_AF1_I2C1;
		HAL_GPIO_Init(GPIOB, &i2c_GPIO_InitStruct);
		MS_TIME_COUNTER_INIT(m_lastWakeupTimer);

		return true;
	}

	return false;
}
