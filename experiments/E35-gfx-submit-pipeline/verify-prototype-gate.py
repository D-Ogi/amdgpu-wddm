"""Compatibility entry point for the shared quality compiler gate."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).resolve().parents[2] / 'tools/quality/prototype_gate.py'), run_name='__main__')
