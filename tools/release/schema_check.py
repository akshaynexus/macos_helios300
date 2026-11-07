#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A small JSON Schema (2020-12) checker for the subset tools/release/manifest.schema.json uses.

No third-party module: release.sh runs on a plain Linux box. Supported keywords: type, const, enum, pattern,
required, properties, additionalProperties, propertyNames, items, minItems, minimum, maximum, minProperties,
maxProperties, minLength, $ref (local #/$defs/...), allOf, if/then. An unknown keyword is an error, so the schema
cannot quietly use something this checker ignores.

    python3 -I tools/release/schema_check.py <schema.json> <document.json>
"""
import json
import re
import sys

KNOWN = {"$schema", "$id", "title", "description", "$defs", "type", "const", "enum", "pattern", "required",
         "properties", "additionalProperties", "propertyNames", "items", "minItems", "minimum", "maximum",
         "minProperties", "maxProperties", "minLength", "$ref", "allOf", "if", "then"}
TYPES = {"object": dict, "array": list, "string": str, "boolean": bool, "null": type(None)}


def _type_ok(value, t):
    if t == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if t == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, TYPES[t])


def check(schema, doc):
    """-> list of 'path: problem' strings (empty = valid)."""
    root = schema
    errors = []

    def resolve(ref):
        if not ref.startswith("#/"):
            raise ValueError(f"only local $ref is supported: {ref}")
        node = root
        for part in ref[2:].split("/"):
            node = node[part]
        return node

    def walk(s, v, path):
        unknown = set(s) - KNOWN
        if unknown:
            raise ValueError(f"schema keyword(s) {sorted(unknown)} at {path} are not supported by this checker")
        before = len(errors)
        if "$ref" in s:
            walk(resolve(s["$ref"]), v, path)
        if "type" in s and not _type_ok(v, s["type"]):
            errors.append(f"{path}: expected {s['type']}, got {type(v).__name__}")
            return
        if "const" in s and v != s["const"]:
            errors.append(f"{path}: must be {s['const']!r}")
        if "enum" in s and v not in s["enum"]:
            errors.append(f"{path}: {v!r} not one of {s['enum']}")
        if isinstance(v, str):
            if "pattern" in s and not re.search(s["pattern"], v):
                errors.append(f"{path}: {v!r} does not match {s['pattern']}")
            if "minLength" in s and len(v) < s["minLength"]:
                errors.append(f"{path}: shorter than {s['minLength']}")
        if _type_ok(v, "number"):
            if "minimum" in s and v < s["minimum"]:
                errors.append(f"{path}: {v} < {s['minimum']}")
            if "maximum" in s and v > s["maximum"]:
                errors.append(f"{path}: {v} > {s['maximum']}")
        if isinstance(v, list):
            if "minItems" in s and len(v) < s["minItems"]:
                errors.append(f"{path}: fewer than {s['minItems']} items")
            if "items" in s:
                for i, item in enumerate(v):
                    walk(s["items"], item, f"{path}[{i}]")
        if isinstance(v, dict):
            for k in s.get("required", []):
                if k not in v:
                    errors.append(f"{path}: missing {k!r}")
            if "minProperties" in s and len(v) < s["minProperties"]:
                errors.append(f"{path}: fewer than {s['minProperties']} properties")
            if "maxProperties" in s and len(v) > s["maxProperties"]:
                errors.append(f"{path}: more than {s['maxProperties']} properties")
            props = s.get("properties", {})
            for k, item in v.items():
                if "propertyNames" in s:
                    walk(s["propertyNames"], k, f"{path}.<{k}>")
                if k in props:
                    walk(props[k], item, f"{path}.{k}")
                elif "additionalProperties" in s:
                    extra = s["additionalProperties"]
                    if extra is False:
                        errors.append(f"{path}: unexpected property {k!r}")
                    elif isinstance(extra, dict):
                        walk(extra, item, f"{path}.{k}")
        for sub in s.get("allOf", []):
            walk(sub, v, path)
        if "if" in s:
            probe = len(errors)
            walk(s["if"], v, path)
            matched = len(errors) == probe
            del errors[probe:]
            if matched and "then" in s:
                walk(s["then"], v, path)
        return len(errors) == before

    walk(schema, doc, "$")
    return errors


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2
    with open(argv[1]) as fh:
        schema = json.load(fh)
    with open(argv[2]) as fh:
        doc = json.load(fh)
    errs = check(schema, doc)
    for e in errs:
        print("schema:", e)
    print(f"schema: {'OK' if not errs else 'FAIL'} ({argv[2]})")
    return 0 if not errs else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
