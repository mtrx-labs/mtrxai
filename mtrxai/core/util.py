"""Tolerant JSON extraction — small local models wrap JSON in prose and code fences."""

from __future__ import annotations

import json


def strip_code_fences(text: str) -> str:
    """Return the inside of the first ```…``` block if the text is fenced, else the text."""
    t = text.strip()
    if not t.startswith("```"):
        return t
    lines = t.splitlines()
    body = lines[1:]  # drop the opening fence (with optional language tag)
    while body and body[-1].strip().startswith("```"):
        body = body[:-1]
    return "\n".join(body)


def extract_json_objects(text: str) -> list[dict]:
    """All syntactically valid JSON objects in `text`, in order of appearance.

    Uses a balanced-brace scanner that respects string literals, so prose between
    objects or braces inside strings cannot confuse it.
    """
    objs: list[dict] = []
    i, n = 0, len(text)
    while i < n:
        if text[i] != "{":
            i += 1
            continue
        depth, instr, esc = 0, False, False
        for j in range(i, n):
            ch = text[j]
            if instr:
                if esc:
                    esc = False
                elif ch == "\\":
                    esc = True
                elif ch == '"':
                    instr = False
            elif ch == '"':
                instr = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    try:
                        obj = json.loads(text[i : j + 1])
                    except (json.JSONDecodeError, ValueError):
                        pass
                    else:
                        if isinstance(obj, dict):
                            objs.append(obj)
                    i = j + 1
                    break
        else:  # unterminated object — stop
            break
        if j >= n and depth > 0:
            break
        i = max(i, j + 1) if depth == 0 else i
    return objs


def first_json_dict(text: str) -> dict | None:
    """The first JSON object in `text`, trying whole-text and fenced forms first."""
    for candidate in (strip_code_fences(text), text):
        c = candidate.strip()
        if c.startswith("{"):
            try:
                obj = json.loads(c)
                if isinstance(obj, dict):
                    return obj
            except (json.JSONDecodeError, ValueError):
                pass
    objs = extract_json_objects(text)
    return objs[0] if objs else None
