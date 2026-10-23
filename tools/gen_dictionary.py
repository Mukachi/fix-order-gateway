#!/usr/bin/env python3
"""Generates include-able tables of the FIX 4.4 dictionary the session checks
against (note 006, D1 and D2) from the FIX Trading Community's orchestrations
(github.com/FIXTradingCommunity/orchestrations, Apache-2.0), fetched by CMake
at a pinned commit into build/. Nothing here, and nothing it writes, is
committed but this script.

  gen_dictionary.py <OrchestraFIX44.xml> <FIX44Session.xml> <commit> <out.hpp>

From the full FIX 4.4 orchestration: every tag number FIX 4.4 defines (14a
against 14c) and every MsgType (2q). From both: the session messages, the
standard header and trailer, their fields' types and enumerations, and their
repeating groups; the two files must agree on these, or the script fails.
"""
import sys
import xml.etree.ElementTree as ET

SESSION = ["0", "1", "2", "3", "4", "5", "A"]  # n (XMLnonFIX) is not supported: valid, not ours
HEADER, TRAILER = "1024", "1025"
STRING_OVERRIDES = (35, 372)  # the orchestration's ids for StandardHeader, StandardTrailer

# Datatypes to the checks the session makes; a field with a code set is
# checked against its values instead.
KINDS = {
    "String": "string", "data": "data", "Length": "length", "SeqNum": "seq_num",
    "NumInGroup": "num_in_group", "int": "integer", "UTCTimestamp": "utc_timestamp",
}


def load(path):
    root = ET.parse(path).getroot()
    ns = {"f": root.tag.split("}")[0][1:]}
    fields = {int(f.get("id")): (f.get("name"), f.get("type")) for f in root.iterfind(".//f:fields/f:field", ns)}
    code_sets = {c.get("name"): [x.get("value") for x in c.iterfind("f:code", ns)]
                 for c in root.iterfind(".//f:codeSets/f:codeSet", ns)}
    components = {c.get("id"): c for c in root.iterfind(".//f:components/f:component", ns)}
    groups = {g.get("id"): g for g in root.iterfind(".//f:groups/f:group", ns)}
    messages = {m.get("msgType"): m for m in root.iterfind(".//f:messages/f:message", ns)}

    def members(el):
        out = []
        for ch in el:
            k = ch.tag.split("}")[1]
            req = ch.get("presence") == "required"
            if k == "fieldRef":
                out.append(("field", int(ch.get("id")), req))
            elif k == "groupRef":
                g = groups[ch.get("id")]
                count = int(g.find("f:numInGroup", ns).get("id"))
                inner = members(g)
                if any(m[0] != "field" for m in inner):
                    raise SystemExit(f"group {g.get('name')}: nested groups or components not handled")
                out.append(("group", count, req, g.get("name"), [(m[1], m[2]) for m in inner]))
            elif k == "componentRef":
                out.append(("component", ch.get("id"), req))
        return out

    layouts = {"header": members(components[HEADER]), "trailer": members(components[TRAILER])}
    for t in SESSION:
        body = [m for m in members(messages[t].find("f:structure", ns)) if m[0] != "component"]
        layouts[t] = body
    return fields, code_sets, layouts, sorted(messages), messages


