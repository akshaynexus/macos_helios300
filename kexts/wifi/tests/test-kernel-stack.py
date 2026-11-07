#!/usr/bin/env python3
"""CLI entry point for the production kernel-stack regression."""
import unittest
from test_kernel_stack import KernelStackTests
if __name__ == '__main__':
    unittest.main(verbosity=2)
