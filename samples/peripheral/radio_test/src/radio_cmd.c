/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>

#include <errno.h>
#include <init.h>
#include <shell/shell.h>
#include <zephyr/types.h>
#include <hal/nrf_power.h>

#if CONFIG_NRF21540_FEM
#include "nrf21540.h"
#endif

#include "radio_test.h"

#if NRF_POWER_HAS_DCDCEN_VDDH
	#define TOGGLE_DCDC_HELP			\
	"Toggle DCDC state <state>, "			\
	"if state = 1 then toggle DC/DC state, or "	\
	"if state = 0 then toggle DC/DC VDDH state"
#elif NRF_POWER_HAS_DCDCEN
	#define TOGGLE_DCDC_HELP			\
	"Toggle DCDC state <state>, "			\
	"Toggle DC/DC state regardless of state value"
#endif

/* Radio parameter configuration. */
static struct radio_param_config {
	/** Radio transmission pattern. */
	enum transmit_pattern tx_pattern;

	/** Radio mode. Data rate and modulation. */
	nrf_radio_mode_t mode;

	/** Radio output power. */
	uint8_t txpower;

	/** Radio start channel (frequency). */
	uint8_t channel_start;

	/** Radio end channel (frequency). */
	uint8_t channel_end;

	/** Delay time in milliseconds. */
	uint32_t delay_ms;

	/** Duty cycle. */
	uint32_t duty_cycle;

#if CONFIG_NRF21540_FEM
	/* nRF21540 configuration. */
	struct radio_test_nrf21540 nrf21540;
#endif /* CONFIG_NRF21540_FEM */
} config = {
	.tx_pattern = TRANSMIT_PATTERN_RANDOM,
	.mode = NRF_RADIO_MODE_BLE_1MBIT,
	.txpower = RADIO_TXPOWER_TXPOWER_0dBm,
	.channel_start = 0,
	.channel_end = 80,
	.delay_ms = 10,
	.duty_cycle = 50,
#if CONFIG_NRF21540_FEM
	.nrf21540.gain = NRF21540_USE_DEFAULT_GAIN
#endif /* CONFIG_NRF21540_FEM */
};

/* Radio test configuration. */
static struct radio_test_config test_config;

/* If true, RX sweep, TX sweep or duty cycle test is performed. */
static bool test_in_progress;

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
static void ieee_channel_check(const struct shell *shell, uint8_t channel)
{
	if (config.mode == NRF_RADIO_MODE_IEEE802154_250KBIT) {
		if ((channel < IEEE_MIN_CHANNEL) ||
		    (channel > IEEE_MAX_CHANNEL)) {
			shell_print(shell,
				"For %s config.mode channel must be between %d and %d",
				STRINGIFY(NRF_RADIO_MODE_IEEE802154_250KBIT),
				IEEE_MIN_CHANNEL,
				IEEE_MAX_CHANNEL);

			shell_print(shell, "Channel set to %d",
				    IEEE_MIN_CHANNEL);
		}

	}
}
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

static int cmd_start_channel_set(const struct shell *shell, size_t argc,
				 char **argv)
{
	uint32_t channel;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	channel = atoi(argv[1]);

	if (channel > 80) {
		shell_error(shell, "Channel must be between 0 and 80");
		return -EINVAL;
	}

	config.channel_start = (uint8_t) channel;

	shell_print(shell, "Start channel set to: %d", channel);
	return 0;
}

static int cmd_end_channel_set(const struct shell *shell, size_t argc,
			       char **argv)
{
	uint32_t channel;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	channel = atoi(argv[1]);

	if (channel > 80) {
		shell_error(shell, "Channel must be between 0 and 80");
		return -EINVAL;
	}

	config.channel_end = (uint8_t) channel;

	shell_print(shell, "End channel set to: %d", channel);
	return 0;
}

static int cmd_time_set(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t time;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	time = atoi(argv[1]);

	if (time > 99) {
		shell_error(shell, "Delay time must be between 0 and 99 ms");
		return -EINVAL;
	}

	config.delay_ms = time;

	shell_print(shell, "Delay time set to: %d", time);
	return 0;
}

