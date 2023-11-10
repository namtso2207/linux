/*
* Copyright (C) 2017-2019 InvenSense, Inc.
*
* This software is licensed under the terms of the GNU General Public
* License version 2, as published by the Free Software Foundation, and
* may be copied, distributed, and modified under those terms.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#define pr_fmt(fmt) "inv_mpu: " fmt
#include "../inv_mpu_iio.h"

static int inv_calc_engine_dur(struct inv_mpu_state *st,
				struct inv_engine_info *ei)
{
	if (!ei->running_rate)
		return -EINVAL;
	if (st->mode_1k_on)
		ei->dur = ei->base_time_1k / ei->orig_rate;
	else if (st->mode_vr_on)
		ei->dur = ei->base_time_vr / ei->orig_rate;
	else
		ei->dur = ei->base_time / ei->orig_rate;
	ei->dur *= ei->divider;

	return 0;
}

static int inv_turn_on_fifo(struct inv_mpu_state *st)
{
	u8 int_en, fifo_en, mode, user;
	int r;
	char data;

	r = inv_plat_single_write(st, REG_FIFO_EN, 0);
	if (r)
		return r;
	r = inv_plat_single_write(st, REG_USER_CTRL, BIT_FIFO_RST);
	if (r)
		return r;
	//dummy fifo read
	inv_plat_read(st, REG_FIFO_R_W, 1, &data);
	fifo_en = 0;
	int_en = 0;

	if (st->gesture_only_on && (!st->batch.timeout)) {
		if (st->chip_config.stationary_detect_enable)
			st->gesture_int_count = STATIONARY_DELAY_THRESHOLD;
		else
			st->gesture_int_count = WOM_DELAY_THRESHOLD;
		int_en |= BIT_WOM_ALL_INT_EN;
	}
	if (st->batch.timeout) {
		if(!st->batch.fifo_wm_th)
			int_en = BIT_DATA_RDY_EN;
	} else {
		int_en = BIT_DATA_RDY_EN;
		if (st->chip_config.eis_enable)
			int_en |= BIT_FSYNC_INT_EN;
	}
	if (st->sensor[SENSOR_GYRO].on)
		fifo_en |= BITS_GYRO_FIFO_EN;

	if (st->sensor[SENSOR_ACCEL].on)
		fifo_en |= BIT_ACCEL_FIFO_EN;
	r = inv_plat_single_write(st, REG_FIFO_EN, fifo_en);
	if (r)
		return r;
	r = inv_plat_single_write(st, REG_INT_ENABLE, int_en);
	if (r)
		return r;
	if (st->gesture_only_on && (!st->batch.timeout)) {
		mode = BIT_ACCEL_INTEL_EN | BIT_ACCEL_INTEL_MODE;
	} else {
		mode = 0;
	}
	r = inv_plat_single_write(st, REG_ACCEL_INTEL_CTRL, mode);
	user = BIT_FIFO_EN;
	r = inv_plat_single_write(st, REG_USER_CTRL, user | st->i2c_dis);

	return r;
}

/*
 *  inv_reset_fifo() - Reset FIFO related registers.
 */
int inv_reset_fifo(struct inv_mpu_state *st, bool turn_off)
{
	int r, i;
	struct inv_timestamp_algo *ts_algo = &st->ts_algo;

	r = inv_turn_on_fifo(st);
	if (r)
		return r;

	ts_algo->last_run_time = get_time_ns();
	ts_algo->reset_ts = ts_algo->last_run_time;
	if (st->mode_1k_on)
		ts_algo->first_sample = MODE_1K_INIT_SAMPLE;
	else if(st->mode_vr_on)
		ts_algo->first_sample = MODE_VR_INIT_SAMPLE;
	else
		ts_algo->first_sample = 1;

	st->last_temp_comp_time = ts_algo->last_run_time;
	st->left_over_size = 0;
	for (i = 0; i < SENSOR_NUM_MAX; i++) {
		st->sensor[i].calib_flag = 0;
		st->sensor[i].sample_calib = 0;
		st->sensor[i].time_calib = ts_algo->last_run_time;
	}

	ts_algo->calib_counter = 0;

	return 0;
}

