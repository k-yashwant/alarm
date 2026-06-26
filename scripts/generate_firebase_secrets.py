#!/usr/bin/env python3
import sys
from pathlib import Path


REQUIRED_KEYS = (
    "FIREBASE_WEB_API_KEY",
    "FIREBASE_AUTH_EMAIL",
    "FIREBASE_AUTH_PASSWORD",
    "FIREBASE_USER_UID",
    "FIREBASE_HOST",
)

PLACEHOLDER_VALUES = (
    "your_firebase_web_api_key",
    "your_firebase_auth_email",
    "your_firebase_auth_password",
)


def parse_env(path):
    values = {}
    for raw_line in path.read_text().splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]
        values[key] = value
    return values


def c_string(value):
    return value.replace("\\", "\\\\").replace('"', '\\"')


def main():
    if len(sys.argv) != 3:
        print("usage: generate_firebase_secrets.py <.env> <output-header>", file=sys.stderr)
        return 2

    env_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    if not env_path.exists():
        print(f"Missing {env_path}. Create it from .env.example and fill Firebase credentials.", file=sys.stderr)
        return 1

    values = parse_env(env_path)
    missing = [key for key in REQUIRED_KEYS if not values.get(key)]
    if missing:
        print(f"Missing required Firebase .env keys: {', '.join(missing)}", file=sys.stderr)
        return 1

    placeholders = [key for key in REQUIRED_KEYS if values[key] in PLACEHOLDER_VALUES]
    if placeholders:
        print(f"Replace placeholder Firebase .env values for: {', '.join(placeholders)}", file=sys.stderr)
        return 1

    output_path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "/* Generated from .env. Do not commit this file. */",
    ]
    for key in REQUIRED_KEYS:
        lines.append(f'#define {key} "{c_string(values[key])}"')
    lines.append("")

    output_path.write_text("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