static int cmd_cancel(const struct shell *shell, size_t argc, char **argv)
{
	radio_test_cancel();
	return 0;
}

static int cmd_data_rate_set(const struct shell *shell, size_t argc,
			     char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Uknown argument: %s", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_tx_carrier_start(const struct shell *shell, size_t argc,
				char **argv)
{
	if (test_in_progress) {
		radio_test_cancel();
		test_in_progress = false;
	}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = UNMODULATED_TX;
	test_config.mode = config.mode;
	test_config.params.unmodulated_tx.txpower = config.txpower;
	test_config.params.unmodulated_tx.channel = config.channel_start;
#if CONFIG_NRF21540_FEM
	test_config.nrf21540.active_delay = config.nrf21540.active_delay;
	test_config.nrf21540.gain = config.nrf21540.gain;
#endif
	radio_test_start(&test_config);

	shell_print(shell, "Start the TX carrier");
	return 0;
}

static void tx_modulated_carrier_end(void)
{
	printk("The modulated TX has finished\n");
}

static int cmd_tx_modulated_carrier_start(const struct shell *shell,
					  size_t argc,
					  char **argv)
{
	if (test_in_progress) {
		radio_test_cancel();
		test_in_progress = false;
	}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = MODULATED_TX;
	test_config.mode = config.mode;
	test_config.params.modulated_tx.txpower = config.txpower;
	test_config.params.modulated_tx.channel = config.channel_start;
	test_config.params.modulated_tx.pattern = config.tx_pattern;
#if CONFIG_NRF21540_FEM
	test_config.nrf21540.active_delay = config.nrf21540.active_delay;
	test_config.nrf21540.gain = config.nrf21540.gain;
#endif

	if (argc == 2) {
		test_config.params.modulated_tx.packets_num = atoi(argv[1]);
		test_config.params.modulated_tx.cb = tx_modulated_carrier_end;
	}

	radio_test_start(&test_config);

	shell_print(shell, "Start the modulated TX carrier");
	return 0;
}

static int cmd_duty_cycle_set(const struct shell *shell, size_t argc,
			      char **argv)
{
	uint32_t duty_cycle;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	duty_cycle = atoi(argv[1]);

	if (duty_cycle > 100) {
		shell_error(shell, "Duty cycle must be between 1 and 99.");
		return -EINVAL;
	}

	config.duty_cycle = duty_cycle;

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = MODULATED_TX_DUTY_CYCLE;
	test_config.mode = config.mode;
	test_config.params.modulated_tx_duty_cycle.txpower = config.txpower;
	test_config.params.modulated_tx_duty_cycle.pattern = config.tx_pattern;
	test_config.params.modulated_tx_duty_cycle.channel =
		config.channel_start;
	test_config.params.modulated_tx_duty_cycle.duty_cycle =
		config.duty_cycle;
#if CONFIG_NRF21540_FEM
	test_config.nrf21540.active_delay = config.nrf21540.active_delay;
	test_config.nrf21540.gain = config.nrf21540.gain;
#endif

	radio_test_start(&test_config);
	test_in_progress = true;

	return 0;
}

#if defined(TOGGLE_DCDC_HELP)
static int cmd_toggle_dc(const struct shell *shell, size_t argc, char **argv)
{
	uint32_t state;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	state = atoi(argv[1]);
	if (state > 1) {
		shell_error(shell, "Invalid DCDC value provided");
		return -EINVAL;
	}

	toggle_dcdc_state((uint8_t) state);

#if NRF_POWER_HAS_DCDCEN_VDDH
	shell_print(shell,
		"DCDC VDDH state %lu\n",
		"Write '0' to toggle state of DCDC REG0\n",
		"Write '1' to toggle state of DCDC REG1",
		nrf_power_dcdcen_vddh_get(NRF_POWER));
#endif /* NRF_POWER_HAS_DCDCEN_VDDH */

#if NRF_POWER_HAS_DCDCEN
	shell_print(shell,
		"DCDC state %lu\n",
		"Write '1' or '0' to toggle",
		nrf_power_dcdcen_get(NRF_POWER));
#endif /* NRF_POWER_HAS_DCDCEN */

	return 0;
}
#endif