static int inv_turn_on_engine(struct inv_mpu_state *st)
{
	u8 v, w;
	int r;
	unsigned int wait_ms;

	if (st->chip_config.gyro_enable | st->chip_config.accel_enable) {
		w = 0;
		if (!st->chip_config.gyro_enable)
			w |= BIT_PWR_GYRO_STBY;
		if (!st->chip_config.accel_enable)
			w |= BIT_PWR_ACCEL_STBY;
	} else if (st->chip_config.compass_enable) {
		w = BIT_PWR_GYRO_STBY;
	} else {
		w = (BIT_PWR_GYRO_STBY | BIT_PWR_ACCEL_STBY);
	}

	r = inv_plat_read(st, REG_PWR_MGMT_2, 1, &v);
	if (r)
		return r;
	r = inv_plat_single_write(st, REG_PWR_MGMT_2, w);
	if (r)
		return r;

	wait_ms = 0;
	/* wait when enabling gyro */
	if (st->chip_config.gyro_enable
		&& (v & BIT_PWR_GYRO_STBY)) {
		wait_ms = INV_ICM20602_GYRO_START_TIME;
	}
	/* wait when disabling gyro */
	if (!(v & BIT_PWR_GYRO_STBY) &&
			!st->chip_config.gyro_enable) {
		if (wait_ms < 150)
			wait_ms = 150;
	}
	/* wait when enabling accel */
	if (st->chip_config.accel_enable
		&& (v & BIT_PWR_ACCEL_STBY)) {
		if (INV_ICM20602_ACCEL_START_TIME > wait_ms)
			wait_ms = INV_ICM20602_ACCEL_START_TIME;
	}
	if (wait_ms)
		msleep(wait_ms);

	if (st->chip_config.has_compass) {
		if (st->chip_config.compass_enable)
			r = st->slave_compass->resume(st);
		else
			r = st->slave_compass->suspend(st);
		if (r)
			return r;
	}

	return 0;
}

static int inv_setup_dmp_rate(struct inv_mpu_state *st)
{
	int i;

	for (i = 0; i < SENSOR_NUM_MAX; i++) {
		if (st->sensor[i].on) {
			st->cntl |= st->sensor[i].output;
			st->sensor[i].dur =
				st->eng_info[st->ts_algo.clock_base].dur;
			st->sensor[i].div = 1;
		}
	}

	return 0;
}

/*
 *  inv_set_lpf_gyro_cycle_avgcfg() - set gyro cycle mode and
 * set avg filter config based on rate.
 */
static int inv_set_lpf_gyro_cycle_avgfltr(struct inv_mpu_state *st, int rate)
{
	const short odr[] = {30, 50, 100, 200, 500};
	const int fltrconfig[] = {
		BIT_AVG_FILTER_30HZ,
		BIT_AVG_FILTER_50HZ,
		BIT_AVG_FILTER_100HZ,
		BIT_AVG_FILTER_200HZ,
		BIT_AVG_FILTER_500HZ
	};
	int i, result, data;
	i = 0;
	while ((rate > odr[i]) && (i < ARRAY_SIZE(fltrconfig) - 1))
		i++;
	data = (BIT_GYRO_CYCLE_EN | fltrconfig[i]);
	result = inv_plat_single_write(st, REG_LP_MODE_CTRL, data);
	return result;
}

/*
 *  inv_set_lpf() - set low pass filer based on fifo rate.
 */
static int inv_set_lpf(struct inv_mpu_state *st, int rate)
{
	const short hz[] = {188, 98, 42, 20, 10, 5};
	const int   d[] = {INV_FILTER_188HZ, INV_FILTER_98HZ,
			INV_FILTER_42HZ, INV_FILTER_20HZ,
			INV_FILTER_10HZ, INV_FILTER_5HZ};
	int i, h, data, result;

	if (st->chip_config.eis_enable || st->ois.en ||
		st->mode_1k_on || st->mode_vr_on) {
		h = (rate >> 1);
		i = 0;
		while ((h < hz[i]) && (i < ARRAY_SIZE(d) - 1))
			i++;
		data = d[i];
		data |= (EXT_SYNC_SET | BIT_FIFO_COUNT_REC);
		result = inv_plat_single_write(st, REG_CONFIG, data);
		if (result)
			return result;

		st->chip_config.lpf = data;
		result = inv_plat_single_write(st, REG_LP_MODE_CTRL, 0);
		st->gyro_lp_mode = 0;
	} else {
		result = inv_set_lpf_gyro_cycle_avgfltr(st, rate);
		if (result)
			return result;
		data = (BIT_FIFO_COUNT_REC);
		result = inv_plat_single_write(st, REG_CONFIG, data | 3);
		st->gyro_lp_mode = 1;
	}

	return result;
}

static int inv_set_div(struct inv_mpu_state *st, int a_d, int g_d)
{
	int result, div;

	if (st->chip_config.gyro_enable)
		div = g_d;
	else
		div = a_d;
	if (st->chip_config.eis_enable)
		div = 0;

	pr_debug("div= %d\n", div);
	result = inv_plat_single_write(st, REG_SAMPLE_RATE_DIV, div);

	return result;
}

