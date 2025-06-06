// ----------------------------------------------------------------------------
// TMC2209.cpp
//
// Authors:
// Peter Polidoro peter@polidoro.io
// ----------------------------------------------------------------------------
#include "peripherals/TMC2209.hpp"

#include "stm32h5xx_hal.h"
#include "stm32h5xx_nucleo.h"

#include <type_traits>
#include <limits>

template<typename T, typename U = long>
T map(T x, T in_min, T in_max, T out_min, T out_max) {
	static_assert(std::is_arithmetic<T>::value, "T must be an arithmetic type");
	static_assert(std::is_integral<T>::value || std::is_floating_point<T>::value,
			"T must be an integral or floating-point type");

	// Cast to a larger type for calculations to avoid potential overflow
	U result = (static_cast<U>(x) - in_min)
			* (static_cast<U>(out_max) - out_min)
			/ (static_cast<U>(in_max) - in_min) + out_min;

	// Ensure the result is within the output range
	if (result < std::numeric_limits<T>::min()) {
		return std::numeric_limits<T>::min();
	} else if (result > std::numeric_limits<T>::max()) {
		return std::numeric_limits<T>::max();
	}

	return static_cast<T>(result);
}

TMC2209::TMC2209() {
	huart_ = nullptr;
	serial_baud_rate_ = 115200;
	serial_address_ = SERIAL_ADDRESS_0;
	hardware_enable_port_ = nullptr;
	hardware_enable_pin_ = 0;
	cool_step_enabled_ = false;
}

void TMC2209::setup(UART_HandleTypeDef *new_huart, long serial_baud_rate,
		SerialAddress serial_address) {
	initialize(new_huart, serial_baud_rate, serial_address);
}

// unidirectional methods

void TMC2209::setHardwareEnablePin(GPIO_TypeDef *port, uint16_t pin) {
	// NB: pin & port must be correctly configured elsewhere
	hardware_enable_port_ = port;
	hardware_enable_pin_ = pin;
}

void TMC2209::enable() {
	if (hardware_enable_port_ != nullptr) {
		HAL_GPIO_WritePin(hardware_enable_port_, hardware_enable_pin_,
				GPIO_PIN_SET);
	}
	chopper_config_.toff = toff_;
	writeStoredChopperConfig();
}

void TMC2209::disable() {
	if (hardware_enable_port_ != nullptr) {
		HAL_GPIO_WritePin(hardware_enable_port_, hardware_enable_pin_,
				GPIO_PIN_RESET);
	}
	chopper_config_.toff = TOFF_DISABLE;
	writeStoredChopperConfig();
}

void TMC2209::setMicrostepsPerStep(uint16_t microsteps_per_step) {
	uint16_t microsteps_per_step_shifted = constrain_(microsteps_per_step,
			MICROSTEPS_PER_STEP_MIN, MICROSTEPS_PER_STEP_MAX);
	microsteps_per_step_shifted = microsteps_per_step >> 1;
	uint16_t exponent = 0;
	while (microsteps_per_step_shifted > 0) {
		microsteps_per_step_shifted = microsteps_per_step_shifted >> 1;
		++exponent;
	}
	setMicrostepsPerStepPowerOfTwo(exponent);
}

void TMC2209::setMicrostepsPerStepPowerOfTwo(uint8_t exponent) {
	switch (exponent) {
	case 0: {
		chopper_config_.mres = MRES_001;
		break;
	}
	case 1: {
		chopper_config_.mres = MRES_002;
		break;
	}
	case 2: {
		chopper_config_.mres = MRES_004;
		break;
	}
	case 3: {
		chopper_config_.mres = MRES_008;
		break;
	}
	case 4: {
		chopper_config_.mres = MRES_016;
		break;
	}
	case 5: {
		chopper_config_.mres = MRES_032;
		break;
	}
	case 6: {
		chopper_config_.mres = MRES_064;
		break;
	}
	case 7: {
		chopper_config_.mres = MRES_128;
		break;
	}
	case 8:
	default: {
		chopper_config_.mres = MRES_256;
		break;
	}
	}
	writeStoredChopperConfig();
}