static int cmd_output_power_set(const struct shell *shell, size_t argc,
				char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Uknown argument: %s", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_transmit_pattern_set(const struct shell *shell, size_t argc,
				    char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Uknown argument: %s.", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_print(const struct shell *shell, size_t argc, char **argv)
{
	shell_print(shell, "Parameters:");

	switch (config.mode) {
#if defined(RADIO_MODE_MODE_Nrf_250Kbit)
	case NRF_RADIO_MODE_NRF_250KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_250KBIT));
		break;

#endif /* defined(RADIO_MODE_MODE_Nrf_250Kbit) */
	case NRF_RADIO_MODE_NRF_1MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_1MBIT));
		break;

	case NRF_RADIO_MODE_NRF_2MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_NRF_2MBIT));
		break;

	case NRF_RADIO_MODE_BLE_1MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_1MBIT));
		break;

	case NRF_RADIO_MODE_BLE_2MBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_2MBIT));
		break;

#if CONFIG_HAS_HW_NRF_RADIO_BLE_CODED
	case NRF_RADIO_MODE_BLE_LR125KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_LR125KBIT));
		break;

	case NRF_RADIO_MODE_BLE_LR500KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_BLE_LR500KBIT));
		break;
#endif /* CONFIG_HAS_HW_NRF_RADIO_BLE_CODED */

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	case NRF_RADIO_MODE_IEEE802154_250KBIT:
		shell_print(shell,
			    "Data rate: %s",
			    STRINGIFY(NRF_RADIO_MODE_IEEE802154_250KBIT));
		break;
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	default:
		shell_print(shell,
			    "Data rate unknown or deprecated: %lu\n\r",
			    config.mode);
		break;
	}

	switch (config.txpower) {
#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
	case RADIO_TXPOWER_TXPOWER_Pos8dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos8dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos8dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos7dBm)
	case RADIO_TXPOWER_TXPOWER_Pos7dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos7dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos7dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos6dBm)
	case RADIO_TXPOWER_TXPOWER_Pos6dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos6dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos6dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos5dBm)
	case RADIO_TXPOWER_TXPOWER_Pos5dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos5dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos5dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos4dBm)
	case RADIO_TXPOWER_TXPOWER_Pos4dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos4dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos4dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos3dBm)
	case RADIO_TXPOWER_TXPOWER_Pos3dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos3dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos3dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos2dBm)
	case RADIO_TXPOWER_TXPOWER_Pos2dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos2dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos2dBm) */

	case RADIO_TXPOWER_TXPOWER_0dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_0dBm));
		break;

#if defined(RADIO_TXPOWER_TXPOWER_Neg1dBm)
	case RADIO_TXPOWER_TXPOWER_Neg1dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg1dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg1dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg2dBm)
	case RADIO_TXPOWER_TXPOWER_Neg2dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg2dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg2dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg3dBm)
	case RADIO_TXPOWER_TXPOWER_Neg3dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg3dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg3dBm) */

	case RADIO_TXPOWER_TXPOWER_Neg4dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg4dBm));
		break;

