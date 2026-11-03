#!/usr/bin/env python3
"""Tests of tools/pack_page.py, run by tools/test-host.sh: python3 tools/test_pack_page.py

- The real page: every string, template literal and regular expression of its script comes out of the stripper byte
  for byte, in order (the literals of the stripped script are the source's), and the packed page holds that script.
- Synthetic scripts (templates over indented and blank lines, // and /* inside strings and templates, regexes with
  / and quotes, </script> spelled inside strings, a comment over lines between two statements with no semicolon):
  node runs the source and the stripped script, and both give the same values.
- A line break after ; { , ( [ goes with the next line's indentation; one after ) or a literal (even one ending
  in ; or ${), and a template literal's own, stay; node gives the same values.
- The CSS and markup rules, and the limit (over it: exit 1, nothing written).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

import pack_page as pp

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PAGE = os.path.join(ROOT, "components", "webui", "www", "index.html")
fails = []


def check(ok, what):
    if not ok:
        fails.append(what)


def literals(js):
    found = []
    out = pp.strip_js(js, found)
    return out, found


# ---- the real page ----
html = open(PAGE, encoding="utf-8").read()
packed = pp.pack(html)
scripts = [m.group(3) for m in pp.re.finditer(r"(<(script)\b[^>]*>)([\s\S]*?)(</\2>)", html, pp.re.I)]
check(scripts, "the page has no script")
for k, js in enumerate(scripts):
    out, src_lits = literals(js)
    _, out_lits = literals(out)
    check(len(src_lits) > 100, "script %d: only %d literals found" % (k, len(src_lits)))
    check(src_lits == out_lits, "script %d: the stripped script's literals differ from the source's" % k)
    at = 0
    for lit in src_lits:  # each one, byte for byte, in order
        j = out.find(lit, at)
        if j < 0:
            check(False, "script %d: a literal is not in the output as it was: %r" % (k, lit[:60]))
            break
        at = j + len(lit)
    check(out in packed, "script %d: the packed page doesn't hold the stripped script" % k)
    check(pp.strip_js(out) == out, "script %d: stripping twice changes it" % k)

# ---- synthetic scripts: the same values before and after (node) ----
SYNTH = r'''
const t = `line 1
    indented ${1 + 2} line

  after a blank line ${ `nested ${'x'} ` } end  `;
const s1 = "a // not a comment", s2 = 'b /* not a comment */', s3 = `c // ${"d /* e */"} f /* g */`;
const r1 = /a\/b[/"']+c/g, r2 = "x/y'z\"".replace(/[/'"]/g, '-');   // a comment after code
const sc = '</script>' + "<\/script>" + `</scr${''}ipt>`;
let a = 1 /* a comment
   over lines, the only break between two statements with no semicolon */ let b = a + 1
const o = { k: `${ {q: 1}.q }` }, w = typeof /x/ === 'object';
const div = 10 / 2 / 1, par = (a + 9) / 2, arr = [8][0] / 4;
if (a) /* inline */ b += 1
return JSON.stringify([t, s1, s2, s3, r1.source, r1.flags, r2, sc, a, b, o.k, w, div, par, arr]);
'''
out, lits = literals(SYNTH)
check(any(x.startswith("`line 1\n    indented ${") for x in lits), "synthetic: the template's first part was not read whole")
check("`line 1\n    indented ${" in out and "} line\n\n  after a blank line ${" in out,
      "synthetic: a template's indented or blank lines changed")
check("// not a comment" in out and "/* not a comment */" in out and "/* g */`" in out,
      "synthetic: // or /* inside a string or template was taken for a comment")
check("// a comment after code" not in out and "over lines" not in out and "inline" not in out,
      "synthetic: a comment is left")
check("let a = 1\nlet b = a + 1" in out, "synthetic: the comment over lines didn't leave its line break")
node = shutil.which("node")
if not node:   # never a silent skip: the host tests need node anyway
    check(False, "node not found: the synthetic scripts can't run")
else:
    def run(code):
        r = subprocess.run([node, "-e", "process.stdout.write((function(){%s})())" % code], capture_output=True, text=True)
        return r.stdout if r.returncode == 0 else "ERROR " + r.stderr.strip().splitlines()[-1]
    a, b = run(SYNTH), run(out)
    check(not a.startswith("ERROR"), "synthetic: the source doesn't run: " + a)
    check(a == b, "synthetic: the stripped script gives other values:\n  %s\n  %s" % (a, b))
    if not a.startswith("ERROR"):
        v = json.loads(a)
        check(v[0].startswith("line 1\n    indented 3 line\n\n  after"), "synthetic: the template's value: %r" % v[0])

# ---- a line break right after the code tokens ; { , ( [ goes (and the next line's indentation with it); one
# after anything else stays, and so does one after a literal ending in such a character ('x;', `${) or inside one ----
JOIN = """const j = [
    1,
    2
], k = f2(
    3, // three
    'x;'
);
function f2(a, b) {
    return a + b
}
const s = 'semi;'
const t = `a,
  ${ 1 }(
[`
let u = (1)
let v = 2; /* a
 comment */

