#!/usr/bin/env python3
"""The web page as the box embeds it (components/webui/CMakeLists.txt): the source keeps its comments;
the embedded copy drops them, with each line's indentation and blank lines, then is gzip -9 compressed (mtime 0, so
the output is reproducible) and its size is checked against the limit.

  pack_page.py <index.html> <out.gz> [--html <out.html>] [--limit BYTES]

- Markup: <!-- --> comments go, except a licence notice (one naming a licence stays in every copy).
- <style>: /* */ comments go, every run of white space becomes one space, none is kept next to { } ; ,
- <style>'s quoted strings are copied as they are; a comment leaves a space.
- <script>: // and /* */ comments go, each line's indentation and trailing space, runs of spaces, blank lines. The
  line breaks stay (no statement relies on a semicolon being added; a /* */ comment that held one leaves it), except
  one right after the code tokens ; { , ( [ (JOIN_AFTER): after those a line break never changes how the script
  parses (no semicolon is ever added there), so the next line follows at once. Strings, template literals
  (each of their lines too) and regular expressions are copied byte for byte.
- The limit: LIMIT bytes gzipped, the one place it is set (the build and the host tests both run this). Over it,
  this exits 1 with the sizes and writes nothing. --limit overrides it (a test of the failure). --html: also
  writes the stripped page (the host tests check that its scripts compile and run, tools/check_pages.js).
  tools/test_pack_page.py tests this file.
"""
import gzip
import os
import re
import sys

LIMIT = 40 * 1024  # the embedded page, gzipped: 40 KB (32 KB before the box menu gesture setting)
KEYWORDS_BEFORE_REGEX = {"return", "typeof", "case", "do", "else", "in", "of", "new", "delete", "void", "throw",
                         "yield", "await", "instanceof"}
# Code tokens a line break after which is dropped. Each is one punctuator as code appends it; a literal is never
# one of them (it is two characters or more: a quote, a backtick, a slash or a template's } and more), so a literal
# that ends in one ("`a${", "'x;'") keeps the line break after it.
JOIN_AFTER = {";", "{", ",", "(", "["}


def strip_js(src, literals=None):
    """src without comments, indentation, trailing spaces and blank lines; literals (strings, template literals,
    regular expressions) untouched, also each line of a template literal that spans lines. literals: a list that
    gets each literal as copied."""
    out = []
    i, n = 0, len(src)
    last = ""          # the last significant token (a punctuator, or a word), for telling a regex from a division
    stack = []         # template literals open around this point: the brace depth of each ${...}
    depth = 0          # braces open in the current ${...}
    joined = [-1]      # len(out) when a line break was last dropped after a JOIN_AFTER token

    def at_line_start():
        return not out or out[-1].endswith("\n")    # a literal never ends in a newline: this one is code's

    def space():                                    # code's white space: one, never at a line's start or twice, nor
        if not at_line_start() and out[-1] != " " and joined[0] != len(out):   # as the indentation of a joined line
            out.append(" ")

    def newline():                                  # code's line break: no trailing space, no blank line; none at
        while out and out[-1] == " ":               # all after a JOIN_AFTER token
            out.pop()
        if out and out[-1] in JOIN_AFTER:
            joined[0] = len(out)
        elif not at_line_start():
            out.append("\n")

    def literal(s):
        out.append(s)
        if literals is not None:
            literals.append(s)

    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":                                   # a line comment: up to the newline
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and nxt == "*":                                   # a block comment: a space in its place, or the
            j = src.find("*/", i + 2)                                 # line break it held (a statement may end there)
            if j < 0:
                raise ValueError("an unclosed /* comment")
            newline() if "\n" in src[i:j] else space()
            i = j + 2
            continue
        if c in "'\"":                                                # a string
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\":
                    j += 1
                elif src[j] == "\n":
                    raise ValueError("a newline in a string at %d" % i)
                j += 1
            if j >= n:
                raise ValueError("an unclosed string at %d" % i)
            literal(src[i:j + 1])
            i = j + 1
            last = "str"
            continue
        if c == "`" or (c == "}" and stack and depth == 0):           # a template literal, or its text after ${...}
            if c == "}":
                depth = stack.pop()                                   # back in the code around the template
            j = i + 1
            while j < n and src[j] != "`" and not (src[j] == "$" and j + 1 < n and src[j + 1] == "{"):
                j += 2 if src[j] == "\\" else 1
            if j >= n:
                raise ValueError("an unclosed template literal at %d" % i)
            if src[j] == "`":
                literal(src[i:j + 1])
                i = j + 1
                last = "str"
            else:                                                     # ${: code until its closing brace
                literal(src[i:j + 2])
                i = j + 2
                stack.append(depth)
                depth = 0
                last = "{"
            continue
        if c == "/":                                                  # a regular expression, or a division
            if last in KEYWORDS_BEFORE_REGEX or last not in ("str", "word", ")", "]", "}"):
                j, cls = i + 1, False
                while j < n:
                    if src[j] == "\\":
                        j += 2
                        continue
                    if src[j] == "\n":
                        raise ValueError("a newline in a regular expression at %d" % i)
                    if src[j] == "[":
                        cls = True
                    elif src[j] == "]":
                        cls = False
                    elif src[j] == "/" and not cls:
                        break
                    j += 1
                j += 1
                while j < n and (src[j].isalnum()):
                    j += 1                                            # its flags
                literal(src[i:j])
                i = j
                last = "str"
                continue
            out.append(c)
            i += 1
            last = "/"
            continue
        if c in " \t\r":
            space()
            i += 1
            continue
        if c == "\n":
            newline()
            i += 1
            continue
        if c.isalnum() or c in "_$":
            j = i
            while j < n and (src[j].isalnum() or src[j] in "_$."):
                j += 1
            word = src[i:j]
            out.append(word)
            i = j
            last = word if word in KEYWORDS_BEFORE_REGEX else "word"
            continue
        if c == "{" and stack:
            depth += 1
        elif c == "}" and stack:
            depth -= 1
        out.append(c)
        i += 1
        last = c
    if stack:
        raise ValueError("an unclosed ${ in a template literal")
    newline()
    return "".join(out).rstrip("\n")