#if defined(RADIO_TXPOWER_TXPOWER_Neg5dBm)
	case RADIO_TXPOWER_TXPOWER_Neg5dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg5dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg5dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg6dBm)
	case RADIO_TXPOWER_TXPOWER_Neg6dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg6dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg6dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg7dBm)
	case RADIO_TXPOWER_TXPOWER_Neg7dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg7dBm));
		break;
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg7dBm) */

	case RADIO_TXPOWER_TXPOWER_Neg8dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg8dBm));
		break;

	case RADIO_TXPOWER_TXPOWER_Neg12dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg12dBm));
		break;

	case RADIO_TXPOWER_TXPOWER_Neg16dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg16dBm));
		break;

	case RADIO_TXPOWER_TXPOWER_Neg20dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg20dBm));
		break;

	case RADIO_TXPOWER_TXPOWER_Neg40dBm:
		shell_print(shell,
			    "TX power: %s",
			    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg40dBm));
		break;

	default:
		shell_print(shell,
			    "TX power unknown: %d",
			    config.txpower);
		break;
	}

	switch (config.tx_pattern) {
	case TRANSMIT_PATTERN_RANDOM:
		shell_print(shell,
			    "Transmission pattern: %s",
			    STRINGIFY(TRANSMIT_PATTERN_RANDOM));
		break;

	case TRANSMIT_PATTERN_11110000:
		shell_print(shell,
			    "Transmission pattern: %s",
			    STRINGIFY(TRANSMIT_PATTERN_11110000));
		break;

	case TRANSMIT_PATTERN_11001100:
		shell_print(shell,
			    "Transmission pattern: %s",
			    STRINGIFY(TRANSMIT_PATTERN_11001100));
		break;

	default:
		shell_print(shell,
			    "Transmission pattern unknown: %d",
			    config.tx_pattern);
		break;
	}

	shell_print(shell,
		"Start Channel: %lu\n"
		"End Channel: %lu\n"
		"Time on each channel: %lu ms\n"
		"Duty cycle: %lu percent\n",
		config.channel_start,
		config.channel_end,
		config.delay_ms,
		config.duty_cycle);

	return 0;
}

static int cmd_rx_sweep_start(const struct shell *shell, size_t argc,
			      char **argv)
{
	memset(&test_config, 0, sizeof(test_config));
	test_config.type = RX_SWEEP;
	test_config.mode = config.mode;
	test_config.params.rx_sweep.channel_start = config.channel_start;
	test_config.params.rx_sweep.channel_end = config.channel_end;
	test_config.params.rx_sweep.delay_ms = config.delay_ms;
#if CONFIG_NRF21540_FEM
	test_config.nrf21540.active_delay = config.nrf21540.active_delay;
	test_config.nrf21540.gain = config.nrf21540.gain;
#endif

	radio_test_start(&test_config);

	test_in_progress = true;

	shell_print(shell, "RX sweep");
	return 0;
}

static int cmd_tx_sweep_start(const struct shell *shell, size_t argc,
			      char **argv)
{
	memset(&test_config, 0, sizeof(test_config));
	test_config.type = TX_SWEEP;
	test_config.mode = config.mode;
	test_config.params.tx_sweep.channel_start = config.channel_start;
	test_config.params.tx_sweep.channel_end = config.channel_end;
	test_config.params.tx_sweep.delay_ms = config.delay_ms;
	test_config.params.tx_sweep.txpower = config.txpower;
#if CONFIG_NRF21540_FEM
	test_config.nrf21540.active_delay = config.nrf21540.active_delay;
	test_config.nrf21540.gain = config.nrf21540.gain;
#endif

	radio_test_start(&test_config);

	test_in_progress = true;

	shell_print(shell, "TX sweep");
	return 0;
}

static int cmd_rx_start(const struct shell *shell, size_t argc, char **argv)
{
	if (test_in_progress) {
		radio_test_cancel();
		test_in_progress = false;
	}

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	ieee_channel_check(shell, config.channel_start);
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	memset(&test_config, 0, sizeof(test_config));
	test_config.type = RX;
	test_config.mode = config.mode;
	test_config.params.rx.channel = config.channel_start;
	test_config.params.rx.pattern = config.tx_pattern;
#if CONFIG_NRF21540_FEM
	test_config.nrf21540.active_delay = config.nrf21540.active_delay;
	test_config.nrf21540.gain = config.nrf21540.gain;
#endif

	radio_test_start(&test_config);

	return 0;
}

#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
static void cmd_pos8dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Pos8dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos8dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos8dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos7dBm)
static void cmd_pos7dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Pos7dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos7dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos7dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos6dBm)
static void cmd_pos6dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Pos6dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos6dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos6dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos5dBm)
static void cmd_pos5dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Pos5dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos5dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos5dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos2dBm)
static void cmd_pos2dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Pos2dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos2dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos2dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos3dBm)
static void cmd_pos3dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Pos3dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos3dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos3dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Pos4dBm)
static void cmd_pos4dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Pos4dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Pos4dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos4dBm) */