def main():
    full_path, session_path, commit, out_path = sys.argv[1:5]
    fields, code_sets, layouts, msg_types, _ = load(full_path)
    s_fields, s_code_sets, s_layouts, _, _ = load(session_path)
    if s_layouts != layouts:
        raise SystemExit("OrchestraFIX44.xml and FIX44Session.xml disagree on the session messages")

    used = set()
    groups = []
    for name, ms in layouts.items():
        for m in ms:
            if m[0] == "field":
                used.add(m[1])
            else:
                used.add(m[1])
                used.update(t for t, _ in m[4])
    # MsgType (35) and RefMsgType (372) are checked as strings: whether a MsgType
    # is valid is D4's own check against every FIX 4.4 MsgType, and the session
    # file lists only its own in the code set (and types 372 as String).
    for t in STRING_OVERRIDES:
        fields[t] = (fields[t][0], "String")
    for t in used:
        if t in STRING_OVERRIDES:
            continue
        if fields[t] != s_fields[t] or (fields[t][1] in code_sets and code_sets[fields[t][1]] != s_code_sets.get(fields[t][1])):
            raise SystemExit(f"tag {t}: the two orchestrations disagree")

    # Code values, concatenated; each field with a code set points into them.
    codes, code_index = [], {}
    for t in sorted(used):
        typ = fields[t][1]
        if typ in code_sets and typ not in code_index:
            code_index[typ] = (len(codes), len(code_sets[typ]))
            codes.extend(code_sets[typ])
    if max(code_index.get(fields[t][1], (0, 0))[0] + code_index.get(fields[t][1], (0, 0))[1] for t in used) > 65535:
        raise SystemExit("too many code values")

    def kind(t):
        typ = fields[t][1]
        if typ in code_sets:
            return "code_set"
        if typ not in KINDS:
            raise SystemExit(f"tag {t} {fields[t][0]}: datatype {typ} not handled")
        return KINDS[typ]

    # Members, every layout's in order; a group is one member (its NumInGroup
    # tag) pointing at its own run of members.
    member_rows, layout_rows, group_rows = [], [], []
    for name, ms in layouts.items():
        first = len(member_rows)
        for m in ms:
            if m[0] == "field":
                member_rows.append((m[1], m[2], 0))
            else:
                group_rows.append((m[1], m[3], m[4]))
                member_rows.append((m[1], m[2], len(group_rows)))
        layout_rows.append((name, first, len(member_rows) - first))
    group_member_rows, group_defs = [], []
    for count, gname, inner in group_rows:
        group_defs.append((count, gname, len(group_member_rows), len(inner)))
        group_member_rows.extend(inner)

    defined = sorted(fields)
    words = max(defined) // 64 + 1
    bits = [0] * words
    for t in defined:
        bits[t // 64] |= 1 << (t % 64)

    o = []
    w = o.append
    w("// Generated by tools/gen_dictionary.py from the FIX Trading Community's")
    w(f"// orchestrations (Apache-2.0), commit {commit}: OrchestraFIX44.xml and")
    w("// FIX44Session.xml. Do not edit. Design: docs/design/006-conformance.md")
    w("// Overridden: MsgType (35) and RefMsgType (372) as strings (MsgType's")
    w("// validity is checked against msg_types, note 006 D4).")
    w("#pragma once")
    w("")
    w("#include <cstdint>")
    w("#include <string_view>")
    w("")
    w("namespace fixgw::fix44 {")
    w("")
    w("// What the session checks a value against (note 006, D3).")
    w("enum class kind : std::uint8_t { string, data, length, seq_num, num_in_group, integer, utc_timestamp, code_set };")
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
    for i in range(0, len(codes), 12):
        w("    " + ", ".join(f'"{c}"' for c in codes[i:i + 12]) + ",")
    w("};")
    w("")
    w("// Each field the session messages, header and trailer use, sorted by tag.")
    w("struct field_info {")
    w("    std::uint32_t tag;")
    w("    kind k;")
    w("    std::uint16_t codes_first, codes_count;")
    w("};")
    w(f"inline constexpr field_info fields[{len(used)}] = {{")
    for t in sorted(used):
        f, n = code_index.get(fields[t][1], (0, 0))
        w(f"    {{{t}, kind::{kind(t)}, {f}, {n}}},  // {fields[t][0]}")
    w("};")
    w("")
    w("// A message's members in order: a field, or a repeating group by its")
    w("// NumInGroup tag (group > 0: groups[group - 1]).")
    w("struct member {")
    w("    std::uint32_t tag;")
    w("    bool required;")
    w("    std::uint8_t group;")
    w("};")
    w(f"inline constexpr member members[{len(member_rows)}] = {{")
    for t, req, g in member_rows:
        w(f"    {{{t}, {'true' if req else 'false'}, {g}}},  // {fields[t][0]}")
    w("};")
    w("")
    w("// A repeating group: its NumInGroup tag and its fields in order, the first")
    w("// starting each entry.")
    w("struct group_member {")
    w("    std::uint32_t tag;")
    w("    bool required;")
    w("};")
    w(f"inline constexpr group_member group_members[{len(group_member_rows)}] = {{")
    for t, req in group_member_rows:
        w(f"    {{{t}, {'true' if req else 'false'}}},  // {fields[t][0]}")
    w("};")
    w("struct group_info {")
    w("    std::uint32_t count_tag;")
    w("    std::uint16_t first, count;")
    w("};")
    w(f"inline constexpr group_info groups[{len(group_defs)}] = {{")
    for c, gname, f, n in group_defs:
        w(f"    {{{c}, {f}, {n}}},  // {gname}")
    w("};")
    w("")
    w("// The header, the trailer and each session message's body: a run of members.")
    w("struct layout {")
    w("    std::string_view name;  // a MsgType, \"header\" or \"trailer\"")
    w("    std::uint16_t first, count;")
    w("};")
    w(f"inline constexpr layout layouts[{len(layout_rows)}] = {{")
    for name, f, n in layout_rows:
        w(f'    {{"{name}", {f}, {n}}},')
    w("};")
    w("")
    w("}  // namespace fixgw::fix44")
    with open(out_path, "w") as fh:
        fh.write("\n".join(o) + "\n")


if __name__ == "__main__":
    main()
