/****************************************************************************
 *
 *   Copyright (c) 2012-2015 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file gyrosim.cpp
 *
 * Driver for the simulated gyro 
 *
 * @author Andrew Tridgell
 * @author Pat Hickey
 * @author Mark Charlebois
 */

#include <px4_config.h>
#include <px4_getopt.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <semaphore.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>

#include <simulator/simulator.h>

#include <systemlib/perf_counter.h>
#include <systemlib/err.h>
#include <systemlib/conversions.h>

#include <board_config.h>
#include <drivers/drv_hrt.h>

#include <drivers/device/device.h>
#include <drivers/device/ringbuffer.h>
#include <drivers/drv_accel.h>
#include <drivers/drv_gyro.h>
#include <mathlib/math/filter/LowPassFilter2p.hpp>
#include <lib/conversion/rotation.h>

#define DIR_READ			0x80
#define DIR_WRITE			0x00

#define MPU_DEVICE_PATH_ACCEL		"/dev/gyrosim_accel"
#define MPU_DEVICE_PATH_GYRO		"/dev/gyrosim_gyro"

// MPU 6000 registers
#define MPUREG_WHOAMI			0x75
#define MPUREG_SMPLRT_DIV		0x19
#define MPUREG_CONFIG			0x1A
#define MPUREG_GYRO_CONFIG		0x1B
#define MPUREG_ACCEL_CONFIG		0x1C
#define MPUREG_FIFO_EN			0x23
#define MPUREG_INT_PIN_CFG		0x37
#define MPUREG_INT_ENABLE		0x38
#define MPUREG_INT_STATUS		0x3A
#define MPUREG_ACCEL_XOUT_H		0x3B
#define MPUREG_ACCEL_XOUT_L		0x3C
#define MPUREG_ACCEL_YOUT_H		0x3D
#define MPUREG_ACCEL_YOUT_L		0x3E
#define MPUREG_ACCEL_ZOUT_H		0x3F
#define MPUREG_ACCEL_ZOUT_L		0x40
#define MPUREG_TEMP_OUT_H		0x41
#define MPUREG_TEMP_OUT_L		0x42
#define MPUREG_GYRO_XOUT_H		0x43
#define MPUREG_GYRO_XOUT_L		0x44
#define MPUREG_GYRO_YOUT_H		0x45
#define MPUREG_GYRO_YOUT_L		0x46
#define MPUREG_GYRO_ZOUT_H		0x47
#define MPUREG_GYRO_ZOUT_L		0x48
#define MPUREG_USER_CTRL		0x6A
#define MPUREG_PWR_MGMT_1		0x6B
#define MPUREG_PWR_MGMT_2		0x6C
#define MPUREG_FIFO_COUNTH		0x72
#define MPUREG_FIFO_COUNTL		0x73
#define MPUREG_FIFO_R_W			0x74
#define MPUREG_PRODUCT_ID		0x0C
#define MPUREG_TRIM1			0x0D
#define MPUREG_TRIM2			0x0E
#define MPUREG_TRIM3			0x0F
#define MPUREG_TRIM4			0x10

// Configuration bits MPU 3000 and MPU 6000 (not revised)?
#define BIT_SLEEP			0x40
#define BIT_H_RESET			0x80
#define BITS_CLKSEL			0x07
#define MPU_CLK_SEL_PLLGYROX		0x01
#define MPU_CLK_SEL_PLLGYROZ		0x03
#define MPU_EXT_SYNC_GYROX		0x02
#define BITS_GYRO_ST_X			0x80
#define BITS_GYRO_ST_Y			0x40
#define BITS_GYRO_ST_Z			0x20
#define BITS_FS_250DPS			0x00
#define BITS_FS_500DPS			0x08
#define BITS_FS_1000DPS			0x10
#define BITS_FS_2000DPS			0x18
#define BITS_FS_MASK			0x18
#define BITS_DLPF_CFG_256HZ_NOLPF2	0x00
#define BITS_DLPF_CFG_188HZ		0x01
#define BITS_DLPF_CFG_98HZ		0x02
#define BITS_DLPF_CFG_42HZ		0x03
#define BITS_DLPF_CFG_20HZ		0x04
#define BITS_DLPF_CFG_10HZ		0x05
#define BITS_DLPF_CFG_5HZ		0x06
#define BITS_DLPF_CFG_2100HZ_NOLPF	0x07
#define BITS_DLPF_CFG_MASK		0x07
#define BIT_INT_ANYRD_2CLEAR		0x10
#define BIT_RAW_RDY_EN			0x01
#define BIT_I2C_IF_DIS			0x10
#define BIT_INT_STATUS_DATA		0x01

// Product ID Description for GYROSIM
// high 4 bits 	low 4 bits
// Product Name	Product Revision
#define GYROSIMES_REV_C4		0x14
#define GYROSIMES_REV_C5		0x15
#define GYROSIMES_REV_D6		0x16
#define GYROSIMES_REV_D7		0x17
#define GYROSIMES_REV_D8		0x18
#define GYROSIM_REV_C4			0x54
#define GYROSIM_REV_C5			0x55
#define GYROSIM_REV_D6			0x56
#define GYROSIM_REV_D7			0x57
#define GYROSIM_REV_D8			0x58
#define GYROSIM_REV_D9			0x59
#define GYROSIM_REV_D10			0x5A

#define GYROSIM_ACCEL_DEFAULT_RANGE_G			8
#define GYROSIM_ACCEL_DEFAULT_RATE			1000
#define GYROSIM_ACCEL_DEFAULT_DRIVER_FILTER_FREQ	30

#define GYROSIM_GYRO_DEFAULT_RANGE_G			8
#define GYROSIM_GYRO_DEFAULT_RATE			1000
#define GYROSIM_GYRO_DEFAULT_DRIVER_FILTER_FREQ		30

#define GYROSIM_DEFAULT_ONCHIP_FILTER_FREQ		42

#define GYROSIM_ONE_G					9.80665f

#ifdef PX4_SPI_BUS_EXT
#define EXTERNAL_BUS PX4_SPI_BUS_EXT
#else
#define EXTERNAL_BUS 0
#endif

/*
  the GYROSIM can only handle high SPI bus speeds on the sensor and
  interrupt status registers. All other registers have a maximum 1MHz
  SPI speed
 */
#define GYROSIM_LOW_BUS_SPEED				1000*1000
#define GYROSIM_HIGH_BUS_SPEED				11*1000*1000 /* will be rounded to 10.4 MHz, within margins for MPU6K */

class GYROSIM_gyro;

class GYROSIM : public device::VDev
{
public:
	GYROSIM(const char *path_accel, const char *path_gyro, enum Rotation rotation);
	virtual ~GYROSIM();

	virtual int		init();

	virtual ssize_t		read(device::file_t *filp, char *buffer, size_t buflen);
	virtual int		ioctl(device::file_t *filp, int cmd, unsigned long arg);
	int			transfer(uint8_t *send, uint8_t *recv, unsigned len);

	/**
	 * Diagnostics - print some basic information about the driver.
	 */
	void			print_info();

	void			print_registers();

protected:
	friend class GYROSIM_gyro;