void TMC2209::setRunCurrent(uint8_t percent) {
	uint8_t run_current = percentToCurrentSetting(percent);
	driver_current_.irun = run_current;
	writeStoredDriverCurrent();
}

void TMC2209::setHoldCurrent(uint8_t percent) {
	uint8_t hold_current = percentToCurrentSetting(percent);

	driver_current_.ihold = hold_current;
	writeStoredDriverCurrent();
}

void TMC2209::setHoldDelay(uint8_t percent) {
	uint8_t hold_delay = percentToHoldDelaySetting(percent);

	driver_current_.iholddelay = hold_delay;
	writeStoredDriverCurrent();
}

void TMC2209::setAllCurrentValues(uint8_t run_current_percent,
		uint8_t hold_current_percent, uint8_t hold_delay_percent) {
	uint8_t run_current = percentToCurrentSetting(run_current_percent);
	uint8_t hold_current = percentToCurrentSetting(hold_current_percent);
	uint8_t hold_delay = percentToHoldDelaySetting(hold_delay_percent);

	driver_current_.irun = run_current;
	driver_current_.ihold = hold_current;
	driver_current_.iholddelay = hold_delay;
	writeStoredDriverCurrent();
}

void TMC2209::enableDoubleEdge() {
	chopper_config_.double_edge = DOUBLE_EDGE_ENABLE;
	writeStoredChopperConfig();
}

void TMC2209::disableDoubleEdge() {
	chopper_config_.double_edge = DOUBLE_EDGE_DISABLE;
	writeStoredChopperConfig();
}

void TMC2209::enableInverseMotorDirection() {
	global_config_.shaft = 1;
	writeStoredGlobalConfig();
}

void TMC2209::disableInverseMotorDirection() {
	global_config_.shaft = 0;
	writeStoredGlobalConfig();
}

void TMC2209::setStandstillMode(TMC2209::StandstillMode mode) {
	pwm_config_.freewheel = mode;
	writeStoredPwmConfig();
}

void TMC2209::enableAutomaticCurrentScaling() {
	pwm_config_.pwm_autoscale = STEPPER_DRIVER_FEATURE_ON;
	writeStoredPwmConfig();
}

void TMC2209::disableAutomaticCurrentScaling() {
	pwm_config_.pwm_autoscale = STEPPER_DRIVER_FEATURE_OFF;
	writeStoredPwmConfig();
}

void TMC2209::enableAutomaticGradientAdaptation() {
	pwm_config_.pwm_autograd = STEPPER_DRIVER_FEATURE_ON;
	writeStoredPwmConfig();
}

void TMC2209::disableAutomaticGradientAdaptation() {
	pwm_config_.pwm_autograd = STEPPER_DRIVER_FEATURE_OFF;
	writeStoredPwmConfig();
}

void TMC2209::setPwmOffset(uint8_t pwm_amplitude) {
	pwm_config_.pwm_offset = pwm_amplitude;
	writeStoredPwmConfig();
}

void TMC2209::setPwmGradient(uint8_t pwm_amplitude) {
	pwm_config_.pwm_grad = pwm_amplitude;
	writeStoredPwmConfig();
}

void TMC2209::setPowerDownDelay(uint8_t power_down_delay) {
	write(ADDRESS_TPOWERDOWN, power_down_delay);
}

void TMC2209::setReplyDelay(uint8_t reply_delay) {
	if (reply_delay > REPLY_DELAY_MAX) {
		reply_delay = REPLY_DELAY_MAX;
	}
	ReplyDelay reply_delay_data;
	reply_delay_data.bytes = 0;
	reply_delay_data.replydelay = reply_delay;
	write(ADDRESS_REPLYDELAY, reply_delay_data.bytes);
}

void TMC2209::moveAtVelocity(int32_t microsteps_per_period) {
	write(ADDRESS_VACTUAL, microsteps_per_period);
}

void TMC2209::moveUsingStepDirInterface() {
	write(ADDRESS_VACTUAL, VACTUAL_STEP_DIR_INTERFACE);
}

