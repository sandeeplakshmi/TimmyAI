// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 the Timmy contributors
//
// Read Apple Calendar events in one EventKit query and print them as JSON.
//
//   osascript -l JavaScript eventkit_events.js START_EPOCH END_EPOCH [--all-calendars] [--request]
//
// Runs inside osascript, so macOS asks about (and remembers) calendar access for osascript, the
// same process Timmy's Hermes gateway already runs under. Read-only: it never changes events.
// Holiday, birthday and other subscribed calendars are skipped unless --all-calendars is given.
// Without --request it never shows a permission prompt: it reports "not_determined" instead.
//
// Output: {"status": "full_access", "events": [{"title", "start", "end", "all_day", "calendar",
//          "location"}...], "skipped_calendars": [...]} or {"status": ..., "error": "..."}.
ObjC.import("EventKit");
ObjC.import("Foundation");

// EKAuthorizationStatus and EKCalendarType values (stable EventKit enums).
const STATUS = { 0: "not_determined", 1: "restricted", 2: "denied", 3: "full_access", 4: "write_only" };
const SUBSCRIBED = { 3: "subscription", 4: "birthday" };

function iso(d) {
    // Local wall-clock time with offset, e.g. 2026-11-17T09:30:00-08:00.
    const f = $.NSDateFormatter.alloc.init;
    f.locale = $.NSLocale.localeWithLocaleIdentifier("en_US_POSIX");
    f.dateFormat = "yyyy-MM-dd'T'HH:mm:ssxxx";
    return ObjC.unwrap(f.stringFromDate(d));
}

function str(v) {
    const s = ObjC.unwrap(v);
    return typeof s === "string" ? s : "";
}

function requestAccess(store) {
    let done = false;
    const cb = (granted, err) => { done = true; };
    if (store.respondsToSelector("requestFullAccessToEventsWithCompletion:")) {
        store.requestFullAccessToEventsWithCompletion(cb);
    } else {
        store.requestAccessToEntityTypeCompletion(0, cb);
    }
    const until = $.NSDate.dateWithTimeIntervalSinceNow(120);
    while (!done && $.NSDate.date.compare(until) < 0) {
        $.NSRunLoop.currentRunLoop.runUntilDate($.NSDate.dateWithTimeIntervalSinceNow(0.1));
    }
}

function run(argv) {
    const start = Number(argv[0]), end = Number(argv[1]);
    if (!(start < end)) return JSON.stringify({ error: "usage: START_EPOCH END_EPOCH [--all-calendars] [--request]" });
    const allCalendars = argv.includes("--all-calendars");

    const store = $.EKEventStore.alloc.init;
    let status = STATUS[$.EKEventStore.authorizationStatusForEntityType(0)] || "unknown";
    if (status === "not_determined" && argv.includes("--request")) {
        requestAccess(store);
        status = STATUS[$.EKEventStore.authorizationStatusForEntityType(0)] || "unknown";
    }
    if (status !== "full_access") {
        return JSON.stringify({ status: status, error: "no calendar read access (" + status + ")" });
    }

    const cals = store.calendarsForEntityType(0);
    const use = $.NSMutableArray.array;
    const skipped = [];
    for (let i = 0; i < cals.count; i++) {
        const c = cals.objectAtIndex(i);
        if (!allCalendars && SUBSCRIBED[c.type] !== undefined) skipped.push(str(c.title));
        else use.addObject(c);
    }
    const events = [];
    if (use.count > 0) {
        const pred = store.predicateForEventsWithStartDateEndDateCalendars(
            $.NSDate.dateWithTimeIntervalSince1970(start), $.NSDate.dateWithTimeIntervalSince1970(end), use);
        const found = store.eventsMatchingPredicate(pred);
        for (let i = 0; i < found.count; i++) {
            const e = found.objectAtIndex(i);
            events.push({
                title: str(e.title), start: iso(e.startDate), end: iso(e.endDate), all_day: !!e.allDay,
                calendar: str(e.calendar.title), location: str(e.location),
            });
        }
    }
    events.sort((a, b) => (a.start < b.start ? -1 : a.start > b.start ? 1 : 0));
    return JSON.stringify({ status: status, events: events, skipped_calendars: skipped });
}
