/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @brief File containing QSPI device interface specific definitions for the
 * Zephyr OS layer of the Wi-Fi driver.
 */

#define DT_DRV_COMPAT nordic_nrf700x_qspi

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/pinctrl.h>

#include <soc.h>
#include <nrfx_qspi.h>
#include <hal/nrf_clock.h>
#include <hal/nrf_gpio.h>

#include "spi_nor.h"
#include "qspi_if.h"

static struct qspi_config *qspi_config;
static unsigned int nonce_last_addr;
static unsigned int nonce_cnt;

/* Main config structure */
static nrfx_qspi_config_t QSPIconfig;

#define INST_0_SCK_FREQUENCY DT_INST_PROP(0, sck_frequency)
BUILD_ASSERT(INST_0_SCK_FREQUENCY >= (NRF_QSPI_BASE_CLOCK_FREQ / 16), "Unsupported SCK frequency.");

/* for accessing devicetree properties of the bus node */
#define QSPI_NODE DT_BUS(DT_DRV_INST(0))
#define QSPI_PROP_AT(prop, idx) DT_PROP_BY_IDX(QSPI_NODE, prop, idx)
#define QSPI_PROP_LEN(prop) DT_PROP_LEN(QSPI_NODE, prop)

#define INST_0_QER                                                                                 \
	_CONCAT(JESD216_DW15_QER_, DT_ENUM_TOKEN(DT_DRV_INST(0), quad_enable_requirements))

static int qspi_device_init(const struct device *dev);
static void qspi_device_uninit(const struct device *dev);

#define QSPI_SCK_DELAY 0
#define WORD_SIZE 4

LOG_MODULE_DECLARE(wifi_nrf, CONFIG_WIFI_NRF700X_BUS_LOG_LEVEL);

/**
 * @brief QSPI buffer structure
 * Structure used both for TX and RX purposes.
 *
 * @param buf is a valid pointer to a data buffer.
 * Can not be NULL.
 * @param len is the length of the data to be handled.
 * If no data to transmit/receive - pass 0.
 */
struct qspi_buf {
	uint8_t *buf;
	size_t len;
};

/**
 * @brief QSPI command structure
 * Structure used for custom command usage.
 *
 * @param op_code is a command value (i.e 0x9F - get Jedec ID)
 * @param tx_buf structure used for TX purposes. Can be NULL if not used.
 * @param rx_buf structure used for RX purposes. Can be NULL if not used.
 */
struct qspi_cmd {
	uint8_t op_code;
	const struct qspi_buf *tx_buf;
	const struct qspi_buf *rx_buf;
};

/**
 * @brief Structure for defining the QSPI NOR access
 */
struct qspi_nor_data {
#ifdef CONFIG_MULTITHREADING
	/* The semaphore to control exclusive access on write/erase. */
	struct k_sem trans;
	/* The semaphore to control exclusive access to the device. */
	struct k_sem sem;
	/* The semaphore to indicate that transfer has completed. */
	struct k_sem sync;
	/* The semaphore to control driver init/uninit. */
	struct k_sem count;
#else /* CONFIG_MULTITHREADING */
	/* A flag that signals completed transfer when threads are
	 * not enabled.
	 */
	volatile bool ready;
#endif /* CONFIG_MULTITHREADING */
};

static inline int qspi_freq_mhz_to_sckfreq(int freq_m)
{
	if (freq_m > (NRF_QSPI_BASE_CLOCK_FREQ / MHZ(1))) {
		return NRF_QSPI_FREQ_DIV1;
	}
#if defined(CONFIG_SOC_SERIES_NRF53X)
	/* QSPIM (6-96MHz) : 192MHz / (2*(SCKFREQ + 1)) */
	return ((192 / freq_m) / 2) - 1;
#else
	/* QSPIM (2-32MHz): 32 MHz / (SCKFREQ + 1) */
	return (32 / freq_m) - 1;
#endif
}

static inline int qspi_dts_freq_to_sckfreq(void)
{
	/* DTS frequency is in HZ */
	return qspi_freq_mhz_to_sckfreq(INST_0_SCK_FREQUENCY / MHZ(1));
}

static inline int qspi_get_mode(bool cpol, bool cpha)
{
	register int ret = -EINVAL;

	if ((!cpol) && (!cpha))
		ret = 0;
	else if (cpol && cpha)
		ret = 1;

	__ASSERT(ret != -EINVAL, "Invalid QSPI mode");

	return ret;
}

static inline bool qspi_write_is_quad(nrf_qspi_writeoc_t lines)
{
	switch (lines) {
	case NRF_QSPI_WRITEOC_PP4IO:
	case NRF_QSPI_WRITEOC_PP4O:
		return true;
	default:
		return false;
	}
}