void TMC2209::enableStealthChop() {
	global_config_.enable_spread_cycle = 0;
	writeStoredGlobalConfig();
}

void TMC2209::disableStealthChop() {
	global_config_.enable_spread_cycle = 1;
	writeStoredGlobalConfig();
}

void TMC2209::setCoolStepDurationThreshold(uint32_t duration_threshold) {
	write(ADDRESS_TCOOLTHRS, duration_threshold);
}

void TMC2209::setStealthChopDurationThreshold(uint32_t duration_threshold) {
	write(ADDRESS_TPWMTHRS, duration_threshold);
}

void TMC2209::setStallGuardThreshold(uint8_t stall_guard_threshold) {
	write(ADDRESS_SGTHRS, stall_guard_threshold);
}

void TMC2209::enableCoolStep(uint8_t lower_threshold, uint8_t upper_threshold) {
	lower_threshold = constrain_(lower_threshold, SEMIN_MIN, SEMIN_MAX);
	cool_config_.semin = lower_threshold;
	upper_threshold = constrain_(upper_threshold, SEMAX_MIN, SEMAX_MAX);
	cool_config_.semax = upper_threshold;
	write(ADDRESS_COOLCONF, cool_config_.bytes);
	cool_step_enabled_ = true;
}

void TMC2209::disableCoolStep() {
	cool_config_.semin = SEMIN_OFF;
	write(ADDRESS_COOLCONF, cool_config_.bytes);
	cool_step_enabled_ = false;
}

void TMC2209::setCoolStepCurrentIncrement(CurrentIncrement current_increment) {
	cool_config_.seup = current_increment;
	write(ADDRESS_COOLCONF, cool_config_.bytes);
}

void TMC2209::setCoolStepMeasurementCount(MeasurementCount measurement_count) {
	cool_config_.sedn = measurement_count;
	write(ADDRESS_COOLCONF, cool_config_.bytes);
}

void TMC2209::enableAnalogCurrentScaling() {
	global_config_.i_scale_analog = 1;
	writeStoredGlobalConfig();
}

void TMC2209::disableAnalogCurrentScaling() {
	global_config_.i_scale_analog = 0;
	writeStoredGlobalConfig();
}

void TMC2209::useExternalSenseResistors() {
	global_config_.internal_rsense = 0;
	writeStoredGlobalConfig();
}

void TMC2209::useInternalSenseResistors() {
	global_config_.internal_rsense = 1;
	writeStoredGlobalConfig();
}

// bidirectional methods

uint8_t TMC2209::getVersion() {
	Input input;
	input.bytes = read(ADDRESS_IOIN);

	return input.version;
}

bool TMC2209::isCommunicating() {
	return (getVersion() == VERSION);
}

bool TMC2209::isSetupAndCommunicating() {
	return serialOperationMode();
}

bool TMC2209::isCommunicatingButNotSetup() {
	return (isCommunicating() && (not isSetupAndCommunicating()));
}

bool TMC2209::hardwareDisabled() {
	Input input;
	input.bytes = read(ADDRESS_IOIN);

	return input.enn;
}

uint16_t TMC2209::getMicrostepsPerStep() {
	uint16_t microsteps_per_step_exponent;
	switch (chopper_config_.mres) {
	case MRES_001: {
		microsteps_per_step_exponent = 0;
		break;
	}
	case MRES_002: {
		microsteps_per_step_exponent = 1;
		break;
	}
	case MRES_004: {
		microsteps_per_step_exponent = 2;
		break;
	}
	case MRES_008: {
		microsteps_per_step_exponent = 3;
		break;
	}
	case MRES_016: {
		microsteps_per_step_exponent = 4;
		break;
	}
	case MRES_032: {
		microsteps_per_step_exponent = 5;
		break;
	}
	case MRES_064: {
		microsteps_per_step_exponent = 6;
		break;
	}
	case MRES_128: {
		microsteps_per_step_exponent = 7;
		break;
	}
	case MRES_256:
	default: {
		microsteps_per_step_exponent = 8;
		break;
	}
	}
	return 1 << microsteps_per_step_exponent;
}

