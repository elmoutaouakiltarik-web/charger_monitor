#!/usr/bin/env python3
"""Validate `charger_settle --ocpp` exports against the real OCPP 2.0.1 schema.

Optional developer tool: the C++ library has no dependencies and this script is
not part of the build. It checks that every element of `events` is a valid
TransactionEventRequest payload by the JSON schema that ships in the `ocpp`
Python package (mobilityhouse/ocpp, OCPP 2.0.1), plus a few consistency checks
the schema cannot express.

    python3 -m venv /tmp/ocppvenv
    /tmp/ocppvenv/bin/pip install ocpp jsonschema
    charger_settle bay-07.journal --ocpp bay-07 12 > ev.json
    /tmp/ocppvenv/bin/python scripts/validate_ocpp_export.py ev.json [more.json ...]
    charger_settle ... | /tmp/ocppvenv/bin/python scripts/validate_ocpp_export.py -

Exit status 0 when every file validates, 1 when any does not, 2 on usage or a
missing dependency.

What the schema does NOT check, and this script does:
  * `date-time` formats are really validated (jsonschema skips them unless an
    optional package is installed), as RFC 3339 with a trailing Z;
  * both events share one transactionId, seqNo is 0 then 1, Started precedes or
    equals Ended;
  * the Started register is 0 and the Ended register equals the session energy
    the wrapper's own numbers imply (an integer, in micro-Wh);
  * `value` is an exact integer within 2^53 (a double-based parser would
    otherwise round it).
"""

import json
import os
import re
import sys

RFC3339_Z = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$")


def load_schema():
    try:
        import jsonschema  # noqa: F401
        import ocpp
    except ImportError as exc:  # pragma: no cover - environment dependent
        sys.stderr.write(f"missing dependency ({exc}); pip install ocpp jsonschema\n")
        sys.exit(2)
    path = os.path.join(os.path.dirname(ocpp.__file__), "v201", "schemas", "TransactionEventRequest.json")
    with open(path, encoding="utf-8") as fh:
        return json.load(fh), path


def check_timestamp(text):
    from datetime import datetime

    if not RFC3339_Z.match(text):
        return False
    try:
        datetime.strptime(text[:19], "%Y-%m-%dT%H:%M:%S")
    except ValueError:
        return False
    return True


def validate_document(doc, validator):
    problems = []
    if doc.get("schema") != "charger.ocpp-events.v1":
        problems.append(f"schema is {doc.get('schema')!r}, expected 'charger.ocpp-events.v1'")
    events = doc.get("events")
    if not isinstance(events, list) or len(events) != 2:
        problems.append("events must be a list of exactly two payloads (Started, Ended)")
        return problems
    for i, ev in enumerate(events):
        for err in sorted(validator.iter_errors(ev), key=lambda e: list(e.path)):
            where = "/".join(str(p) for p in err.path) or "<root>"
            problems.append(f"events[{i}] {where}: {err.message}")
        for ts in [ev.get("timestamp")] + [m.get("timestamp") for m in ev.get("meterValue", [])]:
            if not isinstance(ts, str) or not check_timestamp(ts):
                problems.append(f"events[{i}] timestamp {ts!r} is not RFC 3339 UTC with a trailing Z")
    if problems:
        return problems
    started, ended = events
    if (started["eventType"], ended["eventType"]) != ("Started", "Ended"):
        problems.append("event types must be Started then Ended")
    if (started["seqNo"], ended["seqNo"]) != (0, 1):
        problems.append("seqNo must be 0 then 1")
    if started["transactionInfo"]["transactionId"] != ended["transactionInfo"]["transactionId"]:
        problems.append("the two events carry different transactionIds")
    if started["timestamp"] > ended["timestamp"]:
        problems.append("Started is later than Ended")

    def register(ev, context):
        values = [
            sv
            for mv in ev["meterValue"]
            for sv in mv["sampledValue"]
            if sv.get("measurand") == "Energy.Active.Import.Register" and sv.get("context") == context
        ]
        if len(values) != 1:
            return None
        v = values[0]["value"]
        uom = values[0].get("unitOfMeasure", {})
        if uom != {"unit": "Wh", "multiplier": -6}:
            problems.append(f"{context}: unitOfMeasure must be Wh with multiplier -6, got {uom}")
        if isinstance(v, bool) or not isinstance(v, int) or v < 0 or v > 2**53:
            problems.append(f"{context}: value {v!r} is not an exact non-negative integer within 2^53")
        return v

    begin, end = register(started, "Transaction.Begin"), register(ended, "Transaction.End")
    if begin is None or end is None:
        problems.append("a Transaction.Begin / Transaction.End Energy.Active.Import.Register is missing")
    elif begin != 0:
        problems.append("the session-relative register must start at 0")
    return problems


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    import jsonschema

    schema, path = load_schema()
    validator_cls = jsonschema.validators.validator_for(schema)
    validator_cls.check_schema(schema)
    validator = validator_cls(schema)
    print(f"schema: {path}")
    print(f"        {schema.get('$id', '')}  ({validator_cls.__name__})")
    worst = 0
    for name in argv[1:]:
        text = sys.stdin.read() if name == "-" else open(name, encoding="utf-8").read()
        try:
            doc = json.loads(text)
        except json.JSONDecodeError as exc:
            print(f"{name}: NOT JSON: {exc}")
            worst = 1
            continue
        problems = validate_document(doc, validator)
        sess = doc.get("session", {}) if isinstance(doc, dict) else {}
        label = f"{sess.get('asset_id', '?')}#{sess.get('session_id', '?')} stop={sess.get('stop_reason', '?')}"
        if problems:
            worst = 1
            print(f"{name}: INVALID ({label})")
            for p in problems:
                print(f"    - {p}")
        else:
            ev = doc["events"]
            print(
                f"{name}: VALID ({label}) "
                f"{ev[0]['eventType']}@{ev[0]['timestamp']} -> {ev[1]['eventType']}@{ev[1]['timestamp']} "
                f"stoppedReason={ev[1]['transactionInfo']['stoppedReason']} "
                f"trigger={ev[1]['triggerReason']}"
            )
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv))
