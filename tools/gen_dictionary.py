#!/usr/bin/env python3
"""Generates include-able tables of the FIX 4.4 dictionary the session checks
against (note 006, D1 and D2; note 007, D8) from the FIX Trading Community's
orchestrations (github.com/FIXTradingCommunity/orchestrations, Apache-2.0),
fetched by CMake at a pinned commit into build/. Nothing here, and nothing it
writes, is committed but this script.

  gen_dictionary.py <OrchestraFIX44.xml> <FIX44Session.xml> <commit> <out.hpp>

From the full FIX 4.4 orchestration: every tag number FIX 4.4 defines (14a
against 14c), every MsgType (2q), and the messages the gateway handles: the
seven session messages and the order-entry ones, each flattened (components
inlined, header and trailer included) into one table of its tags, sorted, each
tag with its place: top level or a repeating group, and its position there.
The session file must agree with the full one on the session messages, or the
script fails.
"""
import sys
import xml.etree.ElementTree as ET

SESSION = ["0", "1", "2", "3", "4", "5", "A"]  # n (XMLnonFIX) is not supported: valid, not ours
APPLICATION = ["D", "F", "G", "8", "9", "j"]  # note 007: order entry, and BusinessMessageReject
# MsgType (35) and RefMsgType (372) are checked as strings: whether a MsgType is
# valid is D4's own check against every FIX 4.4 MsgType (note 006), and the
# session file lists only its own in the code set (and types 372 as String).
STRING_OVERRIDES = (35, 372)

# Datatypes to the checks the session makes; a field with a code set is
# checked against its values instead.
KINDS = {
    "String": "string", "Currency": "string", "Exchange": "string", "Country": "string",
    "MultipleValueString": "string", "data": "data", "Length": "length", "SeqNum": "seq_num",
    "NumInGroup": "num_in_group", "int": "integer", "DayOfMonth": "integer", "TagNum": "integer",
    "float": "decimal", "Qty": "decimal", "Price": "decimal", "PriceOffset": "decimal", "Amt": "decimal",
    "Percentage": "decimal", "char": "single_char", "Boolean": "boolean", "UTCTimestamp": "utc_timestamp",
    "UTCTimeOnly": "utc_time_only", "UTCDateOnly": "date", "LocalMktDate": "date", "MonthYear": "month_year",
}


def load(path, wanted):
    root = ET.parse(path).getroot()
    ns = {"f": root.tag.split("}")[0][1:]}
    fields = {int(f.get("id")): [f.get("name"), f.get("type"), f.get("unionDataType")] for f in root.iterfind(".//f:fields/f:field", ns)}
    code_sets = {c.get("name"): (c.get("type"), [x.get("value") for x in c.iterfind("f:code", ns)])
                 for c in root.iterfind(".//f:codeSets/f:codeSet", ns)}
    components = {c.get("id"): c for c in root.iterfind(".//f:components/f:component", ns)}
    groups = {g.get("id"): g for g in root.iterfind(".//f:groups/f:group", ns)}
    messages = {m.get("msgType"): m for m in root.iterfind(".//f:messages/f:message", ns)}

    # A message flattened: (tag, parent NumInGroup tag or 0, position at its
    # level, required, is a NumInGroup, group name).
    def flatten(el, parent, out, counter, required_here=True):
        for ch in el:
            k = ch.tag.split("}")[1]
            req = required_here and ch.get("presence") == "required"
            if k == "fieldRef":
                out.append((int(ch.get("id")), parent, counter[parent], req, False, ""))
                counter[parent] += 1
            elif k == "componentRef":
                # a component's fields are required only if it is
                flatten(components[ch.get("id")], parent, out, counter, req)
            elif k == "groupRef":
                g = groups[ch.get("id")]
                count = int(g.find("f:numInGroup", ns).get("id"))
                out.append((count, parent, counter[parent], req, True, g.get("name")))
                counter[parent] += 1
                counter[count] = 0
                flatten(g, count, out, counter)  # required within each entry (not checked, D3)

    flat = {}
    for t in wanted:
        if t not in messages:
            continue
        out, counter = [], {0: 0}
        flatten(messages[t].find("f:structure", ns), 0, out, counter)
        flat[t] = out
    return fields, code_sets, flat, sorted(messages)


