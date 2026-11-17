# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 the Timmy contributors
"""Tests for the daily-briefing calendar script. Run: python3 -m unittest discover tests"""

import sys
import unittest
from datetime import date
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server/hermes-skills/daily-briefing/scripts"))
import calendar_events as ce  # noqa: E402

MON = date(2026, 11, 16)   # a Monday
SAT = date(2026, 11, 21)
SUN = date(2026, 11, 22)


def d(m, day, y=2026):
    return date(y, m, day)


class ParseWhen(unittest.TestCase):
    def check(self, text, first, last=None, today=MON):
        self.assertEqual(ce.parse_when(text, today), (first, last or first), text)

    def test_relative_days(self):
        self.check("today", MON)
        self.check("", MON)
        self.check("Tomorrow?", d(11, 17))
        self.check("yesterday", d(11, 15))
        self.check("day after tomorrow", d(11, 18))

    def test_weekdays(self):
        self.check("friday", d(11, 20))
        self.check("on fri", d(11, 20))
        self.check("monday", MON)            # today counts
        self.check("next monday", d(11, 23))
        self.check("this wednesday", d(11, 18))
        self.check("sunday", SUN)
        self.check("saturday", d(11, 28), today=SUN)

    def test_spans(self):
        self.check("this week", MON, SUN)
        self.check("this week", SUN, today=SUN)
        self.check("next week", d(11, 23), d(11, 29))
        self.check("this weekend", SAT, SUN)
        self.check("weekend", SAT, SUN, today=SAT)
        self.check("weekend", SUN, today=SUN)
        self.check("next weekend", d(11, 28), d(11, 29))
        self.check("next weekend", d(11, 28), d(11, 29), today=SUN)
        self.check("next 3 days", MON, d(11, 18))
        self.check("this month", MON, d(11, 30))
        self.check("next month", d(12, 1), d(12, 31))
        self.check("next month", d(1, 1, 2027), d(1, 31, 2027), today=d(12, 5))

    def test_explicit_dates(self):
        self.check("2026-11-20", d(11, 20))
        self.check("11/20", d(11, 20))
        self.check("11/20/26", d(11, 20))
        self.check("nov 20", d(11, 20))
        self.check("November 20th, 2027", d(11, 20, 2027))
        self.check("20 nov", d(11, 20))
        self.check("jan 5", d(1, 5, 2027))   # past month/day means next year
        self.check("nov 15", d(11, 15))      # yesterday stays this year

    def test_ranges(self):
        self.check("nov 20 to nov 22", d(11, 20), d(11, 22))
        self.check("nov 20 to 22", d(11, 20), d(11, 22))
        self.check("2026-11-20..2026-11-22", d(11, 20), d(11, 22))
        self.check("from friday through sunday", d(11, 20), SUN)
        self.check("friday to monday", d(11, 20), d(11, 23))

    def test_errors(self):
        for bad in ("any events tomorrow", "feb 30", "blursday", "nov 22 to 2026-11-20"):
            with self.assertRaises(ce.WhenError, msg=bad):
                ce.parse_when(bad, MON)


class Shape(unittest.TestCase):
    def test_events(self):
        raw = {"status": "full_access", "skipped_calendars": ["Holidays"], "events": [
            {"title": "Dentist", "start": "2026-11-17T09:30:00-08:00", "end": "2026-11-17T10:30:00-08:00",
             "all_day": False, "calendar": "Home", "location": "Main St"},
            {"title": "Trip", "start": "2026-11-15T00:00:00-08:00", "end": "2026-11-19T00:00:00-08:00",
             "all_day": True, "calendar": "Home", "location": ""}]}
        out = ce.shape(raw, d(11, 17), d(11, 17))
        self.assertEqual(out["count"], 2)
        self.assertEqual(out["label"], "Tuesday, November 17")
        self.assertEqual(out["skipped_calendars"], ["Holidays"])
        dentist, trip = out["events"]
        self.assertEqual((dentist["time"], dentist["end"], dentist["location"]), ("09:30", "10:30", "Main St"))
        self.assertEqual((trip["time"], trip["day"], trip["until"]), ("all day", "2026-11-17", "2026-11-18"))
        self.assertNotIn("location", trip)

    def test_permission(self):
        out = ce.shape({"status": "not_determined", "error": "no access"}, MON, MON)
        self.assertEqual(out["permission"], "not_determined")
        self.assertNotIn("events", out)


class Fetch(unittest.TestCase):
    def test_one_helper_call_with_range(self):
        done = mock.Mock(returncode=0, stdout='{"status": "full_access", "events": []}', stderr="")
        with mock.patch.object(ce.subprocess, "run", return_value=done) as run:
            out = ce.events_for("tomorrow", today=MON)
        run.assert_called_once()
        cmd = run.call_args[0][0]
        self.assertEqual(cmd[:3], ["osascript", "-l", "JavaScript"])
        self.assertEqual(int(cmd[5]) - int(cmd[4]), 86400)
        self.assertNotIn("--request", cmd)
        self.assertEqual(out["count"], 0)

    def test_range_too_long(self):
        self.assertIn("error", ce.events_for("2026-01-01 to 2026-12-31", today=MON))


if __name__ == "__main__":
    unittest.main()