TMC2209::Settings TMC2209::getSettings() {
	Settings settings;
	settings.is_communicating = isCommunicating();

	if (settings.is_communicating) {
		readAndStoreRegisters();

		settings.is_setup = global_config_.pdn_disable;
		settings.software_enabled = (chopper_config_.toff > TOFF_DISABLE);
		settings.microsteps_per_step = getMicrostepsPerStep();
		settings.inverse_motor_direction_enabled = global_config_.shaft;
		settings.stealth_chop_enabled = not global_config_.enable_spread_cycle;
		settings.standstill_mode = pwm_config_.freewheel;
		settings.irun_percent = currentSettingToPercent(driver_current_.irun);
		settings.irun_register_value = driver_current_.irun;
		settings.ihold_percent = currentSettingToPercent(driver_current_.ihold);
		settings.ihold_register_value = driver_current_.ihold;
		settings.iholddelay_percent = holdDelaySettingToPercent(
				driver_current_.iholddelay);
		settings.iholddelay_register_value = driver_current_.iholddelay;
		settings.automatic_current_scaling_enabled = pwm_config_.pwm_autoscale;
		settings.automatic_gradient_adaptation_enabled =
				pwm_config_.pwm_autograd;
		settings.pwm_offset = pwm_config_.pwm_offset;
		settings.pwm_gradient = pwm_config_.pwm_grad;
		settings.cool_step_enabled = cool_step_enabled_;
		settings.analog_current_scaling_enabled = global_config_.i_scale_analog;
		settings.internal_sense_resistors_enabled =
				global_config_.internal_rsense;
	} else {
		settings.is_setup = false;
		settings.software_enabled = false;
		settings.microsteps_per_step = 0;
		settings.inverse_motor_direction_enabled = false;
		settings.stealth_chop_enabled = false;
		settings.standstill_mode = pwm_config_.freewheel;
		settings.irun_percent = 0;
		settings.irun_register_value = 0;
		settings.ihold_percent = 0;
		settings.ihold_register_value = 0;
		settings.iholddelay_percent = 0;
		settings.iholddelay_register_value = 0;
		settings.automatic_current_scaling_enabled = false;
		settings.automatic_gradient_adaptation_enabled = false;
		settings.pwm_offset = 0;
		settings.pwm_gradient = 0;
		settings.cool_step_enabled = false;
		settings.analog_current_scaling_enabled = false;
		settings.internal_sense_resistors_enabled = false;
	}

	return settings;
}

TMC2209::Status TMC2209::getStatus() {
	DriveStatus drive_status;
	drive_status.bytes = 0;
	drive_status.bytes = read(ADDRESS_DRV_STATUS);
	return drive_status.status;
}

TMC2209::GlobalStatus TMC2209::getGlobalStatus() {
	GlobalStatusUnion global_status_union;
	global_status_union.bytes = 0;
	global_status_union.bytes = read(ADDRESS_GSTAT);
	return global_status_union.global_status;
}

void TMC2209::clearReset() {
	GlobalStatusUnion global_status_union;
	global_status_union.bytes = 0;
	global_status_union.global_status.reset = 1;
	write(ADDRESS_GSTAT, global_status_union.bytes);
}

void TMC2209::clearDriveError() {
	GlobalStatusUnion global_status_union;
	global_status_union.bytes = 0;
	global_status_union.global_status.drv_err = 1;
	write(ADDRESS_GSTAT, global_status_union.bytes);
}

uint8_t TMC2209::getInterfaceTransmissionCounter() {
	return read(ADDRESS_IFCNT);
}

uint32_t TMC2209::getInterstepDuration() {
	return read(ADDRESS_TSTEP);
}

uint16_t TMC2209::getStallGuardResult() {
	return read(ADDRESS_SG_RESULT);
}

uint8_t TMC2209::getPwmScaleSum() {
	PwmScale pwm_scale;
	pwm_scale.bytes = read(ADDRESS_PWM_SCALE);

	return pwm_scale.pwm_scale_sum;
}

