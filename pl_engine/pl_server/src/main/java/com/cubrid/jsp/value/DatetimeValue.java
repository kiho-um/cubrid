/*
 * Copyright (C) 2008 Search Solution Corporation.
 * Copyright (c) 2016 CUBRID Corporation.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * - Neither the name of the <ORGANIZATION> nor the names of its contributors
 *   may be used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 */

package com.cubrid.jsp.value;

import com.cubrid.jsp.exception.TypeMismatchException;
import java.sql.Date;
import java.sql.Time;
import java.sql.Timestamp;
import java.util.Calendar;

public class DatetimeValue extends Value {
    private Timestamp timestamp;

    public DatetimeValue(int year, int mon, int day, int hour, int min, int sec, int msec) {
        super();

        Calendar c = Calendar.getInstance();
        c.set(year, mon, day, hour, min, sec);
        c.set(Calendar.MILLISECOND, msec);

        timestamp = new Timestamp(c.getTimeInMillis());
    }

    public DatetimeValue(
            int year,
            int mon,
            int day,
            int hour,
            int min,
            int sec,
            int msec,
            int mode,
            int dbType) {
        super(mode);

        Calendar c = Calendar.getInstance();
        c.set(year, mon, day, hour, min, sec);
        c.set(Calendar.MILLISECOND, msec);

        timestamp = new Timestamp(c.getTimeInMillis());
        this.dbType = dbType;
    }

    public DatetimeValue(Timestamp timestamp) {
        this.timestamp = timestamp;
    }

    public Date toDate() throws TypeMismatchException {
        return new Date(timestamp.getTime());
    }

    public Time toTime() throws TypeMismatchException {
        return new Time(timestamp.getTime());
    }

    public Timestamp toTimestamp() throws TypeMismatchException {
        long sec = timestamp.getTime() / 1000L; // truncate milli-seconds
        Timestamp ret = new Timestamp(sec * 1000L);
        if (!ValueUtilities.checkValidTimestamp(ret)) {
            throw new TypeMismatchException("out of valid range of a Timestamp: " + timestamp);
        }
        return ret;
    }

    public Timestamp toDatetime() throws TypeMismatchException {
        return timestamp;
    }

    public Object toDefault() throws TypeMismatchException {
        return timestamp;
    }

    public String toString() {
        return timestamp.toString();
    }

    public Date[] toDateArray() throws TypeMismatchException {
        return new Date[] {toDate()};
    }

    public Time[] toTimeArray() throws TypeMismatchException {
        return new Time[] {toTime()};
    }

    public Timestamp[] toTimestampArray() throws TypeMismatchException {
        return new Timestamp[] {toTimestamp()};
    }

    public Timestamp[] toDatetimeArray() throws TypeMismatchException {
        return new Timestamp[] {toDatetime()};
    }

    public Object[] toObjectArray() throws TypeMismatchException {
        return new Object[] {toObject()};
    }

    public String[] toStringArray() throws TypeMismatchException {
        return new String[] {toString()};
    }

    public Object toObject() throws TypeMismatchException {
        return timestamp;
    }
}