	virtual ssize_t		gyro_read(device::file_t *filp, char *buffer, size_t buflen);
	virtual int		gyro_ioctl(device::file_t *filp, int cmd, unsigned long arg);

private:
	GYROSIM_gyro		*_gyro;
	uint8_t			_product;	/** product code */

	struct hrt_call		_call;
	unsigned		_call_interval;

	ringbuffer::RingBuffer	*_accel_reports;

	struct accel_scale	_accel_scale;
	float			_accel_range_scale;
	float			_accel_range_m_s2;
	orb_advert_t		_accel_topic;
	int			_accel_orb_class_instance;
	int			_accel_class_instance;

	ringbuffer::RingBuffer	*_gyro_reports;

	struct gyro_scale	_gyro_scale;
	float			_gyro_range_scale;
	float			_gyro_range_rad_s;

	unsigned		_sample_rate;
	perf_counter_t		_accel_reads;
	perf_counter_t		_gyro_reads;
	perf_counter_t		_sample_perf;
	perf_counter_t		_bad_transfers;
	perf_counter_t		_bad_registers;
	perf_counter_t		_good_transfers;
	perf_counter_t		_reset_retries;
	perf_counter_t		_system_latency_perf;
	perf_counter_t		_controller_latency_perf;

	uint8_t			_register_wait;
	uint64_t		_reset_wait;

	math::LowPassFilter2p	_accel_filter_x;
	math::LowPassFilter2p	_accel_filter_y;
	math::LowPassFilter2p	_accel_filter_z;
	math::LowPassFilter2p	_gyro_filter_x;
	math::LowPassFilter2p	_gyro_filter_y;
	math::LowPassFilter2p	_gyro_filter_z;

	enum Rotation		_rotation;

	// last temperature reading for print_info()
	float			_last_temperature;

	/**
	 * Start automatic measurement.
	 */
	void			start();

	/**
	 * Stop automatic measurement.
	 */
	void			stop();

	/**
	 * Reset chip.
	 *
	 * Resets the chip and measurements ranges, but not scale and offset.
	 */
	int			reset();

	/**
	 * Static trampoline from the hrt_call context; because we don't have a
	 * generic hrt wrapper yet.
	 *
	 * Called by the HRT in interrupt context at the specified rate if
	 * automatic polling is enabled.
	 *
	 * @param arg		Instance pointer for the driver that is polling.
	 */
	static void		measure_trampoline(void *arg);

	/**
	 * Fetch measurements from the sensor and update the report buffers.
	 */
	void			measure();

	/**
	 * Read a register from the GYROSIM
	 *
	 * @param		The register to read.
	 * @return		The value that was read.
	 */
	uint8_t			read_reg(unsigned reg, uint32_t speed=GYROSIM_LOW_BUS_SPEED);
	uint16_t		read_reg16(unsigned reg);

	/**
	 * Write a register in the GYROSIM
	 *
	 * @param reg		The register to write.
	 * @param value		The new value to write.
	 */
	void			write_reg(unsigned reg, uint8_t value);

	/**
	 * Modify a register in the GYROSIM
	 *
	 * Bits are cleared before bits are set.
	 *
	 * @param reg		The register to modify.
	 * @param clearbits	Bits in the register to clear.
	 * @param setbits	Bits in the register to set.
	 */
	void			modify_reg(unsigned reg, uint8_t clearbits, uint8_t setbits);

	/**
	 * Set the GYROSIM measurement range.
	 *
	 * @param max_g		The maximum G value the range must support.
	 * @return		OK if the value can be supported, -ERANGE otherwise.
	 */
	int			set_accel_range(unsigned max_g);

	/**
	 * Swap a 16-bit value read from the GYROSIM to native byte order.
	 */
	uint16_t		swap16(uint16_t val) { return (val >> 8) | (val << 8);	}

	/**
	 * Measurement self test
	 *
	 * @return 0 on success, 1 on failure
	 */
	 int 			self_test();

	/**
	 * Accel self test
	 *
	 * @return 0 on success, 1 on failure
	 */
	int 			accel_self_test();

	/**
	 * Gyro self test
	 *
	 * @return 0 on success, 1 on failure
	 */
	 int 			gyro_self_test();

	/*
	  set low pass filter frequency
	 */
	void _set_dlpf_filter(uint16_t frequency_hz);

	/*
	  set sample rate (approximate) - 1kHz to 5Hz
	*/
	void _set_sample_rate(unsigned desired_sample_rate_hz);

	/* do not allow to copy this class due to pointer data members */
	GYROSIM(const GYROSIM&);
	GYROSIM operator=(const GYROSIM&);

#pragma pack(push, 1)
	/**
	 * Report conversation within the GYROSIM, including command byte and
	 * interrupt status.
	 */
	struct MPUReport {
		uint8_t		cmd;
		uint8_t		status;
		uint8_t		accel_x[2];
		uint8_t		accel_y[2];
		uint8_t		accel_z[2];
		uint8_t		temp[2];
		uint8_t		gyro_x[2];
		uint8_t		gyro_y[2];
		uint8_t		gyro_z[2];
	};
#pragma pack(pop)

	uint8_t _regdata[108];
};

/**
 * Helper class implementing the gyro driver node.
 */
class GYROSIM_gyro : public device::VDev
{
public:
	GYROSIM_gyro(GYROSIM *parent, const char *path);
	~GYROSIM_gyro();

	virtual ssize_t		read(device::file_t *filp, char *buffer, size_t buflen);
	virtual int		ioctl(device::file_t *filp, int cmd, unsigned long arg);

	virtual int		init();

protected:
	friend class GYROSIM;

	void			parent_poll_notify();

private:
	GYROSIM			*_parent;
	orb_advert_t		_gyro_topic;
	int			_gyro_orb_class_instance;
	int			_gyro_class_instance;

	/* do not allow to copy this class due to pointer data members */
	GYROSIM_gyro(const GYROSIM_gyro&);
	GYROSIM_gyro operator=(const GYROSIM_gyro&);
};

/** driver 'main' command */
extern "C" { __EXPORT int gyrosim_main(int argc, char *argv[]); }

