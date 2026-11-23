#!/usr/bin/env python3
"""Opt-in metadata declarations -> C++23 registration and canonical schema manifest."""

import argparse
import json
import os
from pathlib import Path, PurePosixPath
import re
import sys
import tempfile


IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")
QUALIFIED = re.compile(r"[A-Za-z_][A-Za-z_0-9]*(?:::[A-Za-z_][A-Za-z_0-9]*)*\Z")
CPP_TYPE = re.compile(r"[A-Za-z_][A-Za-z_0-9:<> ,*&\[\]]*\Z")


def object_without_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def keys(value, required, optional=()):
    if not isinstance(value, dict):
        raise ValueError("expected an object")
    missing = set(required) - value.keys()
    unknown = value.keys() - set(required) - set(optional)
    if missing or unknown:
        raise ValueError(f"missing keys {sorted(missing)}; unknown keys {sorted(unknown)}")


def positive_integer(value, bits):
    if isinstance(value, str):
        if not re.fullmatch(r"(?:0x[0-9a-fA-F]+|[0-9]+)", value):
            raise ValueError(f"invalid explicit integer {value!r}")
        value = int(value, 16 if value.startswith("0x") else 10)
    if type(value) is not int or not 0 < value < 2**bits:
        raise ValueError(f"expected a nonzero uint{bits}, got {value!r}")
    return value


def text(value):
    if not isinstance(value, str) or not value or not value.isprintable():
        raise ValueError("expected a nonempty printable string")
    value.encode("utf-8")
    return value


def cpp_type(value):
    value = text(value)
    # The generator does not parse C++; the actual compiler validates this type expression.
    if not CPP_TYPE.fullmatch(value.removeprefix("::")):
        raise ValueError(f"unsupported C++ type expression {value!r}")
    return value


def unique(items, key, context):
    seen = set()
    for item in items:
        value = item[key]
        if value in seen:
            raise ValueError(f"duplicate {context} {key}: {value!r}")
        seen.add(value)


def load_schemas(paths):
    types = []
    for path in paths:
        try:
            document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=object_without_duplicates)
            keys(document, ("format_version", "types"))
            if type(document["format_version"]) is not int or document["format_version"] != 1:
                raise ValueError("unsupported metadata declaration format_version")
            if not isinstance(document["types"], list):
                raise ValueError("types must be an array")
            for declaration in document["types"]:
                keys(declaration, ("id", "cpp_type", "name", "version", "kind", "header"), ("fields",))
                header = text(declaration["header"])
                if (not re.fullmatch(r"[A-Za-z_0-9./-]+", header) or
                        PurePosixPath(header).is_absolute() or ".." in PurePosixPath(header).parts):
                    raise ValueError("header must be a portable relative include path without '..'")
                kind = declaration["kind"]
                if kind not in ("VALUE", "COMPONENT", "ENUM"):
                    raise ValueError(f"unknown metadata kind {kind!r}")
                fields = declaration.get("fields", [])
                if not isinstance(fields, list) or (kind == "ENUM" and fields):
                    raise ValueError("fields must be an array; enum declarations cannot contain object fields")
                normalized_fields = []
                for field in fields:
                    keys(field, ("id", "member", "name", "cpp_type"))
                    member = text(field["member"])
                    if not IDENTIFIER.fullmatch(member):
                        raise ValueError(f"invalid C++ member identifier {member!r}")
                    normalized_fields.append({"id": positive_integer(field["id"], 64), "member": member,
                                              "name": text(field["name"]), "cpp_type": cpp_type(field["cpp_type"])})
                for key in ("id", "member", "name"):
                    unique(normalized_fields, key, "field")
                types.append({"id": positive_integer(declaration["id"], 64),
                              "cpp_type": cpp_type(declaration["cpp_type"]), "name": text(declaration["name"]),
                              "version": positive_integer(declaration["version"], 32), "kind": kind,
                              "header": PurePosixPath(header).as_posix(),
                              "fields": sorted(normalized_fields, key=lambda field: field["id"])})
        except (ValueError, OSError) as error:
            raise ValueError(f"{path}: {error}") from error
    for key in ("id", "cpp_type", "name"):
        unique(types, key, "type")
    return sorted(types, key=lambda declaration: declaration["id"])


def literal(value):
    return json.dumps(value, ensure_ascii=False)


def generate(types, function, header_name):
    parts = function.split("::")
    namespace = "::".join(parts[:-1])
    header = "#pragma once\n\n#include <GraphicsEngine/lib/common/type_registry.h>\n\n"
    if namespace:
        header += f"namespace {namespace} {{\n\n"
    header += f"void {parts[-1]}(::NCommon::TypeRegistry& registry);\n"
    if namespace:
        header += f"\n}} // namespace {namespace}\n"
    source = [f'#include "{header_name}"', "#include <GraphicsEngine/lib/common/metadata.h>",
              "#include <type_traits>", "#include <utility>"]
    source += [f"#include <{name}>" for name in sorted({item["header"] for item in types})]
    source += ["", f"void {function}(::NCommon::TypeRegistry& registry) {{"]
    if not types:
        source.append("    (void)registry;")
    for item in types:
        alias = f"MetadataType_{item['id']}"
        source += [f"    using {alias} = {item['cpp_type']};",
                   f"    static_assert({item['id']}ULL != 0 && {item['version']}U != 0);",
                   f"    static_assert(std::is_enum_v<{alias}> == {'true' if item['kind'] == 'ENUM' else 'false'},",
                   '                  "metadata enum kind mismatch");']
        for field in item["fields"]:
            source += [f"    static_assert(std::is_same_v<decltype(std::declval<{alias}&>().{field['member']}),",
                       f"                                 {field['cpp_type']}>, \"metadata field type mismatch\");"]
        source += [f"    registry.Register<{alias}>(", f"        {{{item['id']}ULL}}, {literal(item['name'])},",
                   f"        {item['version']}U, ::NCommon::ETypeKind::{item['kind']}, {{"]
        source += [f"            GRAPHICS_ENGINE_METADATA_FIELD({alias}, {field['member']}, "
                   f"{field['id']}ULL, {literal(field['name'])})," for field in item["fields"]]
        source.append("        });")
    source += ["}", ""]
    manifest = {"format_version": 1, "function": function, "types": types}
    return header, "\n".join(source), json.dumps(manifest, ensure_ascii=False, sort_keys=True, indent=2) + "\n"


def write_outputs(outputs):
    # Validate/render all outputs before modifying any existing files. A CMake stamp is published only on success.
    staged = []
    try:
        for path, content in outputs:
            data = content.encode("utf-8")
            if path.is_file() and path.read_bytes() == data:
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
                staged.append((Path(stream.name), path))
                stream.write(data)
        for temporary, destination in staged:
            os.replace(temporary, destination)
    finally:
        for temporary, _ in staged:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", type=Path, action="append", required=True)
    parser.add_argument("--function", required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    try:
        if not QUALIFIED.fullmatch(args.function):
            raise ValueError("function must be a qualified C++ identifier")
        destinations = [args.header, args.source, args.manifest]
        resolved = [path.resolve() for path in destinations]
        if len(set(resolved)) != 3 or set(resolved) & {path.resolve() for path in args.schema}:
            raise ValueError("outputs must be distinct and must not overwrite declarations")
        if not re.fullmatch(r"[A-Za-z_0-9.-]+", args.header.name):
            raise ValueError("output header must have a portable file name")
        generated = generate(load_schemas(args.schema), args.function, args.header.name)
        write_outputs(list(zip(destinations, generated)))
    except (ValueError, OSError) as error:
        print(f"metadata-codegen: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