static inline bool qspi_read_is_quad(nrf_qspi_readoc_t lines)
{
	switch (lines) {
	case NRF_QSPI_READOC_READ4IO:
	case NRF_QSPI_READOC_READ4O:
		return true;
	default:
		return false;
	}
}

static inline int qspi_get_lines_write(uint8_t lines)
{
	register int ret = -EINVAL;

	switch (lines) {
	case 3:
		ret = NRF_QSPI_WRITEOC_PP4IO;
		break;
	case 2:
		ret = NRF_QSPI_WRITEOC_PP4O;
		break;
	case 1:
		ret = NRF_QSPI_WRITEOC_PP2O;
		break;
	case 0:
		ret = NRF_QSPI_WRITEOC_PP;
		break;
	default:
		break;
	}

	__ASSERT(ret != -EINVAL, "Invalid QSPI write line");

	return ret;
}

static inline int qspi_get_lines_read(uint8_t lines)
{
	register int ret = -EINVAL;

	switch (lines) {
	case 4:
		ret = NRF_QSPI_READOC_READ4IO;
		break;
	case 3:
		ret = NRF_QSPI_READOC_READ4O;
		break;
	case 2:
		ret = NRF_QSPI_READOC_READ2IO;
		break;
	case 1:
		ret = NRF_QSPI_READOC_READ2O;
		break;
	case 0:
		ret = NRF_QSPI_READOC_FASTREAD;
		break;
	default:
		break;
	}

	__ASSERT(ret != -EINVAL, "Invalid QSPI read line");

	return ret;
}

nrfx_err_t _nrfx_qspi_read(void *p_rx_buffer, size_t rx_buffer_length, uint32_t src_address)
{
	return nrfx_qspi_read(p_rx_buffer, rx_buffer_length, src_address);
}

nrfx_err_t _nrfx_qspi_write(void const *p_tx_buffer, size_t tx_buffer_length, uint32_t dst_address)
{
	return nrfx_qspi_write(p_tx_buffer, tx_buffer_length, dst_address);
}

nrfx_err_t _nrfx_qspi_init(nrfx_qspi_config_t const *p_config, nrfx_qspi_handler_t handler,
			   void *p_context)
{
	NRF_QSPI_Type *p_reg = NRF_QSPI;

	nrfx_qspi_init(p_config, handler, p_context);

	/* RDC4IO = 4'hA (register IFTIMING), which means 10 Dummy Cycles for READ4. */
	p_reg->IFTIMING |= qspi_config->RDC4IO;

	/* LOG_DBG("%04x : IFTIMING\n", p_reg->IFTIMING & qspi_config->RDC4IO); */

	/* ACTIVATE task fails for slave bitfile so ignore it */
	return NRFX_SUCCESS;
}


/**
 * @brief Main configuration structure
 */
static struct qspi_nor_data qspi_nor_memory_data = {
#ifdef CONFIG_MULTITHREADING
	.trans = Z_SEM_INITIALIZER(qspi_nor_memory_data.trans, 1, 1),
	.sem = Z_SEM_INITIALIZER(qspi_nor_memory_data.sem, 1, 1),
	.sync = Z_SEM_INITIALIZER(qspi_nor_memory_data.sync, 0, 1),
	.count = Z_SEM_INITIALIZER(qspi_nor_memory_data.count, 0, K_SEM_MAX_LIMIT),
#endif /* CONFIG_MULTITHREADING */
};

NRF_DT_CHECK_NODE_HAS_PINCTRL_SLEEP(QSPI_NODE);

IF_ENABLED(CONFIG_PINCTRL, (PINCTRL_DT_DEFINE(QSPI_NODE)));

/**
 * @brief Converts NRFX return codes to the zephyr ones
 */
static inline int qspi_get_zephyr_ret_code(nrfx_err_t res)
{
	switch (res) {
	case NRFX_SUCCESS:
		return 0;
	case NRFX_ERROR_INVALID_PARAM:
	case NRFX_ERROR_INVALID_ADDR:
		return -EINVAL;
	case NRFX_ERROR_INVALID_STATE:
		return -ECANCELED;
	case NRFX_ERROR_BUSY:
	case NRFX_ERROR_TIMEOUT:
	default:
		return -EBUSY;
	}
}

static inline struct qspi_nor_data *get_dev_data(const struct device *dev)
{
	return dev->data;
}

static inline void qspi_lock(const struct device *dev)
{
#ifdef CONFIG_MULTITHREADING
	struct qspi_nor_data *dev_data = get_dev_data(dev);

	k_sem_take(&dev_data->sem, K_FOREVER);
#else /* CONFIG_MULTITHREADING */
	ARG_UNUSED(dev);
#endif /* CONFIG_MULTITHREADING */
}