GYROSIM::GYROSIM(const char *path_accel, const char *path_gyro, enum Rotation rotation) :
	VDev("GYROSIM", path_accel),
	_gyro(new GYROSIM_gyro(this, path_gyro)),
	_product(GYROSIMES_REV_C4),
	_call{},
	_call_interval(0),
	_accel_reports(nullptr),
	_accel_scale{},
	_accel_range_scale(0.0f),
	_accel_range_m_s2(0.0f),
	_accel_topic(nullptr),
	_accel_orb_class_instance(-1),
	_accel_class_instance(-1),
	_gyro_reports(nullptr),
	_gyro_scale{},
	_gyro_range_scale(0.0f),
	_gyro_range_rad_s(0.0f),
	_sample_rate(1000),
	_accel_reads(perf_alloc(PC_COUNT, "gyrosim_accel_read")),
	_gyro_reads(perf_alloc(PC_COUNT, "gyrosim_gyro_read")),
	_sample_perf(perf_alloc(PC_ELAPSED, "gyrosim_read")),
	_bad_transfers(perf_alloc(PC_COUNT, "gyrosim_bad_transfers")),
	_bad_registers(perf_alloc(PC_COUNT, "gyrosim_bad_registers")),
	_good_transfers(perf_alloc(PC_COUNT, "gyrosim_good_transfers")),
	_reset_retries(perf_alloc(PC_COUNT, "gyrosim_reset_retries")),
	_system_latency_perf(perf_alloc_once(PC_ELAPSED, "sys_latency")),
	_controller_latency_perf(perf_alloc_once(PC_ELAPSED, "ctrl_latency")),
	_register_wait(0),
	_reset_wait(0),
	_accel_filter_x(GYROSIM_ACCEL_DEFAULT_RATE, GYROSIM_ACCEL_DEFAULT_DRIVER_FILTER_FREQ),
	_accel_filter_y(GYROSIM_ACCEL_DEFAULT_RATE, GYROSIM_ACCEL_DEFAULT_DRIVER_FILTER_FREQ),
	_accel_filter_z(GYROSIM_ACCEL_DEFAULT_RATE, GYROSIM_ACCEL_DEFAULT_DRIVER_FILTER_FREQ),
	_gyro_filter_x(GYROSIM_GYRO_DEFAULT_RATE, GYROSIM_GYRO_DEFAULT_DRIVER_FILTER_FREQ),
	_gyro_filter_y(GYROSIM_GYRO_DEFAULT_RATE, GYROSIM_GYRO_DEFAULT_DRIVER_FILTER_FREQ),
	_gyro_filter_z(GYROSIM_GYRO_DEFAULT_RATE, GYROSIM_GYRO_DEFAULT_DRIVER_FILTER_FREQ),
	_rotation(rotation),
	_last_temperature(0)
{
	// disable debug() calls
	_debug_enabled = false;

	_device_id.devid_s.devtype = DRV_ACC_DEVTYPE_GYROSIM;

	/* Prime _gyro with parents devid. */
	_gyro->_device_id.devid = _device_id.devid;
	_gyro->_device_id.devid_s.devtype = DRV_GYR_DEVTYPE_GYROSIM;

	// default accel scale factors
	_accel_scale.x_offset = 0;
	_accel_scale.x_scale  = 1.0f;
	_accel_scale.y_offset = 0;
	_accel_scale.y_scale  = 1.0f;
	_accel_scale.z_offset = 0;
	_accel_scale.z_scale  = 1.0f;

	// default gyro scale factors
	_gyro_scale.x_offset = 0;
	_gyro_scale.x_scale  = 1.0f;
	_gyro_scale.y_offset = 0;
	_gyro_scale.y_scale  = 1.0f;
	_gyro_scale.z_offset = 0;
	_gyro_scale.z_scale  = 1.0f;

	memset(&_call, 0, sizeof(_call));
}

GYROSIM::~GYROSIM()
{
	/* make sure we are truly inactive */
	stop();

	/* delete the gyro subdriver */
	delete _gyro;

	/* free any existing reports */
	if (_accel_reports != nullptr)
		delete _accel_reports;
	if (_gyro_reports != nullptr)
		delete _gyro_reports;

	if (_accel_class_instance != -1)
		unregister_class_devname(ACCEL_BASE_DEVICE_PATH, _accel_class_instance);

	/* delete the perf counter */
	perf_free(_sample_perf);
	perf_free(_accel_reads);
	perf_free(_gyro_reads);
	perf_free(_bad_transfers);
	perf_free(_bad_registers);
	perf_free(_good_transfers);
}

int
GYROSIM::init()
{
	int ret;

	/* do VDevinit first */
	ret = VDev::init();

	/* if probe/setup failed, bail now */
	if (ret != OK) {
		PX4_WARN("VDev setup failed");
		return ret;
	}

	/* allocate basic report buffers */
	_accel_reports = new ringbuffer::RingBuffer(2, sizeof(accel_report));
	if (_accel_reports == nullptr) {
		PX4_WARN("_accel_reports creation failed");
		goto out;
	}

	_gyro_reports = new ringbuffer::RingBuffer(2, sizeof(gyro_report));
	if (_gyro_reports == nullptr) {
		PX4_WARN("_gyro_reports creation failed");
		goto out;
	}

	if (reset() != OK) {
		PX4_WARN("reset failed");
		goto out;
	}

	/* Initialize offsets and scales */
	_accel_scale.x_offset = 0;
	_accel_scale.x_scale  = 1.0f;
	_accel_scale.y_offset = 0;
	_accel_scale.y_scale  = 1.0f;
	_accel_scale.z_offset = 0;
	_accel_scale.z_scale  = 1.0f;

	_gyro_scale.x_offset = 0;
	_gyro_scale.x_scale  = 1.0f;
	_gyro_scale.y_offset = 0;
	_gyro_scale.y_scale  = 1.0f;
	_gyro_scale.z_offset = 0;
	_gyro_scale.z_scale  = 1.0f;


	/* do VDev init for the gyro device node, keep it optional */
	ret = _gyro->init();
	/* if probe/setup failed, bail now */
	if (ret != OK) {
		debug("gyro init failed");
		return ret;
	}

	_accel_class_instance = register_class_devname(ACCEL_BASE_DEVICE_PATH);

	measure();

	/* advertise sensor topic, measure manually to initialize valid report */
	struct accel_report arp;
	_accel_reports->get(&arp);

	/* measurement will have generated a report, publish */
	_accel_topic = orb_advertise_multi(ORB_ID(sensor_accel), &arp,
		&_accel_orb_class_instance, ORB_PRIO_HIGH);

	if (_accel_topic == nullptr) {
		PX4_WARN("ADVERT FAIL");
	}


	/* advertise sensor topic, measure manually to initialize valid report */
	struct gyro_report grp;
	_gyro_reports->get(&grp);

	_gyro->_gyro_topic = orb_advertise_multi(ORB_ID(sensor_gyro), &grp,
		&_gyro->_gyro_orb_class_instance, ORB_PRIO_HIGH);

	if (_gyro->_gyro_topic == nullptr) {
		PX4_WARN("ADVERT FAIL");
	}

out:
	return ret;
}

int GYROSIM::reset()
{
	return OK;
}

int
GYROSIM::transfer(uint8_t *send, uint8_t *recv, unsigned len)
{
	uint8_t cmd = send[0];
	uint8_t reg = cmd & 0x7F;
	const uint8_t MPUREAD = MPUREG_INT_STATUS | DIR_READ;

	if (cmd == MPUREAD) {
		// Get data from the simulator
		Simulator *sim = Simulator::getInstance();
		if (sim == NULL)
			return ENODEV;

		// FIXME - not sure what interrupt status should be
		recv[1] = 0;
		// skip cmd and status bytes
		sim->getMPUReport(&recv[2], len-2);
	}
	else if (cmd & DIR_READ)
	{
		PX4_DEBUG("Reading %u bytes from register %u", len-1, reg);
		memcpy(&_regdata[reg-MPUREG_PRODUCT_ID], &send[1], len-1);
	}
	else {
		PX4_DEBUG("Writing %u bytes to register %u", len-1, reg);
		if (recv)
			memcpy(&recv[1], &_regdata[reg-MPUREG_PRODUCT_ID], len-1);
	}
	return PX4_OK;
}

