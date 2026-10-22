"""Teleprompter's build tool: a stdlib-only Python zipapp around cmake + ninja.

The runtime imports nothing outside the standard library, which is why it can
be shipped as a single ``.pyz`` and run by a stock interpreter.

Ported from Klend-ario's ``scripts/src/klend_ario_build``.
"""

from __future__ import annotations

__version__ = "0.1.0"