static inline void qspi_unlock(const struct device *dev)
{
#ifdef CONFIG_MULTITHREADING
	struct qspi_nor_data *dev_data = get_dev_data(dev);

	k_sem_give(&dev_data->sem);
#else /* CONFIG_MULTITHREADING */
	ARG_UNUSED(dev);
#endif /* CONFIG_MULTITHREADING */
}

static inline void qspi_trans_lock(const struct device *dev)
{
#ifdef CONFIG_MULTITHREADING
	struct qspi_nor_data *dev_data = get_dev_data(dev);

	k_sem_take(&dev_data->trans, K_FOREVER);
#else /* CONFIG_MULTITHREADING */
	ARG_UNUSED(dev);
#endif /* CONFIG_MULTITHREADING */
}

static inline void qspi_trans_unlock(const struct device *dev)
{
#ifdef CONFIG_MULTITHREADING
	struct qspi_nor_data *dev_data = get_dev_data(dev);

	k_sem_give(&dev_data->trans);
#else /* CONFIG_MULTITHREADING */
	ARG_UNUSED(dev);
#endif /* CONFIG_MULTITHREADING */
}

static inline void qspi_wait_for_completion(const struct device *dev, nrfx_err_t res)
{
	struct qspi_nor_data *dev_data = get_dev_data(dev);

	if (res == NRFX_SUCCESS) {
#ifdef CONFIG_MULTITHREADING
		k_sem_take(&dev_data->sync, K_FOREVER);
#else /* CONFIG_MULTITHREADING */
		unsigned int key = irq_lock();

		while (!dev_data->ready) {
			k_cpu_atomic_idle(key);
			key = irq_lock();
		}

		dev_data->ready = false;
		irq_unlock(key);
#endif /* CONFIG_MULTITHREADING */
	}
}

static inline void qspi_complete(struct qspi_nor_data *dev_data)
{
#ifdef CONFIG_MULTITHREADING
	k_sem_give(&dev_data->sync);
#else /* CONFIG_MULTITHREADING */
	dev_data->ready = true;
#endif /* CONFIG_MULTITHREADING */
}

static inline void _qspi_complete(struct qspi_nor_data *dev_data)
{
	if (!qspi_config->easydma)
		return;

	qspi_complete(dev_data);
}
static inline void _qspi_wait_for_completion(const struct device *dev, nrfx_err_t res)
{
	if (!qspi_config->easydma)
		return;

	qspi_wait_for_completion(dev, res);
}

/**
 * @brief QSPI handler
 *
 * @param event Driver event type
 * @param p_context Pointer to context. Use in interrupt handler.
 * @retval None
 */
static void qspi_handler(nrfx_qspi_evt_t event, void *p_context)
{
	struct qspi_nor_data *dev_data = p_context;

	if (event == NRFX_QSPI_EVENT_DONE)
		_qspi_complete(dev_data);
}

static bool qspi_initialized;

static int qspi_device_init(const struct device *dev)
{
	struct qspi_nor_data *dev_data = get_dev_data(dev);
	nrfx_err_t res;
	int ret = 0;

	if (!IS_ENABLED(CONFIG_NRF700X_QSPI_LOW_POWER)) {
		return 0;
	}

	qspi_lock(dev);

	/* In multithreading, driver can call qspi_device_init more than once
	 * before calling qspi_device_uninit. Keepping count, so QSPI is
	 * uninitialized only at the last call (count == 0).
	 */
#ifdef CONFIG_MULTITHREADING
	k_sem_give(&dev_data->count);
#endif

	if (!qspi_initialized) {
		res = nrfx_qspi_init(&QSPIconfig, qspi_handler, dev_data);
		ret = qspi_get_zephyr_ret_code(res);
		NRF_QSPI->IFTIMING |= qspi_config->RDC4IO;
		qspi_initialized = (ret == 0);
	}

	qspi_unlock(dev);

	return ret;
}

static void qspi_device_uninit(const struct device *dev)
{
	bool last = true;

	if (!IS_ENABLED(CONFIG_NRF700X_QSPI_LOW_POWER)) {
		return;
	}

	qspi_lock(dev);

#ifdef CONFIG_MULTITHREADING
	struct qspi_nor_data *dev_data = get_dev_data(dev);

	/* The last thread to finish using the driver uninit the QSPI */
	(void)k_sem_take(&dev_data->count, K_NO_WAIT);
	last = (k_sem_count_get(&dev_data->count) == 0);
#endif

	if (last) {
		while (nrfx_qspi_mem_busy_check() != NRFX_SUCCESS) {
			if (IS_ENABLED(CONFIG_MULTITHREADING))
				k_msleep(50);
			else
				k_busy_wait(50000);
		}

		nrfx_qspi_uninit();

#ifndef CONFIG_PINCTRL
		nrf_gpio_cfg_output(QSPI_PROP_AT(csn_pins, 0));
		nrf_gpio_pin_set(QSPI_PROP_AT(csn_pins, 0));
#endif

		qspi_initialized = false;
	}

	qspi_unlock(dev);
}