static void cmd_pos0dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_0dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_0dBm));
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg1dBm)
static void cmd_neg1dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg1dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg1dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg1dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg2dBm)
static void cmd_neg2dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg2dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg2dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg2dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg3dBm)
static void cmd_neg3dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg3dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg3dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg3dBm) */

static void cmd_neg4dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg4dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg4dBm));
}

#if defined(RADIO_TXPOWER_TXPOWER_Neg5dBm)
static void cmd_neg5dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg5dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg5dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg5dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg6dBm)
static void cmd_neg6dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg6dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg6dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg6dBm) */

#if defined(RADIO_TXPOWER_TXPOWER_Neg7dBm)
static void cmd_neg7dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg7dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg7dBm));
}
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg7dBm) */

static void cmd_neg8dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg8dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg8dBm));
}

static void cmd_neg12dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg12dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg12dBm));
}


static void cmd_neg16dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg16dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg16dBm));
}

static void cmd_neg20dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg20dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg20dBm));
}

static void cmd_neg30dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg30dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg30dBm));
}

static void cmd_neg40dbm(const struct shell *shell, size_t argc, char **argv)
{
	config.txpower = RADIO_TXPOWER_TXPOWER_Neg40dBm;
	shell_print(shell, "TX power: %s",
		    Z_STRINGIFY(RADIO_TXPOWER_TXPOWER_Neg40dBm));
}

static int cmd_nrf_1mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_1MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_1MBIT));

	return 0;
}

static int cmd_nrf_2mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_2MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_2MBIT));

	return 0;
}

#if defined(RADIO_MODE_MODE_Nrf_250Kbit)
static int cmd_nrf_250kbit(const struct shell *shell, size_t argc,
			   char **argv)
{
	config.mode = NRF_RADIO_MODE_NRF_250KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_NRF_250KBIT));

	return 0;
}
#endif /* defined(RADIO_MODE_MODE_Nrf_250Kbit) */

static int cmd_ble_1mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_1MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_1MBIT));

	return 0;
}

static int cmd_ble_2mbit(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_2MBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_2MBIT));

	return 0;
}

#if CONFIG_HAS_HW_NRF_RADIO_BLE_CODED
static int cmd_ble_lr125kbit(const struct shell *shell, size_t argc,
			     char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_LR125KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_LR125KBIT));

	return 0;
}

static int cmd_ble_lr500kbit(const struct shell *shell, size_t argc,
			     char **argv)
{
	config.mode = NRF_RADIO_MODE_BLE_LR500KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_BLE_LR500KBIT));

	return 0;
}
#endif /* CONFIG_HAS_HW_NRF_RADIO_BLE_CODED */

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
static int cmd_ble_ieee(const struct shell *shell, size_t argc, char **argv)
{
	config.mode = NRF_RADIO_MODE_IEEE802154_250KBIT;
	shell_print(shell, "Data rate: %s",
		    STRINGIFY(NRF_RADIO_MODE_IEEE802154_250KBIT));

	return 0;
}
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

static int cmd_pattern_random(const struct shell *shell, size_t argc,
			      char **argv)
{
	config.tx_pattern = TRANSMIT_PATTERN_RANDOM;
	shell_print(shell,
		    "Transmission pattern: %s",
		    STRINGIFY(TRANSMIT_PATTERN_RANDOM));

	return 0;
}

static int cmd_pattern_11110000(const struct shell *shell, size_t argc,
				char **argv)
{
	config.tx_pattern = TRANSMIT_PATTERN_11110000;
	shell_print(shell,
		    "Transmission pattern: %s",
		    STRINGIFY(TRANSMIT_PATTERN_11110000));

	return 0;
}

