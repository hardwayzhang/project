"""End-to-end test: build sample binary + mprof, convert, validate JSON."""

from __future__ import annotations

import json
import os
import sys
import subprocess
from pathlib import Path

PROJ = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJ))

from mprof2pprof.cli import main as cli_main  # noqa: E402
from mprof2pprof.mprof_format import parse_mprof  # noqa: E402


def _run_generator() -> Path:
    out_dir = PROJ / "examples" / "out"
    subprocess.check_call(
        [sys.executable, str(PROJ / "examples" / "generate_sample_mprof.py")],
        cwd=str(PROJ),
    )
    return out_dir


def test_roundtrip(tmp_path: Path) -> None:
    out_dir = _run_generator()
    mprof_path = out_dir / "capture.mprof"
    binary_path = out_dir / "fakeserver"
    out_json = tmp_path / "capture.pprof.json"

    rc = cli_main([
        str(mprof_path),
        "--binary", str(binary_path),
        "--output", str(out_json),
        "--summary",
    ])
    assert rc == 0
    assert out_json.exists()

    doc = json.loads(out_json.read_text())

    # ---- Schema sanity ------------------------------------------------
    for required in ("stringTable", "sampleType", "sample", "mapping",
                     "location", "function", "periodType"):
        assert required in doc, required
    assert doc["stringTable"][0] == ""

    # ---- Mapping refers back to our binary ----------------------------
    assert len(doc["mapping"]) == 1
    mapping = doc["mapping"][0]
    assert doc["stringTable"][mapping["filename"]].endswith("fakeserver")

    # ---- Functions resolved from the binary ---------------------------
    fn_names = {doc["stringTable"][f["name"]] for f in doc["function"]}
    expected = {"Leaf_AllocChunk", "Middle_PrepareBuffer",
                "Outer_HandleRequest", "main"}
    missing = expected - fn_names
    assert not missing, f"functions not symbolised: {missing}; got {fn_names}"

    # ---- Locations carry leaf-first inline-expanded lines -------------
    for loc in doc["location"]:
        assert "line" in loc and loc["line"], loc
        for line in loc["line"]:
            assert line["functionId"] >= 1
            assert isinstance(line["line"], int)

    # ---- Samples reference valid locations and have 4 values ----------
    valid_loc_ids = {loc["id"] for loc in doc["location"]}
    for sample in doc["sample"]:
        for lid in sample["locationId"]:
            assert lid in valid_loc_ids
        assert len(sample["value"]) == 4

    # ---- Tally must reflect what the generator emitted ----------------
    parsed = parse_mprof(str(mprof_path))
    total_alloc_bytes = 0
    for tok in parsed.tokens:
        # MALLOC=0, REALLOC=2 contribute to alloc_space
        if tok.op.value in (0, 2):
            total_alloc_bytes += tok.size
    summed_in_pprof = sum(s["value"][1] for s in doc["sample"])
    assert summed_in_pprof == total_alloc_bytes, (
        f"alloc_space sum mismatch: pprof={summed_in_pprof}, expected={total_alloc_bytes}"
    )


if __name__ == "__main__":
    # Allow running as a script for quick local iteration.
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        test_roundtrip(Path(td))
    print("OK")