/* QSPI send custom command.
 *
 * If this is used for both send and receive the buffer sizes must be
 * equal and cover the whole transaction.
 */
static int qspi_send_cmd(const struct device *dev, const struct qspi_cmd *cmd, bool wren)
{
	/* Check input parameters */
	if (!cmd)
		return -EINVAL;

	const void *tx_buf = NULL;
	size_t tx_len = 0;
	void *rx_buf = NULL;
	size_t rx_len = 0;
	size_t xfer_len = sizeof(cmd->op_code);

	if (cmd->tx_buf) {
		tx_buf = cmd->tx_buf->buf;
		tx_len = cmd->tx_buf->len;
	}

	if (cmd->rx_buf) {
		rx_buf = cmd->rx_buf->buf;
		rx_len = cmd->rx_buf->len;
	}

	if ((rx_len != 0) && (tx_len != 0)) {
		if (rx_len != tx_len)
			return -EINVAL;

		xfer_len += tx_len;
	} else
		/* At least one of these is zero. */
		xfer_len += tx_len + rx_len;

	if (xfer_len > NRF_QSPI_CINSTR_LEN_9B) {
		LOG_WRN("cinstr %02x transfer too long: %zu", cmd->op_code, xfer_len);

		return -EINVAL;
	}

	nrf_qspi_cinstr_conf_t cinstr_cfg = {
		.opcode = cmd->op_code,
		.length = xfer_len,
		.io2_level = true,
		.io3_level = true,
		.wipwait = false,
		.wren = wren,
	};

	qspi_lock(dev);

	int res = nrfx_qspi_cinstr_xfer(&cinstr_cfg, tx_buf, rx_buf);

	qspi_unlock(dev);
	return qspi_get_zephyr_ret_code(res);
}

/* RDSR wrapper.  Negative value is error. */
static int qspi_rdsr(const struct device *dev)
{
	uint8_t sr = -1;
	const struct qspi_buf sr_buf = {
		.buf = &sr,
		.len = sizeof(sr),
	};
	struct qspi_cmd cmd = {
		.op_code = SPI_NOR_CMD_RDSR,
		.rx_buf = &sr_buf,
	};
	int ret = qspi_send_cmd(dev, &cmd, false);

	return (ret < 0) ? ret : sr;
}

/* Wait until RDSR confirms write is not in progress. */
static int qspi_wait_while_writing(const struct device *dev)
{
	int ret;

	do {
		ret = qspi_rdsr(dev);
	} while ((ret >= 0) && ((ret & SPI_NOR_WIP_BIT) != 0U));

	return (ret < 0) ? ret : 0;
}

/**
 * @brief Fills init struct
 *
 * @param config Pointer to the config struct provided by user
 * @param initstruct Pointer to the configuration struct
 * @retval None
 */
static inline void qspi_fill_init_struct(nrfx_qspi_config_t *initstruct)
{
	bool quad_mode;

	/* Configure XIP offset */
	initstruct->xip_offset = 0;

#ifdef CONFIG_PINCTRL
	initstruct->skip_gpio_cfg = true,
	initstruct->skip_psel_cfg = true,
#else
	/* Configure pins */
	initstruct->pins.sck_pin = DT_PROP(QSPI_NODE, sck_pin);
	initstruct->pins.csn_pin = QSPI_PROP_AT(csn_pins, 0);
	initstruct->pins.io0_pin = QSPI_PROP_AT(io_pins, 0);
	initstruct->pins.io1_pin = QSPI_PROP_AT(io_pins, 1);
#if QSPI_PROP_LEN(io_pins) > 2
	initstruct->pins.io2_pin = QSPI_PROP_AT(io_pins, 2);
	initstruct->pins.io3_pin = QSPI_PROP_AT(io_pins, 3);
#else
	initstruct->pins.io2_pin = NRF_QSPI_PIN_NOT_CONNECTED;
	initstruct->pins.io3_pin = NRF_QSPI_PIN_NOT_CONNECTED;
#endif
#endif /* CONFIG_PINCTRL */
	/* Configure Protocol interface */
	initstruct->prot_if.addrmode = NRF_QSPI_ADDRMODE_24BIT;

	initstruct->prot_if.dpmconfig = false;

	/* Configure physical interface */
	initstruct->phy_if.sck_freq = qspi_dts_freq_to_sckfreq();
	/* Using MHZ fails checkpatch constant check */
	if (INST_0_SCK_FREQUENCY >= 16000000) {
		qspi_config->qspi_slave_latency = 1;
	}
	initstruct->phy_if.sck_delay = QSPI_SCK_DELAY;
	initstruct->phy_if.spi_mode = qspi_get_mode(DT_INST_PROP(0, cpol), DT_INST_PROP(0, cpha));

	quad_mode = DT_INST_PROP(0, quad_mode);
	if (quad_mode) {
		initstruct->prot_if.readoc = NRF_QSPI_READOC_READ4IO;
		initstruct->prot_if.writeoc = NRF_QSPI_WRITEOC_PP4IO;
	} else {
		initstruct->prot_if.readoc = NRF_QSPI_READOC_FASTREAD;
		initstruct->prot_if.writeoc = NRF_QSPI_WRITEOC_PP;
	}

	initstruct->phy_if.dpmen = false;
}

