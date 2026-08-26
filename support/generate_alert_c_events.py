#!/usr/bin/env python3
"""Generate nrsc5's ALERT-C event metadata from the OpenStreetMap wiki."""

import argparse
import html
import os
import re
import tempfile
import urllib.request


# The Q column selects an ISO 14819-2 quantifier type, while descriptions use
# markers such as (Q), (a/Q), and lorr(y/ies).
EVENTS_URL = "https://wiki.openstreetmap.org/w/index.php?title=TMC/Event_Code_List&action=raw"
QUANTIFIER = re.compile(r"\bQ\b")
QUANTIFIED_CLAUSE = re.compile(r"\(([^()]*(?:\bQ\b)[^()]*)\)")


def c_string(value):
    output = ['"']
    for byte in value.encode("utf-8"):
        if byte == ord('"'):
            output.append(r'\"')
        elif byte == ord('\\'):
            output.append(r'\\')
        elif 0x20 <= byte < 0x7F:
            output.append(chr(byte))
        else:
            output.append(f"\\{byte:03o}")
    output.append('"')
    return "".join(output)


def clean_wiki_text(value):
    value = re.sub(r"\[\[(?:[^\]|]*\|)?([^\]]+)\]\]", r"\1", value)
    value = re.sub(r"''+", "", value)
    value = re.sub(r"<[^>]+>", "", value)
    return re.sub(r"\s+", " ", html.unescape(value)).strip()


def event_descriptions(description):
    has_quantifier = QUANTIFIER.search(description) is not None
    # A Q-bearing parenthetical is optional detail, so removing it yields the
    # description used when the message supplies no quantifier.
    plain = QUANTIFIED_CLAUSE.sub("", description) if has_quantifier else description
    plain = plain.replace("(s)", "").replace("(es)", "").replace("(y/ies)", "y")
    plain = re.sub(r"\s+([,.])", r"\1", plain)
    plain = re.sub(r"\s+", " ", plain).strip().rstrip(",")
    if not has_quantifier:
        return plain, None

    def quantified_clause(match):
        clause = match.group(1)
        # Canonical markers make runtime replacement unambiguous. Keep word
        # inflections such as accident(s) until the numeric value is known.
        if re.fullmatch(r"Q\s*th", clause):
            return "(Qth)"
        return QUANTIFIER.sub("(Q)", clause.replace("a/Q", "Q"))

    quantified = QUANTIFIED_CLAUSE.sub(quantified_clause, description)
    if quantified.count("(Q)") + quantified.count("(Qth)") != 1:
        raise ValueError(f"invalid quantified ALERT-C description: {description}")
    return plain, quantified


def read_events(url):
    request = urllib.request.Request(url, headers={"User-Agent": "nrsc5-event-generator/1"})
    with urllib.request.urlopen(request, timeout=30) as response:
        source = response.read().decode("utf-8")
    events = {}
    for line in source.splitlines():
        if not line.startswith("|"):
            continue
        columns = line[1:].split("||")
        if len(columns) < 9 or not columns[0].isdigit():
            continue
        code = int(columns[0])
        description = clean_wiki_text(columns[1])
        plain, quantified = event_descriptions(description)
        try:
            quantifier_type = int(columns[3])
        except ValueError:
            quantifier_type = -1
        if not 0 <= code < 2048 or code in events or not description or not plain:
            raise ValueError(f"invalid ALERT-C event row {code}")
        if quantified is not None and not 0 <= quantifier_type <= 12:
            raise ValueError(f"invalid ALERT-C quantifier type for event {code}")
        events[code] = (plain, quantified, quantifier_type)
    if len(events) != 1552:
        raise ValueError(f"expected 1552 ALERT-C events, found {len(events)}")
    return events


def generate(url, output_path):
    events = read_events(url)
    output_dir = os.path.dirname(os.path.abspath(output_path))
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="ascii", newline="",
                                         dir=output_dir, delete=False) as output:
            temporary_path = output.name
            output.write("/* Generated from https://wiki.openstreetmap.org/wiki/TMC/Event_Code_List. */\n")
            for code, (plain, quantified, quantifier_type) in sorted(events.items()):
                quantified_value = c_string(quantified) if quantified is not None else "NULL"
                output.write(
                    f"    [{code}] = {{ {c_string(plain)}, {quantified_value}, "
                    f"{quantifier_type if quantified is not None else 0} }},\n")
        os.replace(temporary_path, output_path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                os.unlink(temporary_path)
            except FileNotFoundError:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="output C initializer include")
    parser.add_argument("--url", default=EVENTS_URL, help="MediaWiki raw-page URL")
    args = parser.parse_args()
    try:
        generate(args.url, args.output)
    except (OSError, ValueError, UnicodeError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