def main():
    full_path, session_path, commit, out_path = sys.argv[1:5]
    fields, code_sets, flat, msg_types = load(full_path, SESSION + APPLICATION)
    s_fields, s_code_sets, s_flat, _ = load(session_path, SESSION)
    for t in STRING_OVERRIDES:
        fields[t][1] = "String"
    for t in SESSION:
        if [r[:5] for r in s_flat[t]] != [r[:5] for r in flat[t]]:
            raise SystemExit(f"OrchestraFIX44.xml and FIX44Session.xml disagree on MsgType {t}")
        for r in flat[t]:
            tag = r[0]
            if tag in STRING_OVERRIDES:
                continue
            if fields[tag][:2] != s_fields[tag][:2] or code_sets.get(fields[tag][1]) != s_code_sets.get(fields[tag][1]):
                raise SystemExit(f"tag {tag}: the two orchestrations disagree")
    for t, rows in flat.items():
        tags = [r[0] for r in rows]
        if len(tags) != len(set(tags)):
            raise SystemExit(f"MsgType {t}: a tag appears in two places; the tables assume one")

    used = sorted({r[0] for rows in flat.values() for r in rows})

    def kind(t):
        typ = fields[t][1]
        if typ in code_sets:
            return "multi_code_set" if code_sets[typ][0] == "MultipleValueString" else "code_set"
        if typ not in KINDS:
            raise SystemExit(f"tag {t} {fields[t][0]}: datatype {typ} not handled")
        return KINDS[typ]

    codes, code_index = [], {}
    for t in used:
        typ = fields[t][1]
        if typ in code_sets and typ not in code_index:
            code_index[typ] = (len(codes), len(code_sets[typ][1]))
            codes.extend(code_sets[typ][1])

    defined = sorted(fields)
    words = max(defined) // 64 + 1
    bits = [0] * words
    for t in defined:
        bits[t // 64] |= 1 << (t % 64)

    # Each message's tags sorted by tag; `parent` is the index, in the same
    # run, of the NumInGroup row the tag belongs to, or 0xffff at top level.
    rows_out, runs = [], []
    for t in SESSION + APPLICATION:
        rows = sorted(flat[t], key=lambda r: r[0])
        index = {r[0]: i for i, r in enumerate(rows)}
        first = len(rows_out)
        for tag, parent, pos, req, is_group, gname in rows:
            p = index[parent] if parent else 0xFFFF
            rows_out.append((tag, p, pos, req, is_group))
        runs.append((t, first, len(rows)))
    if max(r[2] for r in rows_out) > 255:
        raise SystemExit("a position above 255")

    o = []
    w = o.append
    w("// Generated by tools/gen_dictionary.py from the FIX Trading Community's")
    w(f"// orchestrations (Apache-2.0), commit {commit}: OrchestraFIX44.xml and")
    w("// FIX44Session.xml. Do not edit. Design: docs/design/006-conformance.md,")
    w("// docs/design/007-order-entry.md")
    w("// Overridden: MsgType (35) and RefMsgType (372) as strings (MsgType's")
    w("// validity is checked against msg_types, note 006 D4).")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("#include <string_view>")
    w("")
    w("namespace fixgw::fix44 {")
    w("")
    w("// What a value is checked against (note 006, D3).")
    w("enum class kind : std::uint8_t {")
    w("    string, data, length, seq_num, num_in_group, integer, decimal, single_char, boolean,")
    w("    utc_timestamp, utc_time_only, date, month_year, code_set, multi_code_set")
    w("};")
    w("")
    w(f"// Every tag number FIX 4.4 defines ({len(defined)}, the highest {max(defined)}), one bit each.")
    w(f"inline constexpr std::uint32_t highest_tag = {max(defined)};")
    w(f"inline constexpr std::uint64_t defined_tags[{words}] = {{")
    for i in range(0, words, 3):
        w("    " + ", ".join(f"0x{b:016x}u" for b in bits[i:i + 3]) + ",")
    w("};")
    w("")
    w(f"// Every FIX 4.4 MsgType ({len(msg_types)}), sorted.")
    w(f"inline constexpr std::string_view msg_types[{len(msg_types)}] = {{")
    for i in range(0, len(msg_types), 12):
        w("    " + ", ".join(f'"{t}"' for t in msg_types[i:i + 12]) + ",")
    w("};")
    w("")
    w("// Enumerated values, concatenated; a field_info's codes point into them.")
    w(f"inline constexpr std::string_view codes[{len(codes)}] = {{")
    for i in range(0, len(codes), 10):
        w("    " + ", ".join('"' + c.replace("\\", "\\\\").replace('"', '\\"') + '"' for c in codes[i:i + 10]) + ",")
    w("};")
    w("")
    w("// Each field the messages use, sorted by tag.")
    w("// `union_from`: a code set whose union datatype (EP311) also admits any")
    w("// integer from this value up (100, 1000, 4000: user-defined); `union_qty`:")
    w("// one that also admits a quantity.")
    w("struct field_info {")
    w("    std::uint32_t tag;")
    w("    kind k;")
    w("    std::uint16_t codes_first, codes_count;")
    w("    std::uint16_t union_from;")
    w("    bool union_qty;")
    w("};")
    w(f"inline constexpr field_info fields[{len(used)}] = {{")
    for t in used:
        f, n = code_index.get(fields[t][1], (0, 0))
        union = fields[t][2] or ""
        known = {"": 0, "Reserved100Plus": 100, "Reserved1000Plus": 1000, "Reserved4000Plus": 4000, "Qty": 0}
        if union not in known:
            raise SystemExit(f"tag {t}: union datatype {union} not handled")
        w(f"    {{{t}, kind::{kind(t)}, {f}, {n}, {known[union]}, {'true' if union == 'Qty' else 'false'}}},  // {fields[t][0]}")
    w("};")
    w("")
    w("// A message's tags, sorted by tag: `parent` is the index within the same")
    w("// run of the NumInGroup tag whose group the tag is in (no_parent at the top")
    w("// level); `position` its order at its level (0 starts a group's entry).")
    w("inline constexpr std::uint16_t no_parent = 0xffff;")
    w("struct node {")
    w("    std::uint32_t tag;")
    w("    std::uint16_t parent;")
    w("    std::uint8_t position;")
    w("    bool required;  // at its level")
    w("    bool group;     // a NumInGroup tag")
    w("};")
    w(f"inline constexpr node nodes[{len(rows_out)}] = {{")
    for tag, p, pos, req, g in rows_out:
        w(f"    {{{tag}, {p}, {pos}, {'true' if req else 'false'}, {'true' if g else 'false'}}},  // {fields[tag][0]}")
    w("};")
    w("")
    w("// Each message the gateway handles: its run of nodes.")
    w("struct message_def {")
    w("    std::string_view type;")
    w("    std::uint16_t first, count;")
    w("};")
    w(f"inline constexpr message_def messages[{len(runs)}] = {{")
    for t, f, n in runs:
        w(f'    {{"{t}", {f}, {n}}},')
    w("};")
    w("")
    w("}  // namespace fixgw::fix44")
    with open(out_path, "w") as fh:
        fh.write("\n".join(o) + "\n")


if __name__ == "__main__":
    main()
