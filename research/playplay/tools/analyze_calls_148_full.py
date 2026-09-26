#!/usr/bin/env python3
"""Run the full static call-candidate search for the exact raw 148 dump."""

import sys
from analyze_calls_148 import main

if __name__ == '__main__':
    sys.argv.append('--full')
    raise SystemExit(main())