int16_t TMC2209::getPwmScaleAuto() {
	PwmScale pwm_scale;
	pwm_scale.bytes = read(ADDRESS_PWM_SCALE);

	return pwm_scale.pwm_scale_auto;
}

uint8_t TMC2209::getPwmOffsetAuto() {
	PwmAuto pwm_auto;
	pwm_auto.bytes = read(ADDRESS_PWM_AUTO);

	return pwm_auto.pwm_offset_auto;
}

uint8_t TMC2209::getPwmGradientAuto() {
	PwmAuto pwm_auto;
	pwm_auto.bytes = read(ADDRESS_PWM_AUTO);

	return pwm_auto.pwm_gradient_auto;
}

uint16_t TMC2209::getMicrostepCounter() {
	return read(ADDRESS_MSCNT);
}

// private
void TMC2209::initialize(UART_HandleTypeDef *new_huart, long serial_baud_rate,
		SerialAddress serial_address) {
	this->huart_ = new_huart;
	serial_baud_rate_ = serial_baud_rate;

	setOperationModeToSerial(serial_address);
	setRegistersToDefaults();
	clearDriveError();

	minimizeMotorCurrent();
	disable();
	disableAutomaticCurrentScaling();
	disableAutomaticGradientAdaptation();
}

void TMC2209::setOperationModeToSerial(SerialAddress serial_address) {
	serial_address_ = serial_address;

	global_config_.bytes = 0;
	global_config_.i_scale_analog = 0;
	global_config_.pdn_disable = 1;
	global_config_.mstep_reg_select = 1;
	global_config_.multistep_filt = 1;

	writeStoredGlobalConfig();
}

void TMC2209::setRegistersToDefaults() {
	driver_current_.bytes = 0;
	driver_current_.ihold = IHOLD_DEFAULT;
	driver_current_.irun = IRUN_DEFAULT;
	driver_current_.iholddelay = IHOLDDELAY_DEFAULT;
	write(ADDRESS_IHOLD_IRUN, driver_current_.bytes);

	chopper_config_.bytes = CHOPPER_CONFIG_DEFAULT;
	chopper_config_.tbl = TBL_DEFAULT;
	chopper_config_.hend = HEND_DEFAULT;
	chopper_config_.hstart = HSTART_DEFAULT;
	chopper_config_.toff = TOFF_DEFAULT;
	write(ADDRESS_CHOPCONF, chopper_config_.bytes);

	pwm_config_.bytes = PWM_CONFIG_DEFAULT;
	write(ADDRESS_PWMCONF, pwm_config_.bytes);

	cool_config_.bytes = COOLCONF_DEFAULT;
	write(ADDRESS_COOLCONF, cool_config_.bytes);

	write(ADDRESS_TPOWERDOWN, TPOWERDOWN_DEFAULT);
	write(ADDRESS_TPWMTHRS, TPWMTHRS_DEFAULT);
	write(ADDRESS_VACTUAL, VACTUAL_DEFAULT);
	write(ADDRESS_TCOOLTHRS, TCOOLTHRS_DEFAULT);
	write(ADDRESS_SGTHRS, SGTHRS_DEFAULT);
	write(ADDRESS_COOLCONF, COOLCONF_DEFAULT);
}

void TMC2209::readAndStoreRegisters() {
	global_config_.bytes = readGlobalConfigBytes();
	chopper_config_.bytes = readChopperConfigBytes();
	pwm_config_.bytes = readPwmConfigBytes();
}

bool TMC2209::serialOperationMode() {
	GlobalConfig global_config;
	global_config.bytes = readGlobalConfigBytes();

	return global_config.pdn_disable;
}

void TMC2209::minimizeMotorCurrent() {
	driver_current_.irun = CURRENT_SETTING_MIN;
	driver_current_.ihold = CURRENT_SETTING_MIN;
	writeStoredDriverCurrent();
}

