#!/usr/bin/env python3
"""Checks the CSS class naming rules of develop_docs/reference/style-guide.md (section 15).

Run from the project root:  ./scripts/check_css_bem.py
Exits 1 and lists every violation, 0 when clean.

  1. Every class in the epoch 3 stylesheets (public and admin), epoch 3 templates,
     html/assets/js and the C sources is BEM with the project prefix:
     boat-rudder-<block>[__<element>][--<modifier>], words in lowercase joined by single
     hyphens.
  2. No old `boat-rudder__...` name is left anywhere in html/ or src/.
  3. Epoch -1/0/1/2 templates carry no class attribute (they have no stylesheet), except the
     ones listed in OLD_EPOCH_ALLOWED, which epoch 2's inline <style> uses.
  4. The admin stylesheets have literal colors only in their :root token block; rules use
     var(--br-admin-*) (or a public var(--br-color-*, #fallback)).
"""
import glob
import re
import sys

WORD = r"[a-z0-9]+(?:-[a-z0-9]+)*"
BEM = re.compile(rf"^boat-rudder-{WORD}(?:__{WORD})?(?:--{WORD})?$")
# A class completed at runtime: "boat-rudder-code-token--" + kind, "...--w%s", ...
DYNAMIC_PREFIX = re.compile(rf"^boat-rudder-{WORD}(?:__{WORD})?--$")

OLD_EPOCH_ALLOWED = {
    "html/templates/elements/paragraph/paragraph_epoch2.html": {"boat-rudder-paragraph"},
}


def classes_in_css(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    for selector in re.findall(r"([^{}]+)\{", text):
        if selector.strip().startswith("@"):
            continue
        yield from re.findall(r"\.(-?[A-Za-z_][\w-]*)", selector)


def classes_in_markup(text):
    """class="..." attributes (also escaped in C strings), <style> blocks and JS class APIs."""
    for attr in re.findall(r'class=\\?["\']([^"\'\\]*)', text):
        yield from attr.split()
    for block in re.findall(r"<style[^>]*>(.*?)</style>", text, flags=re.S):
        yield from classes_in_css(block)
    for args in re.findall(r"classList\.(?:add|remove|toggle|contains|replace)\(([^)]*)\)", text):
        yield from re.findall(r"'([^']+)'", args)
    for value in re.findall(r"className\s*\+?=\s*'([^']*)'", text):
        yield from value.split()
    for sel in re.findall(r"(?:querySelector(?:All)?|closest|matches)\(\s*['\"]([^'\"]+)['\"]", text):
        yield from re.findall(r"\.(-?[A-Za-z_][\w-]*)", sel)


def is_placeholder(name):
    return bool(re.search(r"[%{}$<>]", name))


def main():
    problems = []

    epoch3 = (glob.glob("html/themes/*/styles*_epoch3.css")
              + [f for f in glob.glob("html/**/*_epoch3.html", recursive=True)]
              + glob.glob("html/assets/js/*.js")
              + glob.glob("src/**/*.c", recursive=True))
    for path in sorted(epoch3):
        text = open(path, encoding="utf-8", errors="replace").read()
        names = classes_in_css(text) if path.endswith(".css") else classes_in_markup(text)
        for name in sorted(set(names)):
            if is_placeholder(name) or BEM.match(name) or DYNAMIC_PREFIX.match(name):
                continue
            problems.append(f"{path}: '{name}' is not boat-rudder-block__element--modifier")

    for path in sorted(glob.glob("html/**/*", recursive=True) + glob.glob("src/**/*", recursive=True)):
        if not path.endswith((".html", ".css", ".js", ".c", ".h")):
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        for lineno, line in enumerate(text.splitlines(), 1):
            if "boat-rudder__" in line:
                problems.append(f"{path}:{lineno}: old 'boat-rudder__' name")

    old_epoch = [f for f in glob.glob("html/**/*.html", recursive=True)
                 if re.search(r"_epoch(-1|0|1|2)\.html$", f)]
    for path in sorted(old_epoch):
        text = open(path, encoding="utf-8", errors="replace").read()
        allowed = OLD_EPOCH_ALLOWED.get(path, set())
        for attr in re.findall(r'class="([^"]*)"', text):
            for name in attr.split():
                if name not in allowed:
                    problems.append(f"{path}: class '{name}' on an epoch without a stylesheet")

    color = re.compile(r"#[0-9a-fA-F]{3,8}\b|rgba?\(")
    for path in sorted(glob.glob("html/themes/*/styles_admin_epoch3.css")):
        text = open(path, encoding="utf-8", errors="replace").read()
        text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
        root = re.search(r":root\s*\{[^}]*\}", text)
        for lineno, line in enumerate(text.splitlines(), 1):
            if root and root.start() <= sum(len(l) + 1 for l in text.splitlines()[:lineno - 1]) < root.end():
                continue
            bare = re.sub(r"var\(--br-color-[\w-]+,\s*#[0-9a-fA-F]{3,8}\)", "", line)
            if color.search(bare):
                problems.append(f"{path}:{lineno}: literal color outside :root - use a --br-admin-* token")

    for problem in problems:
        print(problem)
    print(f"{len(problems)} problem(s)" if problems else "CSS class names: OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