let w = { k: `${
  4 }` }
return JSON.stringify([j, k, s, t, u, v, w.k]);
"""
jout, _ = literals(JOIN)
want = ("const j = [1,2\n], k = f2(3,'x;'\n);function f2(a, b) {return a + b\n}\nconst s = 'semi;'\n"
        "const t = `a,\n  ${ 1 }(\n[`\nlet u = (1)\nlet v = 2;let w = { k: `${\n4 }` }\n"
        "return JSON.stringify([j, k, s, t, u, v, w.k]);")
check(jout == want, "%r,\n  want %r" % (jout, want))
check(pp.strip_js(jout) == jout, "stripping twice changes it")
if node:
    a, b = run(JOIN), run(jout)
    check(not a.startswith("ERROR") and a == b, "the joined script gives other values:\n  %s\n  %s" % (a, b))

# ---- CSS and markup ----
css = 'a{ margin:0/*c*/1px ;\n  content:"a, b; {c} /* x */" }\n/* gone */ b , i{font:12px "X Y", sans-serif}'
got = pp.strip_css(css)
want = 'a{margin:0 1px;content:"a, b; {c} /* x */"}b,i{font:12px "X Y",sans-serif}'
check(got == want, "css: %r, want %r" % (got, want))
mk = '<p>\n  <!-- a note -->\n  <b>x</b>\n</p>\n<!-- Icons: MIT License, (c) someone -->\n'
got = pp.strip_markup(mk)
check(got == '<p>\n<b>x</b>\n</p>\n<!-- Icons: MIT License, (c) someone -->', "markup: %r" % got)
page = '<style>\n a { color:red }\n</style>\n<script>\nconst s = "<\\/script>" // c\n</script>\n'
check(pp.pack(page) == '<style>a{color:red}</style>\n<script>const s = "<\\/script>"</script>\n', "pack: %r" % pp.pack(page))
for bad in ["'open", "`open", "/* open", "a = `${ 1 `"]:
    try:
        pp.strip_js(bad)
        check(False, "an unclosed literal or comment passed: %r" % bad)
    except ValueError:
        pass

# ---- the limit ----
check(pp.LIMIT == 40960, "the limit is %d, not 40 KB" % pp.LIMIT)
with tempfile.TemporaryDirectory() as d:
    gz = os.path.join(d, "index.html.gz")
    devnull = open(os.devnull, "w")
    old = sys.stdout, sys.stderr
    sys.stdout = sys.stderr = devnull
    try:
        over = pp.main(["pack_page.py", PAGE, gz, "--limit", "1000"])
        over_written = os.path.exists(gz)
        ok = pp.main(["pack_page.py", PAGE, gz])
    finally:
        sys.stdout, sys.stderr = old
        devnull.close()
    check(over == 1 and not over_written, "over the limit: exit %r, output written %r" % (over, over_written))
    check(ok == 0 and os.path.getsize(gz) <= pp.LIMIT, "the page is not within the limit")

if fails:
    for f in fails:
        print("test_pack_page: " + f)
    sys.exit(1)
print("test_pack_page: %d literals of the page kept byte for byte; synthetic scripts, CSS, markup and the limit pass"
      % sum(len(literals(js)[1]) for js in scripts))
