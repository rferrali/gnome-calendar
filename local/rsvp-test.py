#!/usr/bin/env python3
"""Reply to a meeting invitation through EDS, the way the fork will.

Personal test helper for the RSVP fork (not for upstream). It finds one event
by summary, sets *your* attendee PARTSTAT, wraps it in a METHOD:REQUEST
VCALENDAR and hands it to e_cal_client_receive_objects(). On EWS that makes
evolution-ews issue Exchange AcceptItem / TentativelyAcceptItem / DeclineItem,
which emails the organizer when the invite asked for replies (RSVP=TRUE),
unless --no-notify passes E_CAL_OPERATION_FLAG_DISABLE_ITIP_MESSAGE.

    local/rsvp-test.py "Test GOA"                 # dry run: show what would happen
    local/rsvp-test.py "Test GOA" accept --send   # really reply
    local/rsvp-test.py "Test GOA" decline --no-notify --send

Verify the result in OWA / the organizer's calendar, not by reading EDS back:
EDS serves from its cache.
"""

import argparse
import sys

import gi

gi.require_version("EDataServer", "1.2")
gi.require_version("ECal", "2.0")
gi.require_version("ICalGLib", "3.0")
from gi.repository import ECal, EDataServer, ICalGLib  # noqa: E402

PARTSTAT = {
    "accept": ICalGLib.ParameterPartstat.ACCEPTED,
    "tentative": ICalGLib.ParameterPartstat.TENTATIVE,
    "decline": ICalGLib.ParameterPartstat.DECLINED,
}


def strip_mailto(value):
    value = (value or "").strip()
    if value.lower().startswith("mailto:"):
        value = value[7:]
    return value.lower()


def my_addresses(registry, client):
    addrs = set()
    ok, cal_email = client.get_backend_property_sync("cal-email-address", None)
    if ok and cal_email:
        addrs.add(cal_email.lower())
    for source in registry.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY):
        ident = source.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY)
        if ident.get_address():
            addrs.add(ident.get_address().lower())
        for alias in (ident.get_aliases_as_hash_table() or {}):
            addrs.add(strip_mailto(alias))
    return addrs


def attendees(comp):
    prop = comp.get_first_property(ICalGLib.PropertyKind.ATTENDEE_PROPERTY)
    while prop:
        yield prop
        prop = comp.get_next_property(ICalGLib.PropertyKind.ATTENDEE_PROPERTY)


def param_str(prop, name):
    return prop.get_parameter_as_string(name) or "-"


def find_event(registry, summary):
    hits = []
    for source in registry.list_enabled(EDataServer.SOURCE_EXTENSION_CALENDAR):
        try:
            client = ECal.Client.connect_sync(source, ECal.ClientSourceType.EVENTS, 10, None)
        except Exception:
            continue
        sexp = '(contains? "summary" "%s")' % summary.replace('"', '\\"')
        ok, objs = client.get_object_list_sync(sexp, None)
        for comp in objs or []:
            hits.append((source, client, comp))
    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("summary", help="substring of the event summary")
    ap.add_argument("response", nargs="?", choices=PARTSTAT, default="accept")
    ap.add_argument("--no-notify", action="store_true", help="reply without emailing the organizer")
    ap.add_argument("--send", action="store_true", help="actually call receive_objects (default: dry run)")
    args = ap.parse_args()

    registry = EDataServer.SourceRegistry.new_sync(None)
    hits = find_event(registry, args.summary)
    if len(hits) != 1:
        print(f"need exactly one matching component, found {len(hits)}:", file=sys.stderr)
        for source, _, comp in hits:
            rid = comp.get_recurrenceid()
            print(f"  [{source.get_display_name()}] {comp.get_summary()}"
                  f"{' rid=' + rid.as_ical_string() if rid and not rid.is_null_time() else ''}", file=sys.stderr)
        return 1

    source, client, comp = hits[0]
    me = my_addresses(registry, client)
    organizer = comp.get_first_property(ICalGLib.PropertyKind.ORGANIZER_PROPERTY)

    print(f"calendar : {source.get_display_name()} ({source.get_uid()})")
    print(f"event    : {comp.get_summary()}")
    print(f"organizer: {strip_mailto(organizer.get_organizer()) if organizer else '-'}")
    print(f"me       : {', '.join(sorted(me))}")

    clone = comp.clone()
    mine = None
    for prop in attendees(clone):
        addr = strip_mailto(prop.get_attendee())
        is_me = addr in me
        print(f"  {'*' if is_me else ' '} {addr:40} PARTSTAT={param_str(prop, 'PARTSTAT'):13} RSVP={param_str(prop, 'RSVP')}")
        if is_me and mine is None:
            mine = prop

    if organizer and strip_mailto(organizer.get_organizer()) in me:
        print("you are the organizer; nothing to reply to", file=sys.stderr)
        return 1
    if mine is None:
        print("you are not among the attendees", file=sys.stderr)
        return 1

    mine.remove_parameter_by_kind(ICalGLib.ParameterKind.PARTSTAT_PARAMETER)
    mine.add_parameter(ICalGLib.Parameter.new_partstat(PARTSTAT[args.response]))

    vcal = ICalGLib.Component.new_vcalendar()
    vcal.add_property(ICalGLib.Property.new_version("2.0"))
    vcal.add_property(ICalGLib.Property.new_prodid("-//gnome-calendar fork//rsvp-test//EN"))
    vcal.set_method(ICalGLib.PropertyMethod.REQUEST)

    # Carry the VTIMEZONEs the event refers to, as Evolution does.
    tzids = set()
    for kind in (ICalGLib.PropertyKind.DTSTART_PROPERTY, ICalGLib.PropertyKind.DTEND_PROPERTY,
                 ICalGLib.PropertyKind.RECURRENCEID_PROPERTY):
        prop = clone.get_first_property(kind)
        tzid = prop.get_parameter_as_string("TZID") if prop else None
        if tzid:
            tzids.add(tzid)
    for tzid in tzids:
        try:
            ok, zone = client.get_timezone_sync(tzid, None)
            if ok and zone:
                vcal.add_component(zone.get_component().clone())
        except Exception as exc:
            print(f"warning: no VTIMEZONE for {tzid}: {exc}", file=sys.stderr)

    vcal.add_component(clone)

    flags = ECal.OperationFlags.DISABLE_ITIP_MESSAGE if args.no_notify else ECal.OperationFlags.NONE
    verb = {"accept": "ACCEPTED", "tentative": "TENTATIVE", "decline": "DECLINED"}[args.response]
    notify = "without notifying" if args.no_notify else (
        "emailing the organizer" if param_str(mine, "RSVP").upper() == "TRUE" else "no reply requested, so no email")
    print(f"\n-> receive_objects METHOD:REQUEST, my PARTSTAT={verb}, {notify}")

    for prop in attendees(clone):
        if prop.get_attendee() == mine.get_attendee():
            print(f"   check: my attendee in the outgoing VCALENDAR now has PARTSTAT={param_str(prop, 'PARTSTAT')}")
    assert vcal.get_method() == ICalGLib.PropertyMethod.REQUEST

    if not args.send:
        print("dry run; add --send to do it")
        return 0

    client.receive_objects_sync(vcal, flags, None)
    print("receive_objects: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
