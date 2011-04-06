/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>
#include <cutils/log.h>

#include "MPU3050Sensor.h"

#define FETCH_FULL_EVENT_BEFORE_RETURN 0

/*****************************************************************************/

MPU3050Sensor::MPU3050Sensor()
    : SensorBase(NULL, "mpu3050"),
      mEnabled(0),
      mInputReader(4),
      mHasPendingEvent(false)
{
    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_GY;
    mPendingEvent.type = SENSOR_TYPE_GYROSCOPE;
    memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));

    if (data_fd) {
        strcpy(input_sysfs_path, "/sys/bus/i2c/drivers/mpu3050_gyro/4-0068/");
        input_sysfs_path_len = strlen(input_sysfs_path);
        enable(0, 1);
    }
}

MPU3050Sensor::~MPU3050Sensor() {
    if (mEnabled) {
        enable(0, 0);
    }
}

int MPU3050Sensor::setInitialState() {
    struct input_absinfo absinfo_x;
    struct input_absinfo absinfo_y;
    struct input_absinfo absinfo_z;
    float value;
    if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_X), &absinfo_x) &&
        !ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_X), &absinfo_y) &&
        !ioctl(data_fd, EVIOCGABS(EVENT_TYPE_GYRO_X), &absinfo_z)) {
        value = absinfo_x.value;
        mPendingEvent.data[0] = value * CONVERT_GYRO_X;
        value = absinfo_x.value;
        mPendingEvent.data[1] = value * CONVERT_GYRO_Y;
        value = absinfo_x.value;
        mPendingEvent.data[2] = value * CONVERT_GYRO_Z;
        mHasPendingEvent = true;
    }
    return 0;
}

int MPU3050Sensor::enable(int32_t, int en) {
    int flags = en ? 1 : 0;
    if (flags != mEnabled) {
        int fd;
        strcpy(&input_sysfs_path[input_sysfs_path_len], "enable");
        fd = open(input_sysfs_path, O_RDWR);
        if (fd >= 0) {
            char buf[2];
            int err;
            buf[1] = 0;
            if (flags) {
                buf[0] = '1';
            } else {
                buf[0] = '0';
            }
            err = write(fd, buf, sizeof(buf));
            close(fd);
            mEnabled = flags;
            setInitialState();
            return 0;
        }
        return -1;
    }
    return 0;
}

bool MPU3050Sensor::hasPendingEvents() const {
    return mHasPendingEvent;
}

int MPU3050Sensor::setDelay(int32_t handle, int64_t delay_ns)
{
    int fd;
    strcpy(&input_sysfs_path[input_sysfs_path_len], "delay");
    fd = open(input_sysfs_path, O_RDWR);
    if (fd >= 0) {
        char buf[80];
        int32_t delay_ms = (int32_t)delay_ns/1000000;
        sprintf(buf, "%d", delay_ms);
        write(fd, buf, strlen(buf)+1);
        close(fd);
        return 0;
    }
    return -1;
}

int MPU3050Sensor::readEvents(sensors_event_t* data, int count)
{
    if (count < 1)
        return -EINVAL;

    if (mHasPendingEvent) {
        mHasPendingEvent = false;
        mPendingEvent.timestamp = getTimestamp();
        *data = mPendingEvent;
        return mEnabled ? 1 : 0;
    }

    ssize_t n = mInputReader.fill(data_fd);
    if (n < 0)
        return n;

    int numEventReceived = 0;
    input_event const* event;

#if FETCH_FULL_EVENT_BEFORE_RETURN
again:
#endif
    while (count && mInputReader.readEvent(&event)) {
        int type = event->type;
        if (type == EV_REL) {
            float value = event->value;
            if (event->code == EVENT_TYPE_GYRO_X) {
                mPendingEvent.data[0] = value * CONVERT_GYRO_X;
            } else if (event->code == EVENT_TYPE_GYRO_Y) {
                mPendingEvent.data[1] = value * CONVERT_GYRO_Y;
            } else if (event->code == EVENT_TYPE_GYRO_Z) {
                mPendingEvent.data[2] = value * CONVERT_GYRO_Z;
            }
        } else if (type == EV_SYN) {
            mPendingEvent.timestamp = timevalToNano(event->time);
            if (mEnabled) {
                *data++ = mPendingEvent;
                count--;
                numEventReceived++;
            }
        } else {
            LOGE("MPU3050Sensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }

#if FETCH_FULL_EVENT_BEFORE_RETURN
    /* if we didn't read a complete event, see if we can fill and
       try again instead of returning with nothing and redoing poll. */
    if (numEventReceived == 0 && mEnabled == 1) {
        n = mInputReader.fill(data_fd);
        if (n)
            goto again;
    }
#endif

    return numEventReceived;
}

