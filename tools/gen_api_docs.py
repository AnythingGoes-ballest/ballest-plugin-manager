"""Generates the API reference pages of the docs site from the script API as it is registered.

    python tools/gen_api_docs.py          writes docs/reference/api/*.md
    python tools/gen_api_docs.py --check  only checks that docs/api-examples.txt covers api.cpp exactly

Every function src/host/api.cpp registers (Global(...), Method(...), including the ones registered in a
`for (const char* type : {...})` loop) must have an entry in docs/api-examples.txt, and every entry there must name
a registered function; otherwise nothing is written and the exit code is 1, so the docs cannot drift from the API.
get_x / set_x pairs are documented together as the property x.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API = ROOT / "src" / "host" / "api.cpp"
EXAMPLES = ROOT / "docs" / "api-examples.txt"
OUT = ROOT / "docs" / "reference" / "api"

STRING = r'"((?:[^"\\]|\\.)*)"'
# Pages in the order of the site's navigation, with the namespaces each one documents.
PAGES = [
    ("log", "Log"), ("host", "Host"), ("plugins", "Plugins"), ("settings", "Settings"), ("registry", "Registry"),
    ("console", "Console"), ("storage", "Storage"), ("ui", "UI"), ("input", "Input"), ("race", "Race"),
    ("editor", "Editor"), ("replay", "Replay"), ("cosmetics", "Cosmetics"),
]


def unescape(s):
    return s.replace('\\"', '"').replace("\\\\", "\\")


def parse_api():
    """[(namespace, type or None, declaration)] in registration order, plus the enums."""
    text = API.read_text(encoding="utf-8")
    entries, enums = [], {}
    namespace, loop_types, depth = "", None, 0
    for line in text.splitlines():
        stripped = line.strip()
        m = re.search(r'SetDefaultNamespace\(' + STRING + r'\)', stripped)
        if m:
            namespace = m.group(1)
            continue
        m = re.match(r'for \(const char\* type : \{(.*)\}\) \{', stripped)
        if m:
            loop_types = re.findall(STRING, m.group(1))
            depth = 1
            continue
        if loop_types is not None:
            m = re.match(r'Method\(type, ' + STRING, stripped)
            if m:
                for t in loop_types:
                    entries.append((namespace, t, unescape(m.group(1))))
            depth += stripped.count("{") - stripped.count("}")
            if depth <= 0:
                loop_types = None
            continue
        m = re.match(r'Global\(' + STRING, stripped)
        if m:
            entries.append((namespace, None, unescape(m.group(1))))
            continue
        m = re.match(r'Method\(' + STRING + r', ' + STRING, stripped)
        if m:
            entries.append((namespace, m.group(1), unescape(m.group(2))))
            continue
        m = re.search(r'RegisterEnumValue\(' + STRING + r', ' + STRING, stripped)
        if m:
            enums.setdefault(f"{namespace}::{m.group(1)}", []).append(m.group(2))
    # Input::Key's named keys are registered from a table, not one call each.
    keys = re.search(r'keys\[\] = \{(.*?)\};', text, re.S)
    if keys:
        enums["Input::Key"] = re.findall(r'\{"(\w+)", 0x', keys.group(1))
    return entries, enums


def name_of(decl):
    return re.match(r'.*?(\w+)\(', decl).group(1)


def collect(entries):
    """One item per documented key: functions, and get_/set_ pairs merged into properties."""
    items = {}
    for namespace, owner, decl in entries:
        name = name_of(decl)
        prefix = f"{namespace}::{owner}." if owner else f"{namespace}::"
        if decl.endswith(" property"):
            prop = name[4:]
            key = prefix + prop
            item = items.setdefault(key, {"namespace": namespace, "owner": owner, "name": prop, "kind": "property",
                                          "type": None, "get": False, "set": False})
            if name.startswith("get_"):
                item["get"] = True
                item["type"] = decl.split(" ")[0]
            else:
                item["set"] = True
                item["type"] = item["type"] or re.search(r'set_\w+\((?:const )?([\w@<>]+)', decl).group(1)
        else:
            items[prefix + name] = {"namespace": namespace, "owner": owner, "name": name, "kind": "function",
                                    "decl": decl}
    return items


def parse_examples():
    """{key: (description, code)} and {section: introduction}."""
    entries, intros = {}, {}
    current, section, lines = None, None, []

    def flush():
        if current is None and section is None:
            return
        body = "\n".join(lines).strip("\n")
        if section is not None:
            intros[section] = body.strip()
            return
        if "\n--\n" not in "\n" + body + "\n":
            raise SystemExit(f"api-examples.txt: entry {current[0]} has no '--' line before its example")
        description, code = re.split(r'^--$', body, maxsplit=1, flags=re.M)
        for key in current:
            entries[key] = (description.strip(), code.strip("\n"))

    started = False
    for line in EXAMPLES.read_text(encoding="utf-8").splitlines():
        if line.startswith("@@ ") or line.startswith("== "):
            if started:
                flush()
            started = True
            if line.startswith("@@ "):
                section, current = line[3:].strip(), None
            else:
                section, current = None, line[3:].split()
            lines = []
        elif started:
            lines.append(line)
    if started:
        flush()
    return entries, intros


def indent(text, spaces=4):
    return "\n".join((" " * spaces + l) if l.strip() else "" for l in text.splitlines())


def render_item(key, item, example):
    description, code = example
    owner = f"{item['namespace']}::{item['owner']}" if item["owner"] else item["namespace"]
    out = [f"### {item['name']}", ""]
    if item["kind"] == "property":
        access = "read and write" if item["get"] and item["set"] else ("read only" if item["get"] else "write only")
        out += ["```cpp", f"{item['type']} {item['name']}    // property of {owner}, {access}", "```", ""]
    else:
        where = f"{owner}." if item["owner"] else f"{owner}::"
        decl = item["decl"]
        name = item["name"]
        out += ["```cpp", decl.replace(f"{name}(", f"{where}{name}(", 1), "```", ""]
    out += [description, "", '??? example "Example"', "    ```cpp", indent(code), "    ```", ""]
    return "\n".join(out)


def main():
    check_only = "--check" in sys.argv
    entries, enums = parse_api()
    items = collect(entries)
    examples, intros = parse_examples()
    missing = [k for k in items if k not in examples]
    unknown = [k for k in examples if k not in items]
    if missing or unknown:
        for k in missing:
            print(f"no entry in docs/api-examples.txt for {k}")
        for k in unknown:
            print(f"docs/api-examples.txt documents {k}, which api.cpp does not register")
        return 1
    print(f"{len(items)} functions and properties, all documented")
    if check_only:
        return 0

    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.md"):
        old.unlink()
    for page, namespace in PAGES:
        out = [f"<!-- Generated by tools/gen_api_docs.py from src/host/api.cpp and docs/api-examples.txt. -->", "",
               f"# {namespace}", "", intros.get(namespace, ""), ""]
        owners = []
        for key, item in items.items():
            if item["namespace"] == namespace and item["owner"] not in owners:
                owners.append(item["owner"])
        for owner in owners:
            if owner:
                out += [f"## {namespace}::{owner}", "", intros.get(f"{namespace}::{owner}", ""), ""]
            elif len(owners) > 1:
                out += [f"## Functions", ""]
            for key, item in items.items():
                if item["namespace"] == namespace and item["owner"] == owner:
                    out.append(render_item(key, item, examples[key]))
        for enum, values in enums.items():
            if enum.startswith(namespace + "::"):
                out += [f"## {enum}", "", ", ".join(f"`{enum.split('::')[0]}::{v}`" for v in values)]
                if enum == "Input::Key":
                    out[-1] += ", plus `Input::A` to `Input::Z` and `Input::N0` to `Input::N9`."
                out.append("")
        (OUT / f"{page}.md").write_text("\n".join(out).rstrip() + "\n", encoding="utf-8")
    print(f"wrote {len(PAGES)} pages to {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