static int inv_set_batch(struct inv_mpu_state *st)
{
	int res = 0;
	u32 w;

	if (st->batch.timeout) {
		w = st->batch.timeout * st->eng_info[ENGINE_GYRO].running_rate
					* st->batch.pk_size / 1000;
		if (w > MAX_BATCH_FIFO_SIZE)
			w = MAX_BATCH_FIFO_SIZE;
	} else {
		w = 0;
	}
	if (st->batch.pk_size)
		w /= st->batch.pk_size;
	st->batch.fifo_wm_th = w;
	pr_debug("running= %d, pksize=%d, to=%d w=%d\n",
		st->eng_info[ENGINE_GYRO].running_rate,
		st->batch.pk_size, st->batch.timeout, w);
	res = inv_plat_single_write(st, REG_FIFO_WM_TH2, w & 0xff);
	if (res)
		return res;
	res = inv_plat_single_write(st, REG_FIFO_WM_TH1, (w >> 8) & 0xff);

	return res;
}

static int inv_set_rate(struct inv_mpu_state *st)
{
	int g_d, a_d, result, i;

	result = inv_setup_dmp_rate(st);
	if (result)
		return result;

	g_d = st->eng_info[ENGINE_GYRO].divider - 1;
	a_d = st->eng_info[ENGINE_ACCEL].divider - 1;
	result = inv_set_div(st, a_d, g_d);
	if (result)
		return result;
	result = inv_set_lpf(st, st->eng_info[ENGINE_GYRO].running_rate);
	if (result)
		return result;
	// set ADLPF at this point not to change after accel is enabled
	result = inv_set_accel_config2(st, false);
	st->batch.pk_size = 0;
	for (i = 0; i < SENSOR_NUM_MAX; i++) {
		if (st->sensor[i].on)
			st->batch.pk_size +=  st->sensor[i].sample_size;
	}

	inv_set_batch(st);

	return result;
}

static int inv_determine_engine(struct inv_mpu_state *st)
{
	int i;
	bool a_en, g_en;
	int accel_rate, gyro_rate;

	a_en = false;
	g_en = false;
	gyro_rate = MPU_INIT_SENSOR_RATE;
	accel_rate = MPU_INIT_SENSOR_RATE;
	/* loop the streaming sensors to see which engine needs to be turned on
		*/
	for (i = 0; i < SENSOR_NUM_MAX; i++) {
		if (st->sensor[i].on) {
			a_en |= st->sensor[i].a_en;
			g_en |= st->sensor[i].g_en;
		}
	}

	st->sensor[SENSOR_TEMP].on = true;
	if (st->chip_config.eis_enable) {
		g_en = true;
		st->eis.frame_count = 0;
		st->eis.fsync_delay = 0;
		st->eis.gyro_counter = 0;
		st->eis.voting_count = 0;
		st->eis.voting_count_sub = 0;
		gyro_rate = BASE_SAMPLE_RATE;
	} else {
		st->eis.eis_triggered = false;
		st->eis.prev_state = false;
	}

	accel_rate = st->sensor[SENSOR_ACCEL].rate;
	gyro_rate  = max(gyro_rate, st->sensor[SENSOR_GYRO].rate);

	st->ts_algo.clock_base = ENGINE_ACCEL;

	if (g_en) {
		/* gyro engine needs to be fastest */
		if (a_en)
			gyro_rate = max(gyro_rate, accel_rate);
		accel_rate = gyro_rate;
		st->ts_algo.clock_base = ENGINE_GYRO;
	} else if (a_en) {
		/* accel engine needs to be fastest if gyro engine is off */
		gyro_rate = accel_rate;
		st->ts_algo.clock_base = ENGINE_ACCEL;
	}

	st->eng_info[ENGINE_GYRO].running_rate = gyro_rate;
	st->eng_info[ENGINE_ACCEL].running_rate = accel_rate;
	if ((gyro_rate > MPU_DEFAULT_DMP_FREQ) ||
					(accel_rate > MPU_DEFAULT_DMP_FREQ)) {
		if ((gyro_rate == MODE_VR_RATE) ||
			(accel_rate == MODE_VR_RATE))
			st->mode_vr_on = true;
		else
			st->mode_1k_on = true;
	} else {
		st->mode_1k_on = false;
		st->mode_vr_on = false;
	}
	/* engine divider for pressure and compass is set later */
	if (st->chip_config.eis_enable || st->mode_1k_on) {
		st->eng_info[ENGINE_GYRO].divider = 1;
		st->eng_info[ENGINE_ACCEL].divider = 1;
		// need to update rate and div for 1khz mode
		for ( i = 0 ; i < SENSOR_L_NUM_MAX ; i++ ) {
			if (st->sensor_l[i].on) {
				st->sensor_l[i].counter = 0;
				if (st->sensor_l[i].rate)
					st->sensor_l[i].div =
					    ((BASE_SAMPLE_RATE /
						 st->eng_info[ENGINE_GYRO].divider) /
						 st->sensor_l[i].rate);
				else
					st->sensor_l[i].div = 0xffff;
			}
		}
	} else if (st->mode_vr_on) {
		st->eng_info[ENGINE_GYRO].divider = 2;
		st->eng_info[ENGINE_ACCEL].divider = 2;
		// need to update rate and div for VR mode
		for ( i = 0 ; i < SENSOR_L_NUM_MAX ; i++ ) {
			if (st->sensor_l[i].on) {
				st->sensor_l[i].counter = 0;
				if (st->sensor_l[i].rate)
					st->sensor_l[i].div =
					    ((BASE_SAMPLE_RATE /
						 st->eng_info[ENGINE_GYRO].divider) /
						 st->sensor_l[i].rate);
				else
					st->sensor_l[i].div = 0xffff;
			}
		}
	} else {
		st->eng_info[ENGINE_GYRO].divider =
			(BASE_SAMPLE_RATE / MPU_DEFAULT_DMP_FREQ) *
			(MPU_DEFAULT_DMP_FREQ /
			st->eng_info[ENGINE_GYRO].running_rate);
		st->eng_info[ENGINE_ACCEL].divider =
			(BASE_SAMPLE_RATE / MPU_DEFAULT_DMP_FREQ) *
			(MPU_DEFAULT_DMP_FREQ /
			st->eng_info[ENGINE_ACCEL].running_rate);
	}

	for ( i = 0 ; i < SENSOR_L_NUM_MAX ; i++ )
		st->sensor_l[i].counter = 0;

	inv_calc_engine_dur(st, &st->eng_info[ENGINE_GYRO]);
	inv_calc_engine_dur(st, &st->eng_info[ENGINE_ACCEL]);

	pr_debug("gen: %d aen: %d grate: %d arate: %d\n",
				g_en, a_en, gyro_rate, accel_rate);

	st->chip_config.gyro_enable = g_en;
	st->chip_config.accel_enable = a_en;

	return 0;
}