/* Configures QSPI memory for the transfer */
static int qspi_nrfx_configure(const struct device *dev)
{
	if (!dev)
		return -ENXIO;

	struct qspi_nor_data *dev_data = dev->data;

	qspi_fill_init_struct(&QSPIconfig);

	nrfx_err_t res = _nrfx_qspi_init(&QSPIconfig, qspi_handler, dev_data);
	int ret = qspi_get_zephyr_ret_code(res);

	if (ret == 0) {
		/* Set QE to match transfer mode. If not using quad
		 * it's OK to leave QE set, but doing so prevents use
		 * of WP#/RESET#/HOLD# which might be useful.
		 *
		 * Note build assert above ensures QER is S1B6.  Other
		 * options require more logic.
		 */
		ret = qspi_rdsr(dev);

		if (ret < 0) {
			LOG_ERR("RDSR failed: %d", ret);
			return ret;
		}

		uint8_t sr = (uint8_t)ret;
		bool qe_value = (qspi_write_is_quad(QSPIconfig.prot_if.writeoc)) ||
				(qspi_read_is_quad(QSPIconfig.prot_if.readoc));
		const uint8_t qe_mask = BIT(6); /* only S1B6 */
		bool qe_state = ((sr & qe_mask) != 0U);

		LOG_DBG("RDSR %02x QE %d need %d: %s", sr, qe_state, qe_value,
			(qe_state != qe_value) ? "updating" : "no-change");

		ret = 0;

		if (qe_state != qe_value) {
			const struct qspi_buf sr_buf = {
				.buf = &sr,
				.len = sizeof(sr),
			};
			struct qspi_cmd cmd = {
				.op_code = SPI_NOR_CMD_WRSR,
				.tx_buf = &sr_buf,
			};

			sr ^= qe_mask;
			ret = qspi_send_cmd(dev, &cmd, true);

			/* Writing SR can take some time, and further
			 * commands sent while it's happening can be
			 * corrupted.  Wait.
			 */
			if (ret == 0)
				ret = qspi_wait_while_writing(dev);
		}

		if (ret < 0)
			LOG_ERR("QE %s failed: %d", qe_value ? "set" : "clear", ret);
	}

	return ret;
}

static inline nrfx_err_t read_non_aligned(const struct device *dev, int addr, void *dest,
					  size_t size)
{
	uint8_t __aligned(WORD_SIZE) buf[WORD_SIZE * 2];
	uint8_t *dptr = dest;

	int flash_prefix = (WORD_SIZE - (addr % WORD_SIZE)) % WORD_SIZE;

	if (flash_prefix > size)
		flash_prefix = size;

	int dest_prefix = (WORD_SIZE - (int)dptr % WORD_SIZE) % WORD_SIZE;

	if (dest_prefix > size)
		dest_prefix = size;

	int flash_suffix = (size - flash_prefix) % WORD_SIZE;
	int flash_middle = size - flash_prefix - flash_suffix;
	int dest_middle = size - dest_prefix - (size - dest_prefix) % WORD_SIZE;

	if (flash_middle > dest_middle) {
		flash_middle = dest_middle;
		flash_suffix = size - flash_prefix - flash_middle;
	}

	nrfx_err_t res = NRFX_SUCCESS;

	/* read from aligned flash to aligned memory */
	if (flash_middle != 0) {
		res = _nrfx_qspi_read(dptr + dest_prefix, flash_middle, addr + flash_prefix);

		_qspi_wait_for_completion(dev, res);

		if (res != NRFX_SUCCESS)
			return res;

		/* perform shift in RAM */
		if (flash_prefix != dest_prefix)
			memmove(dptr + flash_prefix, dptr + dest_prefix, flash_middle);
	}

	/* read prefix */
	if (flash_prefix != 0) {
		res = _nrfx_qspi_read(buf, WORD_SIZE, addr - (WORD_SIZE - flash_prefix));

		_qspi_wait_for_completion(dev, res);

		if (res != NRFX_SUCCESS)
			return res;

		memcpy(dptr, buf + WORD_SIZE - flash_prefix, flash_prefix);
	}

	/* read suffix */
	if (flash_suffix != 0) {
		res = _nrfx_qspi_read(buf, WORD_SIZE * 2, addr + flash_prefix + flash_middle);

		_qspi_wait_for_completion(dev, res);

		if (res != NRFX_SUCCESS)
			return res;

		memcpy(dptr + flash_prefix + flash_middle, buf, flash_suffix);
	}

	return res;
}