/*
  set sample rate (approximate) - 1kHz to 5Hz, for both accel and gyro
*/
void
GYROSIM::_set_sample_rate(unsigned desired_sample_rate_hz)
{
	if (desired_sample_rate_hz == 0 ||
			desired_sample_rate_hz == GYRO_SAMPLERATE_DEFAULT ||
			desired_sample_rate_hz == ACCEL_SAMPLERATE_DEFAULT) {
		desired_sample_rate_hz = GYROSIM_GYRO_DEFAULT_RATE;
	}

	uint8_t div = 1000 / desired_sample_rate_hz;
	if(div>200) div=200;
	if(div<1) div=1;
	write_reg(MPUREG_SMPLRT_DIV, div-1);
	_sample_rate = 1000 / div;
}

/*
  set the DLPF filter frequency. This affects both accel and gyro.
 */
void
GYROSIM::_set_dlpf_filter(uint16_t frequency_hz)
{
	uint8_t filter;

	/*
	   choose next highest filter frequency available
	 */
        if (frequency_hz == 0) {
		filter = BITS_DLPF_CFG_2100HZ_NOLPF;
        } else if (frequency_hz <= 5) {
		filter = BITS_DLPF_CFG_5HZ;
	} else if (frequency_hz <= 10) {
		filter = BITS_DLPF_CFG_10HZ;
	} else if (frequency_hz <= 20) {
		filter = BITS_DLPF_CFG_20HZ;
	} else if (frequency_hz <= 42) {
		filter = BITS_DLPF_CFG_42HZ;
	} else if (frequency_hz <= 98) {
		filter = BITS_DLPF_CFG_98HZ;
	} else if (frequency_hz <= 188) {
		filter = BITS_DLPF_CFG_188HZ;
	} else if (frequency_hz <= 256) {
		filter = BITS_DLPF_CFG_256HZ_NOLPF2;
	} else {
		filter = BITS_DLPF_CFG_2100HZ_NOLPF;
	}
	write_reg(MPUREG_CONFIG, filter);
}

ssize_t
GYROSIM::read(device::file_t *filp, char *buffer, size_t buflen)
{
	unsigned count = buflen / sizeof(accel_report);

	/* buffer must be large enough */
	if (count < 1)
		return -ENOSPC;

	/* if automatic measurement is not enabled, get a fresh measurement into the buffer */
	if (_call_interval == 0) {
		_accel_reports->flush();
		measure();
	}

	/* if no data, error (we could block here) */
	if (_accel_reports->empty())
		return -EAGAIN;

	perf_count(_accel_reads);

	/* copy reports out of our buffer to the caller */
	accel_report *arp = reinterpret_cast<accel_report *>(buffer);
	int transferred = 0;
	while (count--) {
		if (!_accel_reports->get(arp))
			break;
		transferred++;
		arp++;
	}

	/* return the number of bytes transferred */
	return (transferred * sizeof(accel_report));
}

int
GYROSIM::self_test()
{
	if (perf_event_count(_sample_perf) == 0) {
		measure();
	}

	/* return 0 on success, 1 else */
	return (perf_event_count(_sample_perf) > 0) ? 0 : 1;
}

int
GYROSIM::accel_self_test()
{
	return OK;

	if (self_test())
		return 1;

	/* inspect accel offsets */
	if (fabsf(_accel_scale.x_offset) < 0.000001f)
		return 1;
	if (fabsf(_accel_scale.x_scale - 1.0f) > 0.4f || fabsf(_accel_scale.x_scale - 1.0f) < 0.000001f)
		return 1;

	if (fabsf(_accel_scale.y_offset) < 0.000001f)
		return 1;
	if (fabsf(_accel_scale.y_scale - 1.0f) > 0.4f || fabsf(_accel_scale.y_scale - 1.0f) < 0.000001f)
		return 1;

	if (fabsf(_accel_scale.z_offset) < 0.000001f)
		return 1;
	if (fabsf(_accel_scale.z_scale - 1.0f) > 0.4f || fabsf(_accel_scale.z_scale - 1.0f) < 0.000001f)
		return 1;

	return 0;
}

int
GYROSIM::gyro_self_test()
{
	return OK;

	if (self_test())
		return 1;

	/*
	 * Maximum deviation of 20 degrees, according to
	 * http://www.invensense.com/mems/gyro/documents/PS-MPU-6000A-00v3.4.pdf
	 * Section 6.1, initial ZRO tolerance
	 */
	const float max_offset = 0.34f;
	/* 30% scale error is chosen to catch completely faulty units but
	 * to let some slight scale error pass. Requires a rate table or correlation
	 * with mag rotations + data fit to
	 * calibrate properly and is not done by default.
	 */
	const float max_scale = 0.3f;

	/* evaluate gyro offsets, complain if offset -> zero or larger than 20 dps. */
	if (fabsf(_gyro_scale.x_offset) > max_offset)
		return 1;

	/* evaluate gyro scale, complain if off by more than 30% */
	if (fabsf(_gyro_scale.x_scale - 1.0f) > max_scale)
		return 1;

	if (fabsf(_gyro_scale.y_offset) > max_offset)
		return 1;
	if (fabsf(_gyro_scale.y_scale - 1.0f) > max_scale)
		return 1;

	if (fabsf(_gyro_scale.z_offset) > max_offset)
		return 1;
	if (fabsf(_gyro_scale.z_scale - 1.0f) > max_scale)
		return 1;

	/* check if all scales are zero */
	if ((fabsf(_gyro_scale.x_offset) < 0.000001f) &&
		(fabsf(_gyro_scale.y_offset) < 0.000001f) &&
		(fabsf(_gyro_scale.z_offset) < 0.000001f)) {
		/* if all are zero, this device is not calibrated */
		return 1;
	}

	return 0;
}

ssize_t
GYROSIM::gyro_read(device::file_t *filp, char *buffer, size_t buflen)
{
	unsigned count = buflen / sizeof(gyro_report);

	/* buffer must be large enough */
	if (count < 1)
		return -ENOSPC;

	/* if automatic measurement is not enabled, get a fresh measurement into the buffer */
	if (_call_interval == 0) {
		_gyro_reports->flush();
		measure();
	}

	/* if no data, error (we could block here) */
	if (_gyro_reports->empty())
		return -EAGAIN;

	perf_count(_gyro_reads);

	/* copy reports out of our buffer to the caller */
	gyro_report *grp = reinterpret_cast<gyro_report *>(buffer);
	int transferred = 0;
	while (count--) {
		if (!_gyro_reports->get(grp))
			break;
		transferred++;
		grp++;
	}

	/* return the number of bytes transferred */
	return (transferred * sizeof(gyro_report));
}