uint32_t TMC2209::reverseData(uint32_t data) {
	uint32_t reversed_data = 0;
	uint8_t right_shift;
	uint8_t left_shift;
	for (uint8_t i = 0; i < DATA_SIZE; ++i) {
		right_shift = (DATA_SIZE - i - 1) * BITS_PER_BYTE;
		left_shift = i * BITS_PER_BYTE;
		reversed_data |= ((data >> right_shift) & BYTE_MAX_VALUE) << left_shift;
	}
	return reversed_data;
}

template<typename Datagram>
uint8_t TMC2209::calculateCrc(Datagram &datagram, uint8_t datagram_size) {
	uint8_t crc = 0;
	uint8_t byte;
	for (uint8_t i = 0; i < (datagram_size - 1); ++i) {
		byte = (datagram.bytes >> (i * BITS_PER_BYTE)) & BYTE_MAX_VALUE;
		for (uint8_t j = 0; j < BITS_PER_BYTE; ++j) {
			if ((crc >> 7) ^ (byte & 0x01)) {
				crc = (crc << 1) ^ 0x07;
			} else {
				crc = crc << 1;
			}
			byte = byte >> 1;
		}
	}
	return crc;
}

template<typename Datagram>
void TMC2209::sendDatagramUnidirectional(Datagram &datagram,
		uint8_t datagram_size) {
	uint8_t byte;

//	HAL_HalfDuplex_EnableTransmitter(huart_);
	for (uint8_t i = 0; i < datagram_size; ++i) {
		byte = (datagram.bytes >> (i * BITS_PER_BYTE)) & BYTE_MAX_VALUE;
		auto status = HAL_UART_Transmit(huart_, &byte, 1, 1000);
		if (status != HAL_OK) {
			BSP_LED_On(LED_GREEN);
		}
	}
}

template<typename Datagram>
void TMC2209::sendDatagramBidirectional(Datagram &datagram,
		uint8_t datagram_size) {

//	HAL_HalfDuplex_EnableReceiver(huart_);

	// clear the serial receive buffer if necessary
	while (__HAL_UART_GET_FLAG(huart_, UART_FLAG_RXNE)) {
		// Read the received data to clear the RXNE flag
		uint8_t dummy;
		HAL_UART_Receive(huart_, &dummy, 1, 0);
	}

//	HAL_HalfDuplex_EnableTransmitter(huart_);

	// write datagram
	for (uint8_t i = 0; i < datagram_size; ++i) {
		uint8_t byte = (datagram.bytes >> (i * BITS_PER_BYTE)) & BYTE_MAX_VALUE;
		auto status = HAL_UART_Transmit(huart_, &byte, 1, 100);
		if (status != HAL_OK) {
			HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_SET);
			return;
		}
	}

//	HAL_HalfDuplex_EnableReceiver(huart_);

	// wait for bytes sent out on TX line to be echoed on RX line
	uint8_t echo_buf[datagram_size];
	auto reply_status = HAL_UART_Receive(huart_, echo_buf, datagram_size,
			ECHO_DELAY_MAX_MICROSECONDS / 1000);

	if (reply_status != HAL_OK) {
		return;
	}
}

void TMC2209::write(uint8_t register_address, uint32_t data) {
	WriteReadReplyDatagram write_datagram;
	write_datagram.bytes = 0;
	write_datagram.sync = SYNC;
	write_datagram.serial_address = serial_address_;
	write_datagram.register_address = register_address;
	write_datagram.rw = RW_WRITE;
	write_datagram.data = reverseData(data);
	write_datagram.crc = calculateCrc(write_datagram,
			WRITE_READ_REPLY_DATAGRAM_SIZE);

	sendDatagramUnidirectional(write_datagram, WRITE_READ_REPLY_DATAGRAM_SIZE);
}