static int cmd_pattern_11001100(const struct shell *shell, size_t argc,
				char **argv)
{
	config.tx_pattern = TRANSMIT_PATTERN_11001100;
	shell_print(shell,
		    "Transmission pattern: %s",
		    STRINGIFY(TRANSMIT_PATTERN_11001100));

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_data_rate,
	SHELL_CMD(nrf_1Mbit, NULL, "1 Mbit/s Nordic proprietary radio mode",
		  cmd_nrf_1mbit),
	SHELL_CMD(nrf_2Mbit, NULL, "2 Mbit/s Nordic proprietary radio mode",
		  cmd_nrf_2mbit),

#if defined(RADIO_MODE_MODE_Nrf_250Kbit)
	SHELL_CMD(nrf_250Kbit, NULL,
		  "250 kbit/s Nordic proprietary radio mode",
		  cmd_nrf_250kbit),
#endif /* defined(RADIO_MODE_MODE_Nrf_250Kbit) */

	SHELL_CMD(ble_1Mbit, NULL, "1 Mbit/s Bluetooth Low Energy",
		  cmd_ble_1mbit),
	SHELL_CMD(ble_2Mbit, NULL, "2 Mbit/s Bluetooth Low Energy",
		  cmd_ble_2mbit),

#if CONFIG_HAS_HW_NRF_RADIO_BLE_CODED
	SHELL_CMD(ble_lr125Kbit, NULL,
		  "Long range 125 kbit/s TX, 125 kbit/s and 500 kbit/s RX",
		  cmd_ble_lr125kbit),

	SHELL_CMD(ble_lr500Kbit, NULL,
		  "Long range 500 kbit/s TX, 125 kbit/s and 500 kbit/s RX",
		  cmd_ble_lr500kbit),
#endif /* CONFIG_HAS_HW_NRF_RADIO_BLE_CODED */

#if CONFIG_HAS_HW_NRF_RADIO_IEEE802154
	SHELL_CMD(ieee802154_250Kbit, NULL, "IEEE 802.15.4-2006 250 kbit/s",
		  cmd_ble_ieee),
#endif /* CONFIG_HAS_HW_NRF_RADIO_IEEE802154 */

	SHELL_SUBCMD_SET_END
);

static int cmd_print_payload(const struct shell *shell, size_t argc,
			     char **argv)
{
	struct radio_rx_stats rx_stats;

	memset(&rx_stats, 0, sizeof(rx_stats));

	radio_rx_stats_get(&rx_stats);

	shell_print(shell, "Received payload:");
	shell_hexdump(shell, rx_stats.last_packet.buf,
		      rx_stats.last_packet.len);
	shell_print(shell, "Number of packets: %d", rx_stats.packet_cnt);

	return 0;
}

#if CONFIG_NRF21540_FEM
static int cmd_nrf21540(const struct shell *shell, size_t argc, char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Uknown argument: %s.", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_nrf21540_gain_set(const struct shell *shell, size_t argc,
				 char **argv)
{
	uint32_t gain;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	gain = atoi(argv[1]);

	if (gain > NRF21540_TX_GAIN_Max) {
		shell_error(shell, "%s:  Output power must be between 0 and 31",
			    argv[0]);
		return -EINVAL;
	}

	config.nrf21540.gain = gain;

	shell_print(shell, "nRF21540 Tx gain set to %d", gain);

	return 0;
}

static int cmd_nrf21540_antenna_select(const struct shell *shell, size_t argc,
				       char **argv)
{
	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count.", argv[0]);
		return -EINVAL;
	}

	if (argc == 2) {
		shell_error(shell, "Uknown argument: %s.", argv[1]);
		return -EINVAL;
	}

	return 0;
}

static int cmd_nrf21540_antenna_1(const struct shell *shell, size_t argc,
				  char **argv)
{
	shell_print(shell, "ANT1 enabled, ANT2 disabled");

	return nrf21540_antenna_select(NRF21540_ANT1);
}

static int cmd_nrf21540_antenna_2(const struct shell *shell, size_t argc,
				  char **argv)
{
	shell_print(shell, "ANT1 disabled, ANT2 enabled");

	return nrf21540_antenna_select(NRF21540_ANT2);
}

static int cmd_nrf21540_active_delay_set(const struct shell *shell, size_t argc,
					 char **argv)
{
	uint32_t delay;

	if (argc == 1) {
		shell_help(shell);
		return SHELL_CMD_HELP_PRINTED;
	}

	if (argc > 2) {
		shell_error(shell, "%s: bad parameters count", argv[0]);
		return -EINVAL;
	}

	delay = atoi(argv[1]);

	config.nrf21540.active_delay = delay;

	shell_print(shell, "nRF21540 activation delay set to %d us", delay);

	return 0;
}
#endif /* CONFIG_NRF21540_FEM */

