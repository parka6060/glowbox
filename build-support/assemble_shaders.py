"""Assemble editable OBS effects and embed the renderer in the plugin."""
import argparse
from pathlib import Path
import re


def read_local(root, name):
    if not re.fullmatch(r"[A-Za-z0-9_-]+\.effect", name):
        raise ValueError(f"Expected a local .effect filename: {name}")
    return read_manifest_stage(root, name)


def read_manifest_stage(root, name):
    # Manifest stages may live in subdirectories, but never outside shader_root.
    if not re.fullmatch(r"(?:[A-Za-z0-9_-]+/)*[A-Za-z0-9_-]+\.effect", name):
        raise ValueError(f"Expected a relative .effect stage path: {name}")
    shader_root = root.resolve(strict=True)
    stage = (shader_root / name).resolve(strict=True)
    try:
        stage.relative_to(shader_root)
    except ValueError:
        raise ValueError(f"Shader stage escapes shader root: {name}") from None
    return stage.read_bytes().decode("utf-8")


def assemble(root):
    manifest = "manifest.txt"
    names = [line.strip() for line in (root / manifest).read_text().splitlines()
             if line.strip() and not line.lstrip().startswith("#")]
    if not names or len(names) != len(set(names)):
        raise ValueError("Shader manifest must contain unique stages")
    compositor = "\n".join(read_manifest_stage(root, name) for name in names)
    native_root = root / "native"
    renderer = read_local(native_root, "renderer.effect")

    def expand(match):
        stage = read_local(native_root, match.group(1))
        if re.search(r"(?m)^[ \t]*#include\b", stage):
            raise ValueError("Shader stages must not contain nested includes")
        return stage

    renderer = re.sub(r'(?m)^[ \t]*#include[ \t]+"([A-Za-z0-9_-]+\.effect)"[ \t]*\r?$', expand, renderer)
    if re.search(r"(?m)^[ \t]*#include\b", renderer):
        raise ValueError("Renderer includes must name a local .effect stage")
    if ')GBFX"' in renderer:
        raise ValueError("Renderer contains the embedding delimiter")
    parts = [renderer[index:index + 8000] for index in range(0, len(renderer), 8000)]
    header = '#pragma once\nstatic const char *const kRendererShaderParts[] = {\n'
    header += "\n".join('R"GBFX(' + part + ')GBFX",' for part in parts) + "\n};"
    return compositor, header


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("shader_root", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    compositor, header = assemble(args.shader_root)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "compositor.effect").write_bytes(compositor.encode("utf-8"))
    (args.output / "renderer_shader.hpp").write_bytes(header.encode("utf-8"))


if __name__ == "__main__":
    main()