def strip_css(src):
    """src without comments (a space in their place), every run of white space one space, none next to { } ; ,;
    quoted strings untouched."""
    strings = []

    def keep(m):
        if m.group(0).startswith("/*"):
            return " "
        strings.append(m.group(0))
        return "\0"

    text = re.sub(r"\"(?:[^\"\\\n]|\\.)*\"|'(?:[^'\\\n]|\\.)*'|/\*[\s\S]*?\*/", keep, src)
    text = re.sub(r" ?([{};,]) ?", r"\1", re.sub(r"\s+", " ", text)).strip()
    it = iter(strings)
    return re.sub("\0", lambda m: next(it), text)


def strip_markup(src):
    src = re.sub(r"<!--[\s\S]*?-->", lambda m: m.group(0) if re.search(r"licen[cs]e", m.group(0), re.I) else "", src)
    lines = [ln.strip() for ln in src.split("\n")]
    return "\n".join(ln for ln in lines if ln)


def pack(html):
    out, at = [], 0
    for m in re.finditer(r"(<(script|style)\b[^>]*>)([\s\S]*?)(</\2>)", html, re.I):
        out.append(strip_markup(html[at:m.start()]))
        body = strip_js(m.group(3)) if m.group(2).lower() == "script" else strip_css(m.group(3))
        out.append(m.group(1) + body + m.group(4))
        at = m.end()
    out.append(strip_markup(html[at:]))
    return "\n".join(x for x in out if x) + "\n"


def main(argv):
    args = argv[1:]
    limit, html_out = LIMIT, None
    if "--limit" in args:
        k = args.index("--limit")
        limit = int(args[k + 1])
        del args[k:k + 2]
    if "--html" in args:
        k = args.index("--html")
        html_out = args[k + 1]
        del args[k:k + 2]
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    src = open(args[0], encoding="utf-8").read()
    page = pack(src).encode("utf-8")
    gz = gzip.compress(page, 9, mtime=0)
    raw_gz = len(gzip.compress(src.encode("utf-8"), 9, mtime=0))
    print("webui: index.html %d bytes (gzip %d), embedded %d bytes, gzip %d bytes (%.1f KB%s)" % (
        len(src.encode("utf-8")), raw_gz, len(page), len(gz), len(gz) / 1024,
        ", limit %d" % limit if limit else ""))
    if limit and len(gz) > limit:                                     # no output: the build can't go on with it
        sys.stderr.write("webui: the page is %d bytes gzipped, over the %d byte limit\n" % (len(gz), limit))
        if os.path.exists(args[1]):
            os.remove(args[1])
        return 1
    open(args[1], "wb").write(gz)
    if html_out:
        open(html_out, "wb").write(page)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
