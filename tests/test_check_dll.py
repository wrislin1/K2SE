"""tools/check_dll.py must FAIL, not warn, when the built DLL imports anything
but KERNEL32: the bridge loads ws2_32 with LoadLibrary precisely so the import
table stays KERNEL32-only, and a warning was the one check nobody read.
Run: python -m pytest tests/test_check_dll.py"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import check_dll  # noqa: E402


def test_kernel32_only_is_ok():
    ok, label = check_dll.imports_verdict(["KERNEL32.dll"])
    assert ok is True and label == "OK"


def test_any_other_import_fails():
    ok, label = check_dll.imports_verdict(["KERNEL32.dll", "WS2_32.dll"])
    assert ok is False and label == "FAIL"


def test_case_of_the_module_name_does_not_matter():
    assert check_dll.imports_verdict(["kernel32.DLL"])[0] is True


def test_no_imports_at_all_fails_too():
    # A proxy that imports nothing cannot have resolved GetProcAddress/LoadLibrary.
    assert check_dll.imports_verdict([])[0] is False