int
GYROSIM::ioctl(device::file_t *filp, int cmd, unsigned long arg)
{
	switch (cmd) {

	case SENSORIOCRESET:
		return reset();

	case SENSORIOCSPOLLRATE: {
			switch (arg) {

				/* switching to manual polling */
			case SENSOR_POLLRATE_MANUAL:
				stop();
				_call_interval = 0;
				return OK;

				/* external signalling not supported */
			case SENSOR_POLLRATE_EXTERNAL:

				/* zero would be bad */
			case 0:
				return -EINVAL;

				/* set default/max polling rate */
			case SENSOR_POLLRATE_MAX:
				return ioctl(filp, SENSORIOCSPOLLRATE, 1000);

			case SENSOR_POLLRATE_DEFAULT:
				return ioctl(filp, SENSORIOCSPOLLRATE, GYROSIM_ACCEL_DEFAULT_RATE);

				/* adjust to a legal polling interval in Hz */
			default: {
					/* do we need to start internal polling? */
					bool want_start = (_call_interval == 0);

					/* convert hz to hrt interval via microseconds */
					unsigned ticks = 1000000 / arg;

					/* check against maximum sane rate */
					if (ticks < 1000)
						return -EINVAL;

					// adjust filters
					float cutoff_freq_hz = _accel_filter_x.get_cutoff_freq();
					float sample_rate = 1.0e6f/ticks;
					_set_dlpf_filter(cutoff_freq_hz);
					_accel_filter_x.set_cutoff_frequency(sample_rate, cutoff_freq_hz);
					_accel_filter_y.set_cutoff_frequency(sample_rate, cutoff_freq_hz);
					_accel_filter_z.set_cutoff_frequency(sample_rate, cutoff_freq_hz);


					float cutoff_freq_hz_gyro = _gyro_filter_x.get_cutoff_freq();
					_set_dlpf_filter(cutoff_freq_hz_gyro);
					_gyro_filter_x.set_cutoff_frequency(sample_rate, cutoff_freq_hz_gyro);
					_gyro_filter_y.set_cutoff_frequency(sample_rate, cutoff_freq_hz_gyro);
					_gyro_filter_z.set_cutoff_frequency(sample_rate, cutoff_freq_hz_gyro);

					/* update interval for next measurement */
					/* XXX this is a bit shady, but no other way to adjust... */
					_call.period = _call_interval = ticks;

					/* if we need to start the poll state machine, do it */
					if (want_start)
						start();

					return OK;
				}
			}
		}

	case SENSORIOCGPOLLRATE:
		if (_call_interval == 0)
			return SENSOR_POLLRATE_MANUAL;

		return 1000000 / _call_interval;

	case SENSORIOCSQUEUEDEPTH: {
		/* lower bound is mandatory, upper bound is a sanity check */
		if ((arg < 1) || (arg > 100))
			return -EINVAL;

		if (!_accel_reports->resize(arg)) {
			return -ENOMEM;
		}

		return OK;
	}

	case SENSORIOCGQUEUEDEPTH:
		return _accel_reports->size();

	case ACCELIOCGSAMPLERATE:
		return _sample_rate;

	case ACCELIOCSSAMPLERATE:
		_set_sample_rate(arg);
		return OK;

	case ACCELIOCGLOWPASS:
		return _accel_filter_x.get_cutoff_freq();

	case ACCELIOCSLOWPASS:
		// set hardware filtering
		_set_dlpf_filter(arg);
		// set software filtering
		_accel_filter_x.set_cutoff_frequency(1.0e6f / _call_interval, arg);
		_accel_filter_y.set_cutoff_frequency(1.0e6f / _call_interval, arg);
		_accel_filter_z.set_cutoff_frequency(1.0e6f / _call_interval, arg);
		return OK;

	case ACCELIOCSSCALE:
		{
			/* copy scale, but only if off by a few percent */
			struct accel_scale *s = (struct accel_scale *) arg;
			float sum = s->x_scale + s->y_scale + s->z_scale;
			if (sum > 2.0f && sum < 4.0f) {
				memcpy(&_accel_scale, s, sizeof(_accel_scale));
				return OK;
			} else {
				return -EINVAL;
			}
		}

	case ACCELIOCGSCALE:
		/* copy scale out */
		memcpy((struct accel_scale *) arg, &_accel_scale, sizeof(_accel_scale));
		return OK;

	case ACCELIOCSRANGE:
		return set_accel_range(arg);

	case ACCELIOCGRANGE:
		return (unsigned long)((_accel_range_m_s2)/GYROSIM_ONE_G + 0.5f);

	case ACCELIOCSELFTEST:
		return accel_self_test();

	default:
		/* give it to the superclass */
		return VDev::ioctl(filp, cmd, arg);
	}
}

int
GYROSIM::gyro_ioctl(device::file_t *filp, int cmd, unsigned long arg)
{
	switch (cmd) {

		/* these are shared with the accel side */
	case SENSORIOCSPOLLRATE:
	case SENSORIOCGPOLLRATE:
	case SENSORIOCRESET:
		return ioctl(filp, cmd, arg);

	case SENSORIOCSQUEUEDEPTH: {
		/* lower bound is mandatory, upper bound is a sanity check */
		if ((arg < 1) || (arg > 100))
			return -EINVAL;

		if (!_gyro_reports->resize(arg)) {
			return -ENOMEM;
		}

		return OK;
	}

	case SENSORIOCGQUEUEDEPTH:
		return _gyro_reports->size();

	case GYROIOCGSAMPLERATE:
		return _sample_rate;

	case GYROIOCSSAMPLERATE:
		_set_sample_rate(arg);
		return OK;

	case GYROIOCGLOWPASS:
		return _gyro_filter_x.get_cutoff_freq();
	case GYROIOCSLOWPASS:
		// set hardware filtering
		_set_dlpf_filter(arg);
		_gyro_filter_x.set_cutoff_frequency(1.0e6f / _call_interval, arg);
		_gyro_filter_y.set_cutoff_frequency(1.0e6f / _call_interval, arg);
		_gyro_filter_z.set_cutoff_frequency(1.0e6f / _call_interval, arg);
		return OK;

	case GYROIOCSSCALE:
		/* copy scale in */
		memcpy(&_gyro_scale, (struct gyro_scale *) arg, sizeof(_gyro_scale));
		return OK;

	case GYROIOCGSCALE:
		/* copy scale out */
		memcpy((struct gyro_scale *) arg, &_gyro_scale, sizeof(_gyro_scale));
		return OK;

	case GYROIOCSRANGE:
		/* XXX not implemented */
		// XXX change these two values on set:
		// _gyro_range_scale = xx
		// _gyro_range_rad_s = xx
		return -EINVAL;
	case GYROIOCGRANGE:
		return (unsigned long)(_gyro_range_rad_s * 180.0f / M_PI_F + 0.5f);

	case GYROIOCSELFTEST:
		return gyro_self_test();

	default:
		/* give it to the superclass */
		return VDev::ioctl(filp, cmd, arg);
	}
}

uint8_t
GYROSIM::read_reg(unsigned reg, uint32_t speed)
{
	uint8_t cmd[2] = { (uint8_t)(reg | DIR_READ), 0};

        // general register transfer at low clock speed
        //set_frequency(speed);

	transfer(cmd, cmd, sizeof(cmd));

	return cmd[1];
}