SHELL_STATIC_SUBCMD_SET_CREATE(sub_output_power,
#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
	SHELL_CMD(pos8dBm, NULL, "TX power: +8 dBm", cmd_pos8dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos8dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos7dBm)
	SHELL_CMD(pos7dBm, NULL, "TX power: +7 dBm", cmd_pos7dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos7dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos6dBm)
	SHELL_CMD(pos6dBm, NULL, "TX power: +6 dBm", cmd_pos6dbm),
#endif/* defined(RADIO_TXPOWER_TXPOWER_Pos6dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos5dBm)
	SHELL_CMD(pos5dBm, NULL, "TX power: +5 dBm", cmd_pos5dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos5dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos4dBm)
	SHELL_CMD(pos4dBm, NULL, "TX power: +4 dBm", cmd_pos4dbm),
#endif /* RADIO_TXPOWER_TXPOWER_Pos4dBm */
#if defined(RADIO_TXPOWER_TXPOWER_Pos3dBm)
	SHELL_CMD(pos3dBm, NULL, "TX power: +3 dBm", cmd_pos3dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos3dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Pos2dBm)
	SHELL_CMD(pos2dBm, NULL, "TX power: +2 dBm", cmd_pos2dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Pos2dBm) */
	SHELL_CMD(pos0dBm, NULL, "TX power: 0 dBm", cmd_pos0dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg1dBm)
	SHELL_CMD(neg1dBm, NULL, "TX power: -1 dBm", cmd_neg1dbm),
#endif /* RADIO_TXPOWER_TXPOWER_Neg1dBm */
#if defined(RADIO_TXPOWER_TXPOWER_Neg2dBm)
	SHELL_CMD(neg2dBm, NULL, "TX power: -2 dBm", cmd_neg2dbm),
#endif /* RADIO_TXPOWER_TXPOWER_Neg2dBm */
#if defined(RADIO_TXPOWER_TXPOWER_Neg3dBm)
	SHELL_CMD(neg3dBm, NULL, "TX power: -3 dBm", cmd_neg3dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg3dBm) */
	SHELL_CMD(neg4dBm, NULL, "TX power: -4 dBm", cmd_neg4dbm),
#if defined(RADIO_TXPOWER_TXPOWER_Neg5dBm)
	SHELL_CMD(neg5dBm, NULL, "TX power: -5 dBm", cmd_neg5dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg5dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg6dBm)
	SHELL_CMD(neg6dBm, NULL, "TX power: -6 dBm", cmd_neg6dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg6dBm) */
#if defined(RADIO_TXPOWER_TXPOWER_Neg7dBm)
	SHELL_CMD(neg7dBm, NULL, "TX power: -7 dBm", cmd_neg7dbm),
#endif /* defined(RADIO_TXPOWER_TXPOWER_Neg7dBm) */
	SHELL_CMD(neg8dBm, NULL, "TX power: -8 dBm", cmd_neg8dbm),
	SHELL_CMD(neg12dBm, NULL, "TX power: -12 dBm", cmd_neg12dbm),
	SHELL_CMD(neg16dBm, NULL, "TX power: -16 dBm", cmd_neg16dbm),
	SHELL_CMD(neg20dBm, NULL, "TX power: -20 dBm", cmd_neg20dbm),
	SHELL_CMD(neg30dBm, NULL, "TX power: -30 dBm", cmd_neg30dbm),
	SHELL_CMD(neg40dBm, NULL, "TX power: -40 dBm", cmd_neg40dbm),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_transmit_pattern,
	SHELL_CMD(pattern_random, NULL,
		  "Set the transmission pattern to random.",
		  cmd_pattern_random),
	SHELL_CMD(pattern_11110000, NULL,
		  "Set the transmission pattern to 11110000.",
		  cmd_pattern_11110000),
	SHELL_CMD(pattern_11001100, NULL,
		  "Set the transmission pattern to 10101010.",
		  cmd_pattern_11001100),
	SHELL_SUBCMD_SET_END
);

