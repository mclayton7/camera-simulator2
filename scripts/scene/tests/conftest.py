"""Shared pytest setup: tests marked `network` run only with --network."""

import pytest


def pytest_addoption(parser):
    parser.addoption("--network", action="store_true", help="run tests that need the internet")


def pytest_collection_modifyitems(config, items):
    if config.getoption("--network"):
        return
    skip = pytest.mark.skip(reason="needs --network")
    for item in items:
        if "network" in item.keywords:
            item.add_marker(skip)