uint16_t
GYROSIM::read_reg16(unsigned reg)
{
	uint8_t cmd[3] = { (uint8_t)(reg | DIR_READ), 0, 0 };

        // general register transfer at low clock speed
        //set_frequency(GYROSIM_LOW_BUS_SPEED);

	transfer(cmd, cmd, sizeof(cmd));

	return (uint16_t)(cmd[1] << 8) | cmd[2];
}

void
GYROSIM::write_reg(unsigned reg, uint8_t value)
{
	uint8_t	cmd[2];

	cmd[0] = reg | DIR_WRITE;
	cmd[1] = value;

        // general register transfer at low clock speed
        //set_frequency(GYROSIM_LOW_BUS_SPEED);

	transfer(cmd, nullptr, sizeof(cmd));
}

void
GYROSIM::modify_reg(unsigned reg, uint8_t clearbits, uint8_t setbits)
{
	uint8_t	val;

	val = read_reg(reg);
	val &= ~clearbits;
	val |= setbits;
	write_reg(reg, val);
}

int
GYROSIM::set_accel_range(unsigned max_g_in)
{
	// workaround for bugged versions of MPU6k (rev C)
	switch (_product) {
		case GYROSIMES_REV_C4:
		case GYROSIMES_REV_C5:
		case GYROSIM_REV_C4:
		case GYROSIM_REV_C5:
			write_reg(MPUREG_ACCEL_CONFIG, 1 << 3);
			_accel_range_scale = (GYROSIM_ONE_G / 4096.0f);
			_accel_range_m_s2 = 8.0f * GYROSIM_ONE_G;
			return OK;
	}

	uint8_t afs_sel;
	float lsb_per_g;
	float max_accel_g;

	if (max_g_in > 8) { // 16g - AFS_SEL = 3
		afs_sel = 3;
		lsb_per_g = 2048;
		max_accel_g = 16;
	} else if (max_g_in > 4) { //  8g - AFS_SEL = 2
		afs_sel = 2;
		lsb_per_g = 4096;
		max_accel_g = 8;
	} else if (max_g_in > 2) { //  4g - AFS_SEL = 1
		afs_sel = 1;
		lsb_per_g = 8192;
		max_accel_g = 4;
	} else {                //  2g - AFS_SEL = 0
		afs_sel = 0;
		lsb_per_g = 16384;
		max_accel_g = 2;
	}

	write_reg(MPUREG_ACCEL_CONFIG, afs_sel << 3);
	_accel_range_scale = (GYROSIM_ONE_G / lsb_per_g);
	_accel_range_m_s2 = max_accel_g * GYROSIM_ONE_G;

	return OK;
}

void
GYROSIM::start()
{
	/* make sure we are stopped first */
	stop();

	/* discard any stale data in the buffers */
	_accel_reports->flush();
	_gyro_reports->flush();

	/* start polling at the specified rate */
	hrt_call_every(&_call, 1000, _call_interval, (hrt_callout)&GYROSIM::measure_trampoline, this);
}

void
GYROSIM::stop()
{
	hrt_cancel(&_call);
}

void
GYROSIM::measure_trampoline(void *arg)
{
	GYROSIM *dev = reinterpret_cast<GYROSIM *>(arg);

	/* make another measurement */
	dev->measure();
}

void
GYROSIM::measure()
{
	struct MPUReport mpu_report;
	struct Report {
		int16_t		accel_x;
		int16_t		accel_y;
		int16_t		accel_z;
		int16_t		temp;
		int16_t		gyro_x;
		int16_t		gyro_y;
		int16_t		gyro_z;
	} report;

	/* start measuring */
	perf_begin(_sample_perf);

	/*
	 * Fetch the full set of measurements from the GYROSIM in one pass.
	 */
	mpu_report.cmd = DIR_READ | MPUREG_INT_STATUS;

        // sensor transfer at high clock speed
        //set_frequency(GYROSIM_HIGH_BUS_SPEED);

	if (OK != transfer((uint8_t *)&mpu_report, ((uint8_t *)&mpu_report), sizeof(mpu_report)))
		return;

	/*
	 * Convert from big to little endian
	 */

	report.accel_x = int16_t_from_bytes(mpu_report.accel_x);
	report.accel_y = int16_t_from_bytes(mpu_report.accel_y);
	report.accel_z = int16_t_from_bytes(mpu_report.accel_z);

	report.temp = int16_t_from_bytes(mpu_report.temp);

	report.gyro_x = int16_t_from_bytes(mpu_report.gyro_x);
	report.gyro_y = int16_t_from_bytes(mpu_report.gyro_y);
	report.gyro_z = int16_t_from_bytes(mpu_report.gyro_z);

	if (report.accel_x == 0 &&
	    report.accel_y == 0 &&
	    report.accel_z == 0 &&
	    report.temp == 0 &&
	    report.gyro_x == 0 &&
	    report.gyro_y == 0 &&
	    report.gyro_z == 0) {
		// all zero data - probably a VDev bus error
		perf_count(_bad_transfers);
		perf_end(_sample_perf);
                // note that we don't call reset() here as a reset()
                // costs 20ms with interrupts disabled. That means if
                // the mpu6k does go bad it would cause a FMU failure,
                // regardless of whether another sensor is available,
		return;
	}

	perf_count(_good_transfers);

	if (_register_wait != 0) {
		// we are waiting for some good transfers before using
		// the sensor again. We still increment
		// _good_transfers, but don't return any data yet
		_register_wait--;
		return;
	}


	/*
	 * Swap axes and negate y
	 */
	int16_t accel_xt = report.accel_y;
	int16_t accel_yt = ((report.accel_x == -32768) ? 32767 : -report.accel_x);

	int16_t gyro_xt = report.gyro_y;
	int16_t gyro_yt = ((report.gyro_x == -32768) ? 32767 : -report.gyro_x);

	/*
	 * Apply the swap
	 */
	report.accel_x = accel_xt;
	report.accel_y = accel_yt;
	report.gyro_x = gyro_xt;
	report.gyro_y = gyro_yt;

	/*
	 * Report buffers.
	 */
	accel_report		arb;
	gyro_report		grb;

	/*
	 * Adjust and scale results to m/s^2.
	 */
	grb.timestamp = arb.timestamp = hrt_absolute_time();

	// report the error count as the sum of the number of bad
	// transfers and bad register reads. This allows the higher
	// level code to decide if it should use this sensor based on
	// whether it has had failures
        grb.error_count = arb.error_count = perf_event_count(_bad_transfers) + perf_event_count(_bad_registers);

	/*
	 * 1) Scale raw value to SI units using scaling from datasheet.
	 * 2) Subtract static offset (in SI units)
	 * 3) Scale the statically calibrated values with a linear
	 *    dynamically obtained factor
	 *
	 * Note: the static sensor offset is the number the sensor outputs
	 * 	 at a nominally 'zero' input. Therefore the offset has to
	 * 	 be subtracted.
	 *
	 *	 Example: A gyro outputs a value of 74 at zero angular rate
	 *	 	  the offset is 74 from the origin and subtracting
	 *		  74 from all measurements centers them around zero.
	 */


	/* NOTE: Axes have been swapped to match the board a few lines above. */

	arb.x_raw = report.accel_x;
	arb.y_raw = report.accel_y;
	arb.z_raw = report.accel_z;

	float xraw_f = report.accel_x;
	float yraw_f = report.accel_y;
	float zraw_f = report.accel_z;

	// apply user specified rotation
	rotate_3f(_rotation, xraw_f, yraw_f, zraw_f);

	float x_in_new = ((xraw_f * _accel_range_scale) - _accel_scale.x_offset) * _accel_scale.x_scale;
	float y_in_new = ((yraw_f * _accel_range_scale) - _accel_scale.y_offset) * _accel_scale.y_scale;
	float z_in_new = ((zraw_f * _accel_range_scale) - _accel_scale.z_offset) * _accel_scale.z_scale;

	arb.x = _accel_filter_x.apply(x_in_new);
	arb.y = _accel_filter_y.apply(y_in_new);
	arb.z = _accel_filter_z.apply(z_in_new);

	arb.scaling = _accel_range_scale;
	arb.range_m_s2 = _accel_range_m_s2;

	_last_temperature = (report.temp) / 361.0f + 35.0f;

	arb.temperature_raw = report.temp;
	arb.temperature = _last_temperature;

	grb.x_raw = report.gyro_x;
	grb.y_raw = report.gyro_y;
	grb.z_raw = report.gyro_z;

	xraw_f = report.gyro_x;
	yraw_f = report.gyro_y;
	zraw_f = report.gyro_z;

	// apply user specified rotation
	rotate_3f(_rotation, xraw_f, yraw_f, zraw_f);

	float x_gyro_in_new = ((xraw_f * _gyro_range_scale) - _gyro_scale.x_offset) * _gyro_scale.x_scale;
	float y_gyro_in_new = ((yraw_f * _gyro_range_scale) - _gyro_scale.y_offset) * _gyro_scale.y_scale;
	float z_gyro_in_new = ((zraw_f * _gyro_range_scale) - _gyro_scale.z_offset) * _gyro_scale.z_scale;

	grb.x = _gyro_filter_x.apply(x_gyro_in_new);
	grb.y = _gyro_filter_y.apply(y_gyro_in_new);
	grb.z = _gyro_filter_z.apply(z_gyro_in_new);

	grb.scaling = _gyro_range_scale;
	grb.range_rad_s = _gyro_range_rad_s;

	grb.temperature_raw = report.temp;
	grb.temperature = _last_temperature;

	_accel_reports->force(&arb);
	_gyro_reports->force(&grb);

	/* notify anyone waiting for data */
	poll_notify(POLLIN);
	_gyro->parent_poll_notify();

	if (!(_pub_blocked)) {
		/* log the time of this report */
		perf_begin(_controller_latency_perf);
		perf_begin(_system_latency_perf);
		/* publish it */
		orb_publish(ORB_ID(sensor_accel), _accel_topic, &arb);
	}

	if (!(_pub_blocked)) {
		/* publish it */
		orb_publish(ORB_ID(sensor_gyro), _gyro->_gyro_topic, &grb);
	}

	/* stop measuring */
	perf_end(_sample_perf);
}