static int qspi_nor_read(const struct device *dev, int addr, void *dest, size_t size)
{
	if (!dest)
		return -EINVAL;

	/* read size must be non-zero */
	if (!size)
		return 0;

	int rc = qspi_device_init(dev);

	if (rc != 0)
		goto out;

	qspi_lock(dev);

	nrfx_err_t res = read_non_aligned(dev, addr, dest, size);

	qspi_unlock(dev);

	rc = qspi_get_zephyr_ret_code(res);

out:
	qspi_device_uninit(dev);
	return rc;
}

/* addr aligned, sptr not null, slen less than 4 */
static inline nrfx_err_t write_sub_word(const struct device *dev, int addr, const void *sptr,
					size_t slen)
{
	uint8_t __aligned(4) buf[4];
	nrfx_err_t res;

	/* read out the whole word so that unchanged data can be
	 * written back
	 */
	res = _nrfx_qspi_read(buf, sizeof(buf), addr);
	_qspi_wait_for_completion(dev, res);

	if (res == NRFX_SUCCESS) {
		memcpy(buf, sptr, slen);
		res = _nrfx_qspi_write(buf, sizeof(buf), addr);
		_qspi_wait_for_completion(dev, res);
	}

	return res;
}

static int qspi_nor_write(const struct device *dev, int addr, const void *src, size_t size)
{
	if (!src)
		return -EINVAL;

	/* write size must be non-zero, less than 4, or a multiple of 4 */
	if ((size == 0) || ((size > 4) && ((size % 4U) != 0)))
		return -EINVAL;

	/* address must be 4-byte aligned */
	if ((addr % 4U) != 0)
		return -EINVAL;

	nrfx_err_t res = NRFX_SUCCESS;

	int rc = qspi_device_init(dev);

	if (rc != 0)
		goto out;

	qspi_trans_lock(dev);

	qspi_lock(dev);

	if (size < 4U)
		res = write_sub_word(dev, addr, src, size);
	else {
		res = _nrfx_qspi_write(src, size, addr);
		_qspi_wait_for_completion(dev, res);
	}

	qspi_unlock(dev);

	qspi_trans_unlock(dev);

	rc = qspi_get_zephyr_ret_code(res);
out:
	qspi_device_uninit(dev);
	return rc;
}

/**
 * @brief Configure the flash
 *
 * @param dev The flash device structure
 * @param info The flash info structure
 * @return 0 on success, negative errno code otherwise
 */
static int qspi_nor_configure(const struct device *dev)
{
	int ret = qspi_nrfx_configure(dev);

	if (ret != 0)
		return ret;

	qspi_device_uninit(dev);

	return 0;
}

/**
 * @brief Initialize and configure the flash
 *
 * @param name The flash name
 * @return 0 on success, negative errno code otherwise
 */
static int qspi_nor_init(const struct device *dev)
{
#if NRF_CLOCK_HAS_HFCLK192M
	/* Make sure the PCLK192M clock, from which the SCK frequency is
	 * derived, is not prescaled (the default setting after reset is
	 * "divide by 4").
	 */
	nrf_clock_hfclk192m_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_1);
#endif

#ifdef CONFIG_PINCTRL
	int ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(QSPI_NODE), PINCTRL_STATE_DEFAULT);

	if (ret < 0) {
		return ret;
	}
#endif

	IRQ_CONNECT(DT_IRQN(QSPI_NODE), DT_IRQ(QSPI_NODE, priority), nrfx_isr,
		    nrfx_qspi_irq_handler, 0);
	return qspi_nor_configure(dev);
}

#if defined(CONFIG_SOC_SERIES_NRF53X)
static int qspi_cmd_encryption(const struct device *dev, nrf_qspi_encryption_t *p_cfg)
{
	const struct qspi_buf tx_buf = { .buf = (uint8_t *)&p_cfg->nonce[1],
					 .len = sizeof(p_cfg->nonce[1]) };
	const struct qspi_cmd cmd = {
		.op_code = 0x4f,
		.tx_buf = &tx_buf,
	};

	int ret = qspi_device_init(dev);

	if (ret == 0)
		ret = qspi_send_cmd(dev, &cmd, false);

	qspi_device_uninit(dev);

	if (ret < 0)
		LOG_DBG("cmd_encryption failed %d\n", ret);

	return ret;
}
#endif

