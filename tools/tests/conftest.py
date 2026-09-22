"""Shared pytest helpers for tools/ scripts (extension-less python3 executables)."""

import importlib.machinery
import importlib.util
import os
import sys

import pytest

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_tool(name: str):
    """Import tools/<name> (no .py suffix) as a module."""
    path = os.path.join(TOOLS_DIR, name)
    loader = importlib.machinery.SourceFileLoader(name.replace("-", "_"), path)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    sys.modules[loader.name] = module  # dataclasses resolve annotations via sys.modules
    loader.exec_module(module)
    return module


@pytest.fixture(scope="session")
def fleet_doctor():
    return load_tool("fleet-doctor")

# Tests must never reach the real sudo (this Mac has passwordless sudo); a shim on PATH fails loudly.
import os as _os
_os.environ["PATH"] = _os.path.join(_os.path.dirname(__file__), "fakebin") + _os.pathsep + _os.environ.get("PATH", "")