void
GYROSIM::print_info()
{
	perf_print_counter(_sample_perf);
	perf_print_counter(_accel_reads);
	perf_print_counter(_gyro_reads);
	perf_print_counter(_bad_transfers);
	perf_print_counter(_bad_registers);
	perf_print_counter(_good_transfers);
	perf_print_counter(_reset_retries);
	_accel_reports->print_info("accel queue");
	_gyro_reports->print_info("gyro queue");
	PX4_WARN("temperature: %.1f", (double)_last_temperature);
}

void
GYROSIM::print_registers()
{
	char buf[6*13+1];
	int i=0;

	buf[0] = '\0';
	PX4_WARN("GYROSIM registers");
	for (uint8_t reg=MPUREG_PRODUCT_ID; reg<=108; reg++) {
		uint8_t v = read_reg(reg);
		sprintf(&buf[i*6], "%02x:%02x ",(unsigned)reg, (unsigned)v);
		i++;
		if ((i+1) % 13 == 0) {
			PX4_WARN("%s", buf);
			i=0;
			buf[i] = '\0';
		}
	}
	PX4_WARN("%s",buf);
}


GYROSIM_gyro::GYROSIM_gyro(GYROSIM *parent, const char *path) :
	VDev("GYROSIM_gyro", path),
	_parent(parent),
	_gyro_topic(nullptr),
	_gyro_orb_class_instance(-1),
	_gyro_class_instance(-1)
{
}

GYROSIM_gyro::~GYROSIM_gyro()
{
	if (_gyro_class_instance != -1)
		unregister_class_devname(GYRO_BASE_DEVICE_PATH, _gyro_class_instance);
}

int
GYROSIM_gyro::init()
{
	int ret;

	// do base class init
	ret = VDev::init();

	/* if probe/setup failed, bail now */
	if (ret != OK) {
		debug("gyro init failed");
		return ret;
	}

	_gyro_class_instance = register_class_devname(GYRO_BASE_DEVICE_PATH);

	return ret;
}

void
GYROSIM_gyro::parent_poll_notify()
{
	poll_notify(POLLIN);
}

ssize_t
GYROSIM_gyro::read(device::file_t *filp, char *buffer, size_t buflen)
{
	return _parent->gyro_read(filp, buffer, buflen);
}

int
GYROSIM_gyro::ioctl(device::file_t *filp, int cmd, unsigned long arg)
{

	switch (cmd) {
		case DEVIOCGDEVICEID:
			return (int)VDev::ioctl(filp, cmd, arg);
			break;
		default:
			return _parent->gyro_ioctl(filp, cmd, arg);
	}
}

/**
 * Local functions in support of the shell command.
 */
namespace gyrosim
{

GYROSIM	*g_dev_sim; // on simulated bus

int	start(enum Rotation);
int	stop();
int	test();
int	reset();
int	info();
int	regdump();
void	usage();

/**
 * Start the driver.
 *
 * This function only returns if the driver is up and running
 * or failed to detect the sensor.
 */
int
start(enum Rotation rotation)
{
	int fd;
        GYROSIM **g_dev_ptr = &g_dev_sim;
	const char *path_accel = MPU_DEVICE_PATH_ACCEL;
	const char *path_gyro  = MPU_DEVICE_PATH_GYRO;

	if (*g_dev_ptr != nullptr) {
		/* if already started, the still command succeeded */
		PX4_WARN("already started");
		return 0;
	}

	/* create the driver */
	*g_dev_ptr = new GYROSIM(path_accel, path_gyro, rotation);

	if (*g_dev_ptr == nullptr)
		goto fail;

	if (OK != (*g_dev_ptr)->init())
		goto fail;

	/* set the poll rate to default, starts automatic data collection */
	fd = px4_open(path_accel, O_RDONLY);

	if (fd < 0)
		goto fail;

	if (px4_ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT) < 0) {
		px4_close(fd);
		goto fail;
	}

	px4_close(fd);
	return 0;
fail:

	if (*g_dev_ptr != nullptr) {
            delete (*g_dev_ptr);
            *g_dev_ptr = nullptr;
	}

	PX4_WARN("driver start failed");
	return 1;
}