#if CONFIG_NRF21540_FEM
SHELL_STATIC_SUBCMD_SET_CREATE(sub_nrf21540_antenna,
	SHELL_CMD(ant_1, NULL,
		  "ANT1 enabled, ANT2 disabled.",
		  cmd_nrf21540_antenna_1),
	SHELL_CMD(ant_2, NULL,
		  "ANT1 disabled, ANT2 enabled",
		  cmd_nrf21540_antenna_2),
	SHELL_SUBCMD_SET_END
);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_nrf21540,
	SHELL_CMD(tx_gain, NULL,
		  "Set the nRF21540 Front-End-Module Tx gain in an arbitrary units <gain>",
		  cmd_nrf21540_gain_set),
	SHELL_CMD(antenna, &sub_nrf21540_antenna,
		  "Select the nRF21540 Front-End-Module antenna <sub_cmd>",
		  cmd_nrf21540_antenna_select),
	SHELL_CMD(active_delay, NULL,
		  "Set the nRF21540 Front-End-Module activation delay <time us>",
		  cmd_nrf21540_active_delay_set),
	SHELL_SUBCMD_SET_END
);
#endif /* CONFIG_NRF21540_FEM */

SHELL_CMD_REGISTER(start_channel, NULL,
		   "Start the channel for the sweep or the channel for"
		   " the constant carrier <channel>",
		    cmd_start_channel_set);
SHELL_CMD_REGISTER(end_channel, NULL,
		   "End the channel for the sweep <channel>",
		   cmd_end_channel_set);
SHELL_CMD_REGISTER(time_on_channel, NULL,
		   "Time on each channel (between 1 ms and 99 ms) <time>",
		   cmd_time_set);
SHELL_CMD_REGISTER(cancel, NULL, "Cancel the sweep or the carrier",
		   cmd_cancel);
SHELL_CMD_REGISTER(data_rate, &sub_data_rate, "Set data rate <sub_cmd>",
		   cmd_data_rate_set);
SHELL_CMD_REGISTER(start_tx_carrier, NULL, "Start the TX carrier",
		   cmd_tx_carrier_start);
SHELL_CMD_REGISTER(start_tx_modulated_carrier, NULL,
		   "Start the modulated TX carrier",
		   cmd_tx_modulated_carrier_start);
SHELL_CMD_REGISTER(output_power,
		   &sub_output_power,
		   "Output power set <sub_cmd>",
		   cmd_output_power_set);
SHELL_CMD_REGISTER(transmit_pattern,
		   &sub_transmit_pattern,
		   "Set the transmission pattern",
		   cmd_transmit_pattern_set);
SHELL_CMD_REGISTER(start_duty_cycle_modulated_tx, NULL,
		   "Duty cycle in percent (two decimal digits, between 01 and "
		   "99) <duty_cycle>", cmd_duty_cycle_set);
SHELL_CMD_REGISTER(parameters_print, NULL,
		   "Print current delay, channel and so on",
		   cmd_print);
SHELL_CMD_REGISTER(start_rx_sweep, NULL, "Start RX sweep", cmd_rx_sweep_start);
SHELL_CMD_REGISTER(start_tx_sweep, NULL, "Start TX sweep", cmd_tx_sweep_start);
SHELL_CMD_REGISTER(start_rx, NULL, "Start RX", cmd_rx_start);
SHELL_CMD_REGISTER(print_rx, NULL, "Print RX payload", cmd_print_payload);
#if defined(TOGGLE_DCDC_HELP)
SHELL_CMD_REGISTER(toggle_dcdc_state, NULL, TOGGLE_DCDC_HELP, cmd_toggle_dc);
#endif
#if CONFIG_NRF21540_FEM
SHELL_CMD_REGISTER(nrf21540,
		   &sub_nrf21540,
		   "Set nRF21540 Front-End-Module parameters <sub_cmd>",
		   cmd_nrf21540);
#endif /* CONFIG_NRF21540_FEM */

static int radio_cmd_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return radio_test_init(&test_config);
}

SYS_INIT(radio_cmd_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