uint32_t TMC2209::read(uint8_t register_address) {
	ReadRequestDatagram read_request_datagram;
	read_request_datagram.bytes = 0;
	read_request_datagram.sync = SYNC;
	read_request_datagram.serial_address = serial_address_;
	read_request_datagram.register_address = register_address;
	read_request_datagram.rw = RW_READ;
	read_request_datagram.crc = calculateCrc(read_request_datagram,
			READ_REQUEST_DATAGRAM_SIZE);

	sendDatagramBidirectional(read_request_datagram,
			READ_REQUEST_DATAGRAM_SIZE);

//	HAL_HalfDuplex_EnableReceiver(huart_);

	uint8_t reply_buf[WRITE_READ_REPLY_DATAGRAM_SIZE];
	auto reply_status = HAL_UART_Receive(huart_, reply_buf, WRITE_READ_REPLY_DATAGRAM_SIZE, REPLY_DELAY_MAX_MICROSECONDS / 1000);
	if (reply_status != HAL_OK) {
		return 0;
	}

	uint64_t byte;
	uint8_t byte_count = 0;
	WriteReadReplyDatagram read_reply_datagram;
	read_reply_datagram.bytes = 0;
	for (uint8_t i = 0; i < WRITE_READ_REPLY_DATAGRAM_SIZE; ++i) {
		byte = reply_buf[i];
		read_reply_datagram.bytes |= (byte << (byte_count++ * BITS_PER_BYTE));
	}

	return reverseData(read_reply_datagram.data);
}

uint8_t TMC2209::percentToCurrentSetting(uint8_t percent) {
	uint8_t constrained_percent = constrain_(percent, PERCENT_MIN, PERCENT_MAX);
	uint8_t current_setting = map(constrained_percent, PERCENT_MIN, PERCENT_MAX,
			CURRENT_SETTING_MIN, CURRENT_SETTING_MAX);
	return current_setting;
}

uint8_t TMC2209::currentSettingToPercent(uint8_t current_setting) {
	uint8_t percent = map(current_setting, CURRENT_SETTING_MIN,
			CURRENT_SETTING_MAX, PERCENT_MIN, PERCENT_MAX);
	return percent;
}

uint8_t TMC2209::percentToHoldDelaySetting(uint8_t percent) {
	uint8_t constrained_percent = constrain_(percent, PERCENT_MIN, PERCENT_MAX);
	uint8_t hold_delay_setting = map(constrained_percent, PERCENT_MIN,
			PERCENT_MAX, HOLD_DELAY_MIN, HOLD_DELAY_MAX);
	return hold_delay_setting;
}

uint8_t TMC2209::holdDelaySettingToPercent(uint8_t hold_delay_setting) {
	uint8_t percent = map(hold_delay_setting, HOLD_DELAY_MIN, HOLD_DELAY_MAX,
			PERCENT_MIN, PERCENT_MAX);
	return percent;
}

void TMC2209::writeStoredGlobalConfig() {
	write(ADDRESS_GCONF, global_config_.bytes);
}

uint32_t TMC2209::readGlobalConfigBytes() {
	return read(ADDRESS_GCONF);
}

void TMC2209::writeStoredDriverCurrent() {
	write(ADDRESS_IHOLD_IRUN, driver_current_.bytes);

	if (driver_current_.irun >= SEIMIN_UPPER_CURRENT_LIMIT) {
		cool_config_.seimin = SEIMIN_UPPER_SETTING;
	} else {
		cool_config_.seimin = SEIMIN_LOWER_SETTING;
	}
	if (cool_step_enabled_) {
		write(ADDRESS_COOLCONF, cool_config_.bytes);
	}
}

void TMC2209::writeStoredChopperConfig() {
	write(ADDRESS_CHOPCONF, chopper_config_.bytes);
}

uint32_t TMC2209::readChopperConfigBytes() {
	return read(ADDRESS_CHOPCONF);
}

void TMC2209::writeStoredPwmConfig() {
	write(ADDRESS_PWMCONF, pwm_config_.bytes);
}

uint32_t TMC2209::readPwmConfigBytes() {
	return read(ADDRESS_PWMCONF);
}

uint32_t TMC2209::constrain_(uint32_t value, uint32_t low, uint32_t high) {
	return ((value) < (low) ? (low) : ((value) > (high) ? (high) : (value)));
}
