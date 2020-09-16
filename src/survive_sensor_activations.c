#include "string.h"
#include <assert.h>
#include <math.h>
#include <survive.h>

STATIC_CONFIG_ITEM(MOVMENT_THRESHOLD_GYRO, "move-threshold-gyro", 'f', "Threshold to count gyro norms as moving", .075)
STATIC_CONFIG_ITEM(MOVMENT_THRESHOLD_ACC, "move-threshold-acc", 'f', "Threshold to count acc diff norms as moving", .03)
STATIC_CONFIG_ITEM(MOVMENT_THRESHOLD_ANG, "move-threshold-ang", 'f', "Threshold to count light angle diffs as moving",
				   .015)
STATIC_CONFIG_ITEM(FILTER_THRESHOLD_ANG, "filter-threshold-ang-per-sec", 'f',
				   "Threshold to filter light which changes too fast", 50.)

static FLT moveThresholdGyro = 0;
static FLT moveThresholdAcc = 0;
static FLT moveThresholdAng = 0;
static FLT filterLightChange = 0;

bool SurviveSensorActivations_isReadingValid(const SurviveSensorActivations *self, survive_long_timecode tolerance,
											 uint32_t idx, int lh, int axis) {
	const survive_long_timecode *data_timecode = self->timecode[idx][lh];
	if (self->lh_gen != 1 && lh < 2 && self->lengths[idx][lh][axis] == 0)
		return false;

	if (isnan(self->angles[idx][lh][axis]))
		return false;

	survive_long_timecode timecode_now = SurviveSensorActivations_last_time(self);
	assert(timecode_now >= data_timecode[axis]);
	return timecode_now - data_timecode[axis] <= tolerance;
}
bool SurviveSensorActivations_isPairValid(const SurviveSensorActivations *self, uint32_t tolerance,
										  uint32_t timecode_now, uint32_t idx, int lh) {
	const survive_long_timecode *data_timecode = self->timecode[idx][lh];
	if (self->lh_gen != 1 && (self->lengths[idx][lh][0] == 0 || self->lengths[idx][lh][1] == 0))
		return false;

	if (isnan(self->angles[idx][lh][0]) || isnan(self->angles[idx][lh][1]))
		return false;

	return !(timecode_now - data_timecode[0] > tolerance || timecode_now - data_timecode[1] > tolerance);
}

survive_long_timecode SurviveSensorActivations_last_time(const SurviveSensorActivations *self) {
	survive_long_timecode last_time = self->last_light;
	if (self->last_imu > last_time) {
		last_time = self->last_imu;
	}
	return last_time;
}
survive_long_timecode SurviveSensorActivations_stationary_time(const SurviveSensorActivations *self) {
	survive_long_timecode last_time = SurviveSensorActivations_last_time(self);
	survive_long_timecode last_move = self->last_movement;
	assert(last_move <= last_time);
	return last_time - last_move;
}

void SurviveSensorActivations_add_imu(SurviveSensorActivations *self, struct PoserDataIMU *imuData) {
	self->last_imu = imuData->hdr.timecode;
	// fprintf(stderr, "imu tc: %f\n", self->last_imu/ 48000000.);
	if (self->imu_init_cnt > 0) {
		self->imu_init_cnt--;
		return;
	}

	if (isnan(self->accel[0])) {
		for (int i = 0; i < 3; i++) {
			self->accel[i] = imuData->accel[i];
			self->gyro[i] = imuData->gyro[i];
			self->mag[i] = imuData->mag[i];
		}
		self->last_movement = imuData->hdr.timecode;
	} else {
		for (int i = 0; i < 3; i++) {
			self->accel[i] = .98 * self->accel[i] + .02 * imuData->accel[i];
			self->gyro[i] = .98 * self->gyro[i] + .02 * imuData->gyro[i];
			self->mag[i] = .98 * self->mag[i] + .02 * imuData->mag[i];
		}
	}

	if (norm3d(imuData->gyro) > moveThresholdGyro || dist3d(self->accel, imuData->accel) > moveThresholdAcc) {
		self->last_movement = imuData->hdr.timecode;
		// fprintf(stderr, "%f %f\n", norm3d(imuData->gyro), dist3d(self->accel, imuData->accel));
	}
}
bool SurviveSensorActivations_add_gen2(SurviveSensorActivations *self, struct PoserDataLightGen2 *lightData) {
	self->lh_gen = 1;

	if(lightData->common.hdr.pt == POSERDATA_LIGHT_GEN2) {
		int axis = lightData->plane;
		PoserDataLight *l = &lightData->common;
		if (l->sensor_id >= SENSORS_PER_OBJECT)
			return false;

		survive_long_timecode *data_timecode = &self->timecode[l->sensor_id][l->lh][axis];

		FLT *angle = &self->angles[l->sensor_id][l->lh][axis];

		survive_long_timecode long_timecode = l->hdr.timecode;
		FLT change_rate = fabs(*angle - l->angle) / (FLT)(long_timecode - *data_timecode) * 48000000.;

		if (*data_timecode == 0 || change_rate < filterLightChange) {
			if (!isnan(*angle) && fabs(*angle - l->angle) > moveThresholdAng) {
				self->last_light_change = self->last_movement = long_timecode;
			}

			if (isnan(*angle))
				self->last_light_change = long_timecode;

			if (self->angles_center_cnt[l->lh][axis] == 0) {
				self->angles_center[l->lh][axis] = l->angle;
			} else {
				self->angles_center[l->lh][axis] *= .9;
				self->angles_center[l->lh][axis] += .1 * l->angle;
			}
			self->angles_center_cnt[l->lh][axis]++;
			// fprintf(stderr, "Time %f\n", l->hdr.timecode / 48000000.);
			*data_timecode = l->hdr.timecode;
			*angle = l->angle;
		} else {
			return false;
		}
	}

	if(lightData->common.hdr.timecode > self->last_light) {
		self->last_light = lightData->common.hdr.timecode;
		//fprintf(stderr, "lgt tc: %16lx\n", self->last_light);
	}
	return true;
}