int qspi_RDSR2(const struct device *dev, uint8_t *rdsr2)
{
	int ret = 0;
	uint8_t sr = 0;

	const struct qspi_buf sr_buf = {
		.buf = &sr,
		.len = sizeof(sr),
	};
	struct qspi_cmd cmd = {
		.op_code = 0x2f,
		.rx_buf = &sr_buf,
	};

	ret = qspi_device_init(dev);

	ret = qspi_send_cmd(dev, &cmd, false);

	qspi_device_uninit(dev);

	LOG_DBG("RDSR2 = 0x%x\n", sr);

	if (ret == 0)
		*rdsr2 = sr;

	return ret;
}

/* Wait until RDSR2 confirms RPU_WAKE write is successful */
int qspi_validate_rpu_wake_writecmd(const struct device *dev)
{
	int ret = 0;
	uint8_t rdsr2 = 0;

	for (int ii = 0; ii < 1; ii++) {
		ret = qspi_RDSR2(dev, &rdsr2);
		if (ret && (rdsr2 & RPU_WAKEUP_NOW)) {
			return 0;
		}
	}

	return rdsr2;
}


int qspi_RDSR1(const struct device *dev, uint8_t *rdsr1)
{
	int ret = 0;
	uint8_t sr = 0;

	const struct qspi_buf sr_buf = {
		.buf = &sr,
		.len = sizeof(sr),
	};
	struct qspi_cmd cmd = {
		.op_code = 0x1f,
		.rx_buf = &sr_buf,
	};

	ret = qspi_device_init(dev);

	ret = qspi_send_cmd(dev, &cmd, false);

	qspi_device_uninit(dev);

	LOG_DBG("RDSR2 = 0x%x\n", sr);

	if (ret == 0)
		*rdsr1 = sr;

	return ret;
}

/* Wait until RDSR1 confirms RPU_AWAKE/RPU_READY */
int qspi_wait_while_rpu_awake(const struct device *dev)
{
	int ret;
	uint8_t val = 0;

	for (int ii = 0; ii < 10; ii++) {
		ret = qspi_RDSR1(dev, &val);

		LOG_DBG("RDSR1 = 0x%x\n", val);

		if (!ret && (val & RPU_AWAKE_BIT)) {
			break;
		}

		k_msleep(1);
	}

	/* Configure DTS settings */
	if (val & RPU_AWAKE_BIT) {
		/* Restore QSPI clock frequency from DTS */
		QSPIconfig.phy_if.sck_freq = qspi_dts_freq_to_sckfreq();
	}

	return val;
}

/* Wait until RDSR1 confirms RPU_AWAKE/RPU_READY and Firmware is booted */
int qspi_wait_while_firmware_awake(const struct device *dev)
{
	int ret = 0;
	uint8_t sr = 0;

	const struct qspi_buf sr_buf = {
		.buf = &sr,
		.len = sizeof(sr),
	};
	struct qspi_cmd cmd = {
		.op_code = 0x1f,
		.rx_buf = &sr_buf,
	};

	for (int ii = 0; ii < 10; ii++) {
		int ret;

		ret = qspi_device_init(dev);

		if (ret == 0)
			ret = qspi_send_cmd(dev, &cmd, false);

		qspi_device_uninit(dev);

		if ((ret < 0) || (sr != 0x6)) {
			LOG_DBG("ret val = 0x%x\t RDSR1 = 0x%x\n", ret, sr);
		} else {
			LOG_DBG("RDSR1 = 0x%x\n", sr);
			LOG_INF("RPU is awake...\n");
			break;
		}
		k_msleep(1);
	}

	return ret;
}

int qspi_WRSR2(const struct device *dev, uint8_t data)
{
	const struct qspi_buf tx_buf = {
		.buf = &data,
		.len = sizeof(data),
	};
	const struct qspi_cmd cmd = {
		.op_code = 0x3f,
		.tx_buf = &tx_buf,
	};
	int ret = qspi_device_init(dev);

	if (ret == 0)
		ret = qspi_send_cmd(dev, &cmd, false);

	qspi_device_uninit(dev);

	if (ret < 0)
		LOG_ERR("cmd_wakeup RPU failed %d\n", ret);

	return ret;
}

int qspi_cmd_wakeup_rpu(const struct device *dev, uint8_t data)
{
	int ret;

	/* Waking RPU works reliably only with lowest frequency (8MHz) */
	QSPIconfig.phy_if.sck_freq = qspi_freq_mhz_to_sckfreq(8);

	ret = qspi_WRSR2(dev, data);

	return ret;
}

struct device qspi_perip = {
	.data = &qspi_nor_memory_data,
};

int qspi_deinit(void)
{
	LOG_DBG("TODO : %s\n", __func__);

	return 0;
}

int qspi_init(struct qspi_config *config)
{
	unsigned int rc;

	qspi_config = config;

	config->readoc = config->quad_spi ? NRF_QSPI_READOC_READ4IO : NRF_QSPI_READOC_FASTREAD;
	config->writeoc = config->quad_spi ? NRF_QSPI_WRITEOC_PP4IO : NRF_QSPI_WRITEOC_PP;

	rc = qspi_nor_init(&qspi_perip);

	k_sem_init(&qspi_config->lock, 1, 1);

	return rc;
}