int
stop()
{
	GYROSIM **g_dev_ptr = &g_dev_sim;
	if (*g_dev_ptr != nullptr) {
		delete *g_dev_ptr;
		*g_dev_ptr = nullptr;
	} else {
		/* warn, but not an error */
		PX4_WARN("already stopped.");
	}
	return 0;
}

/**
 * Perform some basic functional tests on the driver;
 * make sure we can collect data from the sensor in polled
 * and automatic modes.
 */
int
test()
{
	const char *path_accel = MPU_DEVICE_PATH_ACCEL;
	const char *path_gyro  = MPU_DEVICE_PATH_GYRO;
	accel_report a_report;
	gyro_report g_report;
	ssize_t sz;

	/* get the driver */
	int fd = px4_open(path_accel, O_RDONLY);

	if (fd < 0) {
		PX4_ERR("%s open failed (try 'gyrosim start')", path_accel);
		return 1;
	}

	/* get the driver */
	int fd_gyro = px4_open(path_gyro, O_RDONLY);

	if (fd_gyro < 0) {
		PX4_ERR("%s open failed", path_gyro);
		return 1;
	}

	/* reset to manual polling */
	if (px4_ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_MANUAL) < 0) {
		PX4_ERR("reset to manual polling");
		return 1;
	}

	/* do a simple demand read */
	sz = read(fd, &a_report, sizeof(a_report));

	if (sz != sizeof(a_report)) {
		PX4_WARN("ret: %zd, expected: %zd", sz, sizeof(a_report));
		PX4_ERR("immediate acc read failed");
		return 1;
	}

	PX4_INFO("single read");
	PX4_INFO("time:     %lld", (long long)a_report.timestamp);
	PX4_INFO("acc  x:  \t%8.4f\tm/s^2", (double)a_report.x);
	PX4_INFO("acc  y:  \t%8.4f\tm/s^2", (double)a_report.y);
	PX4_INFO("acc  z:  \t%8.4f\tm/s^2", (double)a_report.z);
	PX4_INFO("acc  x:  \t%d\traw 0x%0x", (short)a_report.x_raw, (unsigned short)a_report.x_raw);
	PX4_INFO("acc  y:  \t%d\traw 0x%0x", (short)a_report.y_raw, (unsigned short)a_report.y_raw);
	PX4_INFO("acc  z:  \t%d\traw 0x%0x", (short)a_report.z_raw, (unsigned short)a_report.z_raw);
	PX4_INFO("acc range: %8.4f m/s^2 (%8.4f g)", (double)a_report.range_m_s2,
	      (double)(a_report.range_m_s2 / GYROSIM_ONE_G));

	/* do a simple demand read */
	sz = read(fd_gyro, &g_report, sizeof(g_report));

	if (sz != sizeof(g_report)) {
		PX4_WARN("ret: %zd, expected: %zd", sz, sizeof(g_report));
		PX4_ERR("immediate gyro read failed");
		return 1;
	}

	PX4_INFO("gyro x: \t% 9.5f\trad/s", (double)g_report.x);
	PX4_INFO("gyro y: \t% 9.5f\trad/s", (double)g_report.y);
	PX4_INFO("gyro z: \t% 9.5f\trad/s", (double)g_report.z);
	PX4_INFO("gyro x: \t%d\traw", (int)g_report.x_raw);
	PX4_INFO("gyro y: \t%d\traw", (int)g_report.y_raw);
	PX4_INFO("gyro z: \t%d\traw", (int)g_report.z_raw);
	PX4_INFO("gyro range: %8.4f rad/s (%d deg/s)", (double)g_report.range_rad_s,
	      (int)((g_report.range_rad_s / M_PI_F) * 180.0f + 0.5f));

	PX4_INFO("temp:  \t%8.4f\tdeg celsius", (double)a_report.temperature);
	PX4_INFO("temp:  \t%d\traw 0x%0x", (short)a_report.temperature_raw, (unsigned short)a_report.temperature_raw);


	/* XXX add poll-rate tests here too */

	px4_close(fd);
	reset();
	PX4_INFO("PASS");

	
	return 0;
}

/**
 * Reset the driver.
 */
int
reset()
{
	const char *path_accel = MPU_DEVICE_PATH_ACCEL;
	int fd = px4_open(path_accel, O_RDONLY);

	if (fd < 0) {
		PX4_ERR("reset failed");
		return 1;
	}


	if (px4_ioctl(fd, SENSORIOCRESET, 0) < 0) {
		PX4_ERR("driver reset failed");
		goto reset_fail;
	}

	if (px4_ioctl(fd, SENSORIOCSPOLLRATE, SENSOR_POLLRATE_DEFAULT) < 0) {
		PX4_ERR("driver poll restart failed");
		goto reset_fail;
	}

        px4_close(fd);
	return 0;

reset_fail:
        px4_close(fd);
	return 1;
}

/**
 * Print a little info about the driver.
 */
int
info()
{
        GYROSIM **g_dev_ptr = &g_dev_sim;
	if (*g_dev_ptr == nullptr) {
		PX4_ERR("driver not running");
		return 1;
	}

	PX4_INFO("state @ %p", *g_dev_ptr);
	(*g_dev_ptr)->print_info();

	return 0;
}

/**
 * Dump the register information
 */
int
regdump()
{
	GYROSIM **g_dev_ptr = &g_dev_sim;
	if (*g_dev_ptr == nullptr) {
		PX4_ERR("driver not running");
		return 1;
	}

	PX4_INFO("regdump @ %p", *g_dev_ptr);
	(*g_dev_ptr)->print_registers();

	return 0;
}

void
usage()
{
	PX4_WARN("missing command: try 'start', 'info', 'test', 'stop', 'reset', 'regdump'");
	PX4_WARN("options:");
	PX4_WARN("    -R rotation");
}

} // namespace

int
gyrosim_main(int argc, char *argv[])
{
	int ch;
	enum Rotation rotation = ROTATION_NONE;
	int ret;

	/* jump over start/off/etc and look at options first */
	int myoptind = 1;
	const char *myoptarg = NULL;
	while ((ch = px4_getopt(argc, argv, "R:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		case 'R':
			rotation = (enum Rotation)atoi(optarg);
			break;
		default:
			gyrosim::usage();
			return 0;
		}
	}

	const char *verb = argv[myoptind];

	/*
	 * Start/load the driver.

	 */
	if (!strcmp(verb, "start")) {
		ret = gyrosim::start(rotation);
	}

	else if (!strcmp(verb, "stop")) {
		ret = gyrosim::stop();
	}

	/*
	 * Test the driver/device.
	 */
	else if (!strcmp(verb, "test")) {
		ret = gyrosim::test();
	}

	/*
	 * Reset the driver.
	 */
	else if (!strcmp(verb, "reset")) {
		ret = gyrosim::reset();
	}

	/*
	 * Print driver information.
	 */
	else if (!strcmp(verb, "info")) {
		ret = gyrosim::info();
	}

	/*
	 * Print register information.
	 */
	else if (!strcmp(verb, "regdump")) {
		ret = gyrosim::regdump();
	}

	else  {
		gyrosim::usage();
		ret = 1;
	}

	return ret;
}