SURVIVE_EXPORT void SurviveSensorActivations_reset(SurviveSensorActivations *self) {
	memset(self, 0, sizeof(SurviveSensorActivations));

	for (int i = 0; i < SENSORS_PER_OBJECT; i++) {
		for (int j = 0; j < NUM_GEN2_LIGHTHOUSES; j++) {
			for (int h = 0; h < 2; h++) {
				self->angles[i][j][h] = NAN;
				self->angles_center[j][h] = NAN;
			}
		}
	}

	for (int i = 0; i < 3; i++) {
		self->accel[i] = NAN;
	}

	self->imu_init_cnt = 30;
}
SURVIVE_EXPORT void SurviveSensorActivations_ctor(SurviveObject *so, SurviveSensorActivations *self) {
	if (so) {
		moveThresholdAcc = survive_configf(so->ctx, MOVMENT_THRESHOLD_ACC_TAG, SC_GET, 0);
		moveThresholdGyro = survive_configf(so->ctx, MOVMENT_THRESHOLD_GYRO_TAG, SC_GET, 0);
		moveThresholdAng = survive_configf(so->ctx, MOVMENT_THRESHOLD_ANG_TAG, SC_GET, 0);
		filterLightChange = survive_configf(so->ctx, FILTER_THRESHOLD_ANG_TAG, SC_GET, 0);
	}

	SurviveSensorActivations_reset(self);
	self->lh_gen = -1;
}

void SurviveSensorActivations_add(SurviveSensorActivations *self, struct PoserDataLightGen1 *_lightData) {
	self->lh_gen = 0;

	int axis = (_lightData->acode & 1);
	PoserDataLight *lightData = &_lightData->common;
	survive_long_timecode *data_timecode = &self->timecode[lightData->sensor_id][lightData->lh][axis];

	FLT *angle = &self->angles[lightData->sensor_id][lightData->lh][axis];
	uint32_t *length = &self->lengths[lightData->sensor_id][lightData->lh][axis];

	if (*length == 0 || fabs(*angle - lightData->angle) > moveThresholdAcc) {
		survive_long_timecode long_timecode = lightData->hdr.timecode;
		// assert(long_timecode > self->last_movement);
		self->last_light_change = self->last_movement = long_timecode;
	}

	*angle = lightData->angle;
	*data_timecode = lightData->hdr.timecode;
	*length = (uint32_t)(_lightData->length * 48000000);
	if(lightData->hdr.timecode > self->last_light)
		self->last_light = lightData->hdr.timecode;
}

static inline survive_long_timecode make_long_timecode(survive_long_timecode prev, survive_timecode current) {
	survive_long_timecode rtn = current | (prev & 0xFFFFFFFF00000000);
	if(rtn < prev && rtn + 0x80000000 < prev) {
		rtn += 0x100000000;
	}
	return rtn;
}
SURVIVE_EXPORT survive_long_timecode SurviveSensorActivations_long_timecode_imu(const SurviveSensorActivations *self, survive_timecode timecode) {
	return make_long_timecode(self->last_imu, timecode);
}
SURVIVE_EXPORT survive_long_timecode SurviveSensorActivations_long_timecode_light(const SurviveSensorActivations *self, survive_timecode timecode) {
	return make_long_timecode(self->last_light, timecode);
}


FLT SurviveSensorActivations_difference(const SurviveSensorActivations *rhs, const SurviveSensorActivations *lhs) {
	FLT rtn = 0;
	int cnt = 0;
	for(size_t i = 0;i < SENSORS_PER_OBJECT;i++) {
		for (size_t lh = 0; lh < NUM_GEN1_LIGHTHOUSES; lh++) {
			for(size_t axis = 0;axis < 2;axis++) {
				if(rhs->lengths[i][lh][axis] > 0 && lhs->lengths[i][lh][axis] > 0) {
					FLT diff = rhs->angles[i][lh][axis] - lhs->angles[i][lh][axis];
					rtn += diff * diff;
					cnt++;
				}
			}
		}
	}
	return rtn / (FLT)cnt;
}

SURVIVE_EXPORT uint32_t SurviveSensorActivations_default_tolerance =
	(uint32_t)(48000000 /*mhz*/ * (16.7 /*ms*/) / 1000) + 5000;