/*
 *  set_inv_enable() - enable function.
 */
int set_inv_enable(struct iio_dev *indio_dev)
{
	int result;
	struct inv_mpu_state *st = iio_priv(indio_dev);

	result = inv_switch_power_in_lp(st, true);
	if (result)
		return result;
	inv_stop_interrupt(st);
	inv_determine_engine(st);
	result = inv_set_rate(st);
	if (result) {
		pr_err("inv_set_rate error\n");
		return result;
	}
	result = inv_turn_on_engine(st);
	if (result) {
		pr_err("inv_turn_on_engine error\n");
		return result;
	}
	result = inv_reset_fifo(st, false);
	if (result)
		return result;
	result = inv_switch_power_in_lp(st, false);
	if ((!st->chip_config.gyro_enable) &&
		(!st->chip_config.accel_enable)) {
		inv_set_power(st, false);
		return 0;
	}

	return result;
}

static int inv_save_interrupt_config(struct inv_mpu_state *st)
{
	int res;

	res = inv_plat_read(st, REG_INT_ENABLE, 1, &st->int_en);

	return res;
}

int inv_stop_interrupt(struct inv_mpu_state *st)
{
	int res;

	res = inv_save_interrupt_config(st);
	if (res)
		return res;

	res = inv_plat_single_write(st, REG_INT_ENABLE, 0);

	return res;
}

int inv_restore_interrupt(struct inv_mpu_state *st)
{
	int res;

	res = inv_plat_single_write(st, REG_INT_ENABLE, st->int_en);

	return res;
}

int inv_stop_stream_interrupt(struct inv_mpu_state *st)
{
	return inv_stop_interrupt(st);
}

int inv_restore_stream_interrupt(struct inv_mpu_state *st)
{
	return inv_restore_interrupt(st);
}

/* dummy function for 20608D */
int inv_enable_pedometer_interrupt(struct inv_mpu_state *st, bool en)
{
	return 0;
}
int inv_dmp_read(struct inv_mpu_state *st, int off, int size, u8 *buf)
{
	return 0;
}
int inv_firmware_load(struct inv_mpu_state *st)
{
	return 0;
}