void qspi_update_nonce(unsigned int addr, int len, int hlread)
{
#if defined(CONFIG_SOC_SERIES_NRF53X)

	NRF_QSPI_Type *p_reg = NRF_QSPI;

	if (!qspi_config->encryption)
		return;

	if (nonce_last_addr == 0 || hlread)
		p_reg->DMA_ENC.NONCE2 = ++nonce_cnt;
	else if ((nonce_last_addr + 4) != addr)
		p_reg->DMA_ENC.NONCE2 = ++nonce_cnt;

	nonce_last_addr = addr + len - 4;

#endif
}

void qspi_addr_check(unsigned int addr, const void *data, unsigned int len)
{
	if ((addr % 4 != 0) || (((unsigned int)data) % 4 != 0) || (len % 4 != 0)) {
		LOG_ERR("%s : Unaligned address %x %x %d %x %x\n", __func__, addr,
		       (unsigned int)data, (addr % 4 != 0), (((unsigned int)data) % 4 != 0),
		       (len % 4 != 0));
	}
}

int qspi_write(unsigned int addr, const void *data, int len)
{
	int status;

	qspi_addr_check(addr, data, len);

	addr |= qspi_config->addrmask;

	k_sem_take(&qspi_config->lock, K_FOREVER);

	qspi_update_nonce(addr, len, 0);

	status = qspi_nor_write(&qspi_perip, addr, data, len);

	k_sem_give(&qspi_config->lock);

	return status;
}

int qspi_read(unsigned int addr, void *data, int len)
{
	int status;

	qspi_addr_check(addr, data, len);

	addr |= qspi_config->addrmask;

	k_sem_take(&qspi_config->lock, K_FOREVER);

	qspi_update_nonce(addr, len, 0);

	status = qspi_nor_read(&qspi_perip, addr, data, len);

	k_sem_give(&qspi_config->lock);

	return status;
}

int qspi_hl_readw(unsigned int addr, void *data)
{
	int status;
	uint8_t *rxb = NULL;
	uint32_t len = 4;

	len = len + (4 * qspi_config->qspi_slave_latency);

	rxb = k_malloc(len);

	if (rxb == NULL) {
		LOG_ERR("%s: ERROR ENOMEM line %d\n", __func__, __LINE__);
		return -ENOMEM;
	}

	memset(rxb, 0, len);

	k_sem_take(&qspi_config->lock, K_FOREVER);

	qspi_update_nonce(addr, 4, 1);

	status = qspi_nor_read(&qspi_perip, addr, rxb, len);

	k_sem_give(&qspi_config->lock);

	*(uint32_t *)data = *(uint32_t *)(rxb + (len - 4));

	k_free(rxb);

	return status;
}

int qspi_hl_read(unsigned int addr, void *data, int len)
{
	int count = 0;

	qspi_addr_check(addr, data, len);

	while (count < (len / 4)) {
		qspi_hl_readw(addr + (4 * count), ((char *)data + (4 * count)));
		count++;
	}

	return 0;
}

int qspi_cmd_sleep_rpu(const struct device *dev)
{
	uint8_t data = 0x0;

	/* printf("TODO : %s:\n", __func__); */
	const struct qspi_buf tx_buf = {
		.buf = &data,
		.len = sizeof(data),
	};

	const struct qspi_cmd cmd = {
		.op_code = 0x3f, /* 0x3f, //WRSR2(0x3F) WakeUP RPU. */
		.tx_buf = &tx_buf,
	};

	int ret = qspi_device_init(dev);

	if (ret == 0) {
		ret = qspi_send_cmd(dev, &cmd, false);
	}

	qspi_device_uninit(dev);

	if (ret < 0) {
		LOG_ERR("cmd_wakeup RPU failed: %d\n", ret);
	}

	return ret;
}

/* Encryption public API */

int qspi_enable_encryption(uint8_t *key)
{
#if defined(CONFIG_SOC_SERIES_NRF53X)
	int err = 0;

	if (qspi_config->encryption)
		return -EALREADY;

	memcpy(qspi_config->p_cfg.key, key, 16);

	err = nrfx_qspi_dma_encrypt(&qspi_config->p_cfg);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_qspi_dma_encrypt failed: %d\n", err);
		return -EIO;
	}

	err = qspi_cmd_encryption(&qspi_perip, &qspi_config->p_cfg);
	if (err != 0) {
		LOG_ERR("qspi_cmd_encryption failed: %d\n", err);
		return -EIO;
	}

	qspi_config->encryption = true;

	return 0;
#else
	return -ENOTSUP;
#endif
}
